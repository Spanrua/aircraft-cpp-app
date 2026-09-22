#!/usr/bin/env python3
"""Qt-facing process adapter for the v5 CNNTRANS trainer.

The Qt application talks to this module through a small JSONL protocol.  The
trainer stays in a child process so the GUI never imports PyTorch, and a
``TrainingProcess`` instance keeps the ``Popen`` handle available for Cancel.
"""

from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import sys
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path, PurePosixPath
from typing import Any, Callable, Iterator, Mapping, Sequence

import numpy as np


PROJECT_ROOT = Path(__file__).resolve().parent
TRAINER_SCRIPT = PROJECT_ROOT / "CNNTRANS_v5_3943B.py"
OPEN_SET_SCRIPT = PROJECT_ROOT / "rf_fingerprint_id" / "open_set_baseline.py"
EXPECTED_IQ_SHAPE = (2, 4096)


@dataclass
class TrainingConfig:
    """Stable options exposed to a Qt training page.

    Defaults reproduce the strongest recorded closed-set experiment.  Set
    ``stitch_windows=1`` for strict single-window training.
    """

    dataset_version: Path
    output_dir: Path
    epochs: int = 6
    batch_size: int = 32
    stitch_windows: int = 8
    stitch_stride: int = 4
    feature_mode: str = "complex_signed_mag"
    model_arch: str = "axis_profile_xlarge"
    classifier_head: str = "hierarchical"
    model_mode: str = "closed_set"
    open_set_action: str = "train"
    checkpoint: Path | None = None
    train_data_mode: str = "raw"
    split_mode: str = "source_group"
    val_ratio: float = 0.2
    internal_dev_ratio: float = 0.2
    selection_only: bool = True
    cache_dir: Path | None = None
    cache_data_root: Path | None = None
    index_cache_dir: Path | None = None
    precompute_cache_only: bool = False
    precompute_batch_size: int = 4096
    overwrite_cache: bool = False
    num_workers: int = 2
    prefetch_factor: int = 1
    blocks_per_batch: int = 16
    batches_per_file_group: int = 1
    steps_per_epoch: int = 1200
    max_val_batches: int = 32
    eval_every: int = 1
    best_full_eval_every: int = 1
    lr: float = 1e-4
    lr_scheduler: str = "cosine"
    weight_decay: float = 0.01
    label_smoothing: float = 0.1
    supcon_weight: float = 0.05
    supcon_temperature: float = 0.10
    dropout: float = 0.6
    seed: int = 42
    deterministic: bool = False
    require_cuda: bool = False
    cancel_flag: Path | None = None
    extra_args: list[str] = field(default_factory=list)

    def __post_init__(self) -> None:
        self.dataset_version = Path(self.dataset_version)
        self.output_dir = Path(self.output_dir)
        if self.cache_dir is not None:
            self.cache_dir = Path(self.cache_dir)
        if self.cache_data_root is not None:
            self.cache_data_root = Path(self.cache_data_root)
        if self.index_cache_dir is not None:
            self.index_cache_dir = Path(self.index_cache_dir)
        if self.checkpoint is not None:
            self.checkpoint = Path(self.checkpoint)
        if self.cancel_flag is not None:
            self.cancel_flag = Path(self.cancel_flag)

    def validate(self) -> None:
        if self.epochs < 1:
            raise ValueError("epochs must be at least 1")
        if self.batch_size < 1:
            raise ValueError("batch_size must be at least 1")
        if self.stitch_windows < 1 or self.stitch_stride < 1:
            raise ValueError("stitch_windows and stitch_stride must be at least 1")
        if self.precompute_batch_size < 1:
            raise ValueError("precompute_batch_size must be at least 1")
        if not 0 <= self.val_ratio < 1:
            raise ValueError("val_ratio must be in [0, 1)")
        if not 0 <= self.internal_dev_ratio < 1:
            raise ValueError("internal_dev_ratio must be in [0, 1)")
        if self.train_data_mode not in {"raw", "cache"}:
            raise ValueError("train_data_mode must be 'raw' or 'cache'")
        if self.model_mode not in {"closed_set", "open_set"}:
            raise ValueError("model_mode must be 'closed_set' or 'open_set'")
        if self.open_set_action not in {"audit", "train", "evaluate"}:
            raise ValueError("open_set_action must be audit, train, or evaluate")
        if not self.dataset_version.exists() or not self.dataset_version.is_dir():
            raise FileNotFoundError(f"Dataset version directory not found: {self.dataset_version}")
        if not self.output_dir:
            raise ValueError("output_dir is required")
        if self.precompute_cache_only and self.cache_dir is None:
            raise ValueError("precompute_cache_only requires cache_dir")
        if self.train_data_mode == "cache" and self.cache_dir is None:
            raise ValueError("cache training requires cache_dir")
        if self.train_data_mode == "cache" and self.cache_dir is not None:
            summary_path = self.cache_dir / "cache_summary.json"
            if not summary_path.exists():
                raise FileNotFoundError(f"Precomputed cache summary not found: {summary_path}")
        if self.require_cuda and not cuda_available():
            raise RuntimeError("CUDA GPU is required for this training task, but torch.cuda.is_available() is false")


