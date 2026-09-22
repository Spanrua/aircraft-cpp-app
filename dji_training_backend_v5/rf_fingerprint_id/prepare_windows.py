#!/usr/bin/env python3
"""Prepare fixed-coordinate RF fingerprint windows from existing NPZ manifests."""

from __future__ import annotations

import argparse
import json
from collections import Counter, defaultdict
from collections.abc import Iterable
from pathlib import Path

import numpy as np
from tqdm import tqdm

from .manifest import WindowRecord, group_records_by_npz, load_window_records, natural_key
from .preprocess import (
    BASE_IQ_WINDOW,
    DEFAULT_WINDOW_SIZES,
    make_feature_tensor,
    record_coordinate_payload,
    rms_energy,
    stitch_iq_windows,
)


def parse_window_sizes(text: str) -> tuple[int, ...]:
    values = tuple(int(part.strip()) for part in text.split(",") if part.strip())
    if not values:
        raise argparse.ArgumentTypeError("At least one window size is required.")
    bad = [value for value in values if value <= 0 or value % BASE_IQ_WINDOW != 0]
    if bad:
        raise argparse.ArgumentTypeError(
            f"Window sizes must be positive multiples of {BASE_IQ_WINDOW}: {bad}"
        )
    return values


def iter_multiscale_groups(
    records: list[WindowRecord],
    window_sizes: tuple[int, ...],
    stride_windows: int,
) -> Iterable[list[WindowRecord]]:
    by_source: dict[str, list[WindowRecord]] = defaultdict(list)
    for record in records:
        by_source[record.source_file].append(record)
    for source_records in by_source.values():
        ordered = sorted(source_records, key=lambda item: item.window_index)
        for window_size in window_sizes:
            span = window_size // BASE_IQ_WINDOW
            if len(ordered) < span:
                continue
            for start in range(0, len(ordered) - span + 1, stride_windows):
                group = ordered[start : start + span]
                expected = list(range(group[0].window_index, group[0].window_index + span))
                actual = [record.window_index for record in group]
                if actual == expected:
                    yield group


def iter_all_multiscale_groups(
    grouped_by_npz: dict[Path, list[WindowRecord]],
    window_sizes: tuple[int, ...],
    stride_windows: int,
) -> Iterable[list[WindowRecord]]:
    for npz_path in sorted(grouped_by_npz, key=natural_key):
        yield from iter_multiscale_groups(
            grouped_by_npz[npz_path],
            window_sizes=window_sizes,
            stride_windows=stride_windows,
        )


def scan_group_stats(groups: Iterable[list[WindowRecord]]) -> dict:
    total = 0
    by_window_size: Counter[int] = Counter()
    for group in groups:
        total += 1
        by_window_size[len(group) * BASE_IQ_WINDOW] += 1
    return {
        "multiscale_groups": total,
        "window_size_counts": {
            str(size): int(count)
            for size, count in sorted(by_window_size.items())
        },
    }


def summarize(records: list[WindowRecord], group_stats: dict) -> dict:
    ids = Counter(record.registered_id for record in records)
    model_labels = Counter(record.model_label for record in records)
    source_counts = Counter(record.source_file for record in records)
    sample_rates = Counter(record.sample_rate_hz for record in records if record.sample_rate_hz)
    centers = Counter(record.center_frequency_hz for record in records if record.center_frequency_hz)
    return {
        "manifest_records": len(records),
        "registered_ids": dict(sorted(ids.items(), key=lambda item: natural_key(item[0]))),
        "model_labels": dict(model_labels.most_common()),
        "source_files": len(source_counts),
        "multiscale_groups": int(group_stats["multiscale_groups"]),
        "window_size_counts": group_stats["window_size_counts"],
        "sample_rates_hz": dict(sample_rates.most_common(8)),
        "center_frequencies_hz": dict(centers.most_common(8)),
        "notes": [
            "registered_id defaults to the dataset folder, not the manifest model label.",
            "The STFT keeps fixed time/frequency coordinates and does not recenter or crop signal blobs.",
            "Morphology is emitted as mask/quality only; it is not the primary cropper.",
            "background and unknown are intentionally absent unless provided by separate manifests/data roots.",
        ],
    }


