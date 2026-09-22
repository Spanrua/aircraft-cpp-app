"""Manifest loading for RF emitter individual identification."""

from __future__ import annotations

import json
import re
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Iterable

from tqdm import tqdm


@dataclass(frozen=True)
class WindowRecord:
    manifest_path: Path
    npz_path: Path
    folder_id: str
    registered_id: str
    model_label: str
    source_file: str
    window_index: int
    source_start_sample: int | None
    source_end_sample: int | None
    sample_rate_hz: float | None
    center_frequency_hz: float | None
    bandwidth_hz: float | None
    score: float | None
    occupancy: float | None
    row_index: int


def natural_key(value: object) -> list[object]:
    parts = re.split(r"(\d+)", str(value).lower())
    return [int(part) if part.isdigit() else part for part in parts]


def normalize_path_text(path_text: object) -> str:
    return str(path_text).replace("\\", "/")


def read_jsonl(path: Path, show_progress: bool = False) -> Iterable[tuple[int, dict]]:
    with path.open("r", encoding="utf-8") as handle:
        rows = tqdm(
            handle,
            desc=f"Scan {path.parent.name}",
            unit="row",
            mininterval=2.0,
            leave=False,
        ) if show_progress else handle
        for row_index, line in enumerate(rows):
            line = line.strip()
            if not line:
                continue
            item = json.loads(line)
            if isinstance(item, dict):
                yield row_index, item


def resolve_manifest_path(raw_path: object, manifest_path: Path, data_root: Path) -> Path | None:
    if not raw_path:
        return None

    native = Path(str(raw_path))
    if native.exists():
        return native

    normalized = normalize_path_text(raw_path)
    posix_path = PurePosixPath(normalized)
    parts = list(posix_path.parts)
    if data_root.name in parts:
        idx = parts.index(data_root.name)
        candidate = data_root.joinpath(*parts[idx + 1 :])
        if candidate.exists():
            return candidate

    candidate = manifest_path.parent / posix_path.name
    if candidate.exists():
        return candidate

    matches = sorted(data_root.rglob(posix_path.name), key=natural_key)
    return matches[0] if matches else None


def _folder_id(data_root: Path, manifest_path: Path, npz_path: Path) -> str:
    try:
        return npz_path.relative_to(data_root).parts[0]
    except (OSError, ValueError, IndexError):
        return manifest_path.parent.name


def _int_or_none(value: object) -> int | None:
    if value in (None, ""):
        return None
    return int(value)


def _float_or_none(value: object) -> float | None:
    if value in (None, ""):
        return None
    return float(value)


def load_window_records(
    data_root: Path,
    id_field: str = "folder",
    max_records: int = 0,
    max_records_per_manifest: int = 0,
    show_progress: bool = False,
) -> list[WindowRecord]:
    """Load manifest rows while preserving model label and registered individual id.

    The existing manifests use the row label for DJI model/family names such as
    B_Air2. For individual identification the default registered id is the
    dataset folder, for example air2_01.
    """
    data_root = Path(data_root)
    records: list[WindowRecord] = []
    path_cache: dict[tuple[Path, str], Path | None] = {}
    folder_cache: dict[Path, str] = {}
    for manifest_path in sorted(data_root.rglob("manifest.jsonl"), key=natural_key):
        manifest_records = 0
        for row_index, item in read_jsonl(manifest_path, show_progress=show_progress):
            raw_path = str(item.get("path") or "")
            cache_key = (manifest_path, raw_path)
            if cache_key not in path_cache:
                path_cache[cache_key] = resolve_manifest_path(raw_path, manifest_path, data_root)
            npz_path = path_cache[cache_key]
            if npz_path is None:
                continue
            if npz_path not in folder_cache:
                folder_cache[npz_path] = _folder_id(data_root, manifest_path, npz_path)
            folder_id = folder_cache[npz_path]
            if id_field == "folder":
                registered_id = folder_id
            else:
                registered_id = str(item.get(id_field) or folder_id)
            records.append(
                WindowRecord(
                    manifest_path=manifest_path,
                    npz_path=npz_path,
                    folder_id=folder_id,
                    registered_id=registered_id,
                    model_label=str(item.get("label") or ""),
                    source_file=str(item.get("source_file") or npz_path.stem),
                    window_index=int(
                        item.get("window_index", item.get("sample_index", row_index))
                    ),
                    source_start_sample=_int_or_none(item.get("source_start_sample")),
                    source_end_sample=_int_or_none(item.get("source_end_sample")),
                    sample_rate_hz=_float_or_none(item.get("sample_rate_hz")),
                    center_frequency_hz=_float_or_none(item.get("center_frequency_hz")),
                    bandwidth_hz=_float_or_none(item.get("bandwidth_hz")),
                    score=_float_or_none(item.get("score")),
                    occupancy=_float_or_none(item.get("occupancy")),
                    row_index=row_index,
                )
            )
            manifest_records += 1
            if max_records and len(records) >= max_records:
                return records
            if max_records_per_manifest and manifest_records >= max_records_per_manifest:
                break
    return records


def group_records_by_npz(records: Iterable[WindowRecord]) -> dict[Path, list[WindowRecord]]:
    grouped: dict[Path, list[WindowRecord]] = {}
    for record in records:
        grouped.setdefault(record.npz_path, []).append(record)
    for path in list(grouped):
        grouped[path] = sorted(grouped[path], key=lambda item: item.window_index)
    return grouped
