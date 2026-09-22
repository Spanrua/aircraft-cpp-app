# -*- coding: utf-8 -*-
"""
CNNTRANS_v5 adapted for the 3943B DJI image-transmission IQ dataset.

Expected dataset:
  3943B_58g_dataset/
    air2_01/samples/B_Air2/*.npz
    ...

Each npz must contain:
  iq: (N, 2, 4096) float32, where channel 0 is I and channel 1 is Q.
"""

import argparse
import bisect
import csv
import hashlib
import importlib.util
import json
import math
import os
import sys
import time
from array import array
from collections import deque
from collections.abc import Mapping
from importlib import metadata
from pathlib import Path, PurePosixPath

REQUIRED_PACKAGES = {
    "numpy": "numpy",
    "torch": "torch",
    "sklearn": "scikit-learn",
    "matplotlib": "matplotlib",
    "tqdm": "tqdm",
}


def check_required_packages(verbose=False):
    missing = [
        pip_name
        for import_name, pip_name in REQUIRED_PACKAGES.items()
        if importlib.util.find_spec(import_name) is None
    ]
    if missing:
        print("Missing Python packages: " + ", ".join(missing))
        print("Install them with:")
        print(
            "python -m pip install --cache-dir /root/autodl-tmp/pip_cache "
            + " ".join(missing)
        )
        raise SystemExit(1)
    if verbose:
        print(f"Python: {sys.executable}", flush=True)
        print(f"Python version: {sys.version.split()[0]}", flush=True)
        for import_name, pip_name in REQUIRED_PACKAGES.items():
            try:
                version = metadata.version(pip_name)
            except metadata.PackageNotFoundError:
                version = "installed"
            print(f"{pip_name}: {version}", flush=True)


if "--check-env" in sys.argv:
    check_required_packages(verbose=True)
    raise SystemExit(0)

check_required_packages()

import matplotlib
import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F
import torch.optim as optim
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from manifest_index_cache import load_manifest_index, save_manifest_index
from rf_window_stats import RF_STATS_DIM, RF_STATS_VERSION, rf_stats_path
from sklearn.metrics import accuracy_score, classification_report, confusion_matrix
from torch.utils.data import BatchSampler, DataLoader, Dataset, WeightedRandomSampler
from tqdm import tqdm


_cancel_event_emitted = False


def emit_json_event(event_type, **payload):
    """Emit one machine-readable event without changing human log output."""

    print(
        json.dumps(
            {"type": event_type, "timestamp": time.time(), **payload},
            ensure_ascii=False,
            default=str,
        ),
        flush=True,
    )


def atomic_write_text(path, text):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = path.with_name(path.name + ".tmp")
    tmp_path.write_text(text, encoding="utf-8")
    os.replace(tmp_path, path)


def atomic_torch_save(payload, path):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = path.with_name(path.name + ".tmp")
    torch.save(payload, tmp_path)
    os.replace(tmp_path, path)


CLASS_NAMES = (
    "air2_01",
    "air2_02",
    "air2s_01",
    "air2s_02",
    "air3_01",
    "air3_02",
    "air3s_01",
    "air3s_02",
    "mini3pro_01",
    "mini3pro_02",
    "mini4pro_01",
    "mini4pro_02",
    "mini5pro_01",
    "mini5pro_02",
    "yu4pro_01",
    "yu4pro_02",
)
NORMALIZED_CLASS_NAMES = {
    name.replace("_", "").replace("-", "").lower(): idx
    for idx, name in enumerate(CLASS_NAMES)
}

CACHE_FEATURE_MODE = "legacy_iq_stft_2ch"
CACHE_DTYPE = "float16"
CACHE_WINDOW_SHAPE = (2, 128, 33)
RAW_IQ_CACHE_DTYPE = "float16"
RAW_IQ_WINDOW_SHAPE = (2, 4096)
STFT_N_FFT = 256
STFT_HOP_LENGTH = 128
PHASE_AWARE_SHORT_N_FFT = 64
PHASE_AWARE_SHORT_HOP_LENGTH = 32
PHASE_AWARE_SEGMENTS = 32
PHASE_AWARE_LAGS = (1, 4, 16, 64)
PHASE_AWARE_SEQUENCE_CHANNELS = 3 + 6 * len(PHASE_AWARE_LAGS)
FEATURE_MODE_CHOICES = ("legacy", "complex_signed_mag")
FEATURE_MODE_IDS = {
    "legacy": CACHE_FEATURE_MODE,
    "complex_signed_mag": "complex_signed_stft_signed_mag_2ch",
}
MANIFEST_SPLIT_NAMES = frozenset({"train", "val", "test", "internal_dev"})


def inspect_manifest_split_contract(data_root):
    """Return the split contract without changing any source manifest."""

    data_root = Path(data_root)
    counts = {}
    record_count = 0
    missing_split_count = 0
    invalid_splits = []
    for manifest_path in sorted(data_root.rglob("manifest.jsonl")):
        with manifest_path.open("r", encoding="utf-8") as manifest_file:
            for line_number, line in enumerate(manifest_file, start=1):
                if not line.strip():
                    continue
                record_count += 1
                try:
                    split = str(json.loads(line).get("split") or "").strip().lower()
                except json.JSONDecodeError:
                    invalid_splits.append(f"{manifest_path}:{line_number}: invalid JSON")
                    continue
                if not split:
                    missing_split_count += 1
                    continue
                if split not in MANIFEST_SPLIT_NAMES:
                    invalid_splits.append(f"{manifest_path}:{line_number}: {split!r}")
                    continue
                counts[split] = counts.get(split, 0) + 1
    source = "manifest" if counts else "source_group"
    if counts and (missing_split_count or invalid_splits):
        examples = "; ".join(invalid_splits[:3])
        raise ValueError(
            "Manifest split contract is incomplete: every record must contain one "
            "of train/val/test/internal_dev when explicit split is used. "
            f"missing={missing_split_count}, invalid={len(invalid_splits)}"
            + (f" ({examples})" if examples else "")
        )
    if not counts and invalid_splits:
        raise ValueError(
            "Manifest contains invalid split values before an explicit split contract "
            f"could be established: {invalid_splits[:3]}"
        )
    return {
        "source": source,
        "counts": counts,
        "records": record_count,
        "missing": missing_split_count,
        "invalid": invalid_splits,
    }


def feature_mode_uses_onesided_stft(feature_mode):
    if feature_mode not in FEATURE_MODE_CHOICES:
        raise ValueError(
            f"Unknown feature_mode={feature_mode!r}; "
            f"expected one of {FEATURE_MODE_CHOICES}."
        )
    return feature_mode == "legacy"


def spectrogram_cache_window_shape(stitch_windows=1):
    time_bins = (4096 * int(stitch_windows)) // STFT_HOP_LENGTH + 1
    return (2, 128, time_bins)


def stable_std(values, dim, epsilon=1e-6):
    mean = values.mean(dim=dim, keepdim=True)
    variance = (values - mean).square().mean(dim=dim)
    return (variance + epsilon).sqrt()


device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
if device.type == "cuda":
    torch.backends.cudnn.benchmark = True
    torch.set_float32_matmul_precision("high")


def set_global_seed(seed):
    torch.manual_seed(seed)
    np.random.seed(seed)
    if torch.cuda.is_available():
        torch.cuda.manual_seed_all(seed)


set_global_seed(42)


def cancellation_requested():
    flag_path = os.environ.get("AIRCRAFT_CANCEL_FLAG", "")
    return bool(flag_path and Path(flag_path).exists())


def raise_if_cancelled():
    global _cancel_event_emitted
    if cancellation_requested():
        if not _cancel_event_emitted:
            emit_json_event(
                "cancelled",
                message="Cancellation requested by Qt workflow",
            )
            _cancel_event_emitted = True
        raise KeyboardInterrupt("Cancellation requested by Qt workflow")


def stable_split_key(text):
    digest = hashlib.md5(text.encode("utf-8")).hexdigest()
    return int(digest[:8], 16) / 0xFFFFFFFF


def infer_label_from_path(path):
    normalized_path = str(path).replace("\\", "/")
    for part in reversed(PurePosixPath(normalized_path).parts):
        normalized_part = part.replace("_", "").replace("-", "").lower()
        if normalized_part in NORMALIZED_CLASS_NAMES:
            return NORMALIZED_CLASS_NAMES[normalized_part]

    normalized = normalized_path.replace("_", "").replace("-", "").lower()
    for normalized_name, idx in NORMALIZED_CLASS_NAMES.items():
        if normalized_name in normalized:
            return idx
    raise ValueError(f"Cannot infer class label from path: {path}")


def load_train_exclude_sources(path):
    if not path:
        return set()
    exclude_path = Path(path)
    if not exclude_path.exists():
        raise FileNotFoundError(f"Train exclude source list not found: {exclude_path}")

    sources = set()
    if exclude_path.suffix.lower() == ".json":
        payload = json.loads(exclude_path.read_text(encoding="utf-8"))
        if isinstance(payload, dict):
            payload = payload.get("items", payload.get("sources", []))
        for item in payload:
            if isinstance(item, str):
                sources.add(item)
            elif isinstance(item, Mapping):
                for key in ("source_file", "npz_path", "path", "npz_name"):
                    value = item.get(key)
                    if value:
                        sources.add(str(value))
                        if key in {"npz_path", "path", "npz_name"}:
                            sources.add(Path(str(value)).name)
                            sources.add(Path(str(value)).stem)
    else:
        with exclude_path.open("r", encoding="utf-8", newline="") as handle:
            reader = csv.DictReader(handle)
            for row in reader:
                if not row:
                    continue
                for key in ("source_file", "npz_path", "path", "npz_name"):
                    value = row.get(key)
                    if value:
                        sources.add(str(value))
                        if key in {"npz_path", "path", "npz_name"}:
                            sources.add(Path(str(value)).name)
                            sources.add(Path(str(value)).stem)
    return {source for source in sources if source}


class IqrawNpzDataset(Dataset):
    """Window-level dataset backed by compressed npz files.

    The dataset indexes npz files as chunks and loads one window on demand, so it
    avoids materializing the full 164 GB dataset in memory.
    """

    def __init__(
        self,
        root_dir,
        split="train",
        val_ratio=0.2,
        is_train=True,
        max_files=None,
        max_files_per_class=0,
        cache_size=2,
        return_raw_iq=True,
        index_cache_dir="",
        internal_dev_ratio=0.0,
        raw_iq_cache_dir="",
        train_exclude_sources=None,
        stitch_windows=1,
        stitch_stride=1,
    ):
        self.root_dir = Path(root_dir)
        self.split = split
        self.val_ratio = val_ratio
        self.is_train = is_train
        self.return_raw_iq = return_raw_iq
        self.index_cache_dir = Path(index_cache_dir) if index_cache_dir else None
        self.internal_dev_ratio = float(internal_dev_ratio)
        self.raw_iq_cache_dir = Path(raw_iq_cache_dir) if raw_iq_cache_dir else None
        self.train_exclude_sources = set(train_exclude_sources or [])
        self.stitch_windows = int(stitch_windows)
        self.stitch_stride = int(stitch_stride)
        if self.stitch_windows < 1:
            raise ValueError("stitch_windows must be >= 1.")
        if self.stitch_stride < 1:
            raise ValueError("stitch_stride must be >= 1.")
        if not 0.0 <= self.internal_dev_ratio < 1.0:
            raise ValueError("internal_dev_ratio must be in [0, 1).")
        self.cache = {}
        self.cache_order = []
        self.cache_size = max(1, int(cache_size))
        self.chunks = []
        # Compact manifest index: path id and local window index fit in uint32;
        # the 16-class label fits in uint8. This avoids millions of Python tuples.
        self.record_path_ids = array("I")
        self.record_local_idxs = array("I")
        self.record_labels = array("B")
        self.record_count = 0
        self.paths = []
        self.path_to_id = {}
        self.cumulative_sizes = []
        self.labels = self.record_labels
        self.groups_by_label = {}
        self.val_groups = set()
        self.internal_dev_groups = set()
        self.source_group_to_paths = {}
        self.selected_source_groups = set()
        self.selected_source_files = set()
        self.path_record_counts = {}
        self.path_min_indices = {}
        self.path_max_indices = {}
        self.manifest_index_issues = []
        self.manifest_split_source = "source_group"
        self.manifest_split_counts = {}

        if not self.root_dir.exists():
            raise FileNotFoundError(f"Dataset directory not found: {self.root_dir}")

        has_manifest = self._load_or_build_manifest_index(
            max_files=max_files,
            max_files_per_class=max_files_per_class,
        )
        if not has_manifest:
            self._index_from_npz_files(max_files=max_files)

        if not self.record_count and not self.chunks:
            raise RuntimeError(f"No usable samples for split={split}")
        self.raw_iq_cache_paths = None
        if self.raw_iq_cache_dir is not None:
            self.raw_iq_cache_paths = self._validate_raw_iq_cache()
        self._print_index_summary()

    def _validate_raw_iq_cache(self):
        if not self.record_count:
            raise ValueError("Raw-IQ cache requires manifest-backed records.")
        cache_paths = [
            raw_iq_cache_path(self.raw_iq_cache_dir, self.root_dir, path)
            for path in self.paths
        ]
        errors = []
        for path_id, (source_path, cache_path) in enumerate(
            zip(self.paths, cache_paths)
        ):
            if not cache_path.exists():
                errors.append(f"{cache_path}: missing raw-IQ cache")
                continue
            values = np.load(cache_path, mmap_mode="r")
            try:
                expected_shape = (
                    self.path_record_counts[path_id],
                    *RAW_IQ_WINDOW_SHAPE,
                )
                if values.shape != expected_shape:
                    errors.append(
                        f"{cache_path}: shape={values.shape}, expected {expected_shape}"
                    )
                if values.dtype != np.dtype(RAW_IQ_CACHE_DTYPE):
                    errors.append(
                        f"{cache_path}: dtype={values.dtype}, "
                        f"expected {RAW_IQ_CACHE_DTYPE}"
                    )
            finally:
                mmap = getattr(values, "_mmap", None)
                if mmap is not None:
                    mmap.close()

            meta_path = cache_path.with_suffix(".json")
            if not meta_path.exists():
                errors.append(f"{meta_path}: missing raw-IQ cache metadata")
                continue
            try:
                metadata = json.loads(meta_path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError) as exc:
                errors.append(f"{meta_path}: invalid metadata ({exc})")
                continue
            source_stat = source_path.stat()
            expected_metadata = {
                "source_npz": str(source_path),
                "source_size": source_stat.st_size,
                "source_mtime_ns": source_stat.st_mtime_ns,
                "dtype": RAW_IQ_CACHE_DTYPE,
                "shape": [self.path_record_counts[path_id], *RAW_IQ_WINDOW_SHAPE],
            }
            for key, expected in expected_metadata.items():
                if metadata.get(key) != expected:
                    errors.append(
                        f"{meta_path}: {key}={metadata.get(key)!r}, "
                        f"expected {expected!r}"
                    )
        if errors:
            raise ValueError(
                "Raw-IQ cache contract validation failed. Examples:\n"
                + "\n".join(errors[:20])
            )
        return cache_paths

    def _load_or_build_manifest_index(self, max_files=None, max_files_per_class=0):
        manifest_paths = sorted(self.root_dir.rglob("manifest.jsonl"))
        if not manifest_paths:
            return False
        cached = None
        if self.index_cache_dir is not None:
            cached = load_manifest_index(
                self.index_cache_dir,
                self.root_dir,
                self.split,
                self.val_ratio,
                self.internal_dev_ratio,
                max_files,
                max_files_per_class,
                self._manifest_index_cache_extra(),
            )
        if cached is not None:
            self._restore_manifest_index(cached)
            print(
                f"[{self.split}] Loaded manifest index cache: "
                f"{cached['entry_dir']}",
                flush=True,
            )
            return True

        indexed = self._index_from_manifest(
            max_files=max_files,
            max_files_per_class=max_files_per_class,
        )
        if indexed and self.index_cache_dir is not None:
            entry_dir = save_manifest_index(
                self.index_cache_dir,
                self.root_dir,
                self.split,
                self.val_ratio,
                self.internal_dev_ratio,
                max_files,
                max_files_per_class,
                self.record_path_ids,
                self.record_local_idxs,
                self.record_labels,
                self._manifest_index_state(),
                self._manifest_index_cache_extra(),
            )
            print(f"[{self.split}] Saved manifest index cache: {entry_dir}", flush=True)
        return indexed

    def _manifest_index_cache_extra(self):
        extra = {}
        if self.stitch_windows > 1 or self.stitch_stride > 1:
            extra.update(
                {
                    "stitch_indexing_version": 2,
                    "stitch_windows": self.stitch_windows,
                    "stitch_stride": self.stitch_stride,
                }
            )
        if self.split != "train" or not self.train_exclude_sources:
            return extra or None
        encoded = json.dumps(
            sorted(self.train_exclude_sources),
            sort_keys=True,
            separators=(",", ":"),
        ).encode("utf-8")
        extra.update(
            {
                "train_exclude_sources_sha1": hashlib.sha1(encoded).hexdigest(),
                "train_exclude_sources_count": len(self.train_exclude_sources),
            }
        )
        return extra

    def _manifest_index_state(self):
        return {
            "paths": [str(path) for path in self.paths],
            "groups_by_label": {
                str(label): sorted(groups)
                for label, groups in self.groups_by_label.items()
            },
            "val_groups": sorted(self.val_groups),
            "internal_dev_groups": sorted(self.internal_dev_groups),
            "source_group_to_paths": {
                group: sorted(paths)
                for group, paths in self.source_group_to_paths.items()
            },
            "selected_source_groups": sorted(self.selected_source_groups),
            "selected_source_files": sorted(self.selected_source_files),
            "path_record_counts": {
                str(key): value for key, value in self.path_record_counts.items()
            },
            "path_min_indices": {
                str(key): value for key, value in self.path_min_indices.items()
            },
            "path_max_indices": {
                str(key): value for key, value in self.path_max_indices.items()
            },
            "manifest_index_issues": self.manifest_index_issues,
            "manifest_split_source": self.manifest_split_source,
            "manifest_split_counts": self.manifest_split_counts,
            "train_exclude_sources_count": (
                len(self.train_exclude_sources) if self.split == "train" else 0
            ),
        }

    def _restore_manifest_index(self, cached):
        state = cached["metadata"]["state"]
        self.record_path_ids = cached["record_path_ids"]
        self.record_local_idxs = cached["record_local_idxs"]
        self.record_labels = cached["record_labels"]
        self.record_count = len(self.record_labels)
        self.labels = self.record_labels
        self.paths = [Path(path) for path in state["paths"]]
        self.path_to_id = {str(path): idx for idx, path in enumerate(self.paths)}
        self.groups_by_label = {
            int(label): set(groups)
            for label, groups in state["groups_by_label"].items()
        }
        self.val_groups = set(state["val_groups"])
        self.internal_dev_groups = set(state.get("internal_dev_groups", []))
        self.source_group_to_paths = {
            group: set(paths)
            for group, paths in state["source_group_to_paths"].items()
        }
        self.selected_source_groups = set(state["selected_source_groups"])
        self.selected_source_files = set(state["selected_source_files"])
        for name in ("path_record_counts", "path_min_indices", "path_max_indices"):
            setattr(
                self,
                name,
                {int(key): value for key, value in state[name].items()},
            )
        self.manifest_index_issues = state["manifest_index_issues"]
        self.manifest_split_source = state.get("manifest_split_source", "source_group")
        self.manifest_split_counts = state.get("manifest_split_counts", {})

    def _register_path(self, npz_path):
        npz_path = Path(npz_path)
        key = str(npz_path)
        if key not in self.path_to_id:
            self.path_to_id[key] = len(self.paths)
            self.paths.append(npz_path)
        return self.path_to_id[key]

    def _resolve_manifest_path(self, raw_path, fallback_manifest_path):
        if not raw_path:
            return None

        raw_text = str(raw_path)
        native_path = Path(raw_text)
        if native_path.exists():
            return native_path

        # Manifests may contain Windows paths even when training on AutoDL Linux.
        # Normalize both slash styles before rebuilding the path under data-root.
        normalized = raw_text.replace("\\", "/")
        posix_path = PurePosixPath(normalized)
        parts = list(posix_path.parts)
        if self.root_dir.name in parts:
            idx = parts.index(self.root_dir.name)
            candidate = self.root_dir.joinpath(*parts[idx + 1:])
            if candidate.exists():
                return candidate

        candidate = fallback_manifest_path.parent / posix_path.name
        if candidate.exists():
            return candidate

        matches = list(self.root_dir.rglob(posix_path.name))
        return matches[0] if matches else None

    def _parse_label(self, raw_label, npz_path):
        if isinstance(raw_label, str):
            for idx, name in enumerate(CLASS_NAMES):
                if raw_label.lower() == name.lower():
                    return idx
        if isinstance(raw_label, (int, np.integer)) and 0 <= int(raw_label) < len(CLASS_NAMES):
            return int(raw_label)
        return infer_label_from_path(npz_path)

    def _manifest_local_index(self, record):
        # In the 3943B manifests, window_index resets inside every detected
        # segment. sample_index is the contiguous row in the referenced NPZ.
        for key in ("npz_index", "local_index", "sample_index", "window_index", "segment_index"):
            value = record.get(key)
            if isinstance(value, (int, np.integer)):
                return int(value)
        return 0

    def _record_manifest_index(self, path_id, local_idx, npz_path):
        count = self.path_record_counts.get(path_id, 0)
        requires_contiguous_indices = (
            self.stitch_windows == 1 and self.stitch_stride == 1
        )
        if (
            requires_contiguous_indices
            and local_idx != count
            and len(self.manifest_index_issues) < 20
        ):
            self.manifest_index_issues.append(
                {
                    "path": str(npz_path),
                    "record_position": count,
                    "local_index": local_idx,
                    "expected_local_index": count,
                }
            )
        self.path_record_counts[path_id] = count + 1
        self.path_min_indices[path_id] = min(
            local_idx,
            self.path_min_indices.get(path_id, local_idx),
        )
        self.path_max_indices[path_id] = max(
            local_idx,
            self.path_max_indices.get(path_id, local_idx),
        )

    def _is_train_excluded_source(self, source_key, npz_path):
        if self.split != "train" or not self.train_exclude_sources:
            return False
        path = Path(npz_path)
        candidates = {
            str(source_key),
            str(path),
            str(path.resolve()),
            path.name,
            path.stem,
        }
        return bool(candidates & self.train_exclude_sources)

    def _manifest_sequence_key(self, item, npz_path, source_key):
        return (
            str(npz_path),
            str(source_key),
            item.get("segment_index"),
        )

    def _stitch_candidates_are_contiguous(self, candidates):
        for left, right in zip(candidates, candidates[1:]):
            if left["sequence_key"] != right["sequence_key"]:
                return False
            if left["local_idx"] + 1 != right["local_idx"]:
                return False
            left_end = left.get("source_end_sample")
            right_start = right.get("source_start_sample")
            if left_end is not None and right_start is not None:
                if int(left_end) != int(right_start):
                    return False
        return True

    def _append_manifest_record(self, npz_path, local_idx, label, group_key, source_key):
        path_id = self._register_path(npz_path)
        self._record_manifest_index(path_id, local_idx, npz_path)
        self.record_path_ids.append(path_id)
        self.record_local_idxs.append(local_idx)
        self.record_labels.append(label)
        self.record_count += 1
        self.selected_source_groups.add(group_key)
        self.selected_source_files.add(source_key)
        return path_id

    def _append_stitched_manifest_candidate(self, pending, candidate):
        if pending and pending[-1]["sequence_key"] != candidate["sequence_key"]:
            pending.clear()
        pending.append(candidate)
        if len(pending) < self.stitch_windows:
            return None
        window = list(pending)[: self.stitch_windows]
        if not self._stitch_candidates_are_contiguous(window):
            pending.popleft()
            return None
        first = window[0]
        for _ in range(min(self.stitch_stride, len(pending))):
            pending.popleft()
        return first

    def _print_index_summary(self):
        if self.record_count:
            labels = np.asarray(self.record_labels, dtype=np.int64)
            counts = np.bincount(labels, minlength=len(CLASS_NAMES))
            nonzero = {
                CLASS_NAMES[idx]: int(count)
                for idx, count in enumerate(counts)
                if count > 0
            }
            print(
                json.dumps(
                    {
                        "split": self.split,
                        "records": self.record_count,
                        "npz_files": len(self.paths),
                        "class_counts": nonzero,
                    },
                    ensure_ascii=False,
                ),
                flush=True,
            )
            return

        print(
            json.dumps(
                {
                    "split": self.split,
                    "windows": len(self),
                    "npz_chunks": len(self.chunks),
                },
                ensure_ascii=False,
            ),
            flush=True,
        )

    def _index_from_manifest(self, max_files=None, max_files_per_class=0):
        manifest_paths = sorted(self.root_dir.rglob("manifest.jsonl"))
        if not manifest_paths:
            return False
        split_contract = inspect_manifest_split_contract(self.root_dir)
        self.manifest_split_source = split_contract["source"]
        self.manifest_split_counts = split_contract["counts"]

        print(
            f"[{self.split}] Found {len(manifest_paths)} manifest files. "
            "Pass 1/2: building leakage-safe source groups...",
            flush=True,
        )
        # Build a deterministic, label-stratified group split before indexing
        # individual windows. All windows from one source recording stay together.
        groups_by_label = {label: set() for label in range(len(CLASS_NAMES))}
        for manifest_path in manifest_paths:
            with open(manifest_path, "r", encoding="utf-8") as handle:
                for line in tqdm(
                    handle,
                    desc=f"[{self.split}] Scan {manifest_path.parent.name}",
                    unit="rows",
                    mininterval=2.0,
                ):
                    if not line.strip():
                        continue
                    item = json.loads(line)
                    npz_path = self._resolve_manifest_path(item.get("path"), manifest_path)
                    if npz_path is None:
                        continue
                    label = self._parse_label(item.get("label"), npz_path)
                    source_key = str(item.get("source_file") or npz_path.stem)
                    group_key = f"{manifest_path.parent.name}::{source_key}"
                    groups_by_label[label].add(group_key)
                    self.source_group_to_paths.setdefault(group_key, set()).add(str(npz_path))

        if max_files_per_class > 0:
            groups_by_label = {
                label: set(sorted(groups, key=stable_split_key)[:max_files_per_class])
                for label, groups in groups_by_label.items()
            }

        allowed_groups = set().union(*groups_by_label.values())
        val_groups = set()
        if self.manifest_split_source != "manifest":
            for label, groups in groups_by_label.items():
                ordered = sorted(groups, key=stable_split_key)
                if self.val_ratio <= 0 or len(ordered) < 2:
                    num_val = 0
                else:
                    num_val = max(1, int(round(len(ordered) * self.val_ratio)))
                    num_val = min(num_val, len(ordered) - 1)
                val_groups.update(ordered[:num_val])
        self.groups_by_label = groups_by_label
        self.val_groups = val_groups
        internal_dev_groups = set()
        if self.manifest_split_source != "manifest" and self.internal_dev_ratio > 0:
            for label, groups in groups_by_label.items():
                train_groups = set(groups) - val_groups
                ordered = sorted(
                    train_groups,
                    key=lambda group: stable_split_key(f"internal-dev::{group}"),
                )
                if len(ordered) >= 2:
                    num_dev = max(
                        1,
                        int(round(len(ordered) * self.internal_dev_ratio)),
                    )
                    internal_dev_groups.update(ordered[: min(num_dev, len(ordered) - 1)])
        self.internal_dev_groups = internal_dev_groups

        print(
            f"[{self.split}] Pass 2/2: indexing windows for this split...",
            flush=True,
        )
        used_path_keys = set()
        for manifest_path in manifest_paths:
            pending_stitch = deque()
            with open(manifest_path, "r", encoding="utf-8") as handle:
                for line in tqdm(
                    handle,
                    desc=f"[{self.split}] Index {manifest_path.parent.name}",
                    unit="rows",
                    mininterval=2.0,
                ):
                    if not line.strip():
                        continue
                    item = json.loads(line)
                    npz_path = self._resolve_manifest_path(item.get("path"), manifest_path)
                    if npz_path is None:
                        continue

                    source_key = str(item.get("source_file") or npz_path.stem)
                    group_key = f"{manifest_path.parent.name}::{source_key}"
                    if group_key not in allowed_groups:
                        continue
                    if self._is_train_excluded_source(source_key, npz_path):
                        continue
                    explicit_split = str(item.get("split") or "").strip().lower()
                    if self.manifest_split_source == "manifest":
                        if explicit_split != self.split:
                            continue
                        in_val = explicit_split == "val"
                        in_internal_dev = explicit_split == "internal_dev"
                    elif explicit_split in MANIFEST_SPLIT_NAMES:
                        expected_split = self.split
                        if explicit_split != expected_split:
                            continue
                        in_val = explicit_split == "val"
                        in_internal_dev = explicit_split == "internal_dev"
                    else:
                        in_val = group_key in val_groups
                        in_internal_dev = group_key in internal_dev_groups
                    if self.split == "train" and (in_val or in_internal_dev):
                        continue
                    if self.split == "val" and not in_val:
                        continue
                    if self.manifest_split_source != "manifest":
                        if self.split == "test" and explicit_split != "test":
                            continue
                        if self.split == "internal_dev" and not in_internal_dev:
                            continue

                    path_key = str(npz_path)
                    if (
                        max_files is not None
                        and path_key not in used_path_keys
                        and len(used_path_keys) >= max_files
                    ):
                        pending_stitch.clear()
                        continue

                    label = self._parse_label(item.get("label"), npz_path)
                    local_idx = self._manifest_local_index(item)
                    if self.stitch_windows == 1 and self.stitch_stride == 1:
                        self._append_manifest_record(
                            npz_path,
                            local_idx,
                            label,
                            group_key,
                            source_key,
                        )
                        used_path_keys.add(path_key)
                        continue

                    candidate = {
                        "npz_path": npz_path,
                        "local_idx": local_idx,
                        "label": label,
                        "group_key": group_key,
                        "source_key": source_key,
                        "sequence_key": self._manifest_sequence_key(
                            item,
                            npz_path,
                            source_key,
                        ),
                        "source_start_sample": item.get("source_start_sample"),
                        "source_end_sample": item.get("source_end_sample"),
                    }
                    stitched_start = self._append_stitched_manifest_candidate(
                        pending_stitch,
                        candidate,
                    )
                    if stitched_start is None:
                        continue
                    self._append_manifest_record(
                        stitched_start["npz_path"],
                        stitched_start["local_idx"],
                        stitched_start["label"],
                        stitched_start["group_key"],
                        stitched_start["source_key"],
                    )
                    used_path_keys.add(str(stitched_start["npz_path"]))

        return True

    def _index_from_npz_files(self, max_files=None):
        npz_files = sorted(self.root_dir.rglob("*.npz"))
        if max_files is not None:
            npz_files = npz_files[:max_files]
        if not npz_files:
            raise FileNotFoundError(f"No .npz files found under: {self.root_dir}")

        self.labels = []
        for npz_path in tqdm(npz_files, desc=f"Indexing {self.split} npz files"):
            key = npz_path.stem
            in_val = stable_split_key(key) < self.val_ratio
            if self.split == "train" and in_val:
                continue
            if self.split in ("val", "test") and not in_val:
                continue

            label = infer_label_from_path(npz_path)
            source_key = str(npz_path)
            group_key = f"{CLASS_NAMES[label]}::{source_key}"
            try:
                with np.load(npz_path, mmap_mode="r") as data:
                    if "iq" not in data:
                        continue
                    num_windows = int(data["iq"].shape[0])
            except Exception as exc:
                print(f"Skip unreadable npz: {npz_path} ({exc})")
                continue

            num_records = 1 + (num_windows - self.stitch_windows) // self.stitch_stride
            if num_records <= 0:
                continue

            self.chunks.append((npz_path, label, num_records))
            self.selected_source_groups.add(group_key)
            self.selected_source_files.add(source_key)
            total = (self.cumulative_sizes[-1] if self.cumulative_sizes else 0) + num_records
            self.cumulative_sizes.append(total)
            self.labels.extend([label] * num_records)

    def __len__(self):
        if self.record_count:
            return self.record_count
        return self.cumulative_sizes[-1]

    def __getitem__(self, idx):
        if self.record_count:
            path_id = self.record_path_ids[idx]
            local_idx = self.record_local_idxs[idx]
            label = self.record_labels[idx]
            npz_path = self.paths[path_id]
            iq = self._load_iq_window(npz_path, local_idx)
            if self.return_raw_iq:
                return torch.from_numpy(iq).float(), torch.tensor(label, dtype=torch.long)
            spectrogram = self._preprocess(iq)
            if self.is_train:
                spectrogram = self._augment(spectrogram)
            return torch.from_numpy(spectrogram).float(), torch.tensor(label, dtype=torch.long)

        chunk_idx = bisect.bisect_right(self.cumulative_sizes, idx)
        prev_total = self.cumulative_sizes[chunk_idx - 1] if chunk_idx > 0 else 0
        local_idx = (idx - prev_total) * self.stitch_stride
        npz_path, label, _ = self.chunks[chunk_idx]

        iq = self._load_iq_window(npz_path, local_idx)
        if self.return_raw_iq:
            return torch.from_numpy(iq).float(), torch.tensor(label, dtype=torch.long)
        spectrogram = self._preprocess(iq)

        if self.is_train:
            spectrogram = self._augment(spectrogram)

        return torch.from_numpy(spectrogram).float(), torch.tensor(label, dtype=torch.long)

    def _load_iq_array(self, npz_path):
        key = str(npz_path)
        if key not in self.cache:
            if len(self.cache_order) >= self.cache_size:
                old_key = self.cache_order.pop(0)
                old_value = self.cache.pop(old_key)
                mmap = getattr(old_value, "_mmap", None)
                if mmap is not None:
                    mmap.close()
            if self.raw_iq_cache_paths is not None:
                path_id = self.path_to_id[key]
                self.cache[key] = np.load(
                    self.raw_iq_cache_paths[path_id],
                    mmap_mode="r",
                )
            else:
                with np.load(npz_path) as data:
                    self.cache[key] = data["iq"].astype(np.float32)
            self.cache_order.append(key)
        return self.cache[key]

    def _load_iq_window(self, npz_path, local_idx):
        iq_array = self._load_iq_array(npz_path)
        end_idx = local_idx + self.stitch_windows
        if end_idx > iq_array.shape[0]:
            raise IndexError(
                f"Manifest stitch window index {local_idx}:{end_idx} is out of range for "
                f"{npz_path} with {iq_array.shape[0]} windows. "
                "Check whether window_index is local to the npz file."
            )
        if self.stitch_windows == 1:
            iq = iq_array[local_idx].astype(np.float32)
        else:
            iq = np.concatenate(
                np.asarray(iq_array[local_idx:end_idx], dtype=np.float32),
                axis=1,
            )
        expected_shape = (2, 4096 * self.stitch_windows)
        if iq.shape != expected_shape:
            iq = np.asarray(iq, dtype=np.float32).reshape(2, -1)
        return iq

    def _preprocess(self, iq):
        iq_tensor = torch.from_numpy(iq).float().unsqueeze(0)
        frontend = GpuStftFrontend(augment=False)
        with torch.no_grad():
            spec = frontend(iq_tensor).squeeze(0).numpy()
        spec = spec.astype(np.float32)
        spec = (spec - spec.mean()) / (spec.std() + 1e-8)
        return spec

    def _augment(self, spec):
        if np.random.rand() < 0.5:
            shift = np.random.randint(-4, 5)
            spec = np.roll(spec, shift, axis=2)

        if np.random.rand() < 0.3:
            spec = spec + np.random.normal(0, 0.03, spec.shape).astype(np.float32)

        if np.random.rand() < 0.3 and spec.shape[2] > 8:
            t_len = np.random.randint(2, min(8, spec.shape[2]) + 1)
            t_start = np.random.randint(0, spec.shape[2] - t_len + 1)
            spec[:, :, t_start:t_start + t_len] = 0

        return spec.astype(np.float32)