def export_features(
    data_root: Path,
    output_dir: Path,
    groups: Iterable[list[WindowRecord]],
    n_fft: int,
    hop_length: int,
    rms_min: float,
    max_exports: int,
) -> dict:
    features_dir = output_dir / "features"
    features_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = output_dir / "fingerprint_windows.jsonl"
    exported = 0
    skipped_low_rms = 0
    current_npz_path: Path | None = None
    current_iq: np.ndarray | None = None

    with manifest_path.open("w", encoding="utf-8") as handle:
        for group_index, records in enumerate(tqdm(groups, desc="Export fingerprint windows", unit="window")):
            if max_exports and exported >= max_exports:
                break
            npz_path = records[0].npz_path
            if current_npz_path != npz_path or current_iq is None:
                with np.load(npz_path, allow_pickle=False) as data:
                    current_iq = np.asarray(data["iq"], dtype=np.float32)
                current_npz_path = npz_path
            local_indices = [record.window_index for record in records]
            iq_2ch = stitch_iq_windows(current_iq, local_indices)
            energy = rms_energy(iq_2ch)
            if energy < rms_min:
                skipped_low_rms += 1
                continue
            tensor, stats = make_feature_tensor(iq_2ch, n_fft=n_fft, hop_length=hop_length)
            feature_path = features_dir / f"fpwin_{group_index:08d}.npz"
            np.savez_compressed(
                feature_path,
                x=tensor.astype(np.float16),
                rms=np.float32(energy),
                occupancy=np.float32(stats["occupancy"]),
                contrast=np.float32(stats["contrast"]),
            )
            payload = record_coordinate_payload(
                records,
                window_size=len(records) * BASE_IQ_WINDOW,
                feature_path=feature_path.relative_to(output_dir),
            )
            payload.update({"rms": energy, **stats})
            handle.write(json.dumps(payload, ensure_ascii=False) + "\n")
            exported += 1

    return {
        "feature_manifest": str(manifest_path),
        "exported": exported,
        "skipped_low_rms": skipped_low_rms,
        "data_root": str(data_root),
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build an independent fixed-coordinate RF fingerprinting manifest/features."
    )
    parser.add_argument("--data-root", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--id-field", default="folder", help="Manifest field to use as registered id, or 'folder'.")
    parser.add_argument("--window-sizes", type=parse_window_sizes, default=DEFAULT_WINDOW_SIZES)
    parser.add_argument("--stride-windows", type=int, default=1)
    parser.add_argument("--n-fft", type=int, default=512)
    parser.add_argument("--hop-length", type=int, default=128)
    parser.add_argument("--rms-min", type=float, default=0.0)
    parser.add_argument("--max-exports", type=int, default=0)
    parser.add_argument("--max-records", type=int, default=0, help="Limit manifest rows for fast audit/smoke runs.")
    parser.add_argument(
        "--max-records-per-manifest",
        type=int,
        default=0,
        help="Limit rows read from each manifest so fast audits still cover classes.",
    )
    parser.add_argument("--audit-only", action="store_true")
    args = parser.parse_args()

    if args.stride_windows <= 0:
        raise ValueError("--stride-windows must be positive.")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    records = load_window_records(
        args.data_root,
        id_field=args.id_field,
        max_records=args.max_records,
        max_records_per_manifest=args.max_records_per_manifest,
        show_progress=True,
    )
    grouped_by_npz = group_records_by_npz(records)
    group_stats = scan_group_stats(
        iter_all_multiscale_groups(
            grouped_by_npz,
            window_sizes=args.window_sizes,
            stride_windows=args.stride_windows,
        )
    )
    report = summarize(records, group_stats)
    report.update(
        {
            "data_root": str(args.data_root),
            "output_dir": str(args.output_dir),
            "window_sizes": list(args.window_sizes),
            "stride_windows": args.stride_windows,
            "n_fft": args.n_fft,
            "hop_length": args.hop_length,
            "audit_only": args.audit_only,
            "max_records": args.max_records,
            "max_records_per_manifest": args.max_records_per_manifest,
        }
    )

    if not args.audit_only:
        report["export"] = export_features(
            data_root=args.data_root,
            output_dir=args.output_dir,
            groups=iter_all_multiscale_groups(
                grouped_by_npz,
                window_sizes=args.window_sizes,
                stride_windows=args.stride_windows,
            ),
            n_fft=args.n_fft,
            hop_length=args.hop_length,
            rms_min=args.rms_min,
            max_exports=args.max_exports,
        )

    report_path = args.output_dir / "fingerprint_prepare_report.json"
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(
        json.dumps(
            {"report": str(report_path), "groups": report["multiscale_groups"]},
            ensure_ascii=False,
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