def cuda_available() -> bool:
    try:
        import torch
    except Exception:
        return False
    return bool(torch.cuda.is_available())


def discover_manifests(dataset_version: Path) -> list[Path]:
    direct = dataset_version / "manifest.jsonl"
    manifests = [direct] if direct.exists() else sorted(dataset_version.rglob("manifest.jsonl"))
    return [path for path in manifests if path.is_file()]


def _resolve_manifest_path(
    raw_path: object,
    manifest_path: Path,
    dataset_version: Path,
    basename_index: Mapping[str, list[Path]] | None = None,
) -> Path | None:
    if not raw_path:
        return None
    raw = str(raw_path).strip().replace("\\", "/")
    posix = PurePosixPath(raw)
    candidates = []
    raw_path_obj = Path(raw)
    if raw_path_obj.is_absolute():
        candidates.append(raw_path_obj)
    candidates.extend(
        (
            manifest_path.parent / posix,
            dataset_version / posix,
            manifest_path.parent / posix.name,
            dataset_version / posix.name,
        )
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    basename_matches = (
        list(basename_index.get(posix.name, ()))
        if basename_index is not None
        else list(dataset_version.rglob(posix.name))
    )
    if len(basename_matches) == 1 and basename_matches[0].is_file():
        return basename_matches[0].resolve()
    return None


def _build_npz_basename_index(dataset_version: Path) -> dict[str, list[Path]]:
    index: dict[str, list[Path]] = {}
    for path in dataset_version.rglob("*.npz"):
        index.setdefault(path.name, []).append(path)
    return index


def _record_index(record: Mapping[str, Any]) -> int | None:
    for key in ("npz_index", "local_index", "sample_index", "window_index", "segment_index"):
        value = record.get(key)
        if value is not None:
            try:
                return int(value)
            except (TypeError, ValueError):
                return None
    return None


def audit_manifest(dataset_version: Path) -> dict[str, Any]:
    """Audit JSONL, referenced NPZ files, IQ arrays, and window indices."""

    manifests = discover_manifests(dataset_version)
    if not manifests:
        raise FileNotFoundError(f"No manifest.jsonl found under {dataset_version}")

    records = 0
    labels: dict[str, int] = {}
    invalid_rows = 0
    missing_path = 0
    missing_npz: list[str] = []
    malformed_npz: list[dict[str, str]] = []
    index_issues: list[dict[str, Any]] = []
    duplicate_windows: list[dict[str, Any]] = []
    npz_cache: dict[Path, tuple[int, tuple[int, ...]]] = {}
    seen_windows: set[tuple[Path, int]] = set()
    split_counts: dict[str, int] = {}
    explicit_split_records = 0
    basename_index = _build_npz_basename_index(dataset_version)

    for manifest_path in manifests:
        with manifest_path.open("r", encoding="utf-8") as handle:
            for line_no, line in enumerate(handle, 1):
                if not line.strip():
                    continue
                try:
                    item = json.loads(line)
                except json.JSONDecodeError as exc:
                    raise ValueError(f"Invalid JSON in {manifest_path}:{line_no}: {exc}") from exc
                if not isinstance(item, Mapping):
                    invalid_rows += 1
                    continue
                records += 1
                label = str(item.get("label", "<missing>"))
                labels[label] = labels.get(label, 0) + 1
                split = str(item.get("split") or "").strip().lower()
                if split:
                    split_counts[split] = split_counts.get(split, 0) + 1
                if split in {"train", "val", "test", "internal_dev"}:
                    explicit_split_records += 1
                resolved = _resolve_manifest_path(
                    item.get("path"),
                    manifest_path,
                    dataset_version,
                    basename_index,
                )
                if resolved is None:
                    if not item.get("path"):
                        missing_path += 1
                    else:
                        missing_npz.append(str(item.get("path")))
                    continue
                if resolved not in npz_cache:
                    try:
                        with np.load(resolved, allow_pickle=False) as payload:
                            if "iq" not in payload:
                                raise ValueError("missing iq array")
                            iq = np.asarray(payload["iq"])
                            shape = tuple(int(value) for value in iq.shape)
                            if len(shape) != 3 or shape[1:] != EXPECTED_IQ_SHAPE:
                                raise ValueError(
                                    f"iq shape {shape}, expected (N, 2, 4096)"
                                )
                            npz_cache[resolved] = (shape[0], shape)
                    except Exception as exc:
                        malformed_npz.append({"path": str(resolved), "error": str(exc)})
                        continue
                window_count, shape = npz_cache[resolved]
                index = _record_index(item)
                if index is None:
                    index_issues.append(
                        {"manifest": str(manifest_path), "line": line_no, "issue": "missing_or_invalid_index"}
                    )
                elif index < 0 or index >= window_count:
                    index_issues.append(
                        {
                            "manifest": str(manifest_path),
                            "line": line_no,
                            "path": str(resolved),
                            "index": index,
                            "window_count": window_count,
                        }
                    )
                else:
                    key = (resolved, index)
                    if key in seen_windows:
                        duplicate_windows.append({"path": str(resolved), "index": index})
                    seen_windows.add(key)

    errors = {
        "invalid_rows": invalid_rows,
        "missing_path": missing_path,
        "missing_npz": missing_npz,
        "malformed_npz": malformed_npz,
        "index_issues": index_issues,
    }
    return {
        "status": "ok" if not any(errors.values()) else "error",
        "manifests": [str(path) for path in manifests],
        "records": records,
        "classes": len(labels),
        "class_counts": labels,
        "npz_files": len(npz_cache),
        "split_source": "manifest" if explicit_split_records else "source_group",
        "split_counts": split_counts,
        "duplicate_windows": duplicate_windows[:100],
        "errors": errors,
    }


def validate_manifest(dataset_version: Path) -> dict[str, Any]:
    report = audit_manifest(dataset_version)
    if report["records"] == 0:
        raise ValueError(f"Manifest contains no records: {dataset_version}")
    if report["status"] != "ok":
        raise ValueError(
            "Dataset audit failed: "
            + json.dumps(report["errors"], ensure_ascii=False)
        )
    return report


def build_command(config: TrainingConfig, python_executable: str | None = None) -> list[str]:
    config.validate()
    if config.model_mode == "open_set":
        return build_open_set_command(
            dataset_version=config.dataset_version,
            output_dir=config.output_dir,
            action=config.open_set_action,
            checkpoint=config.checkpoint,
            python_executable=python_executable,
            stitch_windows=config.stitch_windows,
            stitch_stride=config.stitch_stride,
            cache_dir=config.cache_dir if config.train_data_mode == "cache" else None,
            cache_data_root=config.cache_data_root,
            index_cache_dir=config.index_cache_dir,
            feature_mode=config.feature_mode,
            model_arch=config.model_arch,
            batch_size=config.batch_size,
            num_workers=config.num_workers,
            prefetch_factor=config.prefetch_factor,
            epochs=config.epochs,
            seed=config.seed,
            val_ratio=config.val_ratio,
            extra_args=config.extra_args,
            require_cuda=config.require_cuda,
        )
    if not TRAINER_SCRIPT.exists():
        raise FileNotFoundError(f"Trainer script not found: {TRAINER_SCRIPT}")
    command = [
        python_executable or sys.executable,
        "-u",
        str(TRAINER_SCRIPT),
        "--data-root",
        str(config.dataset_version),
        "--output-dir",
        str(config.output_dir),
        "--epochs",
        str(config.epochs),
        "--batch-size",
        str(config.batch_size),
        "--stitch-windows",
        str(config.stitch_windows),
        "--stitch-stride",
        str(config.stitch_stride),
        "--feature-mode",
        config.feature_mode,
        "--train-data-mode",
        config.train_data_mode,
        "--model-arch",
        config.model_arch,
        "--classifier-head",
        config.classifier_head,
        "--val-ratio",
        str(config.val_ratio),
        "--internal-dev-ratio",
        str(config.internal_dev_ratio),
        "--num-workers",
        str(config.num_workers),
        "--prefetch-factor",
        str(config.prefetch_factor),
        "--seed",
        str(config.seed),
        "--lr",
        str(config.lr),
        "--lr-scheduler",
        config.lr_scheduler,
        "--weight-decay",
        str(config.weight_decay),
        "--label-smoothing",
        str(config.label_smoothing),
        "--supcon-weight",
        str(config.supcon_weight),
        "--supcon-temperature",
        str(config.supcon_temperature),
        "--dropout",
        str(config.dropout),
        "--blocks-per-batch",
        str(config.blocks_per_batch),
        "--batches-per-file-group",
        str(config.batches_per_file_group),
        "--steps-per-epoch",
        str(config.steps_per_epoch),
        "--max-val-batches",
        str(config.max_val_batches),
        "--eval-every",
        str(config.eval_every),
        "--best-full-eval-every",
        str(config.best_full_eval_every),
        "--json-events",
    ]
    if config.precompute_cache_only:
        command.append("--precompute-cache")
        command.extend(["--precompute-batch-size", str(config.precompute_batch_size)])
    if config.overwrite_cache:
        command.append("--overwrite-cache")
    if config.selection_only:
        command.append("--selection-only")
    if config.require_cuda:
        command.append("--require-cuda")
    should_pass_cache = config.precompute_cache_only or config.train_data_mode == "cache"
    if should_pass_cache and config.cache_dir is not None:
        command.extend(["--cache-dir", str(config.cache_dir)])
    if should_pass_cache and config.cache_data_root is not None:
        command.extend(["--cache-data-root", str(config.cache_data_root)])
    if config.index_cache_dir is not None:
        command.extend(["--index-cache-dir", str(config.index_cache_dir)])
    return command + list(config.extra_args)


def build_open_set_command(
    dataset_version: Path,
    output_dir: Path,
    action: str = "evaluate",
    checkpoint: Path | None = None,
    python_executable: str | None = None,
    stitch_windows: int = 8,
    stitch_stride: int = 1,
    cache_dir: Path | None = None,
    extra_args: Sequence[str] = (),
    cache_data_root: Path | None = None,
    index_cache_dir: Path | None = None,
    feature_mode: str = "complex_signed_mag",
    model_arch: str = "axis_profile_xlarge",
    batch_size: int = 32,
    num_workers: int = 2,
    prefetch_factor: int = 1,
    epochs: int = 6,
    seed: int = 42,
    val_ratio: float = 0.2,
    known_acceptance: float = 0.95,
    require_cuda: bool = False,
) -> list[str]:
    if action not in {"audit", "train", "evaluate"}:
        raise ValueError("open-set action must be audit, train, or evaluate")
    if not OPEN_SET_SCRIPT.exists():
        raise FileNotFoundError(f"Open-set script not found: {OPEN_SET_SCRIPT}")
    command = [
        python_executable or sys.executable,
        "-u",
        str(OPEN_SET_SCRIPT),
        action,
        "--data-root",
        str(dataset_version),
        "--output-dir",
        str(output_dir),
        "--stitch-windows",
        str(stitch_windows),
        "--stitch-stride",
        str(stitch_stride),
        "--feature-mode",
        feature_mode,
        "--model-arch",
        model_arch,
        "--batch-size",
        str(batch_size),
        "--num-workers",
        str(num_workers),
        "--prefetch-factor",
        str(prefetch_factor),
        "--seed",
        str(seed),
        "--val-ratio",
        str(val_ratio),
        "--known-acceptance",
        str(known_acceptance),
    ]
    if action == "train":
        command.extend(["--epochs", str(epochs)])
    if cache_dir is not None:
        command.extend(["--cache-dir", str(cache_dir)])
    if cache_data_root is not None:
        command.extend(["--cache-data-root", str(cache_data_root)])
    if index_cache_dir is not None:
        command.extend(["--index-cache-dir", str(index_cache_dir)])
    if checkpoint is not None:
        command.extend(["--checkpoint", str(checkpoint)])
    if require_cuda:
        command.append("--require-cuda")
    return command + list(extra_args)


def _event(event_type: str, **payload: Any) -> dict[str, Any]:
    return {"type": event_type, "timestamp": time.time(), **payload}


class TrainingProcess:
    """Own a trainer subprocess and expose cancellable JSONL events."""

    def __init__(
        self,
        config: TrainingConfig,
        on_event: Callable[[dict[str, Any]], None] | None = None,
        python_executable: str | None = None,
    ) -> None:
        self.config = config
        self.on_event = on_event
        self.python_executable = python_executable
        self.process: subprocess.Popen[str] | None = None
        self.manifest_info: dict[str, Any] | None = None
        self._started = False

    def start(self) -> "TrainingProcess":
        self.config.validate()
        self.manifest_info = validate_manifest(self.config.dataset_version)
        self.config.output_dir.mkdir(parents=True, exist_ok=True)
        request = {"config": asdict(self.config), "manifest": self.manifest_info}
        (self.config.output_dir / "training_request.json").write_text(
            json.dumps(request, default=str, ensure_ascii=False, indent=2),
            encoding="utf-8",
        )
        command = build_command(self.config, self.python_executable)
        env = dict(os.environ)
        env["PYTHONUNBUFFERED"] = "1"
        if self.config.cancel_flag is not None:
            env["AIRCRAFT_CANCEL_FLAG"] = str(self.config.cancel_flag)
        self.process = subprocess.Popen(
            command,
            cwd=str(PROJECT_ROOT),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            env=env,
            start_new_session=True,
        )
        self._started = True
        self.emit(_event("started", command=command, manifest=self.manifest_info, output_dir=str(self.config.output_dir)))
        return self

    def emit(self, event: dict[str, Any]) -> None:
        if self.on_event is not None:
            self.on_event(event)

    def iter_events(self) -> Iterator[dict[str, Any]]:
        if not self._started or self.process is None or self.process.stdout is None:
            raise RuntimeError("TrainingProcess.start() must be called first")
        for line in self.process.stdout:
            text = line.rstrip("\n")
            try:
                parsed = json.loads(text)
            except json.JSONDecodeError:
                self.emit(_event("log", message=text))
                continue
            if isinstance(parsed, Mapping) and parsed.get("type"):
                event = dict(parsed)
                self.emit(event)
                yield event
            else:
                self.emit(_event("log", message=text))

    def wait(self) -> dict[str, Any]:
        if self.process is None:
            raise RuntimeError("TrainingProcess.start() must be called first")
        for _event_item in self.iter_events():
            pass
        return_code = self.process.wait()
        summary_path = self.config.output_dir / "run_summary.json"
        summary: dict[str, Any] = {}
        if summary_path.exists():
            try:
                summary = json.loads(summary_path.read_text(encoding="utf-8"))
            except json.JSONDecodeError:
                pass
        result = {
            "return_code": return_code,
            "success": return_code == 0,
            "summary": summary,
            "output_dir": str(self.config.output_dir),
        }
        self.emit(_event("completed" if return_code == 0 else "failed", **result))
        return result

    def cancel(self) -> None:
        if self.process is None or self.process.poll() is not None:
            return
        if self.config.cancel_flag is not None:
            self.config.cancel_flag.parent.mkdir(parents=True, exist_ok=True)
            self.config.cancel_flag.write_text("cancel\n", encoding="utf-8")
            deadline = time.time() + 5.0
            while self.process.poll() is None and time.time() < deadline:
                time.sleep(0.05)
            if self.process.poll() is not None:
                self.emit(_event("cancel_requested", graceful=True))
                return
        try:
            os.killpg(self.process.pid, signal.SIGTERM)
        except (ProcessLookupError, OSError):
            self.process.terminate()
        self.emit(_event("cancel_requested"))

    terminate = cancel


def start_training(
    config: TrainingConfig,
    on_event: Callable[[dict[str, Any]], None] | None = None,
    python_executable: str | None = None,
) -> TrainingProcess:
    return TrainingProcess(config, on_event=on_event, python_executable=python_executable).start()


def run_training(
    config: TrainingConfig,
    on_event: Callable[[dict[str, Any]], None] | None = None,
    python_executable: str | None = None,
) -> dict[str, Any]:
    return start_training(config, on_event=on_event, python_executable=python_executable).wait()


def _parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Qt-facing CNNTRANS v5 training backend")
    parser.add_argument("--dataset-version", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--epochs", type=int, default=6)
    parser.add_argument("--batch-size", type=int, default=32)
    parser.add_argument("--stitch-windows", type=int, default=8)
    parser.add_argument("--stitch-stride", type=int, default=4)
    parser.add_argument("--feature-mode", choices=("legacy", "complex_signed_mag"), default="complex_signed_mag")
    parser.add_argument("--model-arch", default="axis_profile_xlarge")
    parser.add_argument("--classifier-head", choices=("ce", "hierarchical", "arcface", "cosface"), default="hierarchical")
    parser.add_argument("--model-mode", choices=("closed_set", "open_set"), default="closed_set")
    parser.add_argument("--open-set-action", choices=("audit", "train", "evaluate"), default="train")
    parser.add_argument("--checkpoint", type=Path)
    parser.add_argument("--train-data-mode", choices=("raw", "cache"), default="raw")
    parser.add_argument("--split-mode", choices=("source_group", "session", "temporal"), default="source_group")
    parser.add_argument("--val-ratio", type=float, default=0.2)
    parser.add_argument("--internal-dev-ratio", type=float, default=0.2)
    parser.add_argument("--num-workers", type=int, default=2)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--deterministic", action="store_true")
    parser.add_argument("--cache-dir", type=Path)
    parser.add_argument("--cache-data-root", type=Path)
    parser.add_argument("--index-cache-dir", type=Path)
    parser.add_argument("--precompute-cache-only", action="store_true")
    parser.add_argument("--precompute-batch-size", type=int, default=4096)
    parser.add_argument("--overwrite-cache", action="store_true")
    parser.add_argument("--require-cuda", action="store_true")
    parser.add_argument("--cancel-flag", type=Path)
    parser.add_argument("--check-only", action="store_true")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = _parse_args(argv)
    config = TrainingConfig(
        dataset_version=args.dataset_version,
        output_dir=args.output_dir,
        epochs=args.epochs,
        batch_size=args.batch_size,
        stitch_windows=args.stitch_windows,
        stitch_stride=args.stitch_stride,
        feature_mode=args.feature_mode,
        model_arch=args.model_arch,
        classifier_head=args.classifier_head,
        model_mode=args.model_mode,
        open_set_action=args.open_set_action,
        checkpoint=args.checkpoint,
        train_data_mode=args.train_data_mode,
        split_mode=args.split_mode,
        val_ratio=args.val_ratio,
        internal_dev_ratio=args.internal_dev_ratio,
        num_workers=args.num_workers,
        seed=args.seed,
        deterministic=args.deterministic,
        require_cuda=args.require_cuda,
        cancel_flag=args.cancel_flag,
        cache_dir=args.cache_dir,
        cache_data_root=args.cache_data_root,
        index_cache_dir=args.index_cache_dir,
        precompute_cache_only=args.precompute_cache_only,
        precompute_batch_size=args.precompute_batch_size,
        overwrite_cache=args.overwrite_cache,
    )
    try:
        if args.check_only:
            print(json.dumps(_event("manifest", **audit_manifest(args.dataset_version)), ensure_ascii=False), flush=True)
            return 0
        result = run_training(config, on_event=lambda item: print(json.dumps(item, ensure_ascii=False), flush=True))
        return int(result["return_code"])
    except Exception as exc:
        print(json.dumps(_event("failed", error=str(exc)), ensure_ascii=False), flush=True)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