class GpuStftFrontend(nn.Module):
    """Legacy-style two-channel STFT frontend for raw IQ tensors [B, 2, 4096].

    This mirrors the older pipeline: compute STFT magnitude separately for I
    and Q, keep 128 frequency bins per channel, and preserve two channels for
    the CNN. The output shape is [B, 2, 128, 33] for 4096-sample windows.
    """

    def __init__(
        self,
        n_fft=STFT_N_FFT,
        hop_length=STFT_HOP_LENGTH,
        augment=True,
        feature_mode="legacy",
    ):
        super().__init__()
        if feature_mode not in FEATURE_MODE_CHOICES:
            raise ValueError(
                f"Unknown feature_mode={feature_mode!r}; "
                f"expected one of {FEATURE_MODE_CHOICES}."
            )
        self.n_fft = n_fft
        self.hop_length = hop_length
        self.augment = augment
        self.feature_mode = feature_mode
        self.register_buffer("window", torch.hann_window(n_fft), persistent=False)

    def forward(self, iq):
        if iq.dim() == 4:
            iq = (
                self._normalize_complex_spec(iq)
                if self.feature_mode == "complex_signed_mag"
                else self._normalize_spec(iq)
            )
            if self.training and self.augment:
                return self._augment(iq)
            return iq

        if self.feature_mode == "complex_signed_mag":
            spec = self._complex_signed_magnitude(iq)
            if self.training and self.augment:
                return self._augment(spec)
            return spec

        channel_specs = []
        for channel_idx in range(2):
            signal = iq[:, channel_idx, :]
            signal = signal - signal.mean(dim=1, keepdim=True)
            signal = signal / signal.std(dim=1, keepdim=True).clamp_min(1e-8)
            zxx = torch.stft(
                signal,
                n_fft=self.n_fft,
                hop_length=self.hop_length,
                win_length=self.n_fft,
                window=self.window.to(iq.device),
                center=True,
                pad_mode="constant",
                return_complex=True,
            )
            mag = torch.log1p(zxx.abs()[:, :128, :])
            mean = mag.mean(dim=(1, 2), keepdim=True)
            std = mag.std(dim=(1, 2), keepdim=True).clamp_min(1e-8)
            channel_specs.append((mag - mean) / std)
        spec = torch.stack(channel_specs, dim=1)
        spec = self._normalize_spec(spec)

        if self.training and self.augment:
            spec = self._augment(spec)
        return spec

    def _complex_signed_magnitude(self, iq):
        """Keep both sides of the complex IQ spectrum in the legacy shape."""
        complex_iq = torch.complex(iq[:, 0].float(), iq[:, 1].float())
        complex_iq = complex_iq - complex_iq.mean(dim=1, keepdim=True)
        scale = complex_iq.abs().square().mean(dim=1, keepdim=True).sqrt()
        complex_iq = complex_iq / scale.clamp_min(1e-8)
        zxx = torch.stft(
            complex_iq,
            n_fft=self.n_fft,
            hop_length=self.hop_length,
            win_length=self.n_fft,
            window=self.window.to(iq.device),
            center=True,
            pad_mode="constant",
            return_complex=True,
            onesided=False,
        )
        zxx = torch.fft.fftshift(zxx, dim=1)
        magnitude = torch.log1p(zxx.abs())
        half_spectrum = self.n_fft // 2
        negative = magnitude[:, :half_spectrum, :]
        positive = magnitude[:, half_spectrum:, :]
        spec = torch.stack((negative, positive), dim=1)
        return self._normalize_complex_spec(spec)

    def _normalize_complex_spec(self, spec):
        spec = spec.float()
        mean = spec.mean(dim=(1, 2, 3), keepdim=True)
        std = spec.std(dim=(1, 2, 3), keepdim=True).clamp_min(1e-5)
        return ((spec - mean) / std).clamp(-6.0, 6.0)

    def _normalize_spec(self, spec):
        spec = spec.float()
        mean = spec.mean(dim=(2, 3), keepdim=True)
        std = spec.std(dim=(2, 3), keepdim=True).clamp_min(1e-5)
        spec = (spec - mean) / std
        return spec.clamp(-6.0, 6.0)

    def _augment(self, spec):
        if torch.rand((), device=spec.device) < 0.5:
            gain = torch.empty(
                spec.size(0),
                1,
                1,
                1,
                device=spec.device,
                dtype=spec.dtype,
            ).uniform_(0.85, 1.15)
            spec = spec * gain

        if torch.rand((), device=spec.device) < 0.5:
            shift = int(torch.randint(-4, 5, (), device=spec.device).item())
            spec = torch.roll(spec, shifts=shift, dims=3)

        if torch.rand((), device=spec.device) < 0.3:
            shift = int(torch.randint(-6, 7, (), device=spec.device).item())
            spec = torch.roll(spec, shifts=shift, dims=2)

        if torch.rand((), device=spec.device) < 0.35:
            spec = spec + torch.randn_like(spec) * 0.025

        if torch.rand((), device=spec.device) < 0.3 and spec.shape[3] > 8:
            t_len = int(torch.randint(2, min(8, spec.shape[3]) + 1, (), device=spec.device).item())
            t_start = int(torch.randint(0, spec.shape[3] - t_len + 1, (), device=spec.device).item())
            spec = spec.clone()
            spec[:, :, :, t_start:t_start + t_len] = 0

        if torch.rand((), device=spec.device) < 0.25 and spec.shape[2] > 16:
            f_len = int(torch.randint(4, min(16, spec.shape[2]) + 1, (), device=spec.device).item())
            f_start = int(torch.randint(0, spec.shape[2] - f_len + 1, (), device=spec.device).item())
            spec = spec.clone()
            spec[:, :, f_start:f_start + f_len, :] = 0

        return spec


class DualStftFrontend(nn.Module):
    """Produce aligned legacy and signed-complex views from one IQ window."""

    def __init__(self, augment=True):
        super().__init__()
        self.augment = bool(augment)
        self.legacy = GpuStftFrontend(augment=False, feature_mode="legacy")
        self.complex_signed = GpuStftFrontend(
            augment=False,
            feature_mode="complex_signed_mag",
        )

    def forward(self, inputs):
        if isinstance(inputs, (tuple, list)):
            if len(inputs) != 2:
                raise ValueError("Dual-view input must contain exactly two spectrograms.")
            legacy_input, complex_input = inputs
        else:
            legacy_input = inputs
            complex_input = inputs

        legacy = self.legacy(legacy_input)
        complex_signed = self.complex_signed(complex_input)
        if self.training and self.augment:
            channels = legacy.shape[1]
            combined = self.legacy._augment(
                torch.cat((legacy, complex_signed), dim=1)
            )
            legacy = combined[:, :channels]
            complex_signed = combined[:, channels:]
        return legacy, complex_signed


class PhaseAwareFrontend(nn.Module):
    """Build phase-invariant spectral and temporal views from one raw window."""

    def __init__(self, augment=True):
        super().__init__()
        self.augment = bool(augment)
        self.long_spectrum = GpuStftFrontend(
            augment=False,
            feature_mode="complex_signed_mag",
        )
        self.short_spectrum = GpuStftFrontend(
            n_fft=PHASE_AWARE_SHORT_N_FFT,
            hop_length=PHASE_AWARE_SHORT_HOP_LENGTH,
            augment=False,
            feature_mode="complex_signed_mag",
        )

    def forward(self, iq):
        if iq.ndim != 3 or iq.shape[1:] != (2, 4096):
            raise ValueError(
                "phase_aware_large requires raw I/Q windows shaped [B, 2, 4096]."
            )
        iq = iq.float()
        if self.training and self.augment:
            iq = self._augment_raw_iq(iq)

        long_spectrum = self.long_spectrum(iq)
        short_spectrum = self.short_spectrum(iq)
        short_spectrum = F.interpolate(
            short_spectrum,
            size=long_spectrum.shape[-2:],
            mode="bilinear",
            align_corners=False,
        )
        spectral_view = torch.cat((long_spectrum, short_spectrum), dim=1)
        phase_view = self._phase_sequence(iq)
        return spectral_view, phase_view

    @staticmethod
    def _normalized_complex_iq(iq):
        complex_iq = torch.complex(iq[:, 0], iq[:, 1])
        complex_iq = complex_iq - complex_iq.mean(dim=1, keepdim=True)
        rms = complex_iq.abs().square().mean(dim=1, keepdim=True).sqrt()
        return complex_iq / rms.clamp_min(1e-6)

    def _phase_sequence(self, iq):
        complex_iq = self._normalized_complex_iq(iq)
        amplitude = complex_iq.abs()
        frames = amplitude.reshape(
            len(amplitude),
            PHASE_AWARE_SEGMENTS,
            -1,
        )
        amplitude_scale = amplitude.mean(dim=1, keepdim=True).clamp_min(1e-6)
        amplitude_features = torch.stack(
            (
                frames.mean(dim=2) / amplitude_scale - 1.0,
                frames.std(dim=2, unbiased=False) / amplitude_scale,
                frames.amax(dim=2) / amplitude_scale,
            ),
            dim=1,
        )

        lag_features = []
        for lag in PHASE_AWARE_LAGS:
            left = complex_iq[:, :-lag]
            right = complex_iq[:, lag:]
            product = right * left.conj()
            unit_product = product / product.abs().clamp_min(1e-6)
            mean_unit = unit_product.mean(dim=1, keepdim=True)
            mean_unit = mean_unit / mean_unit.abs().clamp_min(1e-6)
            centered_unit = unit_product * mean_unit.conj()

            usable = (
                centered_unit.shape[1] // PHASE_AWARE_SEGMENTS
            ) * PHASE_AWARE_SEGMENTS
            unit_frames = centered_unit[:, :usable].reshape(
                len(iq),
                PHASE_AWARE_SEGMENTS,
                -1,
            )
            differential_mean = unit_frames.mean(dim=2)

            product_frames = product[:, :usable].reshape(
                len(iq),
                PHASE_AWARE_SEGMENTS,
                -1,
            )
            left_frames = left[:, :usable].reshape_as(product_frames)
            right_frames = right[:, :usable].reshape_as(product_frames)
            local_scale = (
                left_frames.abs().square().mean(dim=2)
                * right_frames.abs().square().mean(dim=2)
            ).clamp_min(1e-8).sqrt()
            correlation = product_frames.mean(dim=2) / local_scale
            correlation = correlation * mean_unit.conj()

            lag_features.extend(
                (
                    differential_mean.real,
                    differential_mean.imag,
                    differential_mean.abs(),
                    correlation.real,
                    correlation.imag,
                    correlation.abs(),
                )
            )

        phase_view = torch.cat(
            (amplitude_features, torch.stack(lag_features, dim=1)),
            dim=1,
        )
        if phase_view.shape[1:] != (
            PHASE_AWARE_SEQUENCE_CHANNELS,
            PHASE_AWARE_SEGMENTS,
        ):
            raise RuntimeError(
                f"Unexpected phase-aware feature shape: {tuple(phase_view.shape)}."
            )
        return phase_view.nan_to_num().clamp(-10.0, 10.0)

    @staticmethod
    def _augment_raw_iq(iq):
        if torch.rand((), device=iq.device) < 0.5:
            gain = torch.empty(
                len(iq), 1, 1, device=iq.device, dtype=iq.dtype
            ).uniform_(0.85, 1.15)
            iq = iq * gain
        if torch.rand((), device=iq.device) < 0.35:
            iq = iq + torch.randn_like(iq) * 0.02
        if torch.rand((), device=iq.device) < 0.5:
            shift = int(torch.randint(-128, 129, (), device=iq.device).item())
            iq = torch.roll(iq, shifts=shift, dims=2)
        if torch.rand((), device=iq.device) < 0.4:
            complex_iq = torch.complex(iq[:, 0], iq[:, 1])
            frequency = torch.empty(
                len(iq), 1, device=iq.device, dtype=iq.dtype
            ).uniform_(-0.03, 0.03)
            sample_index = torch.arange(
                iq.shape[2], device=iq.device, dtype=iq.dtype
            ).unsqueeze(0)
            rotation = torch.polar(torch.ones_like(sample_index), frequency * sample_index)
            complex_iq = complex_iq * rotation
            iq = torch.stack((complex_iq.real, complex_iq.imag), dim=1)
        return iq


class MarginClassifier(nn.Module):
    """ArcFace/CosFace classification head over normalized embeddings."""

    def __init__(self, in_features, out_features, scale=30.0, margin=0.30, mode="arcface"):
        super().__init__()
        if mode not in {"arcface", "cosface"}:
            raise ValueError(f"Unsupported margin classifier mode: {mode}")
        self.weight = nn.Parameter(torch.empty(out_features, in_features))
        nn.init.xavier_uniform_(self.weight)
        self.scale = float(scale)
        self.margin = float(margin)
        self.mode = mode

    def forward(self, embeddings, labels=None):
        cosine = F.linear(
            F.normalize(embeddings.float(), dim=1),
            F.normalize(self.weight.float(), dim=1),
        ).clamp(-1.0 + 1e-7, 1.0 - 1e-7)
        if labels is None:
            return cosine * self.scale

        if self.mode == "arcface":
            phi = torch.cos(torch.acos(cosine) + self.margin)
        else:
            phi = cosine - self.margin
        one_hot = torch.zeros_like(cosine)
        one_hot.scatter_(1, labels.view(-1, 1), 1.0)
        return (one_hot * phi + (1.0 - one_hot) * cosine) * self.scale


class SupConLoss(nn.Module):
    """Supervised contrastive loss for class-compact embeddings."""

    def __init__(self, temperature=0.10):
        super().__init__()
        self.temperature = float(temperature)

    def forward(self, embeddings, labels):
        features = F.normalize(embeddings.float(), dim=1)
        labels = labels.view(-1, 1)
        same_label = torch.eq(labels, labels.T).float().to(features.device)
        logits = torch.matmul(features, features.T) / self.temperature
        logits = logits - logits.max(dim=1, keepdim=True).values.detach()

        self_mask = torch.eye(features.size(0), device=features.device)
        positive_mask = same_label * (1.0 - self_mask)
        exp_logits = torch.exp(logits) * (1.0 - self_mask)
        log_prob = logits - torch.log(exp_logits.sum(dim=1, keepdim=True).clamp_min(1e-12))

        positive_count = positive_mask.sum(dim=1)
        valid = positive_count > 0
        if not valid.any():
            return features.new_tensor(0.0)
        mean_log_prob_pos = (positive_mask * log_prob).sum(dim=1)[valid] / positive_count[valid]
        return -mean_log_prob_pos.mean()


