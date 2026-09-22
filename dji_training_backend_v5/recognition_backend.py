#!/usr/bin/env python3
"""Qt-facing inference adapter for CNNTRANS v5 checkpoints."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

import numpy as np

import CNNTRANS_v5_3943B as trainer


def emit(event_type: str, **payload: Any) -> None:
    print(json.dumps({"type": event_type, **payload}, ensure_ascii=False), flush=True)


def load_run_config(model_path: Path) -> dict[str, Any]:
    config_path = model_path.parent / "run_config.json"
    if not config_path.exists():
        return {}
    try:
        return json.loads(config_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


def checkpoint_state(checkpoint_path: Path, device: Any) -> dict[str, Any]:
    checkpoint = trainer.torch.load(
        checkpoint_path,
        map_location=device,
        weights_only=False,
    )
    if isinstance(checkpoint, dict) and "model_state_dict" in checkpoint:
        return checkpoint["model_state_dict"]
    if isinstance(checkpoint, dict):
        return checkpoint
    raise TypeError(f"Unsupported checkpoint payload: {checkpoint_path}")


def build_input_window(iq: np.ndarray, sample_index: int, stitch_windows: int) -> np.ndarray:
    if iq.ndim != 3 or iq.shape[1] != 2 or iq.shape[2] != 4096:
        raise ValueError(f"Expected IQ array shaped [N, 2, 4096], got {iq.shape}")
    if sample_index < 0 or sample_index >= iq.shape[0]:
        raise IndexError(
            f"sample_index={sample_index} is out of range for {iq.shape[0]} windows"
        )
    stitch_windows = max(1, int(stitch_windows))
    end_index = sample_index + stitch_windows
    if end_index > iq.shape[0]:
        raise IndexError(
            f"Need {stitch_windows} consecutive 4096-point windows from index "
            f"{sample_index}, but sample file only has {iq.shape[0]} windows"
        )
    if stitch_windows == 1:
        return iq[sample_index : sample_index + 1]
    stitched = iq[sample_index:end_index].transpose(1, 0, 2).reshape(2, -1)
    return stitched[np.newaxis, :, :]


def infer(model_path: Path, sample_path: Path, sample_index: int = 0) -> dict[str, Any]:
    if not model_path.is_file():
        raise FileNotFoundError(f"Model checkpoint not found: {model_path}")
    if not sample_path.is_file():
        raise FileNotFoundError(f"Sample NPZ not found: {sample_path}")
    if trainer.device.type != "cuda":
        raise RuntimeError("CUDA GPU is required for CNNTRANS v5 recognition")

    config = load_run_config(model_path)
    args = config.get("args", {}) if isinstance(config.get("args"), dict) else {}
    model_arch = str(config.get("model_arch") or args.get("model_arch") or "axis_profile_xlarge")
    feature_mode = str(config.get("feature_mode") or args.get("feature_mode") or "complex_signed_mag")
    classifier_head = str(args.get("classifier_head") or "hierarchical")
    dropout = float(args.get("dropout", 0.6))
    stitch_windows = int(args.get("stitch_windows") or 1)

    with np.load(sample_path) as data:
        if "iq" not in data:
            raise KeyError(f"Missing iq array in {sample_path}")
        iq = np.asarray(data["iq"], dtype=np.float32)
    inputs_np = build_input_window(iq, sample_index, stitch_windows)

    model = trainer.IqClassifier(
        num_classes=len(trainer.CLASS_NAMES),
        use_gpu_stft=True,
        augment=False,
        dropout=dropout,
        model_arch=model_arch,
        classifier_head=classifier_head,
        feature_mode=feature_mode,
    )
    model.load_state_dict(checkpoint_state(model_path, trainer.device))
    model.to(trainer.device)
    model.eval()

    inputs = trainer.torch.from_numpy(inputs_np).float().to(trainer.device)
    with trainer.torch.no_grad():
        logits = model(inputs)
        if isinstance(logits, tuple):
            logits = logits[0]
        probabilities = trainer.torch.softmax(logits, dim=1)[0]
    values, indices = trainer.torch.topk(probabilities, k=min(5, len(trainer.CLASS_NAMES)))
    top_candidates = [
        {
            "label": trainer.CLASS_NAMES[int(index)],
            "confidence": float(value * 100.0),
        }
        for value, index in zip(values.detach().cpu(), indices.detach().cpu())
    ]
    predicted = top_candidates[0]
    return {
        "model_path": str(model_path),
        "sample_path": str(sample_path),
        "predicted_type": predicted["label"],
        "predicted_individual": predicted["label"],
        "confidence": predicted["confidence"],
        "top_candidates": top_candidates,
        "device": str(trainer.device),
        "model_size": model_arch,
        "feature_mode": feature_mode,
        "stitch_windows": stitch_windows,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Qt-facing CNNTRANS v5 inference adapter")
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--sample", type=Path, required=True)
    parser.add_argument("--sample-index", type=int, default=0)
    args = parser.parse_args()
    try:
        emit("started", model_path=str(args.model), sample_path=str(args.sample))
        result = infer(args.model, args.sample, args.sample_index)
        emit("completed", **result)
        return 0
    except Exception as exc:
        emit("failed", error=str(exc))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
