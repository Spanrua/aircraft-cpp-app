#!/usr/bin/env python3
"""Binary DJI IQ classifier for separating drone signal from receiver noise.

The script deliberately treats cleaning as a reviewable prediction export:
``predict`` never deletes or moves source files.  Training data is supplied as
one or more signal roots and one or more noise roots.  Each root may contain
``manifest.jsonl`` files or bare ``.npz`` files with ``iq`` arrays shaped
``(N, 2, 4096)``.
"""

from __future__ import annotations

import argparse
import csv
import json
import random
from collections import Counter, OrderedDict
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Sequence

import numpy as np
import torch
from sklearn.metrics import accuracy_score, balanced_accuracy_score, confusion_matrix, f1_score, precision_score, recall_score
from torch import nn
from torch.utils.data import DataLoader, Dataset


WINDOW_SHAPE = (2, 4096)
IQRAW_SAMPLES_PER_WINDOW = 4096
LABEL_SIGNAL = 1
LABEL_NOISE = 0


@dataclass(frozen=True)
class SampleRecord:
    path: str
    index: int
    label: int
    group: str
    sample_id: str


def _iqraw_sample_count(path: Path) -> int:
    """Return complete complex-IQ samples for signed int8 interleaved data."""
    return path.stat().st_size // 2


def _read_iqraw_window(path: str, index: int) -> np.ndarray:
    start = index * IQRAW_SAMPLES_PER_WINDOW * 2
    count = IQRAW_SAMPLES_PER_WINDOW * 2
    values = np.fromfile(path, dtype=np.int8, count=count, offset=start)
    if values.size != count:
        raise ValueError(f"{path}: incomplete IQRAW window at index {index}")
    return values.reshape(IQRAW_SAMPLES_PER_WINDOW, 2).T.astype(np.float32)


def set_seed(seed: int) -> None:
    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)


def _resolve_path(raw_path: object, manifest: Path, root: Path, basename_index: dict[str, Path] | None = None) -> Path | None:
    if not raw_path:
        return None
    raw = str(raw_path).strip().replace("\\", "/")
    posix = PurePosixPath(raw)
    candidates = []
    native = Path(raw)
    if native.is_absolute():
        candidates.append(native)
    candidates.extend((manifest.parent / posix, root / posix, manifest.parent / posix.name, root / posix.name))
    parts = list(posix.parts)
    if "samples" in parts:
        sample_idx = parts.index("samples")
        candidates.append(root / manifest.parent.name / Path(*parts[sample_idx:]))
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    if basename_index is not None:
        return basename_index.get(posix.name)
    return None


def _record_index(item: dict) -> int:
    # Preprocess manifests use window_index for the row inside an NPZ chunk,
    # while sample_index may continue globally across all chunks.
    for key in ("npz_index", "local_index", "window_index", "sample_index", "segment_index"):
        value = item.get(key)
        if value is not None:
            try:
                return int(value)
            except (TypeError, ValueError):
                break
    return 0


def _group_key(item: dict, path: Path, label: int) -> str:
    source = str(item.get("source_file") or path.stem).replace("\\", "/")
    return f"{label}:{source}"


def _read_manifest(path: Path, root: Path, label: int, basename_index: dict[str, Path]) -> list[SampleRecord]:
    records: list[SampleRecord] = []
    with path.open("r", encoding="utf-8") as handle:
        for line_no, line in enumerate(handle, 1):
            if not line.strip():
                continue
            item = json.loads(line)
            if not isinstance(item, dict):
                raise ValueError(f"Manifest row is not an object: {path}:{line_no}")
            npz_path = _resolve_path(
                item.get("path") or item.get("sample_file_path"),
                path,
                root,
                basename_index,
            )
            if npz_path is None:
                continue
            index = _record_index(item)
            sample_id = str(item.get("sample_id") or f"{npz_path.name}:{index}")
            records.append(SampleRecord(str(npz_path), index, label, _group_key(item, npz_path, label), sample_id))
    return records


def _read_npz_file(path: Path, label: int) -> list[SampleRecord]:
    with np.load(path, mmap_mode="r") as payload:
        if "iq" not in payload:
            return []
        shape = tuple(int(value) for value in payload["iq"].shape)
    if len(shape) != 3 or shape[1:] != WINDOW_SHAPE:
        raise ValueError(f"{path}: iq shape {shape}, expected (N, 2, 4096)")
    group = f"{label}:{path.resolve()}"
    return [SampleRecord(str(path.resolve()), index, label, group, f"{path.name}:{index}") for index in range(shape[0])]