def paired_class_auxiliary_losses(logits, labels, criterion):
    """Split paired 01/02 classes into family and within-family objectives."""
    if logits.ndim != 2 or logits.shape[1] != len(CLASS_NAMES):
        raise ValueError(
            "Paired-class auxiliary losses require [batch, 16] logits."
        )
    if len(CLASS_NAMES) % 2:
        raise ValueError("Paired-class auxiliary losses require an even class count.")

    paired_logits = logits.reshape(logits.shape[0], len(CLASS_NAMES) // 2, 2)
    family_targets = torch.div(labels, 2, rounding_mode="floor")
    family_logits = torch.logsumexp(paired_logits, dim=2)
    family_loss = criterion(family_logits, family_targets)

    batch_indices = torch.arange(labels.shape[0], device=labels.device)
    individual_logits = paired_logits[batch_indices, family_targets]
    individual_targets = torch.remainder(labels, 2)
    individual_loss = criterion(individual_logits, individual_targets)
    return family_loss, individual_loss


def source_pooled_auxiliary_losses(logits, labels, source_ids, criterion):
    """Supervise source-pooled logits while retaining per-window predictions."""
    if logits.ndim != 2:
        raise ValueError("Source-pooled loss requires [batch, classes] logits.")
    if labels.ndim != 1 or source_ids.ndim != 1:
        raise ValueError("labels and source_ids must be one-dimensional.")
    if logits.shape[0] != labels.shape[0] or labels.shape != source_ids.shape:
        raise ValueError("logits, labels, and source_ids must have matching batches.")
    if logits.shape[0] == 0:
        zero = logits.new_tensor(0.0)
        return zero, zero

    unique_sources, inverse = torch.unique(
        source_ids,
        sorted=True,
        return_inverse=True,
    )
    pooled_logits = logits.new_zeros((unique_sources.numel(), logits.shape[1]))
    pooled_logits.index_add_(0, inverse, logits)
    counts = torch.bincount(inverse, minlength=unique_sources.numel()).to(
        logits.dtype
    )
    pooled_logits = pooled_logits / counts.clamp_min(1).unsqueeze(1)

    pooled_labels = labels.new_empty(unique_sources.numel())
    for source_index in range(unique_sources.numel()):
        source_rows = torch.nonzero(inverse == source_index, as_tuple=False).flatten()
        pooled_labels[source_index] = labels[source_rows[0]]
    if not torch.equal(labels, pooled_labels[inverse]):
        raise ValueError("A source id contains multiple class labels.")

    pooled_ce = criterion(pooled_logits, pooled_labels)
    teacher_probabilities = F.softmax(pooled_logits.detach(), dim=1)[inverse]
    consistency = F.kl_div(
        F.log_softmax(logits, dim=1),
        teacher_probabilities,
        reduction="batchmean",
    )
    return pooled_ce, consistency


class IqClassifier(nn.Module):
    def __init__(
        self,
        num_classes=len(CLASS_NAMES),
        use_gpu_stft=True,
        augment=True,
        dropout=0.5,
        model_arch="base",
        classifier_head="arcface",
        embedding_dim=512,
        margin_scale=30.0,
        arc_margin=0.30,
        cos_margin=0.20,
        rf_stats_dim=0,
        rf_stats_fusion="concat",
        rf_stats_input_mode="real",
        feature_mode="legacy",
    ):
        super().__init__()
        self.num_classes = int(num_classes)
        self.is_dual_view = model_arch == "dual_axis_profile_large"
        self.requires_raw_iq = model_arch == "phase_aware_large"
        if self.requires_raw_iq:
            self.frontend = PhaseAwareFrontend(augment=augment)
        elif self.is_dual_view:
            self.frontend = DualStftFrontend(augment=augment)
        else:
            self.frontend = (
                GpuStftFrontend(augment=augment, feature_mode=feature_mode)
                if use_gpu_stft
                else nn.Identity()
            )
        if model_arch == "base":
            self.backbone = AISNet(
                num_classes=num_classes,
                dropout=dropout,
                embedding_dim=embedding_dim,
            )
        elif model_arch == "light":
            self.backbone = LightAISNet(
                num_classes=num_classes,
                dropout=dropout,
                embedding_dim=min(embedding_dim, 192),
            )
        elif model_arch == "wide_light":
            self.backbone = WideLightAISNet(
                num_classes=num_classes,
                dropout=dropout,
                embedding_dim=min(embedding_dim, 192),
            )
        elif model_arch == "axis_aware":
            self.backbone = AxisAwareAISNet(
                num_classes=num_classes,
                dropout=dropout,
                embedding_dim=min(embedding_dim, 192),
            )
        elif model_arch == "axis_profile_large":
            self.backbone = AxisProfileLargeAISNet(
                num_classes=num_classes,
                dropout=dropout,
                embedding_dim=min(embedding_dim, 256),
            )
        elif model_arch == "axis_profile_xlarge":
            self.backbone = AxisProfileXLargeAISNet(
                num_classes=num_classes,
                dropout=dropout,
                # SC1 is a frozen parameter-matched capacity control. Keep its
                # architecture-defined embedding width independent of the
                # baseline run's generic --embedding-dim value.
                embedding_dim=320,
            )
        elif model_arch == "axis_profile_xlarge_multiscale":
            self.backbone = AxisProfileXLargeMultiScaleAISNet(
                num_classes=num_classes,
                dropout=dropout,
                embedding_dim=320,
            )
        elif model_arch == "axis_profile_xlarge_multiscale_gated":
            self.backbone = AxisProfileXLargeGatedMultiScaleAISNet(
                num_classes=num_classes,
                dropout=dropout,
                embedding_dim=320,
            )
        elif model_arch == "dual_axis_profile_large":
            self.backbone = DualAxisProfileLargeAISNet(
                num_classes=num_classes,
                dropout=dropout,
                embedding_dim=min(embedding_dim, 256),
            )
        elif model_arch == "phase_aware_large":
            self.backbone = PhaseAwareAISNet(
                num_classes=num_classes,
                dropout=dropout,
                embedding_dim=min(embedding_dim, 256),
            )
        elif model_arch == "axis_grid_large":
            self.backbone = AxisGridLargeAISNet(
                num_classes=num_classes,
                dropout=dropout,
                embedding_dim=min(embedding_dim, 256),
            )
        elif model_arch == "medium":
            self.backbone = MediumAISNet(
                num_classes=num_classes,
                dropout=dropout,
                embedding_dim=min(embedding_dim, 256),
            )
        else:
            raise ValueError(
                f"Unknown model_arch={model_arch!r}; expected 'light', "
                "'wide_light', 'axis_aware', 'axis_profile_large', "
                "'axis_profile_xlarge', 'axis_profile_xlarge_multiscale', "
                "'axis_profile_xlarge_multiscale_gated', "
                "'dual_axis_profile_large', 'phase_aware_large', "
                "'axis_grid_large', 'medium', or 'base'."
            )
        self.classifier_head = classifier_head
        self.rf_stats_dim = int(rf_stats_dim)
        self.rf_stats_fusion = str(rf_stats_fusion)
        self.rf_stats_input_mode = str(rf_stats_input_mode)
        if self.rf_stats_dim < 0:
            raise ValueError("rf_stats_dim must be non-negative.")
        if self.rf_stats_fusion not in {"concat", "gated_residual"}:
            raise ValueError(
                "rf_stats_fusion must be 'concat' or 'gated_residual'."
            )
        if self.rf_stats_input_mode not in {"real", "zeros"}:
            raise ValueError("rf_stats_input_mode must be 'real' or 'zeros'.")
        if not self.rf_stats_dim and (
            self.rf_stats_fusion != "concat" or self.rf_stats_input_mode != "real"
        ):
            raise ValueError(
                "RF stats fusion and input controls require rf_stats_dim greater than 0."
            )
        if self.rf_stats_dim:
            stats_embedding_dim = 96
            self.rf_stats_encoder = nn.Sequential(
                nn.Linear(self.rf_stats_dim, stats_embedding_dim),
                nn.LayerNorm(stats_embedding_dim),
                nn.GELU(),
                nn.Dropout(min(float(dropout), 0.3)),
            )
            if self.rf_stats_fusion == "concat":
                self.fusion_head = nn.Sequential(
                    nn.Linear(
                        self.backbone.embedding_dim + stats_embedding_dim,
                        self.backbone.embedding_dim,
                    ),
                    nn.LayerNorm(self.backbone.embedding_dim),
                    nn.GELU(),
                    nn.Dropout(dropout),
                )
            else:
                self.stats_projection = nn.Sequential(
                    nn.Linear(stats_embedding_dim, self.backbone.embedding_dim),
                    nn.LayerNorm(self.backbone.embedding_dim),
                )
                self.stats_residual_gate = nn.Parameter(torch.zeros(()))
            self.fusion_classifier = nn.Linear(self.backbone.embedding_dim, num_classes)
        if classifier_head == "arcface":
            self.margin_head = MarginClassifier(
                self.backbone.embedding_dim,
                num_classes,
                scale=margin_scale,
                margin=arc_margin,
                mode="arcface",
            )
        elif classifier_head == "cosface":
            self.margin_head = MarginClassifier(
                self.backbone.embedding_dim,
                num_classes,
                scale=margin_scale,
                margin=cos_margin,
                mode="cosface",
            )
        elif classifier_head == "ce":
            self.margin_head = None
        elif classifier_head == "hierarchical":
            if self.num_classes % 2:
                raise ValueError("hierarchical classifier requires paired classes.")
            self.margin_head = None
            self.family_classifier = nn.Linear(
                self.backbone.embedding_dim,
                self.num_classes // 2,
            )
            self.individual_classifier = nn.Linear(
                self.backbone.embedding_dim,
                self.num_classes,
            )
            nn.init.zeros_(self.family_classifier.weight)
            nn.init.zeros_(self.family_classifier.bias)
            nn.init.zeros_(self.individual_classifier.weight)
            nn.init.zeros_(self.individual_classifier.bias)
        else:
            raise ValueError(
                f"Unknown classifier_head={classifier_head!r}; expected 'ce', "
                "'hierarchical', 'arcface', or 'cosface'."
            )

    def forward(self, x, labels=None, return_embedding=False):
        stats = None
        if self.rf_stats_dim:
            if not isinstance(x, (tuple, list)) or len(x) != 2:
                raise ValueError(
                    "This model requires compact RF statistics for every window."
                )
            x, stats = x
        elif isinstance(x, (tuple, list)) and not self.is_dual_view:
            if len(x) != 2:
                raise ValueError("Fused model input must contain spectrogram and RF statistics.")
            x, stats = x
        if self.rf_stats_dim and stats is None:
            raise ValueError("This model requires compact RF statistics for every window.")
        if not self.rf_stats_dim and stats is not None:
            raise ValueError("RF statistics were provided to a spectrogram-only model.")
        x = self.frontend(x)
        embedding = self.backbone.forward_features(x)
        if stats is not None:
            if self.rf_stats_input_mode == "zeros":
                stats = torch.zeros_like(stats)
            stats_embedding = self.rf_stats_encoder(stats.float())
            if self.rf_stats_fusion == "concat":
                embedding = self.fusion_head(
                    torch.cat((embedding, stats_embedding), dim=1)
                )
            else:
                stats_residual = self.stats_projection(stats_embedding)
                embedding = embedding + torch.tanh(
                    self.stats_residual_gate
                ) * stats_residual
        if self.classifier_head == "hierarchical":
            flat_logits = (
                self.fusion_classifier(embedding)
                if self.rf_stats_dim
                else self.backbone.classifier(embedding)
            )
            paired_logits = flat_logits.reshape(
                len(embedding),
                self.num_classes // 2,
                2,
            )
            family_log_probabilities = F.log_softmax(
                torch.logsumexp(paired_logits, dim=2)
                + self.family_classifier(embedding),
                dim=1,
            )
            individual_log_probabilities = F.log_softmax(
                paired_logits
                + self.individual_classifier(embedding).reshape(
                    len(embedding), self.num_classes // 2, 2
                ),
                dim=2,
            )
            logits = (
                family_log_probabilities.unsqueeze(2)
                + individual_log_probabilities
            ).reshape(len(embedding), self.num_classes)
        elif self.margin_head is None:
            logits = (
                self.fusion_classifier(embedding)
                if self.rf_stats_dim
                else self.backbone.classifier(embedding)
            )
        else:
            logits = self.margin_head(embedding, labels if self.training else None)
        if return_embedding:
            return logits, embedding
        return logits


def load_initial_model_state(
    model: IqClassifier,
    state_dict: Mapping[str, torch.Tensor],
) -> bool:
    """Load an initialization checkpoint, allowing known zero-gated additions."""
    backbone = getattr(model, "backbone", None)
    optional_prefixes = tuple(
        getattr(backbone, "init_optional_state_prefixes", ())
    )
    hierarchical_keys = {
        "family_classifier.weight",
        "family_classifier.bias",
        "individual_classifier.weight",
        "individual_classifier.bias",
    }
    allowed_hierarchical = (
        hierarchical_keys
        if getattr(model, "classifier_head", None) == "hierarchical"
        else set()
    )
    if not optional_prefixes and not allowed_hierarchical:
        model.load_state_dict(state_dict)
        return False

    incompatible = model.load_state_dict(state_dict, strict=False)
    missing = set(incompatible.missing_keys)
    unexpected = set(incompatible.unexpected_keys)
    optional_missing = {
        key
        for key in missing
        if any(key.startswith(prefix) for prefix in optional_prefixes)
    }
    allowed_missing = allowed_hierarchical | optional_missing
    if unexpected or (missing and not missing <= allowed_missing):
        raise RuntimeError(
            "Initial checkpoint is not compatible with this model; "
            "adaptation only permits known optional zero-initialized additions; "
            f"missing={sorted(missing)}, unexpected={sorted(unexpected)}"
        )
    if not missing:
        return False

    with torch.no_grad():
        if hierarchical_keys & missing:
            model.family_classifier.weight.zero_()
            model.family_classifier.bias.zero_()
            model.individual_classifier.weight.zero_()
            model.individual_classifier.bias.zero_()
    return True


GATED_MULTISCALE_TRAINABLE_PREFIXES = (
    "backbone.time_profile_coarse.",
    "backbone.time_profile_fine.",
    "backbone.multiscale_projection.",
    "backbone.multiscale_gate",
)


def freeze_gated_multiscale_base(model):
    if not isinstance(model.backbone, AxisProfileXLargeGatedMultiScaleAISNet):
        raise ValueError(
            "--freeze-gated-multiscale-base requires "
            "--model-arch axis_profile_xlarge_multiscale_gated."
        )
    trainable = []
    frozen = []
    for name, parameter in model.named_parameters():
        keep_trainable = any(
            name.startswith(prefix) for prefix in GATED_MULTISCALE_TRAINABLE_PREFIXES
        )
        parameter.requires_grad = keep_trainable
        if keep_trainable:
            trainable.append(name)
        else:
            frozen.append(name)
    return {
        "trainable_parameter_names": trainable,
        "frozen_parameter_count": len(frozen),
        "trainable_parameter_count": len(trainable),
    }


def cache_path_for_npz(cache_dir, data_root, npz_path):
    cache_dir = Path(cache_dir)
    data_root = Path(data_root)
    npz_path = Path(npz_path)
    try:
        relative = npz_path.relative_to(data_root)
        return cache_dir / "spec_npy" / relative.with_suffix(".spec.npy")
    except ValueError:
        digest = hashlib.sha1(str(npz_path).encode("utf-8")).hexdigest()[:16]
        return cache_dir / "spec_npy" / f"{digest}_{npz_path.stem}.spec.npy"


def cache_index_path_for_npz(cache_path):
    return Path(cache_path).with_suffix(".index.npy")


def raw_iq_cache_path(cache_dir, data_root, npz_path):
    cache_dir = Path(cache_dir)
    data_root = Path(data_root)
    npz_path = Path(npz_path)
    try:
        relative = npz_path.relative_to(data_root)
        return cache_dir / "raw_iq_npy" / relative.with_suffix(".iq.npy")
    except ValueError:
        digest = hashlib.sha1(str(npz_path).encode("utf-8")).hexdigest()[:16]
        return cache_dir / "raw_iq_npy" / f"{digest}_{npz_path.stem}.iq.npy"


def precompute_phase_raw_iq_cache(
    data_root,
    cache_dir,
    index_cache_dir="",
    val_ratio=0.2,
    internal_dev_ratio=0.2,
    overwrite=False,
):
    """Cache only train/internal-dev raw windows as float16 memmaps."""
    if internal_dev_ratio <= 0:
        raise ValueError("Raw-IQ cache requires a non-empty internal-dev split.")
    data_root = Path(data_root)
    cache_dir = Path(cache_dir)
    datasets = [
        IqrawNpzDataset(
            data_root,
            split=split,
            val_ratio=val_ratio,
            is_train=split == "train",
            cache_size=1,
            return_raw_iq=True,
            index_cache_dir=index_cache_dir,
            internal_dev_ratio=internal_dev_ratio,
        )
        for split in ("train", "internal_dev")
    ]
    split_audit = assert_leakage_safe_split(*datasets)
    path_records = []
    for split, dataset in zip(("train", "internal_dev"), datasets):
        path_records.extend(
            (
                split,
                source_path,
                dataset.path_record_counts[path_id],
            )
            for path_id, source_path in enumerate(dataset.paths)
        )

    estimated_bytes = sum(
        windows * math.prod(RAW_IQ_WINDOW_SHAPE)
        * np.dtype(RAW_IQ_CACHE_DTYPE).itemsize
        for _, _, windows in path_records
    )
    summary = {
        "data_root": str(data_root),
        "cache_dir": str(cache_dir),
        "constructed_splits": ["train", "internal_dev"],
        "official_validation_accessed": False,
        "dtype": RAW_IQ_CACHE_DTYPE,
        "shape_per_window": list(RAW_IQ_WINDOW_SHAPE),
        "estimated_bytes": estimated_bytes,
        "estimated_gib": estimated_bytes / (1024 ** 3),
        "source_files": len(path_records),
        "windows": sum(windows for _, _, windows in path_records),
        "split_audit": split_audit,
        "created": [],
        "skipped": [],
    }
    cache_dir.mkdir(parents=True, exist_ok=True)
    for split, source_path, expected_windows in tqdm(
        path_records,
        desc="Precomputing phase raw-IQ cache",
        unit="file",
    ):
        out_path = raw_iq_cache_path(cache_dir, data_root, source_path)
        meta_path = out_path.with_suffix(".json")
        source_stat = source_path.stat()
        expected_shape = (expected_windows, *RAW_IQ_WINDOW_SHAPE)
        expected_metadata = {
            "source_npz": str(source_path),
            "source_size": source_stat.st_size,
            "source_mtime_ns": source_stat.st_mtime_ns,
            "split": split,
            "windows": expected_windows,
            "dtype": RAW_IQ_CACHE_DTYPE,
            "shape": list(expected_shape),
        }
        if out_path.exists() and meta_path.exists() and not overwrite:
            existing = np.load(out_path, mmap_mode="r")
            try:
                metadata = json.loads(meta_path.read_text(encoding="utf-8"))
                if (
                    existing.shape == expected_shape
                    and existing.dtype == np.dtype(RAW_IQ_CACHE_DTYPE)
                    and all(
                        metadata.get(key) == value
                        for key, value in expected_metadata.items()
                    )
                ):
                    summary["skipped"].append(str(out_path))
                    continue
            finally:
                mmap = getattr(existing, "_mmap", None)
                if mmap is not None:
                    mmap.close()

        out_path.parent.mkdir(parents=True, exist_ok=True)
        temp_path = out_path.with_name(out_path.name + ".tmp")
        with np.load(source_path) as source:
            iq = source["iq"]
            if iq.shape != expected_shape:
                raise ValueError(
                    f"{source_path}: shape={iq.shape}, expected {expected_shape}"
                )
            if not np.isfinite(iq).all():
                raise ValueError(f"{source_path}: raw I/Q contains non-finite values")
            output = np.lib.format.open_memmap(
                temp_path,
                mode="w+",
                dtype=np.dtype(RAW_IQ_CACHE_DTYPE),
                shape=expected_shape,
            )
            for start in range(0, expected_windows, 4096):
                end = min(start + 4096, expected_windows)
                output[start:end] = iq[start:end].astype(
                    RAW_IQ_CACHE_DTYPE,
                    copy=False,
                )
            output.flush()
            mmap = getattr(output, "_mmap", None)
            if mmap is not None:
                mmap.close()
        os.replace(temp_path, out_path)
        meta_path.write_text(
            json.dumps(expected_metadata, ensure_ascii=False, indent=2),
            encoding="utf-8",
        )
        summary["created"].append(str(out_path))

    summary_path = cache_dir / "cache_summary.json"
    summary_path.write_text(
        json.dumps(summary, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    return summary


class PrecomputedSpecDataset(Dataset):
    """Dataset backed by fp16 spectrogram memmaps produced by precompute_cache."""

    def __init__(
        self,
        root_dir,
        cache_dir,
        split="train",
        val_ratio=0.2,
        is_train=True,
        max_files=None,
        max_files_per_class=0,
        cache_size=16,
        index_cache_dir="",
        internal_dev_ratio=0.0,
        feature_mode="legacy",
        cache_data_root=None,
        train_exclude_sources=None,
        stitch_windows=1,
        stitch_stride=1,
    ):
        if feature_mode not in FEATURE_MODE_CHOICES:
            raise ValueError(
                f"Unknown feature_mode={feature_mode!r}; "
                f"expected one of {FEATURE_MODE_CHOICES}."
            )
        self.base = IqrawNpzDataset(
            root_dir,
            split=split,
            val_ratio=val_ratio,
            is_train=is_train,
            max_files=max_files,
            max_files_per_class=max_files_per_class,
            cache_size=1,
            return_raw_iq=True,
            index_cache_dir=index_cache_dir,
            internal_dev_ratio=internal_dev_ratio,
            train_exclude_sources=train_exclude_sources,
            stitch_windows=stitch_windows,
            stitch_stride=stitch_stride,
        )
        self.root_dir = self.base.root_dir
        self.cache_dir = Path(cache_dir)
        self.cache_data_root = Path(cache_data_root) if cache_data_root else self.root_dir
        self.feature_mode = feature_mode
        self.feature_mode_id = FEATURE_MODE_IDS[feature_mode]
        self.stitch_windows = int(stitch_windows)
        self.stitch_stride = int(stitch_stride)
        self.cache_window_shape = spectrogram_cache_window_shape(self.stitch_windows)
        self.split = split
        self.is_train = is_train
        self.record_path_ids = self.base.record_path_ids
        self.record_local_idxs = self.base.record_local_idxs
        self.record_labels = self.base.record_labels
        self.record_count = self.base.record_count
        self.labels = self.record_labels
        self.paths = [
            cache_path_for_npz(self.cache_dir, self.cache_data_root, path)
            for path in self.base.paths
        ]
        self.raw_paths = self.base.paths
        self.start_index_paths = [cache_index_path_for_npz(path) for path in self.paths]
        self.cache = {}
        self.start_index_cache = {}
        self.cache_order = []
        self.cache_size = max(1, int(cache_size))

        missing = [str(path) for path in self.paths if not Path(path).exists()]
        if missing:
            preview = "\n".join(missing[:5])
            raise FileNotFoundError(
                "Missing precomputed spectrogram cache files. "
                "Run with --precompute-cache first. Missing examples:\n"
                + preview
            )
        if self.base.manifest_index_issues:
            raise ValueError(
                "Manifest local indices are not contiguous within each NPZ. "
                f"Examples: {self.base.manifest_index_issues[:3]}"
            )
        self.cache_validation = self._validate_cache_contract()

    def _validate_cache_contract(self):
        errors = []
        for path_id, (raw_path, path) in enumerate(zip(self.raw_paths, self.paths)):
            sample = np.load(path, mmap_mode="r")
            try:
                expected_windows = self.base.path_record_counts.get(path_id)
                if sample.ndim != 4 or tuple(sample.shape[1:]) != self.cache_window_shape:
                    errors.append(
                        f"{path}: shape={tuple(sample.shape)}, expected "
                        f"(N, {', '.join(map(str, self.cache_window_shape))})"
                    )
                if sample.dtype != np.dtype(CACHE_DTYPE):
                    errors.append(
                        f"{path}: dtype={sample.dtype}, expected {CACHE_DTYPE}"
                    )
                if expected_windows is not None and sample.shape[0] != expected_windows:
                    errors.append(
                        f"{path}: windows={sample.shape[0]}, manifest records={expected_windows}"
                    )
            finally:
                mmap = getattr(sample, "_mmap", None)
                if mmap is not None:
                    mmap.close()

            meta_path = path.with_suffix(".json")
            if not meta_path.exists():
                errors.append(f"{meta_path}: missing cache metadata")
                continue
            try:
                meta = json.loads(meta_path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError) as exc:
                errors.append(f"{meta_path}: invalid metadata ({exc})")
                continue

            expected_meta = {
                "feature_mode": self.feature_mode_id,
                "dtype": CACHE_DTYPE,
                "n_fft": STFT_N_FFT,
                "hop_length": STFT_HOP_LENGTH,
                "center": True,
                "onesided": feature_mode_uses_onesided_stft(self.feature_mode),
                "stitch_windows": self.stitch_windows,
                "stitch_stride": self.stitch_stride,
            }
            for key, expected in expected_meta.items():
                if meta.get(key) != expected:
                    errors.append(
                        f"{meta_path}: {key}={meta.get(key)!r}, expected {expected!r}"
                    )
            if Path(str(meta.get("source_npz", ""))).name != raw_path.name:
                errors.append(
                    f"{meta_path}: source_npz does not match {raw_path.name}"
                )
            expected_shape = [
                self.base.path_record_counts.get(path_id),
                *self.cache_window_shape,
            ]
            if meta.get("shape") != expected_shape:
                errors.append(
                    f"{meta_path}: shape={meta.get('shape')}, expected {expected_shape}"
                )
            if self.stitch_windows > 1 or self.stitch_stride > 1:
                index_path = self.start_index_paths[path_id]
                if not index_path.exists():
                    errors.append(f"{index_path}: missing stitch start-index cache")
                else:
                    starts = np.load(index_path, mmap_mode="r")
                    try:
                        if starts.ndim != 1 or starts.dtype != np.dtype("uint32"):
                            errors.append(
                                f"{index_path}: shape={starts.shape}, dtype={starts.dtype}; "
                                "expected one-dimensional uint32"
                            )
                        if starts.shape[0] != sample.shape[0]:
                            errors.append(
                                f"{index_path}: starts={starts.shape[0]}, cache windows={sample.shape[0]}"
                            )
                    finally:
                        mmap = getattr(starts, "_mmap", None)
                        if mmap is not None:
                            mmap.close()

        if errors:
            preview = "\n".join(errors[:20])
            raise ValueError(
                "Precomputed cache contract validation failed. "
                "Use the matching cache or build a new cache directory; existing data "
                "was not modified.\n" + preview
            )
        return {
            "status": "ok",
            "checked_files": len(self.paths),
            "feature_mode": self.feature_mode_id,
            "dtype": CACHE_DTYPE,
            "shape_per_window": list(self.cache_window_shape),
            "n_fft": STFT_N_FFT,
            "hop_length": STFT_HOP_LENGTH,
            "center": True,
            "onesided": feature_mode_uses_onesided_stft(self.feature_mode),
            "stitch_windows": self.stitch_windows,
            "stitch_stride": self.stitch_stride,
        }

    def __len__(self):
        return self.record_count

    def __getitem__(self, idx):
        path_id = self.record_path_ids[idx]
        local_idx = self.record_local_idxs[idx]
        label = self.record_labels[idx]
        spec_array = self._load_spec_array(path_id)
        cache_idx = self._cache_index_for_local_idx(path_id, local_idx)
        if cache_idx >= spec_array.shape[0]:
            raise IndexError(
                f"Cache window index {cache_idx} for local index {local_idx} is out of range for "
                f"{self.paths[path_id]} with {spec_array.shape[0]} windows."
            )
        spec = np.array(spec_array[cache_idx], dtype=np.float16, copy=True)
        return torch.from_numpy(spec), torch.tensor(label, dtype=torch.long)

    def _cache_index_for_local_idx(self, path_id, local_idx):
        if self.stitch_windows == 1 and self.stitch_stride == 1:
            return int(local_idx)
        starts = self._load_start_index_array(path_id)
        offset = int(np.searchsorted(starts, int(local_idx)))
        if offset >= starts.shape[0] or int(starts[offset]) != int(local_idx):
            raise IndexError(
                f"Local stitch start {local_idx} is not present in {self.start_index_paths[path_id]}"
            )
        return offset

    def _load_start_index_array(self, path_id):
        key = str(self.start_index_paths[path_id])
        if key not in self.start_index_cache:
            self.start_index_cache[key] = np.load(key, mmap_mode="r")
        return self.start_index_cache[key]

    def _load_spec_array(self, path_id):
        key = str(self.paths[path_id])
        if key not in self.cache:
            if len(self.cache_order) >= self.cache_size:
                old_key = self.cache_order.pop(0)
                self._close_memmap(self.cache.pop(old_key))
            self.cache[key] = np.load(key, mmap_mode="r")
            self.cache_order.append(key)
        return self.cache[key]

    def _close_memmap(self, value):
        mmap = getattr(value, "_mmap", None)
        if mmap is not None:
            mmap.close()

    def close(self):
        for value in self.cache.values():
            self._close_memmap(value)
        for value in self.start_index_cache.values():
            self._close_memmap(value)
        self.cache.clear()
        self.start_index_cache.clear()
        self.cache_order.clear()

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    def _print_index_summary(self):
        return self.base._print_index_summary()


class DualPrecomputedSpecDataset(Dataset):
    """Return two cache views for the exact same manifest window."""

    def __init__(self, primary, secondary):
        if not isinstance(primary, PrecomputedSpecDataset) or not isinstance(
            secondary,
            PrecomputedSpecDataset,
        ):
            raise ValueError("Dual-view caches require two PrecomputedSpecDataset objects.")
        if primary.feature_mode != "legacy":
            raise ValueError("The primary dual-view cache must use legacy features.")
        if secondary.feature_mode != "complex_signed_mag":
            raise ValueError(
                "The secondary dual-view cache must use complex_signed_mag features."
            )

        alignment_fields = (
            ("path ids", primary.record_path_ids, secondary.record_path_ids),
            ("local indices", primary.record_local_idxs, secondary.record_local_idxs),
            ("labels", primary.record_labels, secondary.record_labels),
        )
        for name, left, right in alignment_fields:
            left_values = (
                np.frombuffer(left, dtype=np.dtype(left.typecode))
                if hasattr(left, "typecode")
                else np.asarray(left)
            )
            right_values = (
                np.frombuffer(right, dtype=np.dtype(right.typecode))
                if hasattr(right, "typecode")
                else np.asarray(right)
            )
            if not np.array_equal(left_values, right_values):
                raise ValueError(f"Dual-view cache {name} are not aligned.")
        if [str(path.resolve()) for path in primary.raw_paths] != [
            str(path.resolve()) for path in secondary.raw_paths
        ]:
            raise ValueError("Dual-view caches do not reference the same raw NPZ paths.")

        self.base = primary
        self.secondary = secondary
        self.root_dir = primary.root_dir
        self.split = primary.split
        self.is_train = primary.is_train
        self.record_path_ids = primary.record_path_ids
        self.record_local_idxs = primary.record_local_idxs
        self.record_labels = primary.record_labels
        self.record_count = primary.record_count
        self.labels = primary.labels
        self.paths = primary.paths
        self.raw_paths = primary.raw_paths
        self.cache_validation = primary.cache_validation
        self.secondary_cache_validation = secondary.cache_validation

    def __len__(self):
        return len(self.base)

    def __getitem__(self, idx):
        primary_spec, primary_label = self.base[idx]
        secondary_spec, secondary_label = self.secondary[idx]
        if primary_label.item() != secondary_label.item():
            raise RuntimeError(f"Dual-view label mismatch at record {idx}.")
        return (primary_spec, secondary_spec), primary_label

    def close(self):
        self.base.close()
        self.secondary.close()

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass


class PrecomputedSpecRfStatsDataset(Dataset):
    """Fuse the legacy spectrogram with compact statistics from the same window."""

    def __init__(
        self,
        dataset,
        stats_cache_dir,
        normalization_path,
        cache_size=4,
        cache_data_root=None,
    ):
        if not isinstance(
            dataset,
            (PrecomputedSpecDataset, DualPrecomputedSpecDataset),
        ):
            raise ValueError("RF statistics fusion requires precomputed spectrograms.")
        self.base = dataset
        self.stats_cache_dir = Path(stats_cache_dir)
        self.cache_data_root = (
            Path(cache_data_root)
            if cache_data_root
            else getattr(dataset, "cache_data_root", dataset.root_dir)
        )
        normalization = json.loads(Path(normalization_path).read_text(encoding="utf-8"))
        if normalization.get("version") != RF_STATS_VERSION:
            raise ValueError("RF statistics normalization version does not match.")
        if normalization.get("dim") != RF_STATS_DIM:
            raise ValueError("RF statistics normalization dimension does not match.")
        if not normalization.get("training_only"):
            raise ValueError("RF statistics normalization must be fitted on training sources only.")
        self.stats_mean = np.asarray(normalization["mean"], dtype=np.float32)
        self.stats_std = np.asarray(normalization["std"], dtype=np.float32)
        if self.stats_mean.shape != (RF_STATS_DIM,) or self.stats_std.shape != (RF_STATS_DIM,):
            raise ValueError("RF statistics normalization vectors have invalid shapes.")
        self.stats_std = np.maximum(self.stats_std, 1e-4)
        self.stats_paths = [
            rf_stats_path(self.stats_cache_dir, self.cache_data_root, raw_path)
            for raw_path in dataset.raw_paths
        ]
        missing = [str(path) for path in self.stats_paths if not path.exists()]
        if missing:
            raise FileNotFoundError(
                "Missing compact RF statistics cache files. Examples:\n"
                + "\n".join(missing[:5])
            )
        self.stats_cache = {}
        self.stats_cache_order = []
        self.stats_cache_size = max(1, int(cache_size))
        self.record_path_ids = dataset.record_path_ids
        self.record_local_idxs = dataset.record_local_idxs
        self.record_labels = dataset.record_labels
        self.record_count = dataset.record_count
        self.labels = dataset.labels
        self.paths = dataset.paths
        self.raw_paths = dataset.raw_paths
        self.cache_validation = dataset.cache_validation

    def __len__(self):
        return len(self.base)

    def __getitem__(self, idx):
        spec, label = self.base[idx]
        path_id = int(self.record_path_ids[idx])
        local_idx = int(self.record_local_idxs[idx])
        stats_array = self._load_stats(path_id)
        stats = np.asarray(stats_array[local_idx], dtype=np.float32)
        stats = np.clip((stats - self.stats_mean) / self.stats_std, -10.0, 10.0)
        return (spec, torch.from_numpy(stats.copy())), label

    def _load_stats(self, path_id):
        if path_id not in self.stats_cache:
            if len(self.stats_cache_order) >= self.stats_cache_size:
                old_id = self.stats_cache_order.pop(0)
                self._close_memmap(self.stats_cache.pop(old_id))
            values = np.load(self.stats_paths[path_id], mmap_mode="r")
            if values.ndim != 2 or values.shape[1] != RF_STATS_DIM:
                raise ValueError(
                    f"RF statistics cache {self.stats_paths[path_id]} has shape {values.shape}."
                )
            self.stats_cache[path_id] = values
            self.stats_cache_order.append(path_id)
        return self.stats_cache[path_id]

    @staticmethod
    def _close_memmap(value):
        mmap = getattr(value, "_mmap", None)
        if mmap is not None:
            mmap.close()

    def close(self):
        for value in self.stats_cache.values():
            self._close_memmap(value)
        self.stats_cache.clear()
        self.stats_cache_order.clear()
        self.base.close()

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass


class SourceAnnotatedDataset(Dataset):
    """Add the NPZ/source id to training samples without changing inference."""

    def __init__(self, dataset):
        if not hasattr(dataset, "record_path_ids"):
            raise ValueError("Source annotations require manifest-backed records.")
        self.dataset = dataset

    def __len__(self):
        return len(self.dataset)

    def __getitem__(self, idx):
        inputs, label = self.dataset[idx]
        source_id = int(self.dataset.record_path_ids[idx])
        return inputs, label, torch.tensor(source_id, dtype=torch.long)


def unwrap_dataset(dataset):
    while isinstance(
        dataset,
        (
            PrecomputedSpecDataset,
            DualPrecomputedSpecDataset,
            PrecomputedSpecRfStatsDataset,
        ),
    ):
        dataset = dataset.base
    return dataset


def assert_leakage_safe_split(train_dataset, val_dataset):
    train_base = unwrap_dataset(train_dataset)
    val_base = unwrap_dataset(val_dataset)
    source_group_overlap = sorted(
        train_base.selected_source_groups & val_base.selected_source_groups
    )
    source_file_overlap = sorted(
        train_base.selected_source_files & val_base.selected_source_files
    )
    train_paths = {str(path.resolve()) for path in train_base.paths}
    val_paths = {str(path.resolve()) for path in val_base.paths}
    path_overlap = sorted(train_paths & val_paths)
    split_plan_matches = (
        train_base.val_groups == val_base.val_groups
        and train_base.groups_by_label == val_base.groups_by_label
    )
    train_counts = np.bincount(
        np.asarray(train_base.labels, dtype=np.int64),
        minlength=len(CLASS_NAMES),
    )
    val_counts = np.bincount(
        np.asarray(val_base.labels, dtype=np.int64),
        minlength=len(CLASS_NAMES),
    )
    missing_train_classes = [
        CLASS_NAMES[index] for index, count in enumerate(train_counts) if count == 0
    ]
    missing_val_classes = [
        CLASS_NAMES[index] for index, count in enumerate(val_counts) if count == 0
    ]
    errors = []
    if source_group_overlap:
        errors.append(f"source_group overlap: {source_group_overlap[:5]}")
    if source_file_overlap:
        errors.append(f"source_file overlap: {source_file_overlap[:5]}")
    if path_overlap:
        errors.append(f"NPZ path overlap: {path_overlap[:5]}")
    if not split_plan_matches:
        errors.append("train and validation did not use the same deterministic split plan")
    if missing_train_classes:
        errors.append(f"training is missing classes: {missing_train_classes}")
    if missing_val_classes:
        errors.append(f"validation is missing classes: {missing_val_classes}")
    if train_base.manifest_index_issues or val_base.manifest_index_issues:
        errors.append("manifest local indices are not contiguous within NPZ files")

    path_label_conflicts = []
    for path in train_base.paths + val_base.paths:
        path_label = infer_label_from_path(path)
        filename_label = infer_label_from_path(path.name)
        if path_label != filename_label:
            path_label_conflicts.append(
                {
                    "path": str(path),
                    "directory_class": CLASS_NAMES[path_label],
                    "filename_class": CLASS_NAMES[filename_label],
                }
            )
    warnings = []
    if path_label_conflicts:
        warnings.append(
            "Some filenames encode a different individual than their class directory; "
            "the exact class directory is authoritative."
        )

    report = {
        "status": "error" if errors else ("warning" if warnings else "ok"),
        "grouping_field": "source_file",
        "split_plan_matches": split_plan_matches,
        "train_source_groups": len(train_base.selected_source_groups),
        "val_source_groups": len(val_base.selected_source_groups),
        "source_group_overlap": source_group_overlap,
        "source_file_overlap": source_file_overlap,
        "npz_path_overlap": path_overlap,
        "train_class_counts": {
            CLASS_NAMES[index]: int(count) for index, count in enumerate(train_counts)
        },
        "val_class_counts": {
            CLASS_NAMES[index]: int(count) for index, count in enumerate(val_counts)
        },
        "missing_train_classes": missing_train_classes,
        "missing_val_classes": missing_val_classes,
        "manifest_index_issues": (
            train_base.manifest_index_issues + val_base.manifest_index_issues
        )[:20],
        "path_label_conflicts": path_label_conflicts,
        "warnings": warnings,
        "errors": errors,
    }
    if errors:
        raise RuntimeError(
            "Leakage-safe split assertion failed: " + "; ".join(errors)
        )
    return report


def build_cache_audit_report(
    cache_dir,
    train_dataset,
    val_dataset,
    additional_datasets=(),
    feature_mode="legacy",
):
    feature_mode_id = FEATURE_MODE_IDS[feature_mode]
    cache_dir = Path(cache_dir)
    datasets = [train_dataset, val_dataset, *additional_datasets]
    bases = [unwrap_dataset(dataset) for dataset in datasets]
    cache_window_shape = getattr(datasets[0], "cache_window_shape", CACHE_WINDOW_SHAPE)
    stitch_windows = getattr(datasets[0], "stitch_windows", 1)
    stitch_stride = getattr(datasets[0], "stitch_stride", 1)
    active_paths = {
        str(path.resolve())
        for base in bases
        for path in base.paths
    }
    available_cache_files = sum(
        1 for _ in (cache_dir / "spec_npy").rglob("*.spec.npy")
    )
    warnings = []
    summary_path = cache_dir / "cache_summary.json"
    summary = {}
    if summary_path.exists():
        try:
            summary = json.loads(summary_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            warnings.append(f"cache_summary.json is unreadable: {exc}")
    else:
        warnings.append("cache_summary.json is missing; per-file metadata was validated")

    expected_summary = {
        "feature_mode": feature_mode_id,
        "dtype": CACHE_DTYPE,
        "shape_per_window": list(cache_window_shape),
        "stitch_windows": stitch_windows,
        "stitch_stride": stitch_stride,
    }
    for key, expected in expected_summary.items():
        if summary and summary.get(key) != expected:
            raise ValueError(
                f"Cache summary {key}={summary.get(key)!r}, expected {expected!r}"
            )
    summary_file_count = summary.get("npz_files")
    if (
        summary_file_count is not None
        and int(summary_file_count) != available_cache_files
    ):
        warnings.append(
            f"cache_summary.json reports {summary_file_count} cache files, but "
            f"{available_cache_files} .spec.npy files currently exist; per-file "
            "metadata and arrays are authoritative"
        )

    return {
        "status": "warning" if warnings else "ok",
        "cache_dir": str(cache_dir.resolve()),
        "active_dataset_files": len(active_paths),
        "available_cache_files": available_cache_files,
        "checked_cache_files": (
            sum(
                dataset.cache_validation["checked_files"]
                for dataset in datasets
            )
        ),
        "feature_mode": feature_mode_id,
        "dtype": CACHE_DTYPE,
        "shape_per_window": list(cache_window_shape),
        "stitch_windows": stitch_windows,
        "stitch_stride": stitch_stride,
        "stft": {
            "n_fft": STFT_N_FFT,
            "hop_length": STFT_HOP_LENGTH,
            "center": True,
            "onesided": feature_mode_uses_onesided_stft(feature_mode),
        },
        "summary_path": str(summary_path),
        "summary_npz_files": summary_file_count,
        "warnings": warnings,
    }


def assert_official_evaluation(val_dataset, labels, predictions):
    expected = len(val_dataset)
    present = set(labels)
    missing_classes = [
        CLASS_NAMES[index] for index in range(len(CLASS_NAMES)) if index not in present
    ]
    errors = []
    if len(labels) != expected:
        errors.append(f"evaluated {len(labels):,} labels, expected {expected:,}")
    if len(predictions) != expected:
        errors.append(f"produced {len(predictions):,} predictions, expected {expected:,}")
    if missing_classes:
        errors.append(f"validation labels are missing classes: {missing_classes}")
    if errors:
        raise RuntimeError(
            "Official validation artifacts would be incomplete: " + "; ".join(errors)
        )
    return {
        "status": "ok",
        "validation_is_full": True,
        "expected_windows": expected,
        "evaluated_windows": len(labels),
        "class_ids": list(range(len(CLASS_NAMES))),
        "missing_classes": [],
    }


def build_test_evaluation_audit(dataset, labels, predictions):
    expected = len(dataset)
    present = set(labels)
    missing_classes = [
        CLASS_NAMES[index] for index in range(len(CLASS_NAMES)) if index not in present
    ]
    return {
        "status": "ok" if len(labels) == expected and len(predictions) == expected else "error",
        "split": "test",
        "expected_windows": expected,
        "evaluated_windows": len(labels),
        "prediction_count": len(predictions),
        "missing_classes": missing_classes,
    }


def precompute_spectrogram_cache(
    data_root,
    cache_dir,
    max_files=None,
    max_files_per_class=0,
    precompute_batch_size=4096,
    overwrite=False,
    feature_mode="legacy",
    index_cache_dir="",
    val_ratio=0.2,
    internal_dev_ratio=0.0,
    selection_only=False,
    train_exclude_sources=None,
    stitch_windows=1,
    stitch_stride=1,
):
    feature_mode_id = FEATURE_MODE_IDS[feature_mode]
    stitch_windows = int(stitch_windows)
    stitch_stride = int(stitch_stride)
    cache_window_shape = spectrogram_cache_window_shape(stitch_windows)
    data_root = Path(data_root)
    cache_dir = Path(cache_dir)
    cache_dir.mkdir(parents=True, exist_ok=True)
    manifest_paths = sorted(data_root.rglob("manifest.jsonl"))
    split_contract = inspect_manifest_split_contract(data_root)
    manifest_digest = hashlib.sha256()
    manifest_record_count = 0
    for manifest_path in manifest_paths:
        with manifest_path.open("rb") as manifest_file:
            manifest_digest.update(manifest_file.read())
        with manifest_path.open("r", encoding="utf-8") as manifest_file:
            for line in manifest_file:
                if not line.strip():
                    continue
                manifest_record_count += 1
    splits = ["train"]
    keep_official_splits = (
        not selection_only or split_contract["source"] == "manifest"
    )
    if keep_official_splits:
        splits.append("val")
        if split_contract["counts"].get("test", 0) > 0:
            splits.append("test")
    if internal_dev_ratio > 0 or split_contract["counts"].get("internal_dev", 0) > 0:
        splits.append("internal_dev")
    datasets = [
        IqrawNpzDataset(
            data_root,
            split=split,
            val_ratio=val_ratio,
            is_train=False,
            max_files=max_files,
            max_files_per_class=max_files_per_class,
            cache_size=1,
            return_raw_iq=True,
            index_cache_dir=index_cache_dir,
            internal_dev_ratio=internal_dev_ratio,
            train_exclude_sources=train_exclude_sources,
            stitch_windows=stitch_windows,
            stitch_stride=stitch_stride,
        )
        for split in splits
    ]
    frontend = GpuStftFrontend(
        augment=False,
        feature_mode=feature_mode,
    ).to(device).eval()
    total_records = sum(dataset.record_count for dataset in datasets)
    estimated_gib = total_records * math.prod(cache_window_shape) * np.dtype(np.float16).itemsize / (1024 ** 3)
    print(
        f"Estimated spectrogram cache size: {estimated_gib:.1f} GiB "
        f"for {total_records:,} windows.",
        flush=True,
    )
    summary = {
        "data_root": str(data_root),
        "cache_dir": str(cache_dir),
        "dtype": CACHE_DTYPE,
        "feature_mode": feature_mode_id,
        "shape_per_window": list(cache_window_shape),
        "stitch_windows": stitch_windows,
        "stitch_stride": stitch_stride,
        "estimated_size_gib": estimated_gib,
        "constructed_splits": splits,
        "split_source": split_contract["source"],
        "manifest_sha256": manifest_digest.hexdigest(),
        "manifest_records": manifest_record_count,
        "manifest_split_counts": split_contract["counts"],
        "official_validation_accessed": False,
        "npz_files": 0,
        "created": [],
        "skipped": [],
        "files": [],
        "status": "partial",
    }

    starts_by_path = {}
    split_by_path = {}
    for dataset in datasets:
        for path_id, local_idx in zip(dataset.record_path_ids, dataset.record_local_idxs):
            raw_path = dataset.paths[int(path_id)]
            starts_by_path.setdefault(str(raw_path), set()).add(int(local_idx))
            split_by_path.setdefault(str(raw_path), set()).add(dataset.split)
    starts_by_path = {
        Path(raw_path): sorted(starts)
        for raw_path, starts in starts_by_path.items()
    }
    summary["npz_files"] = len(starts_by_path)
    summary_path = cache_dir / "cache_summary.json"

    def persist_cache_summary(status="partial"):
        summary["status"] = status
        atomic_write_text(
            summary_path,
            json.dumps(summary, ensure_ascii=False, indent=2),
        )

    persist_cache_summary()
    for npz_path, starts in tqdm(starts_by_path.items(), desc="Precomputing spectrogram cache", unit="file"):
        raise_if_cancelled()
        out_path = cache_path_for_npz(cache_dir, data_root, npz_path)
        meta_path = out_path.with_suffix(".json")
        index_path = cache_index_path_for_npz(out_path)
        source_stat = npz_path.stat()
        file_contract = {
            "source_npz": str(npz_path),
            "source_size": source_stat.st_size,
            "source_mtime_ns": source_stat.st_mtime_ns,
            "split": sorted(split_by_path.get(str(npz_path), set())),
            "spec_path": str(out_path),
            "index_path": str(index_path),
            "metadata_path": str(meta_path),
            "windows": len(starts),
            "dtype": CACHE_DTYPE,
            "shape": [len(starts), *cache_window_shape],
        }
        if out_path.exists() and not overwrite:
            existing = np.load(out_path, mmap_mode="r")
            try:
                if (
                    existing.ndim == 4
                    and tuple(existing.shape[1:]) == cache_window_shape
                    and existing.dtype == np.dtype(CACHE_DTYPE)
                    and meta_path.exists()
                    and index_path.exists()
                ):
                    metadata = json.loads(meta_path.read_text(encoding="utf-8"))
                    starts_array = np.load(index_path, mmap_mode="r")
                    try:
                        if (
                            metadata.get("feature_mode") == feature_mode_id
                            and metadata.get("stitch_windows") == stitch_windows
                            and metadata.get("stitch_stride") == stitch_stride
                            and metadata.get("source_size") == source_stat.st_size
                            and metadata.get("source_mtime_ns") == source_stat.st_mtime_ns
                            and starts_array.shape == (len(starts),)
                            and starts_array.dtype == np.dtype("uint32")
                        ):
                            summary["skipped"].append(str(out_path))
                            summary["files"].append(file_contract)
                            persist_cache_summary()
                            continue
                    finally:
                        starts_mmap = getattr(starts_array, "_mmap", None)
                        if starts_mmap is not None:
                            starts_mmap.close()
                raise ValueError(
                    "Existing cache has old/incompatible shape "
                    f"{tuple(existing.shape[1:])}; expected {cache_window_shape}. "
                    "Use --overwrite-cache or a new --cache-dir."
                )
            finally:
                mmap = getattr(existing, "_mmap", None)
                if mmap is not None:
                    mmap.close()

        out_path.parent.mkdir(parents=True, exist_ok=True)
        with np.load(npz_path) as data:
            iq = data["iq"].astype(np.float32)
        if iq.ndim != 3 or iq.shape[1] != 2:
            iq = iq.reshape(iq.shape[0], 2, -1)

        num_windows = len(starts)
        temp_out_path = out_path.with_name(out_path.name + ".tmp")
        mmap = np.lib.format.open_memmap(
            temp_out_path,
            mode="w+",
            dtype=np.dtype(CACHE_DTYPE),
            shape=(num_windows, *cache_window_shape),
        )
        with torch.no_grad():
            for start in range(0, num_windows, precompute_batch_size):
                end = min(start + precompute_batch_size, num_windows)
                batch_starts = starts[start:end]
                batch_np = np.empty(
                    (len(batch_starts), 2, 4096 * stitch_windows),
                    dtype=np.float32,
                )
                for offset, local_idx in enumerate(batch_starts):
                    stop_idx = local_idx + stitch_windows
                    if stop_idx > iq.shape[0]:
                        raise IndexError(
                            f"{npz_path}: stitch window {local_idx}:{stop_idx} exceeds "
                            f"available {iq.shape[0]} windows"
                        )
                    batch_np[offset] = np.concatenate(iq[local_idx:stop_idx], axis=1)
                batch = torch.from_numpy(batch_np).to(device, non_blocking=True)
                spec = frontend(batch).detach().cpu().numpy().astype(np.float16)
                mmap[start:end] = spec
        mmap.flush()
        starts_array = np.asarray(starts, dtype=np.uint32)
        temp_index_path = index_path.with_name(index_path.name + ".tmp.npy")
        np.save(temp_index_path, starts_array)
        os.replace(temp_out_path, out_path)
        os.replace(temp_index_path, index_path)
        meta = {
            "source_npz": str(npz_path),
            "cache_npy": str(out_path),
            "start_index_npy": str(index_path),
            "source_size": source_stat.st_size,
            "source_mtime_ns": source_stat.st_mtime_ns,
            "windows": num_windows,
            "dtype": CACHE_DTYPE,
            "feature_mode": feature_mode_id,
            "shape": [num_windows, *cache_window_shape],
            "n_fft": STFT_N_FFT,
            "hop_length": STFT_HOP_LENGTH,
            "center": True,
            "onesided": feature_mode_uses_onesided_stft(feature_mode),
            "stitch_windows": stitch_windows,
            "stitch_stride": stitch_stride,
        }
        atomic_write_text(
            meta_path,
            json.dumps(meta, ensure_ascii=False, indent=2),
        )
        summary["created"].append(str(out_path))
        summary["files"].append(file_contract)

        persist_cache_summary()

    persist_cache_summary("complete")
    print(
        json.dumps(
            {
                "cache_dir": str(cache_dir),
                "created": len(summary["created"]),
                "skipped": len(summary["skipped"]),
                "summary_path": str(summary_path),
            },
            ensure_ascii=False,
            indent=2,
        ),
        flush=True,
    )
    return summary


def audit_dataset(
    root_dir,
    output_dir,
    cache_dir="",
    val_ratio=0.2,
    npz_samples=4,
    max_files_per_class=0,
    feature_mode="legacy",
):
    """Validate manifests fully, cache contracts fully, and NPZ contents by sample."""
    feature_mode_id = FEATURE_MODE_IDS[feature_mode]
    expected_onesided = feature_mode_uses_onesided_stft(feature_mode)
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    dataset = IqrawNpzDataset(
        root_dir,
        split="train",
        val_ratio=0.0,
        is_train=False,
        max_files_per_class=max_files_per_class,
        cache_size=1,
    )
    if not dataset.record_count:
        raise RuntimeError("Audit requires manifest.jsonl records.")

    class_counts = np.bincount(
        np.asarray(dataset.record_labels, dtype=np.int64),
        minlength=len(CLASS_NAMES),
    )
    issue_paths = {item["path"] for item in dataset.manifest_index_issues}
    path_stats = {}
    invalid_paths = []
    cache_errors = []
    cache_dir = Path(cache_dir) if cache_dir else None
    for path_id, npz_path in enumerate(dataset.paths):
        records = dataset.path_record_counts.get(path_id, 0)
        stat = {
            "path": str(npz_path),
            "records": records,
            "min_local_index": dataset.path_min_indices.get(path_id),
            "max_local_index": dataset.path_max_indices.get(path_id),
        }
        stat["manifest_index_ok"] = (
            stat["min_local_index"] == 0
            and stat["max_local_index"] == records - 1
            and str(npz_path) not in issue_paths
        )
        if not stat["manifest_index_ok"]:
            invalid_paths.append(stat)

        if cache_dir is not None and cache_dir.exists():
            cache_path = cache_path_for_npz(cache_dir, root_dir, npz_path)
            stat["cache_path"] = str(cache_path)
            if not cache_path.exists():
                stat["cache_ok"] = False
                cache_errors.append(f"missing {cache_path}")
            else:
                cached = np.load(cache_path, mmap_mode="r")
                try:
                    stat["cache_shape"] = list(cached.shape)
                    stat["cache_dtype"] = str(cached.dtype)
                    stat["cache_ok"] = (
                        cached.ndim == 4
                        and cached.shape[0] == records
                        and tuple(cached.shape[1:]) == CACHE_WINDOW_SHAPE
                        and cached.dtype == np.dtype(CACHE_DTYPE)
                    )
                finally:
                    mmap = getattr(cached, "_mmap", None)
                    if mmap is not None:
                        mmap.close()
                meta_path = cache_path.with_suffix(".json")
                stat["cache_meta_path"] = str(meta_path)
                try:
                    meta = json.loads(meta_path.read_text(encoding="utf-8"))
                    stat["cache_meta_ok"] = (
                        meta.get("feature_mode") == feature_mode_id
                        and meta.get("dtype") == CACHE_DTYPE
                        and meta.get("shape") == [records, *CACHE_WINDOW_SHAPE]
                        and meta.get("n_fft") == STFT_N_FFT
                        and meta.get("hop_length") == STFT_HOP_LENGTH
                        and meta.get("center") is True
                        and meta.get("onesided") is expected_onesided
                        and PurePosixPath(
                            str(meta.get("source_npz", "")).replace("\\", "/")
                        ).name == npz_path.name
                    )
                except (OSError, json.JSONDecodeError) as exc:
                    stat["cache_meta_ok"] = False
                    stat["cache_meta_error"] = str(exc)
                if not stat.get("cache_ok") or not stat.get("cache_meta_ok"):
                    cache_errors.append(f"invalid cache contract for {cache_path}")
        path_stats[path_id] = stat

    sampled_npz_ids = set()
    if npz_samples > 0 and dataset.paths:
        sampled_npz_ids = set(
            np.linspace(
                0,
                len(dataset.paths) - 1,
                min(npz_samples, len(dataset.paths)),
                dtype=np.int64,
            ).tolist()
        )
    npz_sample_errors = []
    for path_id in tqdm(sorted(sampled_npz_ids), desc="Spot-checking compressed NPZ files"):
        stat = path_stats[path_id]
        try:
            with np.load(dataset.paths[path_id]) as data:
                if "iq" not in data:
                    raise ValueError("missing iq array")
                iq_shape = tuple(data["iq"].shape)
            stat["sampled_iq_shape"] = list(iq_shape)
            stat["sampled_npz_ok"] = (
                len(iq_shape) == 3
                and tuple(iq_shape[1:]) == (2, 4096)
                and iq_shape[0] == stat["records"]
            )
            if not stat["sampled_npz_ok"]:
                npz_sample_errors.append(stat)
        except Exception as exc:
            stat["sampled_npz_ok"] = False
            stat["sampled_npz_error"] = str(exc)
            npz_sample_errors.append(stat)

    group_counts = {
        CLASS_NAMES[label]: len(dataset.groups_by_label.get(label, set()))
        for label in range(len(CLASS_NAMES))
    }
    missing_classes = [
        CLASS_NAMES[label]
        for label, count in enumerate(class_counts)
        if count == 0
    ]
    classes_with_few_groups = [
        name for name, count in group_counts.items() if count < 2
    ]
    multi_npz_groups = [
        {
            "source_group": group_key,
            "npz_files": sorted(paths),
        }
        for group_key, paths in dataset.source_group_to_paths.items()
        if len(paths) > 1
    ]
    val_groups = set()
    for groups in dataset.groups_by_label.values():
        ordered = sorted(groups, key=stable_split_key)
        if val_ratio > 0 and len(ordered) >= 2:
            num_val = min(
                max(1, int(round(len(ordered) * val_ratio))),
                len(ordered) - 1,
            )
            val_groups.update(ordered[:num_val])
    train_groups = set(dataset.source_group_to_paths) - val_groups
    split_overlap = sorted(train_groups & val_groups)

    warnings = []
    path_label_conflicts = []
    for npz_path in dataset.paths:
        path_label = infer_label_from_path(npz_path)
        filename_label = infer_label_from_path(npz_path.name)
        if path_label != filename_label:
            path_label_conflicts.append(
                {
                    "path": str(npz_path),
                    "directory_class": CLASS_NAMES[path_label],
                    "filename_class": CLASS_NAMES[filename_label],
                }
            )
    if path_label_conflicts:
        warnings.append(
            "One or more filenames disagree with the exact class directory; "
            "the directory class was used."
        )

    dataset_summary_path = Path(root_dir) / "summary.json"
    dataset_summary = {}
    if dataset_summary_path.exists():
        try:
            dataset_summary = json.loads(dataset_summary_path.read_text(encoding="utf-8"))
            if dataset_summary.get("total_files") != len(dataset.paths):
                warnings.append(
                    f"dataset summary reports {dataset_summary.get('total_files')} files; "
                    f"active manifests reference {len(dataset.paths)}"
                )
            if dataset_summary.get("total_windows") != dataset.record_count:
                warnings.append(
                    f"dataset summary reports {dataset_summary.get('total_windows')} windows; "
                    f"active manifests contain {dataset.record_count}"
                )
        except (OSError, json.JSONDecodeError) as exc:
            warnings.append(f"dataset summary is unreadable: {exc}")

    cache_summary_path = cache_dir / "cache_summary.json" if cache_dir else None
    cache_summary_npz_files = None
    if cache_summary_path and cache_summary_path.exists():
        try:
            cache_summary = json.loads(cache_summary_path.read_text(encoding="utf-8"))
            cache_summary_npz_files = cache_summary.get("npz_files")
            if (
                cache_summary_npz_files is not None
                and int(cache_summary_npz_files) != len(dataset.paths)
            ):
                warnings.append(
                    f"cache_summary.json reports {cache_summary_npz_files} files; "
                    f"active manifests reference {len(dataset.paths)}"
                )
        except (OSError, json.JSONDecodeError) as exc:
            warnings.append(f"cache_summary.json is unreadable: {exc}")

    status = "error" if (
        invalid_paths or cache_errors or npz_sample_errors or split_overlap
    ) else (
        "warning" if missing_classes or classes_with_few_groups else "ok"
    )
    if status == "ok" and warnings:
        status = "warning"
    report = {
        "status": status,
        "dataset_root": str(Path(root_dir)),
        "npz_files": len(dataset.paths),
        "manifest_records": dataset.record_count,
        "class_counts": {
            CLASS_NAMES[label]: int(class_counts[label])
            for label in range(len(CLASS_NAMES))
        },
        "source_groups_per_class": group_counts,
        "unique_source_groups": len(dataset.source_group_to_paths),
        "source_groups_mapping_to_multiple_npz": multi_npz_groups,
        "path_label_conflicts": path_label_conflicts,
        "split_assertion": {
            "grouping_field": "source_file",
            "val_ratio": val_ratio,
            "train_source_groups": len(train_groups),
            "val_source_groups": len(val_groups),
            "source_group_overlap": split_overlap,
        },
        "missing_classes": missing_classes,
        "classes_with_less_than_two_source_groups": classes_with_few_groups,
        "npz_checks": list(path_stats.values()),
        "invalid_manifest_indices": invalid_paths,
        "manifest_index_issue_examples": dataset.manifest_index_issues,
        "cache_contract": {
            "cache_dir": str(cache_dir) if cache_dir else None,
            "checked_files": len(dataset.paths) if cache_dir else 0,
            "errors": cache_errors[:20],
            "summary_npz_files": cache_summary_npz_files,
            "warnings": warnings,
        },
        "dataset_summary": {
            "path": str(dataset_summary_path),
            "reported_total_files": dataset_summary.get("total_files"),
            "reported_total_windows": dataset_summary.get("total_windows"),
        },
        "warnings": warnings,
        "npz_spot_check": {
            "sampled_files": len(sampled_npz_ids),
            "errors": npz_sample_errors,
        },
        "notes": [
            "source_file is treated as a grouping identifier, not a numeric file count.",
            "sample_index is the NPZ-local index; window_index resets per segment.",
            "A class needs at least two source groups for a non-empty train/validation split.",
            "Per-file cache metadata is authoritative when cache_summary.json counts are stale.",
        ],
    }
    audit_path = output_dir / "dataset_audit.json"
    audit_path.write_text(
        json.dumps(report, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    print(json.dumps({
        "status": report["status"],
        "npz_files": report["npz_files"],
        "manifest_records": report["manifest_records"],
        "unique_source_groups": report["unique_source_groups"],
        "missing_classes": report["missing_classes"],
        "classes_with_less_than_two_source_groups": report[
            "classes_with_less_than_two_source_groups"
        ],
        "audit_path": str(audit_path),
    }, ensure_ascii=False, indent=2))
    return report


class ChannelAttention(nn.Module):
    def __init__(self, channel, reduction=16):
        super().__init__()
        hidden = max(channel // reduction, 1)
        self.avg_pool = nn.AdaptiveAvgPool2d(1)
        self.fc = nn.Sequential(
            nn.Conv2d(channel, hidden, 1, bias=False),
            nn.ReLU(inplace=True),
            nn.Conv2d(hidden, channel, 1, bias=False),
            nn.Sigmoid(),
        )

    def forward(self, x):
        return x * self.fc(self.avg_pool(x))


class ResidualBlock(nn.Module):
    def __init__(self, in_channels, out_channels, stride=1):
        super().__init__()
        self.conv1 = nn.Sequential(
            nn.Conv2d(in_channels, out_channels, kernel_size=3, stride=stride, padding=1),
            nn.BatchNorm2d(out_channels),
            nn.GELU(),
        )
        self.conv2 = nn.Sequential(
            nn.Conv2d(out_channels, out_channels, kernel_size=3, padding=1),
            nn.BatchNorm2d(out_channels),
        )
        self.ca = ChannelAttention(out_channels)
        self.shortcut = nn.Sequential()
        if stride != 1 or in_channels != out_channels:
            self.shortcut = nn.Sequential(
                nn.Conv2d(in_channels, out_channels, kernel_size=1, stride=stride),
                nn.BatchNorm2d(out_channels),
            )
        self.gelu = nn.GELU()

    def forward(self, x):
        residual = self.shortcut(x)
        x = self.conv1(x)
        x = self.conv2(x)
        return self.gelu(self.ca(x) + residual)


class AISNet(nn.Module):
    def __init__(self, num_classes=len(CLASS_NAMES), dropout=0.5, embedding_dim=512):
        super().__init__()
        self.embedding_dim = int(embedding_dim)
        self.initial = nn.Sequential(
            nn.Conv2d(2, 64, kernel_size=7, stride=2, padding=3),
            nn.BatchNorm2d(64),
            nn.GELU(),
            nn.MaxPool2d(kernel_size=3, stride=2, padding=1),
        )
        self.layer1 = self._make_layer(64, 64, 2)
        self.layer2 = self._make_layer(64, 128, 2, stride=2)
        self.layer3 = self._make_layer(128, 256, 2, stride=2)
        self.layer4 = self._make_layer(256, 512, 2, stride=2)
        self.pool = nn.Sequential(
            nn.AdaptiveAvgPool2d(1),
            nn.Flatten(),
        )
        self.embedding_head = nn.Sequential(
            nn.Linear(512, self.embedding_dim),
            nn.BatchNorm1d(self.embedding_dim),
            nn.GELU(),
            nn.Dropout(dropout),
        )
        self.classifier = nn.Linear(self.embedding_dim, num_classes)

    def _make_layer(self, in_channels, out_channels, blocks, stride=1):
        layers = [ResidualBlock(in_channels, out_channels, stride)]
        for _ in range(1, blocks):
            layers.append(ResidualBlock(out_channels, out_channels))
        return nn.Sequential(*layers)

    def forward_features(self, x):
        x = self.initial(x)
        x = self.layer1(x)
        x = self.layer2(x)
        x = self.layer3(x)
        x = self.layer4(x)
        return self.embedding_head(self.pool(x))

    def forward(self, x, return_embedding=False):
        embedding = self.forward_features(x)
        logits = self.classifier(embedding)
        if return_embedding:
            return logits, embedding
        return logits


class ConvBNAct(nn.Sequential):
    def __init__(self, in_channels, out_channels, stride=1):
        super().__init__(
            nn.Conv2d(
                in_channels,
                out_channels,
                kernel_size=3,
                stride=stride,
                padding=1,
                bias=False,
            ),
            nn.BatchNorm2d(out_channels),
            nn.GELU(),
        )


class LightAISNet(nn.Module):
    """Small CNN baseline for source individual ID.

    The original AISNet has residual blocks, channel attention, and a large
    classifier MLP. This lighter model intentionally removes those parts so it
    is harder to memorize source-file artifacts and easier to use as a baseline.
    """

    def __init__(self, num_classes=len(CLASS_NAMES), dropout=0.5, embedding_dim=192):
        super().__init__()
        self.embedding_dim = int(embedding_dim)
        self.features = nn.Sequential(
            ConvBNAct(2, 32),
            ConvBNAct(32, 32),
            ConvBNAct(32, 64, stride=2),
            ConvBNAct(64, 64),
            ConvBNAct(64, 128, stride=2),
            ConvBNAct(128, 128),
            ConvBNAct(128, 192, stride=2),
        )
        self.pool = nn.Sequential(
            nn.AdaptiveAvgPool2d(1),
            nn.Flatten(),
        )
        self.embedding_head = nn.Sequential(
            nn.Linear(192, self.embedding_dim),
            nn.BatchNorm1d(self.embedding_dim),
            nn.GELU(),
            nn.Dropout(dropout),
        )
        self.classifier = nn.Linear(self.embedding_dim, num_classes)

    def forward_features(self, x):
        return self.embedding_head(self.pool(self.features(x)))

    def forward(self, x, return_embedding=False):
        embedding = self.forward_features(x)
        logits = self.classifier(embedding)
        if return_embedding:
            return logits, embedding
        return logits


class WideLightAISNet(nn.Module):
    """Parameter-control variant of LightAISNet with the same pooling bias."""

    def __init__(self, num_classes=len(CLASS_NAMES), dropout=0.5, embedding_dim=192):
        super().__init__()
        self.embedding_dim = int(embedding_dim)
        self.features = nn.Sequential(
            ConvBNAct(2, 40),
            ConvBNAct(40, 40),
            ConvBNAct(40, 80, stride=2),
            ConvBNAct(80, 80),
            ConvBNAct(80, 160, stride=2),
            ConvBNAct(160, 160),
            ConvBNAct(160, 224, stride=2),
        )
        self.pool = nn.Sequential(
            nn.AdaptiveAvgPool2d(1),
            nn.Flatten(),
        )
        self.embedding_head = nn.Sequential(
            nn.Linear(224, self.embedding_dim),
            nn.BatchNorm1d(self.embedding_dim),
            nn.GELU(),
            nn.Dropout(dropout),
        )
        self.classifier = nn.Linear(self.embedding_dim, num_classes)

    def forward_features(self, x):
        return self.embedding_head(self.pool(self.features(x)))

    def forward(self, x, return_embedding=False):
        embedding = self.forward_features(x)
        logits = self.classifier(embedding)
        if return_embedding:
            return logits, embedding
        return logits


class AxisSummaryBranch(nn.Module):
    """Retain an ordered axis profile after pooling over the other axis."""

    def __init__(
        self,
        channels,
        output_channels=96,
        output_bins=6,
        stable_profile_std=False,
    ):
        super().__init__()
        self.stable_profile_std = bool(stable_profile_std)
        self.profile = nn.Sequential(
            nn.Conv1d(
                channels * 2,
                output_channels,
                kernel_size=3,
                padding=1,
                bias=False,
            ),
            nn.BatchNorm1d(output_channels),
            nn.GELU(),
            nn.AdaptiveAvgPool1d(output_bins),
            nn.Flatten(),
        )

    def forward(self, x, reduce_dim):
        mean = x.mean(dim=reduce_dim)
        std = (
            stable_std(x, dim=reduce_dim)
            if self.stable_profile_std
            else x.std(dim=reduce_dim, unbiased=False)
        )
        return self.profile(torch.cat((mean, std), dim=1))


class AxisAwareAISNet(nn.Module):
    """Light single-window backbone that preserves frequency/time structure."""

    def __init__(self, num_classes=len(CLASS_NAMES), dropout=0.5, embedding_dim=192):
        super().__init__()
        self.embedding_dim = int(embedding_dim)
        self.features = nn.Sequential(
            ConvBNAct(2, 32),
            ConvBNAct(32, 48),
            ConvBNAct(48, 64, stride=(2, 1)),
            ConvBNAct(64, 96),
            ConvBNAct(96, 128, stride=(2, 1)),
            ConvBNAct(128, 160, stride=(2, 1)),
        )
        self.frequency_profile = AxisSummaryBranch(160)
        self.time_profile = AxisSummaryBranch(160)
        pooled_dim = 2 * 96 * 6 + 2 * 160
        self.embedding_head = nn.Sequential(
            nn.Linear(pooled_dim, self.embedding_dim),
            nn.BatchNorm1d(self.embedding_dim),
            nn.GELU(),
            nn.Dropout(dropout),
        )
        self.classifier = nn.Linear(self.embedding_dim, num_classes)

    def forward_features(self, x):
        x = self.features(x)
        frequency_profile = self.frequency_profile(x, reduce_dim=3)
        time_profile = self.time_profile(x, reduce_dim=2)
        global_mean = x.mean(dim=(2, 3))
        global_std = x.std(dim=(2, 3), unbiased=False)
        pooled = torch.cat(
            (frequency_profile, time_profile, global_mean, global_std),
            dim=1,
        )
        return self.embedding_head(pooled)

    def forward(self, x, return_embedding=False):
        embedding = self.forward_features(x)
        logits = self.classifier(embedding)
        if return_embedding:
            return logits, embedding
        return logits


class AxisProfileLargeAISNet(nn.Module):
    """Capacity control that retains the original factorized axis summaries."""

    def __init__(self, num_classes=len(CLASS_NAMES), dropout=0.5, embedding_dim=256):
        super().__init__()
        self.embedding_dim = int(embedding_dim)
        self.features = nn.Sequential(
            ConvBNAct(2, 40),
            ConvBNAct(40, 56),
            ConvBNAct(56, 80, stride=(2, 1)),
            ConvBNAct(80, 112),
            ConvBNAct(112, 160, stride=(2, 1)),
            ConvBNAct(160, 192, stride=(2, 1)),
        )
        self.frequency_profile = AxisSummaryBranch(192, output_channels=128)
        self.time_profile = AxisSummaryBranch(192, output_channels=128)
        pooled_dim = 2 * 128 * 6 + 2 * 192
        self.embedding_head = nn.Sequential(
            nn.Linear(pooled_dim, self.embedding_dim),
            nn.BatchNorm1d(self.embedding_dim),
            nn.GELU(),
            nn.Dropout(dropout),
        )
        self.classifier = nn.Linear(self.embedding_dim, num_classes)

    def forward_features(self, x):
        x = self.features(x)
        frequency_profile = self.frequency_profile(x, reduce_dim=3)
        time_profile = self.time_profile(x, reduce_dim=2)
        global_mean = x.mean(dim=(2, 3))
        global_std = x.std(dim=(2, 3), unbiased=False)
        pooled = torch.cat(
            (frequency_profile, time_profile, global_mean, global_std),
            dim=1,
        )
        return self.embedding_head(pooled)

    def forward(self, x, return_embedding=False):
        embedding = self.forward_features(x)
        logits = self.classifier(embedding)
        if return_embedding:
            return logits, embedding
        return logits


class AxisProfileXLargeAISNet(nn.Module):
    """Parameter-matched single-view control for the dual-view backbone."""

    def __init__(self, num_classes=len(CLASS_NAMES), dropout=0.5, embedding_dim=320):
        super().__init__()
        self.embedding_dim = int(embedding_dim)
        self.features = nn.Sequential(
            ConvBNAct(2, 56),
            ConvBNAct(56, 84),
            ConvBNAct(84, 120, stride=(2, 1)),
            ConvBNAct(120, 176),
            ConvBNAct(176, 248, stride=(2, 1)),
            ConvBNAct(248, 312, stride=(2, 1)),
        )
        self.frequency_profile = AxisSummaryBranch(
            312,
            output_channels=160,
        )
        self.time_profile = AxisSummaryBranch(
            312,
            output_channels=160,
        )
        pooled_dim = 2 * 160 * 6 + 2 * 312
        self.embedding_head = nn.Sequential(
            nn.Linear(pooled_dim, self.embedding_dim),
            nn.BatchNorm1d(self.embedding_dim),
            nn.GELU(),
            nn.Dropout(dropout),
        )
        self.classifier = nn.Linear(self.embedding_dim, num_classes)

    def forward_features(self, x):
        x = self.features(x)
        frequency_profile = self.frequency_profile(x, reduce_dim=3)
        time_profile = self.time_profile(x, reduce_dim=2)
        global_mean = x.mean(dim=(2, 3))
        global_std = x.std(dim=(2, 3), unbiased=False)
        pooled = torch.cat(
            (frequency_profile, time_profile, global_mean, global_std),
            dim=1,
        )
        return self.embedding_head(pooled)

    def forward(self, x, return_embedding=False):
        embedding = self.forward_features(x)
        logits = self.classifier(embedding)
        if return_embedding:
            return logits, embedding
        return logits


class AxisProfileXLargeMultiScaleAISNet(AxisProfileXLargeAISNet):
    """XLarge backbone with coarse and fine temporal profile branches."""

    def __init__(self, num_classes=len(CLASS_NAMES), dropout=0.5, embedding_dim=320):
        super().__init__(
            num_classes=num_classes,
            dropout=dropout,
            embedding_dim=embedding_dim,
        )
        self.time_profile_coarse = AxisSummaryBranch(
            312,
            output_channels=48,
            output_bins=3,
        )
        self.time_profile_fine = AxisSummaryBranch(
            312,
            output_channels=48,
            output_bins=12,
        )
        pooled_dim = 2 * 160 * 6 + 2 * 312 + 48 * 3 + 48 * 12
        self.embedding_head = nn.Sequential(
            nn.Linear(pooled_dim, self.embedding_dim),
            nn.BatchNorm1d(self.embedding_dim),
            nn.GELU(),
            nn.Dropout(dropout),
        )
        self.classifier = nn.Linear(self.embedding_dim, num_classes)

    def forward_features(self, x):
        x = self.features(x)
        frequency_profile = self.frequency_profile(x, reduce_dim=3)
        time_profile = self.time_profile(x, reduce_dim=2)
        time_profile_coarse = self.time_profile_coarse(x, reduce_dim=2)
        time_profile_fine = self.time_profile_fine(x, reduce_dim=2)
        global_mean = x.mean(dim=(2, 3))
        global_std = x.std(dim=(2, 3), unbiased=False)
        pooled = torch.cat(
            (
                frequency_profile,
                time_profile,
                time_profile_coarse,
                time_profile_fine,
                global_mean,
                global_std,
            ),
            dim=1,
        )
        return self.embedding_head(pooled)


class AxisProfileXLargeGatedMultiScaleAISNet(AxisProfileXLargeAISNet):
    """XLarge backbone with zero-gated coarse/fine temporal residuals."""

    init_optional_state_prefixes = (
        "backbone.time_profile_coarse.",
        "backbone.time_profile_fine.",
        "backbone.multiscale_projection.",
        "backbone.multiscale_gate",
    )

    def __init__(self, num_classes=len(CLASS_NAMES), dropout=0.5, embedding_dim=320):
        super().__init__(
            num_classes=num_classes,
            dropout=dropout,
            embedding_dim=embedding_dim,
        )
        self.time_profile_coarse = AxisSummaryBranch(
            312,
            output_channels=48,
            output_bins=3,
        )
        self.time_profile_fine = AxisSummaryBranch(
            312,
            output_channels=48,
            output_bins=12,
        )
        self.multiscale_projection = nn.Sequential(
            nn.Linear(48 * 3 + 48 * 12, self.embedding_dim),
            nn.LayerNorm(self.embedding_dim),
            nn.GELU(),
            nn.Dropout(dropout),
        )
        self.multiscale_gate = nn.Parameter(torch.zeros(()))

    def forward_features(self, x):
        x = self.features(x)
        frequency_profile = self.frequency_profile(x, reduce_dim=3)
        time_profile = self.time_profile(x, reduce_dim=2)
        global_mean = x.mean(dim=(2, 3))
        global_std = x.std(dim=(2, 3), unbiased=False)
        base_pooled = torch.cat(
            (frequency_profile, time_profile, global_mean, global_std),
            dim=1,
        )
        base_embedding = self.embedding_head(base_pooled)
        multiscale_pooled = torch.cat(
            (
                self.time_profile_coarse(x, reduce_dim=2),
                self.time_profile_fine(x, reduce_dim=2),
            ),
            dim=1,
        )
        multiscale_embedding = self.multiscale_projection(multiscale_pooled)
        return base_embedding + torch.tanh(self.multiscale_gate) * multiscale_embedding


class AxisProfileLargeEncoder(nn.Module):
    """Classifier-free profile encoder used by the two-view backbone."""

    def __init__(
        self,
        dropout=0.5,
        embedding_dim=256,
        input_channels=2,
        stable_global_std=False,
        stable_profile_std=False,
    ):
        super().__init__()
        self.embedding_dim = int(embedding_dim)
        self.stable_global_std = bool(stable_global_std)
        self.stable_profile_std = bool(stable_profile_std)
        self.features = nn.Sequential(
            ConvBNAct(input_channels, 40),
            ConvBNAct(40, 56),
            ConvBNAct(56, 80, stride=(2, 1)),
            ConvBNAct(80, 112),
            ConvBNAct(112, 160, stride=(2, 1)),
            ConvBNAct(160, 192, stride=(2, 1)),
        )
        self.frequency_profile = AxisSummaryBranch(
            192,
            output_channels=128,
            stable_profile_std=self.stable_profile_std,
        )
        self.time_profile = AxisSummaryBranch(
            192,
            output_channels=128,
            stable_profile_std=self.stable_profile_std,
        )
        pooled_dim = 2 * 128 * 6 + 2 * 192
        self.embedding_head = nn.Sequential(
            nn.Linear(pooled_dim, self.embedding_dim),
            nn.BatchNorm1d(self.embedding_dim),
            nn.GELU(),
            nn.Dropout(dropout),
        )

    def forward(self, x):
        x = self.features(x)
        frequency_profile = self.frequency_profile(x, reduce_dim=3)
        time_profile = self.time_profile(x, reduce_dim=2)
        global_mean = x.mean(dim=(2, 3))
        global_std = (
            stable_std(x, dim=(2, 3))
            if self.stable_global_std
            else x.std(dim=(2, 3), unbiased=False)
        )
        pooled = torch.cat(
            (frequency_profile, time_profile, global_mean, global_std),
            dim=1,
        )
        return self.embedding_head(pooled)


class DualAxisProfileLargeAISNet(nn.Module):
    """Fuse legacy and signed-complex views inside one window classifier."""

    def __init__(self, num_classes=len(CLASS_NAMES), dropout=0.5, embedding_dim=256):
        super().__init__()
        self.embedding_dim = int(embedding_dim)
        self.legacy_encoder = AxisProfileLargeEncoder(
            dropout=dropout,
            embedding_dim=self.embedding_dim,
        )
        self.complex_encoder = AxisProfileLargeEncoder(
            dropout=dropout,
            embedding_dim=self.embedding_dim,
        )
        self.fusion = nn.Sequential(
            nn.Linear(2 * self.embedding_dim, self.embedding_dim),
            nn.LayerNorm(self.embedding_dim),
            nn.GELU(),
            nn.Dropout(dropout),
        )
        self.classifier = nn.Linear(self.embedding_dim, num_classes)

    def forward_features(self, inputs):
        if not isinstance(inputs, (tuple, list)) or len(inputs) != 2:
            raise ValueError("Dual-axis backbone requires legacy and complex views.")
        legacy, complex_signed = inputs
        return self.fusion(
            torch.cat(
                (
                    self.legacy_encoder(legacy),
                    self.complex_encoder(complex_signed),
                ),
                dim=1,
            )
        )

    def forward(self, inputs, return_embedding=False):
        embedding = self.forward_features(inputs)
        logits = self.classifier(embedding)
        if return_embedding:
            return logits, embedding
        return logits


class PhaseSequenceEncoder(nn.Module):
    """Encode window-local amplitude, phase-step, and autocorrelation tracks."""

    def __init__(self, dropout=0.5, embedding_dim=256):
        super().__init__()
        self.embedding_dim = int(embedding_dim)
        self.features = nn.Sequential(
            nn.Conv1d(
                PHASE_AWARE_SEQUENCE_CHANNELS,
                64,
                kernel_size=5,
                padding=2,
                bias=False,
            ),
            nn.BatchNorm1d(64),
            nn.GELU(),
            nn.Conv1d(64, 96, kernel_size=3, stride=2, padding=1, bias=False),
            nn.BatchNorm1d(96),
            nn.GELU(),
            nn.Conv1d(96, 128, kernel_size=3, padding=1, bias=False),
            nn.BatchNorm1d(128),
            nn.GELU(),
            nn.Conv1d(128, 160, kernel_size=3, stride=2, padding=1, bias=False),
            nn.BatchNorm1d(160),
            nn.GELU(),
        )
        self.profile = nn.AdaptiveAvgPool1d(8)
        self.embedding_head = nn.Sequential(
            nn.Linear(160 * 8 + 160 * 2, self.embedding_dim),
            nn.LayerNorm(self.embedding_dim),
            nn.GELU(),
            nn.Dropout(dropout),
        )

    def forward(self, x):
        x = self.features(x)
        pooled = torch.cat(
            (
                self.profile(x).flatten(1),
                x.mean(dim=2),
                stable_std(x, dim=2),
            ),
            dim=1,
        )
        return self.embedding_head(pooled)


class PhaseAwareAISNet(nn.Module):
    """Fuse multi-resolution magnitude and phase-invariant raw-IQ structure."""

    def __init__(self, num_classes=len(CLASS_NAMES), dropout=0.5, embedding_dim=256):
        super().__init__()
        self.embedding_dim = int(embedding_dim)
        self.spectral_encoder = AxisProfileLargeEncoder(
            dropout=dropout,
            embedding_dim=self.embedding_dim,
            input_channels=4,
            stable_global_std=True,
            stable_profile_std=True,
        )
        self.phase_encoder = PhaseSequenceEncoder(
            dropout=dropout,
            embedding_dim=self.embedding_dim,
        )
        self.fusion = nn.Sequential(
            nn.Linear(2 * self.embedding_dim, self.embedding_dim),
            nn.LayerNorm(self.embedding_dim),
            nn.GELU(),
            nn.Dropout(dropout),
        )
        self.classifier = nn.Linear(self.embedding_dim, num_classes)

    def forward_features(self, inputs):
        if not isinstance(inputs, (tuple, list)) or len(inputs) != 2:
            raise ValueError(
                "Phase-aware backbone requires spectral and phase-sequence views."
            )
        spectral_view, phase_view = inputs
        return self.fusion(
            torch.cat(
                (
                    self.spectral_encoder(spectral_view),
                    self.phase_encoder(phase_view),
                ),
                dim=1,
            )
        )

    def forward(self, inputs, return_embedding=False):
        embedding = self.forward_features(inputs)
        logits = self.classifier(embedding)
        if return_embedding:
            return logits, embedding
        return logits


class AxisGridLargeAISNet(nn.Module):
    """Parameter-matched 2D summary that retains time-frequency co-location."""

    def __init__(self, num_classes=len(CLASS_NAMES), dropout=0.5, embedding_dim=256):
        super().__init__()
        self.embedding_dim = int(embedding_dim)
        self.grid_size = (8, 6)
        self.features = nn.Sequential(
            ConvBNAct(2, 40),
            ConvBNAct(40, 56),
            ConvBNAct(56, 80, stride=(2, 1)),
            ConvBNAct(80, 112),
            ConvBNAct(112, 160, stride=(2, 1)),
            ConvBNAct(160, 192, stride=(2, 1)),
        )
        grid_channels = 26
        self.grid_projection = nn.Sequential(
            nn.Conv2d(192, grid_channels, kernel_size=1, bias=False),
            nn.BatchNorm2d(grid_channels),
            nn.GELU(),
        )
        self.grid_pool = nn.AdaptiveAvgPool2d(self.grid_size)
        pooled_dim = 2 * grid_channels * self.grid_size[0] * self.grid_size[1]
        pooled_dim += 2 * 192
        self.embedding_head = nn.Sequential(
            nn.Linear(pooled_dim, self.embedding_dim),
            nn.BatchNorm1d(self.embedding_dim),
            nn.GELU(),
            nn.Dropout(dropout),
        )
        self.classifier = nn.Linear(self.embedding_dim, num_classes)

    def forward_features(self, x):
        x = self.features(x)
        grid = self.grid_projection(x)
        grid_mean = self.grid_pool(grid)
        grid_second_moment = self.grid_pool(grid.square())
        grid_std = (
            grid_second_moment - grid_mean.square()
        ).clamp_min(1e-6).sqrt()
        global_mean = x.mean(dim=(2, 3))
        global_std = x.std(dim=(2, 3), unbiased=False)
        pooled = torch.cat(
            (
                grid_mean.flatten(1),
                grid_std.flatten(1),
                global_mean,
                global_std,
            ),
            dim=1,
        )
        return self.embedding_head(pooled)

    def forward(self, x, return_embedding=False):
        embedding = self.forward_features(x)
        logits = self.classifier(embedding)
        if return_embedding:
            return logits, embedding
        return logits


def group_norm(channels, max_groups=16):
    groups = min(max_groups, channels)
    while channels % groups:
        groups -= 1
    return nn.GroupNorm(groups, channels)


class MediumResidualBlock(nn.Module):
    """GroupNorm residual block that avoids source-dependent batch statistics."""

    def __init__(self, in_channels, out_channels, stride=1):
        super().__init__()
        self.conv1 = nn.Conv2d(
            in_channels,
            out_channels,
            kernel_size=3,
            stride=stride,
            padding=1,
            bias=False,
        )
        self.norm1 = group_norm(out_channels)
        self.conv2 = nn.Conv2d(
            out_channels,
            out_channels,
            kernel_size=3,
            padding=1,
            bias=False,
        )
        self.norm2 = group_norm(out_channels)
        self.shortcut = nn.Identity()
        if stride != 1 or in_channels != out_channels:
            self.shortcut = nn.Sequential(
                nn.Conv2d(
                    in_channels,
                    out_channels,
                    kernel_size=1,
                    stride=stride,
                    bias=False,
                ),
                group_norm(out_channels),
            )

    def forward(self, x):
        residual = self.shortcut(x)
        x = F.gelu(self.norm1(self.conv1(x)))
        x = self.norm2(self.conv2(x))
        return F.gelu(x + residual)


class AttentiveStatsPool2d(nn.Module):
    """Learn a window-local weighting and retain first and second moments."""

    def __init__(self, channels):
        super().__init__()
        hidden = max(channels // 4, 32)
        self.attention = nn.Sequential(
            nn.Conv2d(channels, hidden, kernel_size=1),
            nn.GELU(),
            nn.Conv2d(hidden, 1, kernel_size=1),
        )

    def forward(self, x):
        features = x.flatten(2)
        weights = F.softmax(self.attention(x).flatten(2), dim=2)
        mean = (features * weights).sum(dim=2)
        variance = ((features - mean.unsqueeze(2)).square() * weights).sum(dim=2)
        std = variance.clamp_min(1e-6).sqrt()
        return torch.cat((mean, std), dim=1)


class MediumAISNet(nn.Module):
    """Intermediate-capacity single-window model with attentive statistics."""

    def __init__(self, num_classes=len(CLASS_NAMES), dropout=0.5, embedding_dim=256):
        super().__init__()
        self.embedding_dim = int(embedding_dim)
        self.stem = nn.Sequential(
            nn.Conv2d(2, 48, kernel_size=5, stride=(2, 1), padding=2, bias=False),
            group_norm(48),
            nn.GELU(),
        )
        self.stages = nn.Sequential(
            MediumResidualBlock(48, 48),
            MediumResidualBlock(48, 48),
            MediumResidualBlock(48, 96, stride=2),
            MediumResidualBlock(96, 96),
            MediumResidualBlock(96, 160, stride=2),
            MediumResidualBlock(160, 160),
            MediumResidualBlock(160, 256, stride=2),
            MediumResidualBlock(256, 256),
        )
        self.pool = AttentiveStatsPool2d(256)
        self.embedding_head = nn.Sequential(
            nn.Linear(512, self.embedding_dim),
            nn.LayerNorm(self.embedding_dim),
            nn.GELU(),
            nn.Dropout(dropout),
        )
        self.classifier = nn.Linear(self.embedding_dim, num_classes)

    def forward_features(self, x):
        return self.embedding_head(self.pool(self.stages(self.stem(x))))

    def forward(self, x, return_embedding=False):
        embedding = self.forward_features(x)
        logits = self.classifier(embedding)
        if return_embedding:
            return logits, embedding
        return logits


class GroupedBalancedBatchSampler(BatchSampler):
    """Balance labels while limiting repeated paths and windows.

    A batch is made of class/file blocks. Labels are sampled without replacement
    when possible. NPZ paths are scheduled with a configurable size weight, and
    each path uses a shuffled window queue before any window is repeated.
    """

    def __init__(
        self,
        dataset,
        batch_size,
        blocks_per_batch=2,
        batches_per_file_group=4,
        seed=42,
        num_batches=None,
        path_weight_power=0.5,
        shuffle_within_batch=True,
    ):
        if not dataset.record_count:
            raise ValueError("GroupedBalancedBatchSampler requires manifest records.")
        if batch_size % blocks_per_batch != 0:
            raise ValueError("batch_size must be divisible by blocks_per_batch.")

        self.dataset = dataset
        self.batch_size = batch_size
        self.blocks_per_batch = blocks_per_batch
        self.batches_per_file_group = max(1, int(batches_per_file_group))
        self.block_size = batch_size // blocks_per_batch
        self.seed = seed
        self.path_weight_power = float(path_weight_power)
        if not 0.0 <= self.path_weight_power <= 1.0:
            raise ValueError("path_weight_power must be between 0 and 1.")
        self.shuffle_within_batch = bool(shuffle_within_batch)
        self.epoch = 0
        full_num_batches = max(1, math.ceil(len(dataset) / batch_size))
        self.num_batches = (
            min(full_num_batches, max(1, int(num_batches)))
            if num_batches is not None and int(num_batches) > 0
            else full_num_batches
        )
        self.records_by_label_path = {}
        self.last_epoch_stats = None

        for record_idx in range(dataset.record_count):
            path_id = dataset.record_path_ids[record_idx]
            label = dataset.record_labels[record_idx]
            label_paths = self.records_by_label_path.setdefault(label, {})
            label_paths.setdefault(path_id, array("I")).append(record_idx)

        self.active_labels = sorted(self.records_by_label_path)
        if not self.active_labels:
            raise ValueError("No labels available for grouped sampling.")

    def __iter__(self):
        epoch = self.epoch
        rng = np.random.default_rng(self.seed + epoch)
        self.epoch += 1

        labels = np.asarray(self.active_labels, dtype=np.int64)
        num_groups = math.ceil(self.num_batches / self.batches_per_file_group)
        label_groups = []
        label_group_counts = {int(label): 0 for label in labels}
        for _ in range(num_groups):
            if self.blocks_per_batch <= len(labels):
                choices = rng.choice(
                    labels,
                    size=self.blocks_per_batch,
                    replace=False,
                )
            else:
                choices = np.resize(labels, self.blocks_per_batch)
                rng.shuffle(choices)
            label_groups.append(choices)
            for label in choices:
                label_group_counts[int(label)] += 1

        path_schedules = {}
        path_offsets = {int(label): 0 for label in labels}
        for label, count in label_group_counts.items():
            path_map = self.records_by_label_path[label]
            path_ids = np.asarray(list(path_map), dtype=np.int64)
            weights = np.asarray(
                [len(path_map[int(path_id)]) for path_id in path_ids],
                dtype=np.float64,
            ) ** self.path_weight_power

            if count >= len(path_ids):
                schedule = path_ids.tolist()
                remaining = count - len(path_ids)
                if remaining:
                    positions = (rng.random() + np.arange(remaining)) / remaining
                    cumulative = np.cumsum(weights / weights.sum())
                    indexes = np.searchsorted(cumulative, positions, side="right")
                    schedule.extend(path_ids[indexes].tolist())
                rng.shuffle(schedule)
                path_schedules[label] = schedule
            else:
                probabilities = weights / weights.sum()
                path_schedules[label] = rng.choice(
                    path_ids,
                    size=count,
                    replace=False,
                    p=probabilities,
                ).tolist()

        window_orders = {}
        window_offsets = {}
        seen_records = np.zeros(self.dataset.record_count, dtype=np.bool_)
        seen_paths = set()
        duplicate_draws = 0
        sampled_windows = 0
        path_draw_counts = {}
        distinct_class_counts = []

        def next_windows(label, path_id):
            selected = []
            path_map = self.records_by_label_path[label]
            candidates = np.frombuffer(path_map[path_id], dtype=np.uint32)
            while len(selected) < self.block_size:
                order = window_orders.get(path_id)
                offset = window_offsets.get(path_id, 0)
                if order is None or offset >= len(order):
                    order = rng.permutation(candidates)
                    window_orders[path_id] = order
                    offset = 0
                take = min(self.block_size - len(selected), len(order) - offset)
                selected.extend(order[offset:offset + take].tolist())
                window_offsets[path_id] = offset + take
            return selected

        yielded = 0
        for choices in label_groups:
            group_paths = []
            for raw_label in choices:
                label = int(raw_label)
                offset = path_offsets[label]
                path_id = int(path_schedules[label][offset])
                path_offsets[label] = offset + 1
                group_paths.append((label, path_id))

            for _ in range(self.batches_per_file_group):
                if yielded >= self.num_batches:
                    break
                batch = []
                for label, path_id in group_paths:
                    selected = next_windows(label, path_id)
                    unique_selected = np.unique(selected)
                    duplicate_draws += len(selected) - len(unique_selected)
                    duplicate_draws += int(seen_records[unique_selected].sum())
                    seen_records[unique_selected] = True
                    seen_paths.add(path_id)
                    path_draw_counts[path_id] = (
                        path_draw_counts.get(path_id, 0) + len(selected)
                    )
                    sampled_windows += len(selected)
                    batch.extend(selected)
                if self.shuffle_within_batch:
                    rng.shuffle(batch)
                distinct_class_counts.append(len(set(int(label) for label in choices)))
                yielded += 1
                yield batch

        unique_windows = int(seen_records.sum())
        self.last_epoch_stats = {
            "sampler_epoch": epoch,
            "sampled_windows": sampled_windows,
            "unique_windows": unique_windows,
            "duplicate_draws": duplicate_draws,
            "duplicate_rate": duplicate_draws / max(sampled_windows, 1),
            "paths_seen": len(seen_paths),
            "total_paths": sum(
                len(path_map) for path_map in self.records_by_label_path.values()
            ),
            "max_path_draws": max(path_draw_counts.values(), default=0),
            "min_distinct_classes_per_batch": min(distinct_class_counts, default=0),
            "max_distinct_classes_per_batch": max(distinct_class_counts, default=0),
        }

    def __len__(self):
        return self.num_batches


def create_batch_sampler(
    dataset,
    batch_size,
    blocks_per_batch=2,
    batches_per_file_group=4,
    seed=42,
    num_batches=None,
    path_weight_power=0.5,
):
    if dataset.record_count:
        return GroupedBalancedBatchSampler(
            dataset,
            batch_size=batch_size,
            blocks_per_batch=blocks_per_batch,
            batches_per_file_group=batches_per_file_group,
            seed=seed,
            num_batches=num_batches,
            path_weight_power=path_weight_power,
        )

    # Fallback for directories without manifests. It remains random and
    # class-balanced, but cannot provide the same NPZ locality guarantee.
    class_counts = np.bincount(dataset.labels, minlength=len(CLASS_NAMES))
    class_counts = np.maximum(class_counts, 1)
    class_weights = 1.0 / torch.sqrt(torch.tensor(class_counts, dtype=torch.float))
    sample_weights = class_weights[torch.tensor(dataset.labels, dtype=torch.long)]
    num_samples = (
        min(len(sample_weights), int(num_batches) * batch_size)
        if num_batches is not None and int(num_batches) > 0
        else len(sample_weights)
    )
    sampler = WeightedRandomSampler(sample_weights, num_samples, replacement=True)
    return BatchSampler(sampler, batch_size=batch_size, drop_last=True)


class StratifiedEvalBatchSampler(BatchSampler):
    """Balanced validation sampler used when validation is batch-limited."""

    def __init__(self, dataset, batch_size, max_batches, seed=2026):
        if not dataset.record_count:
            raise ValueError("StratifiedEvalBatchSampler requires manifest records.")
        self.dataset = dataset
        self.batch_size = batch_size
        self.max_batches = max_batches
        self.seed = seed
        self.records_by_label = {}
        for record_idx in range(dataset.record_count):
            label = int(dataset.record_labels[record_idx])
            self.records_by_label.setdefault(label, array("I")).append(record_idx)
        self.active_labels = sorted(self.records_by_label)

    def __iter__(self):
        rng = np.random.default_rng(self.seed)
        total = self.max_batches * self.batch_size
        labels = np.resize(np.asarray(self.active_labels, dtype=np.int64), total)
        rng.shuffle(labels)
        batch = []
        for label in labels:
            candidates = self.records_by_label[int(label)]
            record_idx = candidates[int(rng.integers(len(candidates)))]
            batch.append(record_idx)
            if len(batch) == self.batch_size:
                yield batch
                batch = []

    def __len__(self):
        return self.max_batches


def move_inputs_to_device(inputs):
    if isinstance(inputs, (tuple, list)):
        return tuple(move_inputs_to_device(value) for value in inputs)
    return inputs.to(device, non_blocking=True)


def evaluate(model, loader, max_batches=0):
    model.eval()
    all_preds = []
    all_labels = []
    with torch.no_grad():
        for batch_idx, (inputs, labels) in enumerate(loader):
            if max_batches and batch_idx >= max_batches:
                break
            inputs = move_inputs_to_device(inputs)
            labels = labels.to(device, non_blocking=True)
            with torch.cuda.amp.autocast(enabled=device.type == "cuda"):
                outputs = model(inputs)
            preds = outputs.argmax(dim=1)
            all_preds.extend(preds.cpu().numpy().tolist())
            all_labels.extend(labels.cpu().numpy().tolist())
    acc = accuracy_score(all_labels, all_preds) * 100
    return acc, all_labels, all_preds


def evaluate_with_logits(model, loader, max_batches=0):
    """Evaluate independent windows and retain compact per-window logits."""
    model.eval()
    all_logits = []
    all_labels = []
    with torch.no_grad():
        for batch_idx, (inputs, labels) in enumerate(loader):
            if max_batches and batch_idx >= max_batches:
                break
            inputs = move_inputs_to_device(inputs)
            with torch.cuda.amp.autocast(enabled=device.type == "cuda"):
                outputs = model(inputs)
            all_logits.append(outputs.float().cpu().numpy().astype(np.float16))
            all_labels.append(labels.numpy().astype(np.uint8, copy=False))
    if not all_labels:
        return 0.0, np.empty(0, dtype=np.uint8), np.empty(0, dtype=np.uint8), np.empty(
            (0, len(CLASS_NAMES)), dtype=np.float16
        )
    logits = np.concatenate(all_logits)
    labels = np.concatenate(all_labels)
    predictions = logits.argmax(axis=1).astype(np.uint8)
    accuracy = float((predictions == labels).mean() * 100.0)
    return accuracy, labels, predictions, logits


def save_detailed_predictions(path, dataset, labels, predictions, logits):
    """Save strict single-window outputs with their manifest coordinates."""
    base = unwrap_dataset(dataset)
    if len(labels) != len(base):
        raise ValueError(
            f"Detailed predictions have {len(labels):,} rows, expected {len(base):,}."
        )
    np.savez_compressed(
        path,
        labels=np.asarray(labels, dtype=np.uint8),
        predictions=np.asarray(predictions, dtype=np.uint8),
        logits=np.asarray(logits, dtype=np.float16),
        source_ids=np.asarray(base.record_path_ids, dtype=np.uint32),
        local_indices=np.asarray(base.record_local_idxs, dtype=np.uint32),
        source_paths=np.asarray([str(path) for path in base.paths], dtype=np.str_),
    )


def print_classification_report(val_labels, val_preds):
    """Print a stable 16-class report even when validation lacks classes."""
    if len(val_labels) == 0:
        print("Validation set is empty; classification report skipped.")
        return "", {}

    label_ids = list(range(len(CLASS_NAMES)))
    present = set(val_labels) | set(val_preds)
    missing = [CLASS_NAMES[i] for i in label_ids if i not in present]
    if missing:
        print(f"Validation missing classes: {', '.join(missing)}")

    report_text = classification_report(
        val_labels,
        val_preds,
        labels=label_ids,
        target_names=CLASS_NAMES,
        digits=4,
        zero_division=0,
    )
    report_dict = classification_report(
        val_labels,
        val_preds,
        labels=label_ids,
        target_names=CLASS_NAMES,
        output_dict=True,
        zero_division=0,
    )
    print(report_text)
    return report_text, report_dict


def plot_training_history(history, output_dir):
    if not history:
        return
    epochs = [item["epoch"] for item in history]
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    axes[0, 0].plot(epochs, [item["train_loss"] for item in history], label="Train loss")
    axes[0, 0].set_title("Training Loss")
    axes[0, 1].plot(epochs, [item["train_acc"] for item in history], label="Train accuracy")
    axes[0, 1].plot(epochs, [item["val_acc"] for item in history], label="Validation accuracy")
    axes[0, 1].set_title("Accuracy (%)")
    axes[1, 0].plot(epochs, [item["learning_rate"] for item in history], label="Learning rate")
    axes[1, 0].set_title("Learning Rate")
    axes[1, 1].axis("off")
    for axis in axes.flat[:3]:
        axis.set_xlabel("Epoch")
        axis.grid(alpha=0.25)
        axis.legend()
    fig.tight_layout()
    fig.savefig(Path(output_dir) / "training_curves.png", dpi=160)
    plt.close(fig)


def plot_confusion_matrix_visuals(val_labels, val_preds, output_dir):
    label_ids = list(range(len(CLASS_NAMES)))
    matrix = confusion_matrix(val_labels, val_preds, labels=label_ids)
    np.save(Path(output_dir) / "confusion_matrix.npy", matrix)
    if len(val_labels) == 0:
        return matrix

    row_totals = matrix.sum(axis=1, keepdims=True)
    normalized = np.divide(
        matrix,
        row_totals,
        out=np.zeros_like(matrix, dtype=np.float64),
        where=row_totals != 0,
    )
    fig, axes = plt.subplots(1, 2, figsize=(22, 9))
    for axis, values, title, fmt in (
        (axes[0], matrix, "Confusion Matrix (Count)", "d"),
        (axes[1], normalized, "Confusion Matrix (Row Normalized)", ".2f"),
    ):
        image = axis.imshow(values, cmap="Blues", aspect="auto")
        axis.set_title(title)
        axis.set_xlabel("Predicted label")
        axis.set_ylabel("True label")
        axis.set_xticks(label_ids, CLASS_NAMES, rotation=60, ha="right")
        axis.set_yticks(label_ids, CLASS_NAMES)
        fig.colorbar(image, ax=axis, fraction=0.046, pad=0.04)
        threshold = values.max() * 0.6 if values.size else 0
        for row in range(len(CLASS_NAMES)):
            for col in range(len(CLASS_NAMES)):
                text = format(values[row, col], fmt)
                axis.text(
                    col,
                    row,
                    text,
                    ha="center",
                    va="center",
                    color="white" if values[row, col] > threshold else "black",
                    fontsize=7,
                )
    fig.tight_layout()
    fig.savefig(Path(output_dir) / "confusion_matrix.png", dpi=160)
    plt.close(fig)
    return matrix


def compute_confusion_diagnostics(val_labels, val_preds, top_k=5):
    label_ids = list(range(len(CLASS_NAMES)))
    matrix = confusion_matrix(val_labels, val_preds, labels=label_ids)
    row_totals = matrix.sum(axis=1)
    col_totals = matrix.sum(axis=0)
    diag = np.diag(matrix).astype(np.float64)

    precision = np.divide(
        diag,
        col_totals,
        out=np.zeros_like(diag, dtype=np.float64),
        where=col_totals != 0,
    )
    recall = np.divide(
        diag,
        row_totals,
        out=np.zeros_like(diag, dtype=np.float64),
        where=row_totals != 0,
    )
    f1 = np.divide(
        2 * precision * recall,
        precision + recall,
        out=np.zeros_like(diag, dtype=np.float64),
        where=(precision + recall) != 0,
    )

    per_class = {}
    confusion_pairs = []
    for true_idx, class_name in enumerate(CLASS_NAMES):
        off_diag = matrix[true_idx].copy()
        off_diag[true_idx] = 0
        top_indices = off_diag.argsort()[-top_k:][::-1]
        per_class[class_name] = {
            "support": int(row_totals[true_idx]),
            "precision": float(precision[true_idx]),
            "recall": float(recall[true_idx]),
            "f1": float(f1[true_idx]),
            "top_confusions": [
                {
                    "predicted": CLASS_NAMES[pred_idx],
                    "count": int(off_diag[pred_idx]),
                    "rate": float(off_diag[pred_idx] / row_totals[true_idx])
                    if row_totals[true_idx] else 0.0,
                }
                for pred_idx in top_indices
                if off_diag[pred_idx] > 0
            ],
        }
        for pred_idx, count in enumerate(off_diag):
            if count > 0:
                confusion_pairs.append(
                    {
                        "true": class_name,
                        "predicted": CLASS_NAMES[pred_idx],
                        "count": int(count),
                        "rate": float(count / row_totals[true_idx])
                        if row_totals[true_idx] else 0.0,
                    }
                )

    same_model_pairs = {}
    for true_idx in range(0, len(CLASS_NAMES), 2):
        pred_idx = true_idx + 1
        left = CLASS_NAMES[true_idx]
        right = CLASS_NAMES[pred_idx]
        same_model_pairs[f"{left}->{right}"] = {
            "count": int(matrix[true_idx, pred_idx]),
            "rate": float(matrix[true_idx, pred_idx] / row_totals[true_idx])
            if row_totals[true_idx] else 0.0,
        }
        same_model_pairs[f"{right}->{left}"] = {
            "count": int(matrix[pred_idx, true_idx]),
            "rate": float(matrix[pred_idx, true_idx] / row_totals[pred_idx])
            if row_totals[pred_idx] else 0.0,
        }

    confusion_pairs.sort(key=lambda item: item["count"], reverse=True)
    return {
        "macro_f1": float(f1.mean()),
        "min_class_f1": float(f1.min()) if len(f1) else 0.0,
        "worst_classes": sorted(
            (
                {"class": CLASS_NAMES[idx], "f1": float(f1[idx])}
                for idx in range(len(CLASS_NAMES))
            ),
            key=lambda item: item["f1"],
        )[:5],
        "per_class": per_class,
        "major_confusion_pairs": confusion_pairs[:top_k],
        "same_model_pairs": same_model_pairs,
    }


def init_trackio_if_requested(args, config):
    if not args.trackio:
        return None
    try:
        import trackio
    except ImportError:
        print(
            "Trackio requested but not installed. Install with: "
            "python -m pip install --cache-dir /root/autodl-tmp/pip_cache trackio",
            flush=True,
        )
        return None

    init_kwargs = {
        "project": args.trackio_project,
        "name": args.trackio_run_name or None,
        "group": args.trackio_group or None,
        "config": config,
    }
    if args.trackio_space_id:
        init_kwargs["space_id"] = args.trackio_space_id
    init_kwargs = {key: value for key, value in init_kwargs.items() if value is not None}
    trackio.init(**init_kwargs)
    return trackio


def log_trackio_epoch(trackio, epoch_record):
    if trackio is None:
        return
    metrics = {
        "epoch": epoch_record["epoch"],
        "train_loss": epoch_record["train_loss"],
        "train_acc": epoch_record["train_acc"],
        "val_acc": epoch_record["val_acc"],
        "selection_val_acc": epoch_record.get("selection_val_acc", epoch_record["val_acc"]),
        "best_val_acc": epoch_record.get("best_val_acc", 0.0),
        "val_drop_from_best": epoch_record.get("val_drop_from_best", 0.0),
        "consecutive_val_drop_epochs": epoch_record.get("consecutive_val_drop_epochs", 0),
        "learning_rate": epoch_record["learning_rate"],
        "train_val_gap": epoch_record["train_val_gap"],
        "macro_f1": epoch_record.get("macro_f1", 0.0),
        "min_class_f1": epoch_record.get("min_class_f1", 0.0),
    }
    metrics = {key: value for key, value in metrics.items() if value is not None}
    for class_name, values in epoch_record.get("per_class", {}).items():
        metrics[f"per_class_f1/{class_name}"] = values["f1"]
        metrics[f"per_class_recall/{class_name}"] = values["recall"]
    for pair_name, values in epoch_record.get("same_model_pairs", {}).items():
        metrics[f"same_model_confusion/{pair_name}"] = values["rate"]
    trackio.log(metrics)


def fire_training_alerts(
    trackio,
    epoch_record,
    epochs_without_improvement,
    val_drop_patience=3,
    val_drop_delta=1.0,
    collapse_confusion_rate=0.30,
    same_model_confusion_rate=0.15,
):
    if trackio is None:
        return
    gap = epoch_record["train_val_gap"]
    if gap > 45.0 and epoch_record["epoch"] >= 2:
        trackio.alert(
            title="Large train/val gap",
            text=f"Gap is {gap:.2f}% at epoch {epoch_record['epoch']}; likely overfitting or domain gap.",
            level=trackio.AlertLevel.WARN,
        )
    if epoch_record.get("min_class_f1", 1.0) < 0.20:
        worst = epoch_record.get("worst_classes", [])[:3]
        trackio.alert(
            title="Low per-class F1",
            text=f"Worst classes: {json.dumps(worst, ensure_ascii=False)}",
            level=trackio.AlertLevel.WARN,
        )
    if epochs_without_improvement >= 5:
        trackio.alert(
            title="Validation plateau",
            text=f"Validation accuracy has not improved for {epochs_without_improvement} epochs.",
            level=trackio.AlertLevel.WARN,
        )
    drop_epochs = epoch_record.get("consecutive_val_drop_epochs", 0)
    drop_from_best = epoch_record.get("val_drop_from_best", 0.0)
    if val_drop_patience > 0 and drop_epochs >= val_drop_patience:
        trackio.alert(
            title="Validation regression",
            text=(
                f"Validation has stayed at least {val_drop_delta:.2f}% below best "
                f"for {drop_epochs} full evals. Current drop: {drop_from_best:.2f}%. "
                "Consider stopping this run and lowering lr/margin or increasing regularization."
            ),
            level=trackio.AlertLevel.WARN,
        )
    top_confusions = epoch_record.get("major_confusion_pairs", [])
    if top_confusions and top_confusions[0].get("rate", 0.0) >= collapse_confusion_rate:
        item = top_confusions[0]
        trackio.alert(
            title="Severe validation confusion",
            text=(
                f"{item['true']} is predicted as {item['predicted']} for "
                f"{100 * item['rate']:.1f}% of its validation samples "
                f"(count={item['count']}). This can indicate class-boundary collapse "
                "or source-domain bias."
            ),
            level=trackio.AlertLevel.WARN,
        )
    same_model_pairs = epoch_record.get("same_model_pairs", {})
    if same_model_pairs:
        worst_pair, worst_values = max(
            same_model_pairs.items(),
            key=lambda item: item[1].get("rate", 0.0),
        )
        if worst_values.get("rate", 0.0) >= same_model_confusion_rate:
            trackio.alert(
                title="High same-model individual confusion",
                text=(
                    f"{worst_pair} confusion reached "
                    f"{100 * worst_values['rate']:.1f}% "
                    f"(count={worst_values['count']}). "
                    "The model may be learning model-level features more than individual fingerprints."
                ),
                level=trackio.AlertLevel.WARN,
            )


def train_model(
    model,
    train_loader,
    monitor_val_loader,
    selection_val_loader,
    output_dir,
    num_epochs=80,
    steps_per_epoch=0,
    max_val_batches=0,
    eval_every=1,
    best_full_eval_every=5,
    selection_metric="accuracy",
    resume_checkpoint="",
    init_checkpoint="",
    lr=3e-4,
    weight_decay=5e-4,
    label_smoothing=0.05,
    supcon_weight=0.10,
    supcon_temperature=0.10,
    family_loss_weight=0.0,
    individual_loss_weight=0.0,
    source_pooled_loss_weight=0.0,
    source_consistency_weight=0.0,
    lr_scheduler="onecycle",
    trackio_val_drop_patience=3,
    trackio_val_drop_delta=1.0,
    trackio_collapse_confusion_rate=0.30,
    trackio_same_model_confusion_rate=0.15,
    trackio_run=None,
    json_events=False,
):
    model = model.to(device)
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    if init_checkpoint:
        init_path = Path(init_checkpoint)
        if not init_path.exists():
            raise FileNotFoundError(f"Init checkpoint not found: {init_path}")
        init_state = torch.load(
            init_path,
            map_location=device,
            weights_only=False,
        )
        if isinstance(init_state, dict) and "model_state_dict" in init_state:
            init_state = init_state["model_state_dict"]
        adapted_hierarchical_head = load_initial_model_state(model, init_state)
        suffix = (
            " with a zero-initialized hierarchical residual head"
            if adapted_hierarchical_head
            else ""
        )
        print(
            f"Initialized model weights from {init_path}{suffix}.",
            flush=True,
        )

    if lr_scheduler not in {"onecycle", "constant", "cosine"}:
        raise ValueError(f"Unsupported lr_scheduler: {lr_scheduler}")
    initial_lr = lr / 10.0 if lr_scheduler == "onecycle" else lr
    trainable_parameters = [p for p in model.parameters() if p.requires_grad]
    if not trainable_parameters:
        raise ValueError("Model has no trainable parameters.")
    optimizer = optim.AdamW(
        trainable_parameters,
        lr=initial_lr,
        weight_decay=weight_decay,
    )
    # The train batch sampler already balances classes. Applying another class
    # weight here would over-correct minority classes.
    criterion = nn.CrossEntropyLoss(label_smoothing=label_smoothing)
    supcon_criterion = SupConLoss(temperature=supcon_temperature)
    effective_steps_per_epoch = steps_per_epoch if steps_per_epoch > 0 else len(train_loader)
    effective_steps_per_epoch = min(effective_steps_per_epoch, len(train_loader))
    total_training_steps = effective_steps_per_epoch * num_epochs
    if lr_scheduler == "onecycle":
        scheduler = optim.lr_scheduler.OneCycleLR(
            optimizer,
            max_lr=lr,
            steps_per_epoch=effective_steps_per_epoch,
            epochs=num_epochs,
            pct_start=0.2,
        )
    elif lr_scheduler == "cosine":
        scheduler = optim.lr_scheduler.CosineAnnealingLR(
            optimizer,
            T_max=total_training_steps,
            eta_min=lr * 0.1,
        )
    else:
        scheduler = optim.lr_scheduler.LambdaLR(
            optimizer,
            lr_lambda=lambda _step: 1.0,
        )
    scaler = torch.cuda.amp.GradScaler(enabled=device.type == "cuda")

    best_acc = 0.0
    best_score = None
    best_epoch = 0
    epochs_without_improvement = 0
    consecutive_val_drop_epochs = 0
    history = []
    start_epoch = 0
    if resume_checkpoint:
        checkpoint_path = Path(resume_checkpoint)
        if not checkpoint_path.exists():
            raise FileNotFoundError(f"Resume checkpoint not found: {checkpoint_path}")
        checkpoint = torch.load(
            checkpoint_path,
            map_location=device,
            weights_only=False,
        )
        required = {
            "model_state_dict",
            "optimizer_state_dict",
            "scheduler_state_dict",
            "scaler_state_dict",
            "epoch",
        }
        missing = sorted(required - set(checkpoint))
        if missing:
            raise ValueError(
                f"Resume checkpoint is missing training state: {missing}. "
                "Use last_checkpoint.pth, not an inference-only model file."
            )
        if checkpoint.get("num_epochs", num_epochs) != num_epochs:
            raise ValueError(
                "Resume requires the same --epochs used by the interrupted run: "
                f"checkpoint={checkpoint.get('num_epochs')}, requested={num_epochs}."
            )
        if checkpoint.get("steps_per_epoch", effective_steps_per_epoch) != effective_steps_per_epoch:
            raise ValueError(
                "Resume requires the same effective steps per epoch: "
                f"checkpoint={checkpoint.get('steps_per_epoch')}, "
                f"requested={effective_steps_per_epoch}."
            )
        if checkpoint.get("selection_metric", selection_metric) != selection_metric:
            raise ValueError(
                "Resume requires the same --selection-metric: "
                f"checkpoint={checkpoint.get('selection_metric')}, "
                f"requested={selection_metric}."
            )
        checkpoint_scheduler = checkpoint.get("lr_scheduler", "onecycle")
        if checkpoint_scheduler != lr_scheduler:
            raise ValueError(
                "Resume requires the same --lr-scheduler: "
                f"checkpoint={checkpoint_scheduler}, requested={lr_scheduler}."
            )
        model.load_state_dict(checkpoint["model_state_dict"])
        optimizer.load_state_dict(checkpoint["optimizer_state_dict"])
        scheduler.load_state_dict(checkpoint["scheduler_state_dict"])
        scaler.load_state_dict(checkpoint["scaler_state_dict"])
        start_epoch = int(checkpoint["epoch"])
        best_acc = float(checkpoint.get("best_val_acc", 0.0))
        restored_best_score = checkpoint.get("best_selection_score")
        best_score = (
            float(restored_best_score)
            if restored_best_score is not None
            else (best_acc if selection_metric == "accuracy" else None)
        )
        best_epoch = int(checkpoint.get("best_epoch", 0))
        epochs_without_improvement = int(
            checkpoint.get("epochs_without_improvement", 0)
        )
        consecutive_val_drop_epochs = int(
            checkpoint.get("consecutive_val_drop_epochs", 0)
        )
        history = list(checkpoint.get("history", []))
        if "torch_rng_state" in checkpoint:
            torch.set_rng_state(checkpoint["torch_rng_state"].cpu())
        if torch.cuda.is_available() and checkpoint.get("cuda_rng_state_all") is not None:
            torch.cuda.set_rng_state_all(
                [state.cpu() for state in checkpoint["cuda_rng_state_all"]]
            )
        if "numpy_rng_state" in checkpoint:
            np.random.set_state(checkpoint["numpy_rng_state"])
        sampler_epoch = checkpoint.get("train_sampler_epoch")
        if sampler_epoch is not None and hasattr(train_loader.batch_sampler, "epoch"):
            train_loader.batch_sampler.epoch = int(sampler_epoch)
        if start_epoch >= num_epochs:
            raise ValueError(
                f"Checkpoint already completed epoch {start_epoch} of {num_epochs}."
            )
        print(
            f"Resumed {checkpoint_path} after epoch {start_epoch}; "
            f"best {selection_metric}={best_score} at epoch {best_epoch}.",
            flush=True,
        )

    if init_checkpoint and selection_val_loader is not None:
        initial_acc, initial_labels, initial_preds = evaluate(
            model,
            selection_val_loader,
            max_batches=0,
        )
        initial_diagnostics = compute_confusion_diagnostics(
            initial_labels,
            initial_preds,
        )
        best_acc = initial_acc
        best_score = (
            initial_acc
            if selection_metric == "accuracy"
            else 100.0 * initial_diagnostics["macro_f1"]
        )
        best_epoch = 0
        atomic_torch_save(model.state_dict(), output_dir / "best_model.pth")
        atomic_torch_save(
            {
                "epoch": 0,
                "best_val_acc": best_acc,
                "best_selection_score": best_score,
                "selection_metric": selection_metric,
                "validation_scope": "full",
                "class_names": CLASS_NAMES,
                "model_state_dict": model.state_dict(),
            },
            output_dir / "best_checkpoint.pth",
        )
        atomic_write_text(
            output_dir / "initial_validation.json",
            json.dumps(
                {
                    "full_val_acc": initial_acc,
                    "selection_metric": selection_metric,
                    "selection_score": best_score,
                    "macro_f1": initial_diagnostics["macro_f1"],
                    "min_class_f1": initial_diagnostics["min_class_f1"],
                },
                ensure_ascii=False,
                indent=2,
            ),
        )
        print(
            f"Initial checkpoint full validation | Acc: {initial_acc:.2f}% | "
            f"Macro F1: {initial_diagnostics['macro_f1']:.4f}",
            flush=True,
        )

    for epoch in range(start_epoch, num_epochs):
        raise_if_cancelled()
        model.train()
        train_loss = 0.0
        train_ce_loss = 0.0
        train_supcon_loss = 0.0
        train_family_loss = 0.0
        train_individual_loss = 0.0
        train_source_pooled_loss = 0.0
        train_source_consistency_loss = 0.0
        correct = 0
        total = 0

        with tqdm(
            train_loader,
            desc=f"Epoch {epoch + 1}/{num_epochs}",
            unit="batch",
            total=effective_steps_per_epoch,
            mininterval=2.0,
        ) as pbar:
            for batch_idx, batch in enumerate(pbar):
                raise_if_cancelled()
                if batch_idx >= effective_steps_per_epoch:
                    break
                if len(batch) == 3:
                    inputs, labels, source_ids = batch
                    source_ids = source_ids.to(device, non_blocking=True)
                else:
                    inputs, labels = batch
                    source_ids = None
                inputs = move_inputs_to_device(inputs)
                labels = labels.to(device, non_blocking=True)
                optimizer.zero_grad(set_to_none=True)

                with torch.cuda.amp.autocast(enabled=device.type == "cuda"):
                    outputs, embeddings = model(inputs, labels=labels, return_embedding=True)
                    ce_loss = criterion(outputs, labels)
                    if supcon_weight > 0:
                        supcon_loss = supcon_criterion(embeddings, labels)
                    else:
                        supcon_loss = embeddings.new_tensor(0.0)
                    if family_loss_weight > 0 or individual_loss_weight > 0:
                        family_loss, individual_loss = paired_class_auxiliary_losses(
                            outputs,
                            labels,
                            criterion,
                        )
                    else:
                        family_loss = embeddings.new_tensor(0.0)
                        individual_loss = embeddings.new_tensor(0.0)
                    if source_pooled_loss_weight > 0 or source_consistency_weight > 0:
                        if source_ids is None:
                            raise ValueError(
                                "Source auxiliary losses require source ids in each batch."
                            )
                        source_pooled_loss, source_consistency_loss = (
                            source_pooled_auxiliary_losses(
                                outputs,
                                labels,
                                source_ids,
                                criterion,
                            )
                        )
                    else:
                        source_pooled_loss = embeddings.new_tensor(0.0)
                        source_consistency_loss = embeddings.new_tensor(0.0)
                    loss = (
                        ce_loss
                        + supcon_weight * supcon_loss
                        + family_loss_weight * family_loss
                        + individual_loss_weight * individual_loss
                        + source_pooled_loss_weight * source_pooled_loss
                        + source_consistency_weight * source_consistency_loss
                    )
                if not torch.isfinite(loss):
                    if trackio_run is not None:
                        trackio_run.alert(
                            title="Non-finite loss",
                            text=f"Loss became {loss.item()} at epoch {epoch + 1}, batch {batch_idx}.",
                            level=trackio_run.AlertLevel.ERROR,
                        )
                    raise RuntimeError(
                        f"Non-finite loss at epoch {epoch + 1}, batch {batch_idx}: {loss.item()}"
                    )

                scaler.scale(loss).backward()
                scaler.unscale_(optimizer)
                nn.utils.clip_grad_norm_(model.parameters(), 1.0)
                scaler.step(optimizer)
                scaler.update()
                scheduler.step()

                train_loss += loss.item()
                train_ce_loss += ce_loss.item()
                train_supcon_loss += supcon_loss.item()
                train_family_loss += family_loss.item()
                train_individual_loss += individual_loss.item()
                train_source_pooled_loss += source_pooled_loss.item()
                train_source_consistency_loss += source_consistency_loss.item()
                predicted = outputs.argmax(dim=1)
                total += labels.size(0)
                correct += predicted.eq(labels).sum().item()
                pbar.set_postfix(
                    loss=f"{loss.item():.4f}",
                    ce=f"{ce_loss.item():.4f}",
                    sc=f"{supcon_loss.item():.4f}",
                    fam=f"{family_loss.item():.4f}",
                    ind=f"{individual_loss.item():.4f}",
                    src=f"{source_pooled_loss.item():.4f}",
                    con=f"{source_consistency_loss.item():.4f}",
                    acc=f"{100 * correct / total:.2f}%",
                )

        should_monitor_eval = ((epoch + 1) % eval_every == 0) or (epoch + 1 == num_epochs)
        if should_monitor_eval:
            monitor_val_acc, monitor_val_labels, monitor_val_preds = evaluate(
                model,
                monitor_val_loader,
                max_batches=max_val_batches,
            )
        else:
            monitor_val_acc, monitor_val_labels, monitor_val_preds = best_acc, [], []

        should_full_eval = (
            selection_val_loader is not None
            and (
                best_full_eval_every <= 1
                or (epoch + 1) % best_full_eval_every == 0
                or (epoch + 1 == num_epochs)
            )
        )
        if should_full_eval:
            full_val_acc, full_val_labels, full_val_preds = evaluate(
                model,
                selection_val_loader,
                max_batches=0,
            )
        else:
            full_val_acc, full_val_labels, full_val_preds = None, [], []

        train_acc = 100 * correct / max(total, 1)
        current_lr = optimizer.param_groups[0]["lr"]
        diagnostics = {}
        if should_full_eval:
            diagnostics = compute_confusion_diagnostics(full_val_labels, full_val_preds)
        full_selection_score = None
        if should_full_eval and full_val_acc is not None:
            full_selection_score = (
                full_val_acc
                if selection_metric == "accuracy"
                else 100.0 * diagnostics["macro_f1"]
            )
        epoch_record = {
            "epoch": epoch + 1,
            "train_loss": train_loss / max(effective_steps_per_epoch, 1),
            "train_ce_loss": train_ce_loss / max(effective_steps_per_epoch, 1),
            "train_supcon_loss": train_supcon_loss / max(effective_steps_per_epoch, 1),
            "train_family_loss": train_family_loss / max(effective_steps_per_epoch, 1),
            "train_individual_loss": train_individual_loss / max(effective_steps_per_epoch, 1),
            "train_source_pooled_loss": train_source_pooled_loss / max(effective_steps_per_epoch, 1),
            "train_source_consistency_loss": train_source_consistency_loss / max(effective_steps_per_epoch, 1),
            "train_acc": train_acc,
            "val_acc": monitor_val_acc,
            "full_val_acc": full_val_acc if full_val_acc is not None else None,
            "validation_scope": "full" if should_full_eval else "monitor_sample",
            "selection_val_acc": full_val_acc,
            "selection_metric": selection_metric,
            "selection_score": full_selection_score,
            "learning_rate": current_lr,
            "train_val_gap": (
                train_acc - full_val_acc if full_val_acc is not None else None
            ),
            "macro_f1": diagnostics.get("macro_f1", 0.0),
            "min_class_f1": diagnostics.get("min_class_f1", 0.0),
            "worst_classes": diagnostics.get("worst_classes", []),
            "major_confusion_pairs": diagnostics.get("major_confusion_pairs", []),
            "same_model_pairs": diagnostics.get("same_model_pairs", {}),
            "per_class": diagnostics.get("per_class", {}),
            "sampling": getattr(
                train_loader.batch_sampler,
                "last_epoch_stats",
                None,
            ),
        }
        if (
            should_full_eval
            and full_val_acc is not None
            and (best_score is None or full_selection_score > best_score)
        ):
            best_score = full_selection_score
            best_acc = full_val_acc
            best_epoch = epoch + 1
            epochs_without_improvement = 0
            consecutive_val_drop_epochs = 0
            atomic_torch_save(model.state_dict(), output_dir / "best_model.pth")
            atomic_torch_save(
                {
                    "epoch": best_epoch,
                    "best_val_acc": best_acc,
                    "best_selection_score": best_score,
                    "selection_metric": selection_metric,
                    "validation_scope": "full",
                    "class_names": CLASS_NAMES,
                    "model_state_dict": model.state_dict(),
                },
                output_dir / "best_checkpoint.pth",
            )
        elif should_full_eval:
            epochs_without_improvement += 1
            if best_acc > 0 and full_val_acc is not None and full_val_acc <= best_acc - trackio_val_drop_delta:
                consecutive_val_drop_epochs += 1
            else:
                consecutive_val_drop_epochs = 0
        epoch_record["best_val_acc"] = best_acc
        epoch_record["best_selection_score"] = best_score
        epoch_record["best_epoch"] = best_epoch
        epoch_record["val_drop_from_best"] = (
            max(0.0, best_acc - full_val_acc)
            if full_val_acc is not None
            else None
        )
        epoch_record["consecutive_val_drop_epochs"] = consecutive_val_drop_epochs
        history.append(epoch_record)
        if json_events:
            emit_json_event("epoch", **epoch_record)

        full_acc_text = ""
        if full_val_acc is not None:
            full_acc_text = f"Full Val Acc: {full_val_acc:.2f}% | "
        print(
            f"Epoch {epoch + 1}/{num_epochs} summary | "
            f"Train Loss: {train_loss / max(effective_steps_per_epoch, 1):.4f} | "
            f"Train Acc: {train_acc:.2f}% | "
            f"Val Acc: {monitor_val_acc:.2f}% | "
            f"{full_acc_text}"
            f"Best Acc: {best_acc:.2f}% @ epoch {best_epoch}",
            flush=True,
        )
        sampling_stats = epoch_record.get("sampling")
        if sampling_stats:
            print(
                "Sampling | "
                f"Unique: {sampling_stats['unique_windows']:,}/"
                f"{sampling_stats['sampled_windows']:,} | "
                f"Duplicates: {100 * sampling_stats['duplicate_rate']:.2f}% | "
                f"NPZ coverage: {sampling_stats['paths_seen']}/"
                f"{sampling_stats['total_paths']} | "
                f"Classes/batch: {sampling_stats['min_distinct_classes_per_batch']}-"
                f"{sampling_stats['max_distinct_classes_per_batch']}",
                flush=True,
            )
        if should_full_eval:
            print(confusion_matrix(full_val_labels, full_val_preds, labels=list(range(len(CLASS_NAMES)))))
            print(
                "Diagnostics | "
                f"Train-Val Gap: {epoch_record['train_val_gap']:.2f}% | "
                f"Macro F1: {epoch_record['macro_f1']:.4f} | "
                f"Min F1: {epoch_record['min_class_f1']:.4f}",
                flush=True,
            )
            print(
                "Worst classes: "
                + json.dumps(epoch_record["worst_classes"], ensure_ascii=False),
                flush=True,
            )
            print(
                "Top confusions: "
                + json.dumps(epoch_record["major_confusion_pairs"], ensure_ascii=False),
                flush=True,
            )
        elif should_monitor_eval:
            print(f"Full validation skipped this epoch; best_full_eval_every={best_full_eval_every}.", flush=True)
        else:
            print(f"Validation skipped this epoch; eval_every={eval_every}.", flush=True)
        print("-" * 60, flush=True)
        log_trackio_epoch(trackio_run, epoch_record)
        if should_full_eval:
            fire_training_alerts(
                trackio_run,
                epoch_record,
                epochs_without_improvement,
                val_drop_patience=trackio_val_drop_patience,
                val_drop_delta=trackio_val_drop_delta,
                collapse_confusion_rate=trackio_collapse_confusion_rate,
                same_model_confusion_rate=trackio_same_model_confusion_rate,
            )

        atomic_write_text(
            output_dir / "history.json",
            json.dumps(history, ensure_ascii=False, indent=2),
        )
        atomic_torch_save(
            {
                "epoch": epoch + 1,
                "num_epochs": num_epochs,
                "steps_per_epoch": effective_steps_per_epoch,
                "selection_metric": selection_metric,
                "lr_scheduler": lr_scheduler,
                "best_selection_score": best_score,
                "best_val_acc": best_acc,
                "best_epoch": best_epoch,
                "epochs_without_improvement": epochs_without_improvement,
                "consecutive_val_drop_epochs": consecutive_val_drop_epochs,
                "class_names": CLASS_NAMES,
                "model_state_dict": model.state_dict(),
                "optimizer_state_dict": optimizer.state_dict(),
                "scheduler_state_dict": scheduler.state_dict(),
                "scaler_state_dict": scaler.state_dict(),
                "history": history,
                "torch_rng_state": torch.get_rng_state(),
                "cuda_rng_state_all": (
                    torch.cuda.get_rng_state_all() if torch.cuda.is_available() else None
                ),
                "numpy_rng_state": np.random.get_state(),
                "train_sampler_epoch": getattr(
                    train_loader.batch_sampler,
                    "epoch",
                    None,
                ),
            },
            output_dir / "last_checkpoint.pth",
        )

    plot_training_history(history, output_dir)
    atomic_torch_save(model.state_dict(), output_dir / "final_model.pth")
    return model, best_acc


def parse_args():
    parser = argparse.ArgumentParser(description="Train CNNTRANS_v5 on 3943B npz IQ dataset.")
    parser.add_argument(
        "--data-root",
        default="/root/autodl-tmp/3943B_58g_dataset",
    )
    parser.add_argument(
        "--output-dir",
        default="/root/autodl-tmp/cnntrans_v5_output",
    )
    parser.add_argument(
        "--seed",
        type=int,
        default=42,
        help="Random seed for torch, CUDA, NumPy, and samplers.",
    )
    parser.add_argument(
        "--cache-dir",
        default="",
        help="Directory containing precomputed fp16 spectrogram .npy files.",
    )
    parser.add_argument(
        "--cache-data-root",
        default="",
        help=(
            "Data root used when mapping NPZ paths to --cache-dir. "
            "Use this when a derived manifest root references NPZ files from "
            "the original dataset root."
        ),
    )
    parser.add_argument(
        "--train-exclude-sources",
        default="",
        help=(
            "CSV/JSON list of source_file or npz_path entries to exclude only "
            "from the train split. Internal-dev and validation split planning "
            "remain based on the full manifest."
        ),
    )
    parser.add_argument(
        "--stitch-windows",
        type=int,
        default=1,
        help=(
            "Concatenate this many consecutive 4096-sample windows from the same "
            "NPZ/segment at load time. Use 8 for 32768-sample long-window trials."
        ),
    )
    parser.add_argument(
        "--stitch-stride",
        type=int,
        default=1,
        help="Stride, in original 4096-sample windows, between stitched examples.",
    )
    parser.add_argument(
        "--secondary-cache-dir",
        default="",
        help=(
            "Aligned complex-signed cache used only by "
            "--model-arch dual_axis_profile_large."
        ),
    )
    parser.add_argument(
        "--feature-mode",
        choices=FEATURE_MODE_CHOICES,
        default="legacy",
        help=(
            "Spectrogram frontend/cache contract. legacy keeps the original "
            "separate I/Q one-sided STFT; complex_signed_mag keeps negative and "
            "positive complex-IQ spectrum halves."
        ),
    )
    parser.add_argument(
        "--train-data-mode",
        choices=("raw", "cache"),
        default="raw",
        help="Use original IQ windows or an explicitly selected precomputed cache.",
    )
    parser.add_argument(
        "--index-cache-dir",
        default="",
        help="Directory for fingerprinted mmap manifest indexes.",
    )
    parser.add_argument(
        "--rf-stats-cache-dir",
        default="",
        help="Optional compact per-window RF statistics cache to fuse with legacy STFT.",
    )
    parser.add_argument(
        "--rf-stats-normalization",
        default="",
        help="Training-only mean/std JSON for --rf-stats-cache-dir.",
    )
    parser.add_argument(
        "--rf-stats-fusion",
        choices=("concat", "gated_residual"),
        default="concat",
        help=(
            "How compact RF statistics modify the spectrogram embedding. "
            "gated_residual starts from a zero stats contribution."
        ),
    )
    parser.add_argument(
        "--rf-stats-input-mode",
        choices=("real", "zeros"),
        default="real",
        help=(
            "Use real compact statistics, or zeros as a parameter-matched "
            "information control."
        ),
    )
    parser.add_argument(
        "--precompute-cache",
        action="store_true",
        help="Build the spectrogram cache under --cache-dir, then exit.",
    )
    parser.add_argument(
        "--raw-iq-cache-dir",
        default="",
        help="Float16 raw-IQ memmap cache used by phase_aware_large.",
    )
    parser.add_argument(
        "--precompute-phase-raw-iq-cache",
        action="store_true",
        help="Build only train/internal-dev raw-IQ memmaps, then exit.",
    )
    parser.add_argument(
        "--precompute-batch-size",
        type=int,
        default=4096,
        help="Raw IQ windows per GPU batch during cache precomputation.",
    )
    parser.add_argument(
        "--overwrite-cache",
        action="store_true",
        help="Overwrite existing precomputed cache files.",
    )
    parser.add_argument(
        "--require-cuda",
        action="store_true",
        help="Fail immediately instead of silently falling back to CPU.",
    )
    parser.add_argument("--epochs", type=int, default=10)
    parser.add_argument("--batch-size", type=int, default=64)
    parser.add_argument(
        "--lr",
        type=float,
        default=3e-4,
        help="Learning rate, or peak learning rate when using OneCycleLR.",
    )
    parser.add_argument(
        "--lr-scheduler",
        choices=("onecycle", "constant", "cosine"),
        default="onecycle",
        help="Per-step learning-rate schedule.",
    )
    parser.add_argument(
        "--weight-decay",
        type=float,
        default=5e-4,
        help="AdamW weight decay. Increase when train/val gap is large.",
    )
    parser.add_argument(
        "--label-smoothing",
        type=float,
        default=0.05,
        help="CrossEntropy label smoothing for noisy or difficult validation splits.",
    )
    parser.add_argument(
        "--dropout",
        type=float,
        default=0.6,
        help="Dropout probability in the classifier head.",
    )
    parser.add_argument(
        "--model-arch",
        choices=(
            "light",
            "wide_light",
            "axis_aware",
            "axis_profile_large",
            "axis_profile_xlarge",
            "axis_profile_xlarge_multiscale",
            "axis_profile_xlarge_multiscale_gated",
            "dual_axis_profile_large",
            "phase_aware_large",
            "axis_grid_large",
            "medium",
            "base",
        ),
        default="base",
        help=(
            "Model backbone. wide_light is a parameter-control for light; "
            "axis variants retain ordered frequency/time structure; xlarge is "
            "the parameter-matched dual-view capacity control; multiscale adds "
            "coarse/fine temporal profiles inside one model; the dual "
            "variant fuses legacy and signed-complex views inside one model; "
            "phase_aware_large consumes raw I/Q and adds invariant phase tracks."
        ),
    )
    parser.add_argument(
        "--freeze-gated-multiscale-base",
        action="store_true",
        help=(
            "For axis_profile_xlarge_multiscale_gated, freeze the initialized "
            "stitch8 base path and train only the zero-gated multiscale branch."
        ),
    )
    parser.add_argument(
        "--classifier-head",
        choices=("ce", "hierarchical", "arcface", "cosface"),
        default="arcface",
        help=(
            "Classification head. hierarchical factors each class into an "
            "8-way family and a family-conditional individual decision."
        ),
    )
    parser.add_argument(
        "--embedding-dim",
        type=int,
        default=512,
        help="Embedding dimension before the classifier or margin head.",
    )
    parser.add_argument(
        "--margin-scale",
        type=float,
        default=30.0,
        help="Logit scale for ArcFace/CosFace heads.",
    )
    parser.add_argument(
        "--arc-margin",
        type=float,
        default=0.30,
        help="Angular margin in radians for ArcFace.",
    )
    parser.add_argument(
        "--cos-margin",
        type=float,
        default=0.20,
        help="Cosine margin for CosFace.",
    )
    parser.add_argument(
        "--supcon-weight",
        type=float,
        default=0.10,
        help="Weight for supervised contrastive auxiliary loss. Use 0 to disable.",
    )
    parser.add_argument(
        "--supcon-temperature",
        type=float,
        default=0.10,
        help="Temperature for supervised contrastive loss.",
    )
    parser.add_argument(
        "--family-loss-weight",
        type=float,
        default=0.0,
        help="Weight for the auxiliary 8-family classification loss.",
    )
    parser.add_argument(
        "--individual-loss-weight",
        type=float,
        default=0.0,
        help="Weight for the auxiliary 01/02 loss within the true family.",
    )
    parser.add_argument(
        "--source-pooled-loss-weight",
        type=float,
        default=0.0,
        help="Training-only CE weight for logits averaged within each source block.",
    )
    parser.add_argument(
        "--source-consistency-weight",
        type=float,
        default=0.0,
        help="Training-only KL weight from each window to its source-pooled teacher.",
    )
    parser.add_argument(
        "--no-augment",
        action="store_true",
        help="Disable spectrogram augmentation. Useful for ablations in individual identification.",
    )
    parser.add_argument(
        "--legacy-like",
        action="store_true",
        help="Force a legacy-style configuration: no augmentation, lower lr, light regularization.",
    )
    parser.add_argument("--blocks-per-batch", type=int, default=2)
    parser.add_argument(
        "--batches-per-file-group",
        type=int,
        default=4,
        help="Reuse the same selected NPZ files for N consecutive train batches.",
    )
    parser.add_argument(
        "--path-weight-power",
        type=float,
        default=0.5,
        help=(
            "NPZ selection weight exponent: 0 balances source files, 1 balances "
            "windows, and the default 0.5 is a compromise."
        ),
    )
    parser.add_argument("--cache-size", type=int, default=2)
    parser.add_argument("--num-workers", type=int, default=4)
    parser.add_argument(
        "--prefetch-factor",
        type=int,
        default=1,
        help="Batches prefetched by each DataLoader worker. Keep at 1 for large cached batches.",
    )
    parser.add_argument(
        "--no-persistent-workers",
        action="store_true",
        help="Disable persistent DataLoader workers to reduce retained worker memory.",
    )
    parser.add_argument(
        "--no-pin-memory",
        action="store_true",
        help="Disable pinned-memory staging if workers are killed by memory or /dev/shm pressure.",
    )
    parser.add_argument("--val-ratio", type=float, default=0.2)
    parser.add_argument(
        "--internal-dev-ratio",
        type=float,
        default=0.0,
        help=(
            "Fraction of official-training source groups reserved for checkpoint "
            "selection. The official validation remains frozen for final evaluation."
        ),
    )
    parser.add_argument(
        "--selection-only",
        action="store_true",
        help=(
            "Train and evaluate only on the source-disjoint internal-dev split; "
            "do not run model inference on the official validation split."
        ),
    )
    parser.add_argument("--max-files", type=int, default=None, help="Debug only: limit indexed npz files.")
    parser.add_argument(
        "--max-files-per-class",
        type=int,
        default=0,
        help="Debug only: deterministically limit source groups in every class.",
    )
    parser.add_argument(
        "--steps-per-epoch",
        type=int,
        default=0,
        help="Limit training batches per epoch. 0 means use the full split.",
    )
    parser.add_argument(
        "--max-val-batches",
        type=int,
        default=0,
        help="Limit validation batches per evaluation. 0 means full validation.",
    )
    parser.add_argument(
        "--eval-every",
        type=int,
        default=1,
        help="Run validation every N epochs and always on the final epoch.",
    )
    parser.add_argument(
        "--best-full-eval-every",
        type=int,
        default=5,
        help="Run a full validation pass every N epochs to choose the best checkpoint.",
    )
    parser.add_argument(
        "--selection-metric",
        choices=("accuracy", "macro_f1"),
        default="accuracy",
        help="Metric used to select best_checkpoint.pth; it is always computed on full validation.",
    )
    parser.add_argument(
        "--resume-checkpoint",
        default="",
        help="Resume an interrupted run from last_checkpoint.pth with optimizer/scheduler state.",
    )
    parser.add_argument(
        "--init-checkpoint",
        default="",
        help="Initialize model weights from an existing best_model.pth or checkpoint before a fresh run.",
    )
    parser.add_argument(
        "--full-final-eval",
        action="store_true",
        help="Deprecated compatibility flag; official final evaluation is always full.",
    )
    parser.add_argument(
        "--cpu-stft",
        action="store_true",
        help="Use the old CPU/DataLoader STFT path. Default is batched GPU STFT.",
    )
    parser.add_argument(
        "--audit-only",
        action="store_true",
        help="Validate manifest indices and class coverage, then exit.",
    )
    parser.add_argument(
        "--json-events",
        action="store_true",
        help="Emit machine-readable JSONL dataset, epoch, and result events.",
    )
    parser.add_argument(
        "--audit-npz-samples",
        type=int,
        default=4,
        help="Number of compressed NPZ files to decompress during audit spot-checking.",
    )
    parser.add_argument(
        "--trackio",
        action="store_true",
        help="Log epoch metrics and diagnostic alerts with Hugging Face Trackio.",
    )
    parser.add_argument(
        "--trackio-project",
        default="3943b-dji-individual-id",
        help="Trackio project name.",
    )
    parser.add_argument(
        "--trackio-run-name",
        default="",
        help="Optional Trackio run name.",
    )
    parser.add_argument(
        "--trackio-group",
        default="cnntrans-v5",
        help="Optional Trackio run group.",
    )
    parser.add_argument(
        "--trackio-space-id",
        default="",
        help="Optional Hugging Face Space id for remote Trackio syncing.",
    )
    parser.add_argument(
        "--trackio-val-drop-patience",
        type=int,
        default=3,
        help="Warn after N full evals stay below best by --trackio-val-drop-delta.",
    )
    parser.add_argument(
        "--trackio-val-drop-delta",
        type=float,
        default=1.0,
        help="Validation accuracy drop from best, in percentage points, before counting a regression epoch.",
    )
    parser.add_argument(
        "--trackio-collapse-confusion-rate",
        type=float,
        default=0.30,
        help="Warn when the largest true-to-predicted confusion rate exceeds this fraction.",
    )
    parser.add_argument(
        "--trackio-same-model-confusion-rate",
        type=float,
        default=0.15,
        help="Warn when the worst 01/02 same-model confusion rate exceeds this fraction.",
    )
    return parser.parse_args()


def apply_legacy_like_preset(args):
    if not args.legacy_like:
        return
    args.no_augment = True
    args.dropout = 0.6
    args.lr = 1e-4
    args.weight_decay = 1e-4
    args.label_smoothing = 0.0
    args.classifier_head = "ce"
    args.supcon_weight = 0.0


def main():
    args = parse_args()
    if args.require_cuda and not torch.cuda.is_available():
        raise RuntimeError(
            "CUDA GPU is required for this training task, but torch.cuda.is_available() is false"
        )
    set_global_seed(args.seed)
    apply_legacy_like_preset(args)
    auxiliary_weights = (
        args.family_loss_weight,
        args.individual_loss_weight,
        args.source_pooled_loss_weight,
        args.source_consistency_weight,
    )
    if any(weight < 0 for weight in auxiliary_weights):
        raise ValueError("Auxiliary loss weights must be non-negative.")
    if (
        args.family_loss_weight > 0 or args.individual_loss_weight > 0
    ) and args.classifier_head != "ce":
        raise ValueError("Paired-class auxiliary losses require --classifier-head ce.")
    os.makedirs(args.output_dir, exist_ok=True)
    output_dir = Path(args.output_dir)
    if args.resume_checkpoint:
        checkpoint_dir = Path(args.resume_checkpoint).resolve().parent
        if checkpoint_dir != output_dir.resolve():
            raise ValueError(
                "Resume must use the checkpoint's existing --output-dir so the "
                f"previous best checkpoint remains available: {checkpoint_dir}"
            )
    if args.resume_checkpoint and args.init_checkpoint:
        raise ValueError("Use only one of --resume-checkpoint or --init-checkpoint.")
    if args.stitch_windows < 1:
        raise ValueError("--stitch-windows must be >= 1.")
    if args.stitch_stride < 1:
        raise ValueError("--stitch-stride must be >= 1.")
    stitch_requested = args.stitch_windows > 1 or args.stitch_stride > 1
    if not args.cache_dir:
        args.cache_dir = "/root/autodl-tmp/cnntrans_v5_spec_cache"
    manifest_split_contract = inspect_manifest_split_contract(args.data_root)
    dual_view_requested = args.model_arch == "dual_axis_profile_large"
    phase_aware_requested = args.model_arch == "phase_aware_large"
    if dual_view_requested != bool(args.secondary_cache_dir):
        raise ValueError(
            "dual_axis_profile_large requires --secondary-cache-dir, and the "
            "secondary cache is not valid for other model architectures."
        )
    if dual_view_requested and args.feature_mode != "legacy":
        raise ValueError(
            "dual_axis_profile_large requires --feature-mode legacy for its primary cache."
        )
    if phase_aware_requested and args.cpu_stft:
        raise ValueError("phase_aware_large requires raw I/Q input; remove --cpu-stft.")
    if phase_aware_requested and args.rf_stats_cache_dir:
        raise ValueError(
            "phase_aware_large derives compact RF structure from raw I/Q and does not "
            "accept --rf-stats-cache-dir."
        )
    if stitch_requested and args.model_arch == "phase_aware_large":
        raise ValueError("--stitch-windows is not supported by phase_aware_large.")
    if stitch_requested and args.secondary_cache_dir:
        raise ValueError("--stitch-windows is not supported with precomputed dual-view caches.")
    if stitch_requested and args.precompute_phase_raw_iq_cache:
        raise ValueError("--stitch-windows is not supported by raw-IQ cache precomputation.")
    if stitch_requested and args.raw_iq_cache_dir:
        raise ValueError("--stitch-windows currently loads original NPZ windows directly.")
    if stitch_requested and args.rf_stats_cache_dir:
        raise ValueError("--stitch-windows changes window geometry and cannot use 4096 RF stats.")
    if not 0.0 <= args.internal_dev_ratio < 1.0:
        raise ValueError("--internal-dev-ratio must be in [0, 1).")
    if (
        args.selection_only
        and args.internal_dev_ratio <= 0
        and manifest_split_contract["source"] != "manifest"
    ):
        raise ValueError("--selection-only requires --internal-dev-ratio greater than 0.")
    train_exclude_sources = load_train_exclude_sources(args.train_exclude_sources)

    if args.audit_only:
        audit_report = audit_dataset(
            args.data_root,
            args.output_dir,
            cache_dir=args.cache_dir,
            val_ratio=args.val_ratio,
            npz_samples=args.audit_npz_samples,
            max_files_per_class=args.max_files_per_class,
            feature_mode=args.feature_mode,
        )
        if audit_report["status"] == "error":
            raise SystemExit("Dataset audit failed; inspect dataset_audit.json.")
        return

    if args.precompute_cache:
        precompute_spectrogram_cache(
            args.data_root,
            args.cache_dir,
            max_files=args.max_files,
            max_files_per_class=args.max_files_per_class,
            precompute_batch_size=args.precompute_batch_size,
            overwrite=args.overwrite_cache,
            feature_mode=args.feature_mode,
            index_cache_dir=args.index_cache_dir,
            val_ratio=args.val_ratio,
            internal_dev_ratio=args.internal_dev_ratio,
            selection_only=args.selection_only,
            train_exclude_sources=train_exclude_sources,
            stitch_windows=args.stitch_windows,
            stitch_stride=args.stitch_stride,
        )
        return
    if args.precompute_phase_raw_iq_cache:
        if not args.raw_iq_cache_dir:
            raise ValueError(
                "--precompute-phase-raw-iq-cache requires --raw-iq-cache-dir."
            )
        precompute_phase_raw_iq_cache(
            args.data_root,
            args.raw_iq_cache_dir,
            index_cache_dir=args.index_cache_dir,
            val_ratio=args.val_ratio,
            internal_dev_ratio=args.internal_dev_ratio,
            overwrite=args.overwrite_cache,
        )
        return

    use_precomputed_cache = bool(
        args.train_data_mode == "cache"
        and args.cache_dir
        and Path(args.cache_dir).exists()
        and not phase_aware_requested
    )
    explicit_manifest_split = manifest_split_contract["source"] == "manifest"
    manifest_split_counts = manifest_split_contract["counts"]
    keep_official_validation = not args.selection_only or explicit_manifest_split
    need_internal_dev = (
        args.internal_dev_ratio > 0
        or manifest_split_counts.get("internal_dev", 0) > 0
    )
    has_explicit_test = manifest_split_counts.get("test", 0) > 0
    secondary_train_dataset = None
    secondary_val_dataset = None
    secondary_internal_dev_dataset = None
    secondary_test_dataset = None
    val_dataset = None
    test_dataset = None
    if use_precomputed_cache:
        train_dataset = PrecomputedSpecDataset(
            args.data_root,
            args.cache_dir,
            split="train",
            val_ratio=args.val_ratio,
            is_train=True,
            max_files=args.max_files,
            max_files_per_class=args.max_files_per_class,
            cache_size=args.cache_size,
            index_cache_dir=args.index_cache_dir,
            internal_dev_ratio=args.internal_dev_ratio,
            feature_mode=args.feature_mode,
            cache_data_root=args.cache_data_root or None,
            train_exclude_sources=train_exclude_sources,
            stitch_windows=args.stitch_windows,
            stitch_stride=args.stitch_stride,
        )
        val_dataset = (
            PrecomputedSpecDataset(
                args.data_root,
                args.cache_dir,
                split="val",
                val_ratio=args.val_ratio,
                is_train=False,
                max_files=args.max_files,
                max_files_per_class=args.max_files_per_class,
                cache_size=args.cache_size,
                index_cache_dir=args.index_cache_dir,
                internal_dev_ratio=args.internal_dev_ratio,
                feature_mode=args.feature_mode,
                cache_data_root=args.cache_data_root or None,
                train_exclude_sources=train_exclude_sources,
                stitch_windows=args.stitch_windows,
                stitch_stride=args.stitch_stride,
            )
            if keep_official_validation
            else None
        )
        test_dataset = (
            PrecomputedSpecDataset(
                args.data_root,
                args.cache_dir,
                split="test",
                val_ratio=args.val_ratio,
                is_train=False,
                max_files=args.max_files,
                max_files_per_class=args.max_files_per_class,
                cache_size=args.cache_size,
                index_cache_dir=args.index_cache_dir,
                internal_dev_ratio=args.internal_dev_ratio,
                feature_mode=args.feature_mode,
                cache_data_root=args.cache_data_root or None,
                train_exclude_sources=train_exclude_sources,
                stitch_windows=args.stitch_windows,
                stitch_stride=args.stitch_stride,
            )
            if has_explicit_test
            else None
        )
        internal_dev_dataset = (
            PrecomputedSpecDataset(
                args.data_root,
                args.cache_dir,
                split="internal_dev",
                val_ratio=args.val_ratio,
                is_train=False,
                max_files=args.max_files,
                max_files_per_class=args.max_files_per_class,
                cache_size=args.cache_size,
                index_cache_dir=args.index_cache_dir,
                internal_dev_ratio=args.internal_dev_ratio,
                feature_mode=args.feature_mode,
                cache_data_root=args.cache_data_root or None,
                train_exclude_sources=train_exclude_sources,
                stitch_windows=args.stitch_windows,
                stitch_stride=args.stitch_stride,
            )
            if need_internal_dev
            else None
        )
        if dual_view_requested:
            if not Path(args.secondary_cache_dir).exists():
                raise FileNotFoundError(
                    f"Secondary cache directory not found: {args.secondary_cache_dir}"
                )
            secondary_train_dataset = PrecomputedSpecDataset(
                args.data_root,
                args.secondary_cache_dir,
                split="train",
                val_ratio=args.val_ratio,
                is_train=True,
                max_files=args.max_files,
                max_files_per_class=args.max_files_per_class,
                cache_size=args.cache_size,
                index_cache_dir=args.index_cache_dir,
                internal_dev_ratio=args.internal_dev_ratio,
                feature_mode="complex_signed_mag",
                cache_data_root=args.cache_data_root or None,
                train_exclude_sources=train_exclude_sources,
                stitch_windows=args.stitch_windows,
                stitch_stride=args.stitch_stride,
            )
            secondary_val_dataset = (
                PrecomputedSpecDataset(
                    args.data_root,
                    args.secondary_cache_dir,
                    split="val",
                    val_ratio=args.val_ratio,
                    is_train=False,
                    max_files=args.max_files,
                    max_files_per_class=args.max_files_per_class,
                    cache_size=args.cache_size,
                    index_cache_dir=args.index_cache_dir,
                    internal_dev_ratio=args.internal_dev_ratio,
                    feature_mode="complex_signed_mag",
                    cache_data_root=args.cache_data_root or None,
                    train_exclude_sources=train_exclude_sources,
                    stitch_windows=args.stitch_windows,
                    stitch_stride=args.stitch_stride,
                )
                if keep_official_validation
                else None
            )
            secondary_test_dataset = (
                PrecomputedSpecDataset(
                    args.data_root,
                    args.secondary_cache_dir,
                    split="test",
                    val_ratio=args.val_ratio,
                    is_train=False,
                    max_files=args.max_files,
                    max_files_per_class=args.max_files_per_class,
                    cache_size=args.cache_size,
                    index_cache_dir=args.index_cache_dir,
                    internal_dev_ratio=args.internal_dev_ratio,
                    feature_mode="complex_signed_mag",
                    cache_data_root=args.cache_data_root or None,
                    train_exclude_sources=train_exclude_sources,
                    stitch_windows=args.stitch_windows,
                    stitch_stride=args.stitch_stride,
                )
                if has_explicit_test
                else None
            )
            secondary_internal_dev_dataset = (
                PrecomputedSpecDataset(
                    args.data_root,
                    args.secondary_cache_dir,
                    split="internal_dev",
                    val_ratio=args.val_ratio,
                    is_train=False,
                    max_files=args.max_files,
                    max_files_per_class=args.max_files_per_class,
                    cache_size=args.cache_size,
                    index_cache_dir=args.index_cache_dir,
                    internal_dev_ratio=args.internal_dev_ratio,
                    feature_mode="complex_signed_mag",
                    cache_data_root=args.cache_data_root or None,
                    train_exclude_sources=train_exclude_sources,
                    stitch_windows=args.stitch_windows,
                    stitch_stride=args.stitch_stride,
                )
                if need_internal_dev
                else None
            )
            train_dataset = DualPrecomputedSpecDataset(
                train_dataset,
                secondary_train_dataset,
            )
            if val_dataset is not None:
                val_dataset = DualPrecomputedSpecDataset(
                    val_dataset,
                    secondary_val_dataset,
                )
            if test_dataset is not None:
                test_dataset = DualPrecomputedSpecDataset(
                    test_dataset,
                    secondary_test_dataset,
                )
            if internal_dev_dataset is not None:
                internal_dev_dataset = DualPrecomputedSpecDataset(
                    internal_dev_dataset,
                    secondary_internal_dev_dataset,
                )
    else:
        if dual_view_requested:
            raise ValueError(
                "dual_axis_profile_large currently requires both precomputed caches."
            )
        train_dataset = IqrawNpzDataset(
            args.data_root,
            split="train",
            val_ratio=args.val_ratio,
            is_train=True,
            max_files=args.max_files,
            max_files_per_class=args.max_files_per_class,
            cache_size=args.cache_size,
            return_raw_iq=not args.cpu_stft,
            index_cache_dir=args.index_cache_dir,
            internal_dev_ratio=args.internal_dev_ratio,
            raw_iq_cache_dir=(args.raw_iq_cache_dir if phase_aware_requested else ""),
            train_exclude_sources=train_exclude_sources,
            stitch_windows=args.stitch_windows,
            stitch_stride=args.stitch_stride,
        )
        val_dataset = (
            IqrawNpzDataset(
                args.data_root,
                split="val",
                val_ratio=args.val_ratio,
                is_train=False,
                max_files=args.max_files,
                max_files_per_class=args.max_files_per_class,
                cache_size=args.cache_size,
                return_raw_iq=not args.cpu_stft,
                index_cache_dir=args.index_cache_dir,
                internal_dev_ratio=args.internal_dev_ratio,
                raw_iq_cache_dir=(args.raw_iq_cache_dir if phase_aware_requested else ""),
                train_exclude_sources=train_exclude_sources,
                stitch_windows=args.stitch_windows,
                stitch_stride=args.stitch_stride,
            )
            if keep_official_validation
            else None
        )
        test_dataset = (
            IqrawNpzDataset(
                args.data_root,
                split="test",
                val_ratio=args.val_ratio,
                is_train=False,
                max_files=args.max_files,
                max_files_per_class=args.max_files_per_class,
                cache_size=args.cache_size,
                return_raw_iq=not args.cpu_stft,
                index_cache_dir=args.index_cache_dir,
                internal_dev_ratio=args.internal_dev_ratio,
                raw_iq_cache_dir=(args.raw_iq_cache_dir if phase_aware_requested else ""),
                train_exclude_sources=train_exclude_sources,
                stitch_windows=args.stitch_windows,
                stitch_stride=args.stitch_stride,
            )
            if has_explicit_test
            else None
        )
        internal_dev_dataset = (
            IqrawNpzDataset(
                args.data_root,
                split="internal_dev",
                val_ratio=args.val_ratio,
                is_train=False,
                max_files=args.max_files,
                max_files_per_class=args.max_files_per_class,
                cache_size=args.cache_size,
                return_raw_iq=not args.cpu_stft,
                index_cache_dir=args.index_cache_dir,
                internal_dev_ratio=args.internal_dev_ratio,
                raw_iq_cache_dir=(args.raw_iq_cache_dir if phase_aware_requested else ""),
                train_exclude_sources=train_exclude_sources,
                stitch_windows=args.stitch_windows,
                stitch_stride=args.stitch_stride,
            )
            if need_internal_dev
            else None
        )

    use_source_auxiliary = (
        args.source_pooled_loss_weight > 0
        or args.source_consistency_weight > 0
    )
    use_rf_stats = bool(args.rf_stats_cache_dir)
    if use_rf_stats != bool(args.rf_stats_normalization):
        raise ValueError(
            "Use --rf-stats-cache-dir and --rf-stats-normalization together."
        )
    if use_rf_stats:
        if not use_precomputed_cache:
            raise ValueError("Compact RF statistics currently require the legacy spec cache.")
        train_dataset = PrecomputedSpecRfStatsDataset(
            train_dataset,
            args.rf_stats_cache_dir,
            args.rf_stats_normalization,
            cache_size=args.cache_size,
            cache_data_root=args.cache_data_root or None,
        )
        if val_dataset is not None:
            val_dataset = PrecomputedSpecRfStatsDataset(
                val_dataset,
                args.rf_stats_cache_dir,
                args.rf_stats_normalization,
                cache_size=args.cache_size,
                cache_data_root=args.cache_data_root or None,
            )
        if test_dataset is not None:
            test_dataset = PrecomputedSpecRfStatsDataset(
                test_dataset,
                args.rf_stats_cache_dir,
                args.rf_stats_normalization,
                cache_size=args.cache_size,
                cache_data_root=args.cache_data_root or None,
            )
        if internal_dev_dataset is not None:
            internal_dev_dataset = PrecomputedSpecRfStatsDataset(
                internal_dev_dataset,
                args.rf_stats_cache_dir,
                args.rf_stats_normalization,
                cache_size=args.cache_size,
                cache_data_root=args.cache_data_root or None,
            )
    if use_source_auxiliary:
        train_base = unwrap_dataset(train_dataset)
        multi_path_sources = {
            source_group: paths
            for source_group, paths in train_base.source_group_to_paths.items()
            if len(paths) != 1
        }
        if multi_path_sources:
            raise ValueError(
                "Source auxiliary losses currently require one NPZ path per "
                f"source_file; found {len(multi_path_sources)} exceptions."
            )

    split_validation_dataset = (
        internal_dev_dataset if args.selection_only and internal_dev_dataset is not None
        else val_dataset
    )
    if split_validation_dataset is None:
        raise RuntimeError("No validation dataset is available for split auditing.")
    split_audit = assert_leakage_safe_split(
        train_dataset,
        split_validation_dataset,
    )
    split_audit["validation_scope"] = (
        "internal_dev"
        if args.selection_only and internal_dev_dataset is not None
        else "official_validation"
    )
    if test_dataset is not None:
        test_split_audit = assert_leakage_safe_split(train_dataset, test_dataset)
        test_split_audit["validation_scope"] = "test"
        atomic_write_text(
            output_dir / "test_split_audit.json",
            json.dumps(test_split_audit, ensure_ascii=False, indent=2),
        )
    (output_dir / "split_audit.json").write_text(
        json.dumps(split_audit, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    cache_audit = None
    internal_dev_audit = None
    if internal_dev_dataset is not None:
        internal_dev_audit = assert_leakage_safe_split(
            train_dataset,
            internal_dev_dataset,
        )
        internal_dev_audit["validation_scope"] = "internal_dev"
        if val_dataset is not None:
            official_dev_audit = assert_leakage_safe_split(
                internal_dev_dataset,
                val_dataset,
            )
            internal_dev_audit["official_validation_overlap"] = {
                "source_group_overlap": official_dev_audit["source_group_overlap"],
                "source_file_overlap": official_dev_audit["source_file_overlap"],
                "npz_path_overlap": official_dev_audit["npz_path_overlap"],
            }
        else:
            internal_dev_audit["official_validation_overlap"] = "NOT_ACCESSED"
        (output_dir / "internal_dev_split_audit.json").write_text(
            json.dumps(internal_dev_audit, ensure_ascii=False, indent=2),
            encoding="utf-8",
        )
    if use_precomputed_cache:
        cache_validation_dataset = (
            internal_dev_dataset if args.selection_only else val_dataset
        )
        if explicit_manifest_split:
            cache_validation_dataset = val_dataset
        additional_cache_datasets = (
            ()
            if (args.selection_only and not explicit_manifest_split)
            or internal_dev_dataset is None
            else (internal_dev_dataset,)
        )
        if test_dataset is not None:
            additional_cache_datasets = tuple(additional_cache_datasets) + (test_dataset,)
        primary_cache_audit = build_cache_audit_report(
            args.cache_dir,
            train_dataset,
            cache_validation_dataset,
            additional_datasets=additional_cache_datasets,
            feature_mode=args.feature_mode,
        )
        if dual_view_requested:
            secondary_cache_validation_dataset = (
                secondary_internal_dev_dataset
                if args.selection_only
                else secondary_val_dataset
            )
            if explicit_manifest_split:
                secondary_cache_validation_dataset = secondary_val_dataset
            additional_secondary_cache_datasets = (
                ()
                if (args.selection_only and not explicit_manifest_split)
                or secondary_internal_dev_dataset is None
                else (secondary_internal_dev_dataset,)
            )
            if secondary_test_dataset is not None:
                additional_secondary_cache_datasets = (
                    tuple(additional_secondary_cache_datasets)
                    + (secondary_test_dataset,)
                )
            secondary_cache_audit = build_cache_audit_report(
                args.secondary_cache_dir,
                secondary_train_dataset,
                secondary_cache_validation_dataset,
                additional_datasets=additional_secondary_cache_datasets,
                feature_mode="complex_signed_mag",
            )
            cache_audit = {
                "status": (
                    "warning"
                    if "warning"
                    in {
                        primary_cache_audit["status"],
                        secondary_cache_audit["status"],
                    }
                    else "ok"
                ),
                "dual_view": True,
                "primary": primary_cache_audit,
                "secondary": secondary_cache_audit,
            }
        else:
            cache_audit = primary_cache_audit
        (output_dir / "cache_audit.json").write_text(
            json.dumps(cache_audit, ensure_ascii=False, indent=2),
            encoding="utf-8",
        )

    manifest_split_records = sum(manifest_split_counts.values())

    loader_kwargs = {
        "num_workers": args.num_workers,
        "pin_memory": (device.type == "cuda" and not args.no_pin_memory),
    }
    if args.num_workers > 0:
        loader_kwargs["persistent_workers"] = not args.no_persistent_workers
        loader_kwargs["prefetch_factor"] = max(1, int(args.prefetch_factor))

    train_loader_dataset = (
        SourceAnnotatedDataset(train_dataset)
        if use_source_auxiliary
        else train_dataset
    )
    train_loader = DataLoader(
        train_loader_dataset,
        batch_sampler=create_batch_sampler(
            train_dataset,
            batch_size=args.batch_size,
            blocks_per_batch=args.blocks_per_batch,
            batches_per_file_group=args.batches_per_file_group,
            seed=args.seed,
            num_batches=args.steps_per_epoch,
            path_weight_power=args.path_weight_power,
        ),
        **loader_kwargs,
    )
    full_val_loader = (
        DataLoader(
            val_dataset,
            batch_size=args.batch_size,
            shuffle=False,
            **loader_kwargs,
        )
        if val_dataset is not None
        else None
    )
    full_test_loader = (
        DataLoader(
            test_dataset,
            batch_size=args.batch_size,
            shuffle=False,
            **loader_kwargs,
        )
        if test_dataset is not None
        else None
    )
    selection_dataset = internal_dev_dataset or val_dataset
    selection_full_loader = DataLoader(
        selection_dataset,
        batch_size=args.batch_size,
        shuffle=False,
        **loader_kwargs,
    )
    if args.max_val_batches > 0:
        monitor_val_loader = DataLoader(
            selection_dataset,
            batch_sampler=StratifiedEvalBatchSampler(
                selection_dataset,
                batch_size=args.batch_size,
                max_batches=args.max_val_batches,
                seed=args.seed + 1000,
            ),
            **loader_kwargs,
        )
    else:
        monitor_val_loader = selection_full_loader

    model = IqClassifier(
        num_classes=len(CLASS_NAMES),
        use_gpu_stft=(not args.cpu_stft) or use_precomputed_cache,
        augment=not args.no_augment,
        dropout=args.dropout,
        model_arch=args.model_arch,
        classifier_head=args.classifier_head,
        embedding_dim=args.embedding_dim,
        margin_scale=args.margin_scale,
        arc_margin=args.arc_margin,
        cos_margin=args.cos_margin,
        rf_stats_dim=RF_STATS_DIM if use_rf_stats else 0,
        rf_stats_fusion=args.rf_stats_fusion,
        rf_stats_input_mode=args.rf_stats_input_mode,
        feature_mode=args.feature_mode,
    )
    gated_multiscale_freeze = None
    if args.freeze_gated_multiscale_base:
        gated_multiscale_freeze = freeze_gated_multiscale_base(model)
    total_params = sum(p.numel() for p in model.parameters())
    trainable_params = sum(p.numel() for p in model.parameters() if p.requires_grad)
    config = {
        "args": vars(args),
        "class_names": CLASS_NAMES,
        "device": str(device),
        "total_params": total_params,
        "trainable_params": trainable_params,
        "gated_multiscale_freeze": gated_multiscale_freeze,
        "use_precomputed_cache": use_precomputed_cache,
        "official_validation_indexed": val_dataset is not None,
        "test_indexed": test_dataset is not None,
        "split_window_counts": {
            "train": len(train_dataset),
            "val": len(val_dataset) if val_dataset is not None else 0,
            "test": len(test_dataset) if test_dataset is not None else 0,
            "internal_dev": (
                len(internal_dev_dataset)
                if internal_dev_dataset is not None
                else 0
            ),
        },
        "split_audit": split_audit,
        "internal_dev_audit": internal_dev_audit,
        "cache_audit": cache_audit,
        "train_data_mode": args.train_data_mode,
        "require_cuda": bool(args.require_cuda),
        "cuda_available": bool(torch.cuda.is_available()),
        "gpu_name": (
            torch.cuda.get_device_name(torch.cuda.current_device())
            if torch.cuda.is_available()
            else ""
        ),
        "split_source": "manifest" if manifest_split_records else "source_group",
        "manifest_split_counts": manifest_split_counts,
        "selection_only_requested": bool(args.selection_only),
        "selection_only_overridden_by_manifest_split": bool(
            args.selection_only and explicit_manifest_split
        ),
        "train_exclude_sources_count": len(train_exclude_sources),
        "train_exclude_sources_sha1": (
            hashlib.sha1(
                json.dumps(
                    sorted(train_exclude_sources),
                    sort_keys=True,
                    separators=(",", ":"),
                ).encode("utf-8")
            ).hexdigest()
            if train_exclude_sources
            else ""
        ),
    }
    trackio_run = init_trackio_if_requested(args, config)
    atomic_write_text(
        Path(args.output_dir) / "run_config.json",
        json.dumps(config, ensure_ascii=False, indent=2),
    )
    print(f"Classes: {json.dumps(CLASS_NAMES, ensure_ascii=False)}")
    official_window_text = (
        f"{len(val_dataset):,}" if val_dataset is not None else "SEALED"
    )
    print(
        f"Train windows: {len(train_dataset):,} | "
        f"Selection windows: {len(selection_dataset):,} | "
        f"Official val windows: {official_window_text}"
    )
    print(f"Model arch: {args.model_arch}")
    if use_rf_stats:
        print(
            "RF stats: "
            f"fusion={args.rf_stats_fusion}, input_mode={args.rf_stats_input_mode}",
            flush=True,
        )
    print(
        "Classifier: "
        f"{args.classifier_head}, embedding_dim={args.embedding_dim}, "
        f"supcon_weight={args.supcon_weight}",
        flush=True,
    )
    print(f"Total params: {total_params:,} ({total_params / 1e6:.2f}M)")
    print(f"Trainable params: {trainable_params:,}")
    if args.json_events:
        emit_json_event(
            "dataset",
            train_windows=len(train_dataset),
            selection_windows=len(selection_dataset),
            official_val_windows=(len(val_dataset) if val_dataset is not None else None),
            model_arch=args.model_arch,
            stitch_windows=args.stitch_windows,
            stitch_stride=args.stitch_stride,
        )
    if gated_multiscale_freeze:
        print(
            "Gated multiscale freeze: training only "
            f"{gated_multiscale_freeze['trainable_parameter_count']} parameter tensors "
            "in the zero-gated branch.",
            flush=True,
        )
    if args.legacy_like:
        print(
            "Legacy-like preset enabled: "
            f"no_augment={args.no_augment}, lr={args.lr}, "
            f"weight_decay={args.weight_decay}, label_smoothing={args.label_smoothing}, "
            f"dropout={args.dropout}, classifier_head={args.classifier_head}, "
            f"supcon_weight={args.supcon_weight}",
            flush=True,
        )

    model, best_acc = train_model(
        model,
        train_loader,
        monitor_val_loader,
        selection_full_loader,
        output_dir=args.output_dir,
        num_epochs=args.epochs,
        steps_per_epoch=args.steps_per_epoch,
        max_val_batches=args.max_val_batches,
        eval_every=max(1, args.eval_every),
        best_full_eval_every=max(1, args.best_full_eval_every),
        selection_metric=args.selection_metric,
        resume_checkpoint=args.resume_checkpoint,
        init_checkpoint=args.init_checkpoint,
        lr=args.lr,
        weight_decay=args.weight_decay,
        label_smoothing=args.label_smoothing,
        supcon_weight=args.supcon_weight,
        supcon_temperature=args.supcon_temperature,
        family_loss_weight=args.family_loss_weight,
        individual_loss_weight=args.individual_loss_weight,
        source_pooled_loss_weight=args.source_pooled_loss_weight,
        source_consistency_weight=args.source_consistency_weight,
        lr_scheduler=args.lr_scheduler,
        trackio_val_drop_patience=args.trackio_val_drop_patience,
        trackio_val_drop_delta=args.trackio_val_drop_delta,
        trackio_collapse_confusion_rate=args.trackio_collapse_confusion_rate,
        trackio_same_model_confusion_rate=args.trackio_same_model_confusion_rate,
        trackio_run=trackio_run,
        json_events=args.json_events,
    )

    best_model_path = Path(args.output_dir) / "best_model.pth"
    if best_model_path.exists():
        best_state = torch.load(best_model_path, map_location=device)
        if isinstance(best_state, dict) and "model_state_dict" in best_state:
            best_state = best_state["model_state_dict"]
        model.load_state_dict(best_state)
        model.to(device)

    selection_acc = None
    selection_labels = None
    selection_preds = None
    selection_logits = None
    if internal_dev_dataset is not None or args.selection_only:
        (
            selection_acc,
            selection_labels,
            selection_preds,
            selection_logits,
        ) = evaluate_with_logits(model, selection_full_loader, max_batches=0)
        save_detailed_predictions(
            output_dir / (
                "internal_dev_predictions.npz"
                if internal_dev_dataset is not None
                else "val_predictions.npz"
            ),
            internal_dev_dataset if internal_dev_dataset is not None else val_dataset,
            selection_labels,
            selection_preds,
            selection_logits,
        )
        print(
            f"Best checkpoint "
            f"{'internal-dev' if internal_dev_dataset is not None else 'validation'} "
            f"accuracy: {selection_acc:.2f}%",
            flush=True,
        )

    if args.selection_only:
        if selection_acc is None or selection_labels is None or selection_preds is None:
            raise RuntimeError("Selection-only evaluation did not produce predictions.")
        selection_diagnostics = compute_confusion_diagnostics(
            selection_labels,
            selection_preds,
        )
        history_path = output_dir / "history.json"
        history = (
            json.loads(history_path.read_text(encoding="utf-8"))
            if history_path.exists()
            else []
        )
        best_epoch_record = next(
            (
                item
                for item in reversed(history)
                if item.get("epoch") == item.get("best_epoch")
            ),
            {},
        )
        selection_scope = (
            "internal_dev" if internal_dev_dataset is not None else "official_validation"
        )
        selection_prediction_name = (
            "internal_dev_predictions.npz"
            if internal_dev_dataset is not None
            else "val_predictions.npz"
        )
        artifacts = [
            "best_model.pth",
            "best_checkpoint.pth",
            "last_checkpoint.pth",
            "final_model.pth",
            "run_config.json",
            "history.json",
            "training_curves.png",
            selection_prediction_name,
            "split_audit.json",
        ]
        if internal_dev_dataset is not None:
            artifacts.append("internal_dev_split_audit.json")
        if test_dataset is not None:
            artifacts.append("test_split_audit.json")
        if use_precomputed_cache:
            artifacts.append("cache_audit.json")
        atomic_write_text(
            output_dir / "run_summary.json",
            json.dumps(
                {
                    "best_val_acc": best_acc,
                    "best_model_evaluated_acc": selection_acc,
                    "best_checkpoint_selection": (
                        f"full_{selection_scope}_{args.selection_metric}"
                    ),
                    "checkpoint_selection_scope": selection_scope,
                    "official_validation_evaluated": val_dataset is not None
                    and selection_scope == "official_validation"
                    and not args.selection_only,
                    "selection_only_requested": bool(args.selection_only),
                    "selection_only_overridden_by_manifest_split": bool(
                        args.selection_only and explicit_manifest_split
                    ),
                    "best_epoch": best_epoch_record.get("best_epoch"),
                    "best_epoch_train_val_gap": best_epoch_record.get("train_val_gap"),
                    "final_macro_f1": selection_diagnostics.get("macro_f1"),
                    "final_min_class_f1": selection_diagnostics.get("min_class_f1"),
                    "final_worst_classes": selection_diagnostics.get(
                        "worst_classes", []
                    ),
                    "final_top_confusions": selection_diagnostics.get(
                        "major_confusion_pairs", []
                    ),
                    "selection_is_full": True,
                    "max_val_batches": args.max_val_batches,
                    "train_windows": len(train_dataset),
                    "selection_windows": len(selection_dataset),
                    "official_val_windows": (
                        len(val_dataset) if val_dataset is not None else None
                    ),
                    "official_validation_indexed": val_dataset is not None,
                    "test_windows": len(test_dataset) if test_dataset is not None else 0,
                    "artifacts": artifacts,
                },
                ensure_ascii=False,
                indent=2,
            ),
        )
        print(
            f"Final {selection_scope} Acc: {selection_acc:.2f}% | "
            f"Best selection Acc: {best_acc:.2f}%",
            flush=True,
        )
        if args.json_events:
            emit_json_event(
                "result",
                final_accuracy=selection_acc,
                best_accuracy=best_acc,
                evaluation_scope=selection_scope,
                official_validation_evaluated=(
                    selection_scope == "official_validation"
                    and not args.selection_only
                ),
                selection_only_requested=bool(args.selection_only),
                selection_only_overridden_by_manifest_split=bool(
                    args.selection_only and explicit_manifest_split
                ),
                output_dir=str(output_dir),
            )
        if trackio_run is not None:
            trackio_run.log({
                "final_internal_dev_acc": selection_acc,
                "best_val_acc": best_acc,
            })
            trackio_run.finish()
        return

    if val_dataset is None or full_val_loader is None:
        raise RuntimeError("Official evaluation requires an indexed official split.")
    val_acc, val_labels, val_preds, val_logits = evaluate_with_logits(
        model,
        full_val_loader,
        max_batches=0,
    )
    evaluation_audit = assert_official_evaluation(
        val_dataset,
        val_labels,
        val_preds,
    )
    test_acc = None
    test_labels = None
    test_preds = None
    test_logits = None
    test_evaluation_audit = None
    test_diagnostics = None
    if test_dataset is not None and full_test_loader is not None:
        (
            test_acc,
            test_labels,
            test_preds,
            test_logits,
        ) = evaluate_with_logits(
            model,
            full_test_loader,
            max_batches=0,
        )
        test_evaluation_audit = build_test_evaluation_audit(
            test_dataset,
            test_labels,
            test_preds,
        )
        atomic_write_text(
            output_dir / "test_evaluation_audit.json",
            json.dumps(test_evaluation_audit, ensure_ascii=False, indent=2),
        )
        save_detailed_predictions(
            output_dir / "test_predictions.npz",
            test_dataset,
            test_labels,
            test_preds,
            test_logits,
        )
        test_diagnostics = compute_confusion_diagnostics(test_labels, test_preds)
    atomic_write_text(
        output_dir / "official_evaluation_audit.json",
        json.dumps(evaluation_audit, ensure_ascii=False, indent=2),
    )
    report_text, report_dict = print_classification_report(val_labels, val_preds)
    atomic_write_text(
        output_dir / "classification_report.txt",
        report_text,
    )
    atomic_write_text(
        output_dir / "classification_report.json",
        json.dumps(report_dict, ensure_ascii=False, indent=2),
    )
    save_detailed_predictions(
        output_dir / "val_predictions.npz",
        val_dataset,
        val_labels,
        val_preds,
        val_logits,
    )
    plot_confusion_matrix_visuals(val_labels, val_preds, output_dir)
    final_diagnostics = compute_confusion_diagnostics(val_labels, val_preds)
    history_path = output_dir / "history.json"
    history = (
        json.loads(history_path.read_text(encoding="utf-8"))
        if history_path.exists()
        else []
    )
    best_epoch_record = next(
        (
            item
            for item in reversed(history)
            if item.get("epoch") == item.get("best_epoch")
        ),
        {},
    )
    artifacts = [
        "best_model.pth",
        "best_checkpoint.pth",
        "last_checkpoint.pth",
        "final_model.pth",
        "run_config.json",
        "history.json",
        "training_curves.png",
        "classification_report.txt",
        "classification_report.json",
        "confusion_matrix.npy",
        "confusion_matrix.png",
        "val_predictions.npz",
        "split_audit.json",
        "official_evaluation_audit.json",
    ]
    if test_dataset is not None:
        artifacts.extend(
            ["test_predictions.npz", "test_evaluation_audit.json", "test_split_audit.json"]
        )
    if use_precomputed_cache:
        artifacts.append("cache_audit.json")
    if internal_dev_dataset is not None:
        artifacts.extend(
            ["internal_dev_split_audit.json", "internal_dev_predictions.npz"]
        )
    checkpoint_selection_scope = (
        "internal_dev" if internal_dev_dataset is not None else "official_validation"
    )
    atomic_write_text(
        output_dir / "run_summary.json",
        json.dumps(
            {
                "best_val_acc": best_acc,
                "best_model_evaluated_acc": val_acc,
                "test_acc": test_acc,
                "test_macro_f1": (
                    test_diagnostics.get("macro_f1")
                    if test_diagnostics is not None
                    else None
                ),
                "best_checkpoint_selection": (
                    f"full_{checkpoint_selection_scope}_{args.selection_metric}"
                ),
                "checkpoint_selection_scope": checkpoint_selection_scope,
                "official_validation_evaluated": True,
                "best_epoch": best_epoch_record.get("best_epoch"),
                "best_epoch_train_val_gap": best_epoch_record.get("train_val_gap"),
                "final_macro_f1": final_diagnostics.get("macro_f1"),
                "final_min_class_f1": final_diagnostics.get("min_class_f1"),
                "final_worst_classes": final_diagnostics.get("worst_classes", []),
                "final_top_confusions": final_diagnostics.get("major_confusion_pairs", []),
                "validation_is_full": True,
                "max_val_batches": args.max_val_batches,
                "train_windows": len(train_dataset),
                "val_windows": len(val_dataset),
                "test_windows": len(test_dataset) if test_dataset is not None else 0,
                "split_window_counts": {
                    "train": len(train_dataset),
                    "val": len(val_dataset),
                    "test": len(test_dataset) if test_dataset is not None else 0,
                    "internal_dev": (
                        len(internal_dev_dataset)
                        if internal_dev_dataset is not None
                        else 0
                    ),
                },
                "artifacts": artifacts,
            },
            ensure_ascii=False,
            indent=2,
        ),
    )
    test_text = f"{test_acc:.2f}%" if test_acc is not None else "not available"
    print(
        f"Final Val Acc: {val_acc:.2f}% | "
        f"Final Test Acc: {test_text} | Best Val Acc: {best_acc:.2f}%"
    )
    if args.json_events:
        emit_json_event(
            "result",
            final_accuracy=val_acc,
            best_accuracy=best_acc,
            test_accuracy=test_acc,
            evaluation_scope="official_validation",
            official_validation_evaluated=True,
            output_dir=str(output_dir),
        )
    if trackio_run is not None:
        trackio_run.log({
            "final_val_acc": val_acc,
            "best_val_acc": best_acc,
        })
        trackio_run.finish()


if __name__ == "__main__":
    main()
