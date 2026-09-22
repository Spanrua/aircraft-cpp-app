"""Fixed-coordinate STFT preprocessing for RF emitter fingerprinting."""

from __future__ import annotations

from dataclasses import asdict
from pathlib import Path

import numpy as np

from .manifest import WindowRecord


BASE_IQ_WINDOW = 4096
DEFAULT_WINDOW_SIZES = (4096, 8192, 16384, 32768, 65536)


def complex_from_iq(iq: np.ndarray) -> np.ndarray:
    if iq.ndim == 2 and iq.shape[0] == 2:
        return iq[0].astype(np.float32, copy=False) + 1j * iq[1].astype(np.float32, copy=False)
    if iq.ndim == 1 and np.issubdtype(iq.dtype, np.complexfloating):
        return iq.astype(np.complex64, copy=False)
    raise ValueError(f"Expected IQ shape [2, N] or complex [N], got {iq.shape}.")


def stitch_iq_windows(iq: np.ndarray, local_indices: list[int]) -> np.ndarray:
    if iq.ndim != 3 or iq.shape[1] != 2 or iq.shape[2] != BASE_IQ_WINDOW:
        raise ValueError(f"Expected iq array [N, 2, {BASE_IQ_WINDOW}], got {iq.shape}.")
    if not local_indices:
        raise ValueError("local_indices must not be empty.")
    expected = list(range(local_indices[0], local_indices[0] + len(local_indices)))
    if local_indices != expected:
        raise ValueError(f"Window indices must be contiguous, got {local_indices[:4]}...")
    selected = iq[np.asarray(local_indices, dtype=np.int64)]
    return selected.transpose(1, 0, 2).reshape(2, -1)


def rms_energy(iq_2ch: np.ndarray) -> float:
    x = complex_from_iq(iq_2ch)
    return float(np.sqrt(np.mean(np.abs(x) ** 2) + 1e-12))


def stft_fixed_coordinate(
    iq_2ch: np.ndarray,
    n_fft: int = 512,
    hop_length: int = 128,
    window: np.ndarray | None = None,
) -> np.ndarray:
    """Return log-power STFT without frequency recentering or signal cropping."""
    x = complex_from_iq(iq_2ch)
    if len(x) < n_fft:
        raise ValueError(f"Window length {len(x)} is shorter than n_fft={n_fft}.")
    win = np.hanning(n_fft).astype(np.float32) if window is None else window.astype(np.float32)
    frame_count = 1 + (len(x) - n_fft) // hop_length
    shape = (frame_count, n_fft)
    strides = (x.strides[0] * hop_length, x.strides[0])
    frames = np.lib.stride_tricks.as_strided(x, shape=shape, strides=strides)
    spectrum = np.fft.fftshift(np.fft.fft(frames * win[None, :], axis=1), axes=1)
    power = np.log1p(np.abs(spectrum) ** 2).astype(np.float32)
    return power.T


def binary_dilate(mask: np.ndarray, radius: int = 1) -> np.ndarray:
    out = mask.astype(bool, copy=True)
    for shift in range(1, radius + 1):
        out[:, shift:] |= mask[:, :-shift]
        out[:, :-shift] |= mask[:, shift:]
        out[shift:, :] |= mask[:-shift, :]
        out[:-shift, :] |= mask[shift:, :]
    return out


def binary_erode(mask: np.ndarray, radius: int = 1) -> np.ndarray:
    out = mask.astype(bool, copy=True)
    for shift in range(1, radius + 1):
        shifted = np.zeros_like(mask, dtype=bool)
        shifted[:, shift:] = mask[:, :-shift]
        out &= shifted
        shifted = np.zeros_like(mask, dtype=bool)
        shifted[:, :-shift] = mask[:, shift:]
        out &= shifted
        shifted = np.zeros_like(mask, dtype=bool)
        shifted[shift:, :] = mask[:-shift, :]
        out &= shifted
        shifted = np.zeros_like(mask, dtype=bool)
        shifted[:-shift, :] = mask[shift:, :]
        out &= shifted
    return out


def morphology_mask(spec: np.ndarray, threshold_sigma: float = 1.5, radius: int = 1) -> np.ndarray:
    """Create a quality/mask channel; this is not used to crop the spectrogram."""
    median = np.median(spec)
    mad = np.median(np.abs(spec - median)) + 1e-6
    threshold = median + threshold_sigma * 1.4826 * mad
    raw = spec > threshold
    closed = binary_erode(binary_dilate(raw, radius=radius), radius=radius)
    return closed.astype(np.float32)


def quality_score(spec: np.ndarray, mask: np.ndarray) -> dict[str, float]:
    occupied = float(mask.mean())
    if occupied <= 0.0:
        contrast = 0.0
    else:
        foreground = spec[mask > 0.5]
        background = spec[mask <= 0.5]
        contrast = float(foreground.mean() - background.mean()) if len(background) else 0.0
    return {"occupancy": occupied, "contrast": contrast}


def make_feature_tensor(iq_2ch: np.ndarray, n_fft: int, hop_length: int) -> tuple[np.ndarray, dict[str, float]]:
    spec = stft_fixed_coordinate(iq_2ch, n_fft=n_fft, hop_length=hop_length)
    mask = morphology_mask(spec)
    masked_spec = spec * mask
    stats = quality_score(spec, mask)
    stacked = np.stack([spec, mask, masked_spec], axis=0).astype(np.float32)
    return stacked, stats


def record_coordinate_payload(
    records: list[WindowRecord],
    window_size: int,
    feature_path: Path | None = None,
) -> dict:
    first = records[0]
    last = records[-1]
    payload = {
        "feature_path": str(feature_path) if feature_path else "",
        "registered_id": first.registered_id,
        "model_label": first.model_label,
        "folder_id": first.folder_id,
        "source_file": first.source_file,
        "npz_path": str(first.npz_path),
        "window_size": int(window_size),
        "base_window_indices": [record.window_index for record in records],
        "source_start_sample": first.source_start_sample,
        "source_end_sample": last.source_end_sample,
        "sample_rate_hz": first.sample_rate_hz,
        "center_frequency_hz": first.center_frequency_hz,
        "bandwidth_hz": first.bandwidth_hz,
        "rows": [asdict(record) | {"manifest_path": str(record.manifest_path), "npz_path": str(record.npz_path)} for record in records],
    }
    return payload