def _read_iqraw_file(path: Path, label: int) -> list[SampleRecord]:
    window_count = _iqraw_sample_count(path) // IQRAW_SAMPLES_PER_WINDOW
    group = f"{label}:{path.resolve()}"
    band = "F24" if "F24" in path.name.upper() else "F58" if "F58" in path.name.upper() else "unknown"
    return [
        SampleRecord(str(path.resolve()), index, label, group, f"{path.name}:{index}:{band}")
        for index in range(window_count)
    ]


def discover_records(
    roots: Sequence[Path],
    label: int,
    max_windows_per_file: int = 0,
    max_files: int = 0,
) -> list[SampleRecord]:
    """Read manifest-backed windows, falling back to bare NPZ indexing."""
    records: list[SampleRecord] = []
    files_seen = 0
    for root in roots:
        root = root.resolve()
        if not root.is_dir():
            raise FileNotFoundError(f"Data root not found: {root}")
        direct_npz = sorted(root.glob("*/samples/**/*.npz"))
        if direct_npz:
            for path in direct_npz:
                if max_files > 0 and files_seen >= max_files:
                    break
                file_records = _read_npz_file(path, label)
                if max_windows_per_file > 0:
                    file_records = file_records[:max_windows_per_file]
                records.extend(file_records)
                files_seen += 1
            continue
        direct_manifests = sorted(root.glob("*/manifest.jsonl"))
        manifests = direct_manifests or sorted(root.glob("manifest.jsonl"))
        if manifests:
            basename_index = {path.name: path.resolve() for path in root.rglob("*.npz")}
            for manifest in manifests:
                manifest_records = _read_manifest(manifest, root, label, basename_index)
                if max_files > 0:
                    allowed_paths = []
                    for record in manifest_records:
                        if record.path not in allowed_paths:
                            allowed_paths.append(record.path)
                        if len(allowed_paths) >= max_files:
                            break
                    manifest_records = [record for record in manifest_records if record.path in set(allowed_paths)]
                records.extend(manifest_records)
        else:
            for path in sorted(root.rglob("*.npz")):
                records.extend(_read_npz_file(path, label))
            for path in sorted(root.rglob("*.iqraw")):
                if max_files > 0 and files_seen >= max_files:
                    break
                records.extend(_read_iqraw_file(path, label))
                files_seen += 1
    if max_windows_per_file > 0:
        counts: Counter[str] = Counter()
        limited: list[SampleRecord] = []
        for record in records:
            if counts[record.path] >= max_windows_per_file:
                continue
            counts[record.path] += 1
            limited.append(record)
        records = limited
    unique: list[SampleRecord] = []
    seen: set[tuple[str, int, int]] = set()
    for record in records:
        key = (record.path, record.index, record.label)
        if key in seen:
            continue
        seen.add(key)
        unique.append(record)
    records = unique
    if not records:
        raise ValueError(f"No usable IQ windows found under: {', '.join(map(str, roots))}")
    return records


def split_by_group(records: Sequence[SampleRecord], val_ratio: float, seed: int) -> tuple[list[SampleRecord], list[SampleRecord]]:
    """Split whole source recordings, preserving both binary classes."""
    if not 0.0 <= val_ratio < 1.0:
        raise ValueError("val_ratio must be in [0, 1)")
    groups_by_label: dict[int, list[str]] = {LABEL_NOISE: [], LABEL_SIGNAL: []}
    for record in records:
        if record.group not in groups_by_label[record.label]:
            groups_by_label[record.label].append(record.group)
    rng = random.Random(seed)
    val_groups: set[str] = set()
    for label, groups in groups_by_label.items():
        if len(groups) < 2 and val_ratio > 0:
            raise ValueError(f"Class {label} has {len(groups)} source group(s); need at least 2 for validation")
        shuffled = sorted(groups)
        rng.shuffle(shuffled)
        count = 0 if val_ratio == 0 else max(1, int(round(len(shuffled) * val_ratio)))
        count = min(count, len(shuffled) - 1)
        val_groups.update(shuffled[:count])
    train = [record for record in records if record.group not in val_groups]
    val = [record for record in records if record.group in val_groups]
    if not train or not val:
        raise ValueError("Split produced an empty train or validation set")
    if {record.label for record in train} != {LABEL_NOISE, LABEL_SIGNAL} or {record.label for record in val} != {LABEL_NOISE, LABEL_SIGNAL}:
        raise ValueError("Both train and validation must contain signal and noise groups")
    return train, val


