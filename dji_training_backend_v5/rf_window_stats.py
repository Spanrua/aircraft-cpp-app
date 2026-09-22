"""Compact RF statistics computed independently from each I/Q window."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Iterable

import numpy as np
import torch
from tqdm import tqdm


RF_STATS_VERSION = 1
RF_STATS_SEGMENTS = 8
RF_STATS_AUTOCORR_LAGS = (1, 4, 16, 64)
RF_STATS_NAMES = (
    "mean_i",
    "mean_q",
    "log_std_i",
    "log_std_q",
    "log_rms",
    "normalized_dc_i",
    "normalized_dc_q",
    "iq_correlation",
    "log_iq_variance_ratio",
    "amplitude_mean_over_rms",
    "amplitude_std_over_rms",
    "amplitude_skewness",
    "amplitude_kurtosis",
    "papr",
    "phase_step_cos_mean",
    "phase_step_sin_mean",
    "phase_step_resultant",
    *(f"segment_cfo_{index}" for index in range(RF_STATS_SEGMENTS)),
    "cfo_mean",
    "cfo_std",
    "cfo_range",
    "cfo_slope",
    *(
        name
        for lag in RF_STATS_AUTOCORR_LAGS
        for name in (f"autocorr_{lag}_real", f"autocorr_{lag}_imag", f"autocorr_{lag}_abs")
    ),
    *(f"segment_log_power_{index}" for index in range(RF_STATS_SEGMENTS)),
)
RF_STATS_DIM = len(RF_STATS_NAMES)


def compute_rf_window_stats(iq: torch.Tensor) -> torch.Tensor:
    """Return [B, RF_STATS_DIM] features without cross-window information."""
    if iq.ndim != 3 or iq.shape[1] != 2 or iq.shape[2] != 4096:
        raise ValueError(f"Expected I/Q windows [B, 2, 4096], got {tuple(iq.shape)}.")
    x = iq.float()
    i = x[:, 0]
    q = x[:, 1]
    eps = 1e-8
    mean_i = i.mean(dim=1)
    mean_q = q.mean(dim=1)
    centered_i = i - mean_i.unsqueeze(1)
    centered_q = q - mean_q.unsqueeze(1)
    variance_i = centered_i.square().mean(dim=1).clamp_min(eps)
    variance_q = centered_q.square().mean(dim=1).clamp_min(eps)
    std_i = variance_i.sqrt()
    std_q = variance_q.sqrt()
    raw_power = (i.square() + q.square()).mean(dim=1).clamp_min(eps)
    rms = raw_power.sqrt()
    covariance = (centered_i * centered_q).mean(dim=1)
    correlation = covariance / (std_i * std_q).clamp_min(eps)

    amplitude = (i.square() + q.square()).clamp_min(eps).sqrt()
    amplitude_mean = amplitude.mean(dim=1)
    amplitude_centered = amplitude - amplitude_mean.unsqueeze(1)
    amplitude_std = amplitude_centered.square().mean(dim=1).clamp_min(eps).sqrt()
    normalized_amplitude = amplitude_centered / amplitude_std.unsqueeze(1)
    amplitude_skewness = normalized_amplitude.pow(3).mean(dim=1)
    amplitude_kurtosis = normalized_amplitude.pow(4).mean(dim=1)
    papr = amplitude.square().amax(dim=1) / raw_power

    previous_i, next_i = i[:, :-1], i[:, 1:]
    previous_q, next_q = q[:, :-1], q[:, 1:]
    phase_real = next_i * previous_i + next_q * previous_q
    phase_imag = next_q * previous_i - next_i * previous_q
    phase_norm = (phase_real.square() + phase_imag.square()).clamp_min(eps).sqrt()
    phase_cos = phase_real / phase_norm
    phase_sin = phase_imag / phase_norm
    phase_cos_mean = phase_cos.mean(dim=1)
    phase_sin_mean = phase_sin.mean(dim=1)
    phase_resultant = (phase_cos_mean.square() + phase_sin_mean.square()).sqrt()

    usable_phase_steps = (phase_real.shape[1] // RF_STATS_SEGMENTS) * RF_STATS_SEGMENTS
    segment_real = phase_real[:, :usable_phase_steps].reshape(
        len(x), RF_STATS_SEGMENTS, -1
    ).sum(dim=2)
    segment_imag = phase_imag[:, :usable_phase_steps].reshape(
        len(x), RF_STATS_SEGMENTS, -1
    ).sum(dim=2)
    segment_cfo = torch.atan2(segment_imag, segment_real) / torch.pi
    cfo_mean = segment_cfo.mean(dim=1)
    cfo_std = segment_cfo.std(dim=1, unbiased=False)
    cfo_range = segment_cfo.amax(dim=1) - segment_cfo.amin(dim=1)
    positions = torch.linspace(-1.0, 1.0, RF_STATS_SEGMENTS, device=x.device)
    cfo_slope = (segment_cfo * positions).sum(dim=1) / positions.square().sum()

    complex_power = (centered_i.square() + centered_q.square()).mean(dim=1).clamp_min(eps)
    autocorrelation_features = []
    for lag in RF_STATS_AUTOCORR_LAGS:
        left_i, right_i = centered_i[:, :-lag], centered_i[:, lag:]
        left_q, right_q = centered_q[:, :-lag], centered_q[:, lag:]
        corr_real = (right_i * left_i + right_q * left_q).mean(dim=1) / complex_power
        corr_imag = (right_q * left_i - right_i * left_q).mean(dim=1) / complex_power
        autocorrelation_features.extend(
            (corr_real, corr_imag, (corr_real.square() + corr_imag.square()).sqrt())
        )

    segment_length = i.shape[1] // RF_STATS_SEGMENTS
    segment_power = (
        i[:, : segment_length * RF_STATS_SEGMENTS]
        .reshape(len(x), RF_STATS_SEGMENTS, segment_length)
        .square()
        + q[:, : segment_length * RF_STATS_SEGMENTS]
        .reshape(len(x), RF_STATS_SEGMENTS, segment_length)
        .square()
    ).mean(dim=2).clamp_min(eps)
    segment_log_power = torch.log(segment_power / raw_power.unsqueeze(1))

    scalar_features = [
        mean_i,
        mean_q,
        std_i.log(),
        std_q.log(),
        rms.log(),
        mean_i / rms,
        mean_q / rms,
        correlation,
        (variance_i / variance_q).log(),
        amplitude_mean / rms,
        amplitude_std / rms,
        amplitude_skewness,
        amplitude_kurtosis,
        papr,
        phase_cos_mean,
        phase_sin_mean,
        phase_resultant,
    ]
    output = torch.cat(
        (
            torch.stack(scalar_features, dim=1),
            segment_cfo,
            torch.stack((cfo_mean, cfo_std, cfo_range, cfo_slope), dim=1),
            torch.stack(autocorrelation_features, dim=1),
            segment_log_power,
        ),
        dim=1,
    )
    if output.shape[1] != RF_STATS_DIM:
        raise RuntimeError(f"RF statistics produced {output.shape[1]} features, expected {RF_STATS_DIM}.")
    return output.nan_to_num(nan=0.0, posinf=20.0, neginf=-20.0).clamp(-20.0, 20.0)


def rf_stats_path(cache_dir: str | Path, data_root: str | Path, npz_path: str | Path) -> Path:
    relative = Path(npz_path).resolve().relative_to(Path(data_root).resolve())
    return Path(cache_dir) / "stats_npy" / relative.with_suffix(".rfstats.npy")


def precompute_rf_stats_for_paths(
    paths: Iterable[str | Path],
    data_root: str | Path,
    cache_dir: str | Path,
    batch_size: int = 2048,
    device: str | None = None,
    overwrite: bool = False,
) -> dict:
    """Build one float16 stats memmap per NPZ while preserving local row order."""
    if batch_size <= 0:
        raise ValueError("batch_size must be positive.")
    target_device = torch.device(device or ("cuda" if torch.cuda.is_available() else "cpu"))
    completed = 0
    windows = 0
    paths = [Path(path) for path in paths]
    for raw_path in tqdm(paths, desc="RF stats cache", unit="source", mininterval=2.0):
        raw_path = Path(raw_path)
        output_path = rf_stats_path(cache_dir, data_root, raw_path)
        metadata_path = output_path.with_suffix(".json")
        if output_path.exists() and metadata_path.exists() and not overwrite:
            metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
            source_stat = raw_path.stat()
            if (
                metadata.get("version") == RF_STATS_VERSION
                and metadata.get("dim") == RF_STATS_DIM
                and metadata.get("source_size") == source_stat.st_size
                and metadata.get("source_mtime_ns") == source_stat.st_mtime_ns
            ):
                completed += 1
                windows += int(metadata["windows"])
                continue
        with np.load(raw_path, allow_pickle=False) as data:
            iq = np.asarray(data["iq"])
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output = np.lib.format.open_memmap(
            output_path,
            mode="w+",
            dtype=np.float16,
            shape=(len(iq), RF_STATS_DIM),
        )
        for start in range(0, len(iq), batch_size):
            batch = torch.from_numpy(iq[start : start + batch_size]).to(
                target_device,
                non_blocking=True,
            )
            with torch.no_grad():
                values = compute_rf_window_stats(batch).cpu().numpy().astype(np.float16)
            output[start : start + len(values)] = values
        output.flush()
        del output
        metadata_path.write_text(
            json.dumps(
                {
                    "version": RF_STATS_VERSION,
                    "source_npz": str(raw_path),
                    "windows": len(iq),
                    "dim": RF_STATS_DIM,
                    "dtype": "float16",
                    "feature_names": RF_STATS_NAMES,
                    "source_size": raw_path.stat().st_size,
                    "source_mtime_ns": raw_path.stat().st_mtime_ns,
                },
                ensure_ascii=False,
                indent=2,
            ),
            encoding="utf-8",
        )
        completed += 1
        windows += len(iq)
    summary = {
        "version": RF_STATS_VERSION,
        "files": completed,
        "windows": windows,
        "dim": RF_STATS_DIM,
        "dtype": "float16",
        "feature_names": RF_STATS_NAMES,
    }
    cache_dir = Path(cache_dir)
    cache_dir.mkdir(parents=True, exist_ok=True)
    (cache_dir / "rf_stats_summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    return summary


def fit_rf_stats_normalization(
    paths: Iterable[str | Path],
    data_root: str | Path,
    cache_dir: str | Path,
    output_path: str | Path,
) -> dict:
    """Fit fixed feature normalization using training sources only."""
    total = 0
    feature_sum = np.zeros(RF_STATS_DIM, dtype=np.float64)
    feature_square_sum = np.zeros(RF_STATS_DIM, dtype=np.float64)
    source_count = 0
    for path in paths:
        values = np.load(rf_stats_path(cache_dir, data_root, path), mmap_mode="r")
        if values.ndim != 2 or values.shape[1] != RF_STATS_DIM:
            raise ValueError(f"Invalid RF statistics shape for {path}: {values.shape}")
        for start in range(0, len(values), 65536):
            chunk = np.asarray(values[start : start + 65536], dtype=np.float64)
            feature_sum += chunk.sum(axis=0)
            feature_square_sum += np.square(chunk).sum(axis=0)
            total += len(chunk)
        source_count += 1
    if total == 0:
        raise ValueError("Cannot fit RF statistics normalization without windows.")
    mean = feature_sum / total
    variance = feature_square_sum / total - np.square(mean)
    std = np.sqrt(np.maximum(variance, 1e-8))
    report = {
        "version": RF_STATS_VERSION,
        "training_only": True,
        "sources": source_count,
        "windows": total,
        "dim": RF_STATS_DIM,
        "feature_names": RF_STATS_NAMES,
        "mean": mean.tolist(),
        "std": std.tolist(),
    }
    output_path = Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(
        json.dumps(report, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    return report
