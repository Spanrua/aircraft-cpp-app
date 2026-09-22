"""Persistent mmap-backed indexes for large JSONL RF manifests."""

from __future__ import annotations

import hashlib
import json
import os
import tempfile
from pathlib import Path
from typing import Any

import numpy as np


INDEX_CACHE_VERSION = 1


def _manifest_fingerprint(data_root: Path) -> list[dict[str, Any]]:
    entries = []
    for path in sorted(data_root.rglob("manifest.jsonl")):
        stat = path.stat()
        entries.append(
            {
                "path": str(path.relative_to(data_root)),
                "size": stat.st_size,
                "mtime_ns": stat.st_mtime_ns,
            }
        )
    return entries


def _cache_config(
    data_root: Path,
    split: str,
    val_ratio: float,
    internal_dev_ratio: float,
    max_files: int | None,
    max_files_per_class: int,
    extra_config: dict[str, Any] | None = None,
) -> dict[str, Any]:
    config = {
        "version": INDEX_CACHE_VERSION,
        "data_root": str(data_root.resolve()),
        "split": split,
        "val_ratio": float(val_ratio),
        "internal_dev_ratio": float(internal_dev_ratio),
        "max_files": max_files,
        "max_files_per_class": int(max_files_per_class),
    }
    if extra_config:
        config["extra_config"] = extra_config
    return config


def _entry_dir(cache_dir: Path, config: dict[str, Any]) -> Path:
    encoded = json.dumps(config, sort_keys=True, separators=(",", ":")).encode()
    key = hashlib.sha256(encoded).hexdigest()[:20]
    return cache_dir / f"manifest_index_v{INDEX_CACHE_VERSION}_{key}"


def load_manifest_index(
    cache_dir: str | Path,
    data_root: str | Path,
    split: str,
    val_ratio: float,
    internal_dev_ratio: float,
    max_files: int | None,
    max_files_per_class: int,
    extra_config: dict[str, Any] | None = None,
) -> dict[str, Any] | None:
    """Load a cache entry only when its split config and manifests still match."""
    cache_dir = Path(cache_dir)
    data_root = Path(data_root)
    config = _cache_config(
        data_root,
        split,
        val_ratio,
        internal_dev_ratio,
        max_files,
        max_files_per_class,
        extra_config,
    )
    entry_dir = _entry_dir(cache_dir, config)
    metadata_path = entry_dir / "metadata.json"
    if not metadata_path.exists():
        return None

    try:
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        if metadata.get("config") != config:
            return None
        if metadata.get("manifest_fingerprint") != _manifest_fingerprint(data_root):
            return None
        arrays = {
            "record_path_ids": np.load(entry_dir / "record_path_ids.npy", mmap_mode="r"),
            "record_local_idxs": np.load(entry_dir / "record_local_idxs.npy", mmap_mode="r"),
            "record_labels": np.load(entry_dir / "record_labels.npy", mmap_mode="r"),
        }
    except (OSError, ValueError, json.JSONDecodeError):
        return None

    lengths = {len(value) for value in arrays.values()}
    if lengths != {int(metadata.get("record_count", -1))}:
        return None
    if arrays["record_path_ids"].dtype != np.uint32:
        return None
    if arrays["record_local_idxs"].dtype != np.uint32:
        return None
    if arrays["record_labels"].dtype != np.uint8:
        return None
    return {"entry_dir": entry_dir, "metadata": metadata, **arrays}


def _atomic_save_npy(path: Path, values: np.ndarray) -> None:
    with tempfile.NamedTemporaryFile(dir=path.parent, suffix=".npy", delete=False) as handle:
        temp_path = Path(handle.name)
        np.save(handle, values, allow_pickle=False)
    os.replace(temp_path, path)


def save_manifest_index(
    cache_dir: str | Path,
    data_root: str | Path,
    split: str,
    val_ratio: float,
    internal_dev_ratio: float,
    max_files: int | None,
    max_files_per_class: int,
    record_path_ids: Any,
    record_local_idxs: Any,
    record_labels: Any,
    state: dict[str, Any],
    extra_config: dict[str, Any] | None = None,
) -> Path:
    """Write arrays first and metadata last so partial entries are never loaded."""
    cache_dir = Path(cache_dir)
    data_root = Path(data_root)
    config = _cache_config(
        data_root,
        split,
        val_ratio,
        internal_dev_ratio,
        max_files,
        max_files_per_class,
        extra_config,
    )
    entry_dir = _entry_dir(cache_dir, config)
    entry_dir.mkdir(parents=True, exist_ok=True)

    path_ids = np.asarray(record_path_ids, dtype=np.uint32)
    local_idxs = np.asarray(record_local_idxs, dtype=np.uint32)
    labels = np.asarray(record_labels, dtype=np.uint8)
    if not (len(path_ids) == len(local_idxs) == len(labels)):
        raise ValueError("Manifest index arrays must have matching lengths.")

    _atomic_save_npy(entry_dir / "record_path_ids.npy", path_ids)
    _atomic_save_npy(entry_dir / "record_local_idxs.npy", local_idxs)
    _atomic_save_npy(entry_dir / "record_labels.npy", labels)
    metadata = {
        "config": config,
        "manifest_fingerprint": _manifest_fingerprint(data_root),
        "record_count": len(labels),
        "state": state,
    }
    metadata_path = entry_dir / "metadata.json"
    with tempfile.NamedTemporaryFile(
        dir=entry_dir,
        mode="w",
        encoding="utf-8",
        suffix=".json",
        delete=False,
    ) as handle:
        temp_path = Path(handle.name)
        json.dump(metadata, handle, ensure_ascii=False, separators=(",", ":"))
    os.replace(temp_path, metadata_path)
    return entry_dir