def _feature(iq: np.ndarray) -> np.ndarray:
    if iq.shape != WINDOW_SHAPE:
        raise ValueError(f"IQ window shape {iq.shape}, expected {WINDOW_SHAPE}")
    complex_iq = np.asarray(iq[0], dtype=np.float32) + 1j * np.asarray(iq[1], dtype=np.float32)
    n_fft, hop = 256, 128
    padded = np.pad(complex_iq, (n_fft // 2, n_fft // 2), mode="constant")
    frames = np.lib.stride_tricks.sliding_window_view(padded, n_fft)[::hop]
    window = np.hanning(n_fft).astype(np.float32)
    spectrum = np.fft.fft(frames * window, axis=-1)[..., : n_fft // 2 + 1]
    log_power = np.log1p(np.abs(spectrum).astype(np.float32))
    log_power = log_power.T
    return ((log_power - log_power.mean()) / (log_power.std() + 1e-6)).astype(np.float32)


class IQDataset(Dataset):
    def __init__(self, records: Sequence[SampleRecord], cache_files: int = 2) -> None:
        self.records = list(records)
        self.cache_files = max(1, int(cache_files))
        self._cache: OrderedDict[str, np.ndarray] = OrderedDict()

    def __len__(self) -> int:
        return len(self.records)

    def _load(self, path: str) -> np.ndarray:
        if path in self._cache:
            self._cache.move_to_end(path)
            return self._cache[path]
        if path.lower().endswith(".iqraw"):
            iq = np.empty((0, *WINDOW_SHAPE), dtype=np.float32)
        else:
            with np.load(path, allow_pickle=False) as payload:
                iq = np.asarray(payload["iq"], dtype=np.float32)
        self._cache[path] = iq
        self._cache.move_to_end(path)
        while len(self._cache) > self.cache_files:
            self._cache.popitem(last=False)
        return iq

    def __getitem__(self, index: int) -> tuple[torch.Tensor, torch.Tensor]:
        record = self.records[index]
        if record.path.lower().endswith(".iqraw"):
            iq = _read_iqraw_window(record.path, record.index)
        else:
            iq = self._load(record.path)[record.index]
        return torch.from_numpy(_feature(iq)[None]), torch.tensor(record.label, dtype=torch.float32)


def _load_record_iq(record: SampleRecord, cache: OrderedDict[str, np.ndarray]) -> np.ndarray:
    if record.path.lower().endswith(".iqraw"):
        return _read_iqraw_window(record.path, record.index)
    if record.path not in cache:
        with np.load(record.path, allow_pickle=False) as payload:
            cache[record.path] = np.asarray(payload["iq"], dtype=np.float32)
    cache.move_to_end(record.path)
    while len(cache) > 2:
        cache.popitem(last=False)
    return cache[record.path][record.index]


def build_feature_cache(records: Sequence[SampleRecord], output_dir: Path, cache_files: int = 2) -> list[SampleRecord]:
    output_dir.mkdir(parents=True, exist_ok=True)
    features_path = output_dir / "features.npy"
    metadata_path = output_dir / "records.jsonl"
    cache: OrderedDict[str, np.ndarray] = OrderedDict()
    features = np.lib.format.open_memmap(features_path, mode="w+", dtype=np.float32, shape=(len(records), 1, 129, 33))
    with metadata_path.open("w", encoding="utf-8") as handle:
        for offset, record in enumerate(records):
            features[offset, 0] = _feature(_load_record_iq(record, cache))
            handle.write(json.dumps({"path": record.path, "index": record.index, "label": record.label, "group": record.group, "sample_id": record.sample_id}, ensure_ascii=False) + "\n")
    del features
    (output_dir / "metadata.json").write_text(json.dumps({"records": len(records), "shape": [1, 129, 33], "feature": "log_power_stft_v1"}, indent=2), encoding="utf-8")
    return list(records)


class FeatureDataset(Dataset):
    def __init__(self, features_path: Path, records: Sequence[SampleRecord]) -> None:
        self.features = np.load(features_path, mmap_mode="r")
        self.records = list(records)

    def __len__(self) -> int:
        return len(self.records)

    def __getitem__(self, index: int) -> tuple[torch.Tensor, torch.Tensor]:
        return torch.from_numpy(np.array(self.features[index], copy=True)), torch.tensor(self.records[index].label, dtype=torch.float32)


class SignalNoiseCNN(nn.Module):
    def __init__(self, dropout: float = 0.2) -> None:
        super().__init__()
        self.features = nn.Sequential(
            nn.Conv2d(1, 16, 3, padding=1), nn.BatchNorm2d(16), nn.ReLU(), nn.MaxPool2d(2),
            nn.Conv2d(16, 32, 3, padding=1), nn.BatchNorm2d(32), nn.ReLU(), nn.MaxPool2d(2),
            nn.Conv2d(32, 64, 3, padding=1), nn.BatchNorm2d(64), nn.ReLU(), nn.AdaptiveAvgPool2d((1, 1)),
        )
        self.classifier = nn.Sequential(nn.Flatten(), nn.Dropout(dropout), nn.Linear(64, 1))

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return self.classifier(self.features(x)).squeeze(1)


def _metrics(labels: np.ndarray, probabilities: np.ndarray, threshold: float) -> dict[str, float | list[list[int]]]:
    predictions = (probabilities >= threshold).astype(np.int64)
    return {
        "accuracy": float(accuracy_score(labels, predictions)),
        "balanced_accuracy": float(balanced_accuracy_score(labels, predictions)),
        "precision_signal": float(precision_score(labels, predictions, zero_division=0)),
        "recall_signal": float(recall_score(labels, predictions, zero_division=0)),
        "f1_signal": float(f1_score(labels, predictions, zero_division=0)),
        "confusion_matrix": confusion_matrix(labels, predictions, labels=[0, 1]).tolist(),
    }


def _evaluate(model: nn.Module, loader: DataLoader, device: torch.device, threshold: float) -> tuple[float, dict[str, float | list[list[int]]]]:
    model.eval()
    losses: list[float] = []
    labels: list[np.ndarray] = []
    probabilities: list[np.ndarray] = []
    criterion = nn.BCEWithLogitsLoss()
    with torch.no_grad():
        for features, targets in loader:
            logits = model(features.to(device))
            losses.append(float(criterion(logits, targets.to(device)).item()))
            labels.append(targets.numpy().astype(np.int64))
            probabilities.append(torch.sigmoid(logits).cpu().numpy())
    return float(np.mean(losses)), _metrics(np.concatenate(labels), np.concatenate(probabilities), threshold)


def train(args: argparse.Namespace) -> int:
    set_seed(args.seed)
    signal_records = discover_records(args.signal_root, LABEL_SIGNAL, args.max_windows_per_file, args.max_files)
    noise_records = discover_records(args.noise_root, LABEL_NOISE, args.max_windows_per_file, args.max_files)
    train_records, val_records = split_by_group(signal_records + noise_records, args.val_ratio, args.seed)
    output_dir = args.output_dir
    output_dir.mkdir(parents=True, exist_ok=True)
    cache_root = output_dir / "feature_cache"
    train_cache = cache_root / "train"
    val_cache = cache_root / "val"
    build_feature_cache(train_records, train_cache, args.cache_files)
    build_feature_cache(val_records, val_cache, args.cache_files)
    train_loader = DataLoader(FeatureDataset(train_cache / "features.npy", train_records), batch_size=args.batch_size, shuffle=True, num_workers=args.num_workers)
    val_loader = DataLoader(FeatureDataset(val_cache / "features.npy", val_records), batch_size=args.batch_size, shuffle=False, num_workers=args.num_workers)
    device = torch.device("cuda" if torch.cuda.is_available() and not args.cpu else "cpu")
    model = SignalNoiseCNN(args.dropout).to(device)
    counts = Counter(record.label for record in train_records)
    pos_weight = torch.tensor([counts[LABEL_NOISE] / max(1, counts[LABEL_SIGNAL])], device=device)
    criterion = nn.BCEWithLogitsLoss(pos_weight=pos_weight)
    optimizer = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=args.weight_decay)
    best_score = -float("inf")
    history = []
    for epoch in range(1, args.epochs + 1):
        model.train()
        train_losses = []
        for features, targets in train_loader:
            optimizer.zero_grad(set_to_none=True)
            loss = criterion(model(features.to(device)), targets.to(device))
            loss.backward()
            optimizer.step()
            train_losses.append(float(loss.item()))
        val_loss, val_metrics = _evaluate(model, val_loader, device, args.threshold)
        row = {"epoch": epoch, "train_loss": float(np.mean(train_losses)), "val_loss": val_loss, **val_metrics}
        history.append(row)
        print(json.dumps(row, ensure_ascii=False), flush=True)
        score = float(val_metrics["f1_signal"])
        if score > best_score:
            best_score = score
            torch.save({"model": model.state_dict(), "feature": "log_power_stft_v1", "threshold": args.threshold}, output_dir / "best_model.pt")
    metadata = {
        "class_names": {"0": "noise", "1": "signal"},
        "feature": "log_power_stft_v1",
        "threshold": args.threshold,
        "seed": args.seed,
        "train_records": len(train_records),
        "val_records": len(val_records),
        "train_groups": sorted({record.group for record in train_records}),
        "val_groups": sorted({record.group for record in val_records}),
        "history": history,
    }
    (output_dir / "training_metadata.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2), encoding="utf-8")
    return 0


def _load_model(model_path: Path, device: torch.device) -> tuple[SignalNoiseCNN, float]:
    checkpoint = torch.load(model_path, map_location=device, weights_only=False)
    model = SignalNoiseCNN().to(device)
    model.load_state_dict(checkpoint["model"])
    model.eval()
    return model, float(checkpoint.get("threshold", 0.5))


def predict(args: argparse.Namespace) -> int:
    device = torch.device("cuda" if torch.cuda.is_available() and not args.cpu else "cpu")
    model, saved_threshold = _load_model(args.model, device)
    records = discover_records(args.input_root, LABEL_NOISE, args.max_windows_per_file)
    dataset = IQDataset([SampleRecord(record.path, record.index, 0, record.group, record.sample_id) for record in records], args.cache_files)
    loader = DataLoader(dataset, batch_size=args.batch_size, shuffle=False, num_workers=args.num_workers)
    threshold = args.threshold if args.threshold is not None else saved_threshold
    args.output_csv.parent.mkdir(parents=True, exist_ok=True)
    with args.output_csv.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=["path", "index", "sample_id", "group", "prob_signal", "prediction"])
        writer.writeheader()
        offset = 0
        with torch.no_grad():
            for features, _ in loader:
                probabilities = torch.sigmoid(model(features.to(device))).cpu().numpy()
                for probability in probabilities:
                    record = records[offset]
                    writer.writerow({"path": record.path, "index": record.index, "sample_id": record.sample_id, "group": record.group, "prob_signal": f"{float(probability):.8f}", "prediction": "signal" if probability >= threshold else "noise"})
                    offset += 1
    summary = {"threshold": threshold, "records": len(records), "output_csv": str(args.output_csv)}
    args.output_csv.with_suffix(".json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False), flush=True)
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    train_parser = subparsers.add_parser("train", help="train on signal and noise roots")
    train_parser.add_argument("--signal-root", type=Path, action="append", required=True)
    train_parser.add_argument("--noise-root", type=Path, action="append", required=True)
    train_parser.add_argument("--output-dir", type=Path, required=True)
    train_parser.add_argument("--epochs", type=int, default=10)
    train_parser.add_argument("--batch-size", type=int, default=64)
    train_parser.add_argument("--val-ratio", type=float, default=0.2)
    train_parser.add_argument("--max-windows-per-file", type=int, default=0)
    train_parser.add_argument("--max-files", type=int, default=0)
    train_parser.add_argument("--cache-dir", type=Path, help="optional external feature cache directory")
    train_parser.add_argument("--cache-files", type=int, default=2)
    train_parser.add_argument("--num-workers", type=int, default=0)
    train_parser.add_argument("--lr", type=float, default=1e-3)
    train_parser.add_argument("--weight-decay", type=float, default=1e-4)
    train_parser.add_argument("--dropout", type=float, default=0.2)
    train_parser.add_argument("--threshold", type=float, default=0.5)
    train_parser.add_argument("--seed", type=int, default=42)
    train_parser.add_argument("--cpu", action="store_true")
    train_parser.set_defaults(func=train)
    predict_parser = subparsers.add_parser("predict", help="export per-window signal/noise predictions")
    predict_parser.add_argument("--model", type=Path, required=True)
    predict_parser.add_argument("--input-root", type=Path, action="append", required=True)
    predict_parser.add_argument("--output-csv", type=Path, required=True)
    predict_parser.add_argument("--threshold", type=float)
    predict_parser.add_argument("--max-windows-per-file", type=int, default=0)
    predict_parser.add_argument("--cache-files", type=int, default=2)
    predict_parser.add_argument("--batch-size", type=int, default=64)
    predict_parser.add_argument("--num-workers", type=int, default=0)
    predict_parser.add_argument("--cpu", action="store_true")
    predict_parser.set_defaults(func=predict)
    return parser


if __name__ == "__main__":
    parsed_args = build_parser().parse_args()
    raise SystemExit(parsed_args.func(parsed_args))
