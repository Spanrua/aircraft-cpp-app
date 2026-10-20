#!/usr/bin/env python3
"""Analyze PSC sweep CSV files for video-link candidate bands and weak labels.

This tool intentionally uses only sweep CSV files.  It does not read IQRAW,
CAP, NPZ, or any other IQ data.  The supported CSV shapes are:

* frame_index,bin_index,frequency_hz,level_dbuv
* frame_index,bin_index,frequency_hz,level_dbuv,drone_id,model,behavior_code,behavior_name
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
import statistics
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from datetime import datetime, timedelta
from pathlib import Path
from typing import Iterable, Sequence

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError as exc:  # pragma: no cover - exercised only on misconfigured machines
    raise SystemExit("Pillow is required to render waterfall plots.") from exc


TIMESTAMP_RE = re.compile(r"_(\d{8})_(\d{6})_(\d{3})(?:_part(\d+))?\.csv$", re.IGNORECASE)
DEFAULT_INPUT_ROOT = Path(__file__).resolve().parents[1] / "data" / "sweep"
DEFAULT_OUTPUT_ROOT = DEFAULT_INPUT_ROOT / "analysis_results"

REQUIRED_COLUMNS = ("frame_index", "bin_index", "frequency_hz", "level_dbuv")
OPTIONAL_METADATA_COLUMNS = ("drone_id", "model", "behavior_code", "behavior_name")

INVALID_LEVEL_DB = 199.0
ABSOLUTE_HIT_LEVEL_DB = 20.0
RELATIVE_HIT_MARGIN_DB = 8.0
MIN_HOT_BINS = 2
SINGLE_BIN_PEAK_DB = 45.0
MAX_FRAME_GAP_CYCLES = 2
MAX_PLOT_COUNT = 24
MAX_WATERFALL_ROWS = 200
WATERFALL_COLOR_LOW_DB = -30.0
WATERFALL_COLOR_HIGH_DB = 45.0
OVERVIEW_IMAGE_COUNT = 12
SIGNAL_PROBABILITY_THRESHOLD = 0.50
VIDEO_LINK_BANDWIDTH_MHZ = (10.0, 20.0, 40.0)
DEEP_INPUT_HEIGHT = 96
DEEP_INPUT_WIDTH = 256
DEEP_CROP_HALF_BANDWIDTH_MHZ = 30.0
DEEP_INPUT_CHANNELS = 3
DEEP_DEFAULT_EPOCHS = 80
DEEP_DEFAULT_BATCH_SIZE = 16
DEEP_EARLY_STOPPING_PATIENCE = 12
DEEP_MAX_SHIFT_PIXELS = 32
DEEP_MIN_EPOCHS = 8


@dataclass(frozen=True)
class SweepMetadata:
    drone_id: str = ""
    model: str = ""
    behavior_code: str = ""
    behavior_name: str = ""

    @property
    def normalized_model(self) -> str:
        return normalize_label(self.model)

    @property
    def signal_noise_label(self) -> str:
        model = self.normalized_model
        if not model:
            return ""
        return "noise" if model == "noise" else "signal"

    @property
    def type_label(self) -> str:
        model = self.normalized_model
        if not model or model == "noise":
            return ""
        return model


@dataclass(frozen=True)
class FrameHit:
    frame_index: int
    start_hz: float
    stop_hz: float
    peak_frequency_hz: float
    peak_level_dbuv: float
    background_dbuv: float
    hot_bin_count: int
    step_hz: float

    @property
    def bandwidth_hz(self) -> float:
        return max(0.0, self.stop_hz - self.start_hz)

    @property
    def peak_margin_db(self) -> float:
        return self.peak_level_dbuv - self.background_dbuv


@dataclass(frozen=True)
class FrameStats:
    frame_index: int
    start_hz: float
    stop_hz: float
    bin_count: int
    background_dbuv: float
    mean_level_dbuv: float
    peak_level_dbuv: float
    peak_margin_db: float
    hot_bin_ratio: float
    hot_bin_count: int
    level_std_db: float


@dataclass
class FileAnalysis:
    path: Path
    timestamp: datetime | None
    part_number: int | None
    metadata: SweepMetadata
    total_frames: int
    first_frame_index: int
    last_frame_index: int
    valid_rows: int
    invalid_rows: int
    min_frequency_hz: float
    max_frequency_hz: float
    step_hz: float
    frame_ranges: list[tuple[int, float, float]]
    frame_stats: list[FrameStats]
    hits: list[FrameHit]
    features: dict[str, float] = field(default_factory=dict)
    events: list["CandidateEvent"] = field(default_factory=list)

    @property
    def duration_s(self) -> float:
        return 10.0

    @property
    def cycle_length(self) -> int:
        return infer_cycle_length(self.frame_ranges, self.step_hz)


@dataclass
class CandidateEvent:
    source_file: str
    timestamp: datetime | None
    part_number: int | None
    total_frames: int
    first_frame_index: int
    last_frame_index: int
    frame_hits: list[FrameHit] = field(default_factory=list)

    def add(self, hit: FrameHit) -> None:
        self.frame_hits.append(hit)
        self.first_frame_index = min(self.first_frame_index, hit.frame_index)
        self.last_frame_index = max(self.last_frame_index, hit.frame_index)

    @property
    def hit_frame_count(self) -> int:
        return len({hit.frame_index for hit in self.frame_hits})

    @property
    def start_frequency_hz(self) -> float:
        return min(hit.start_hz for hit in self.frame_hits)

    @property
    def stop_frequency_hz(self) -> float:
        return max(hit.stop_hz for hit in self.frame_hits)

    @property
    def center_frequency_hz(self) -> float:
        return (self.start_frequency_hz + self.stop_frequency_hz) / 2.0

    @property
    def observed_bandwidth_hz(self) -> float:
        return max(0.0, self.stop_frequency_hz - self.start_frequency_hz)

    @property
    def nominal_bandwidth_mhz(self) -> float:
        return nominal_video_bandwidth_mhz(self.observed_bandwidth_hz / 1e6)

    @property
    def nominal_bandwidth_hz(self) -> float:
        return self.nominal_bandwidth_mhz * 1e6

    @property
    def nominal_start_frequency_hz(self) -> float:
        return self.center_frequency_hz - self.nominal_bandwidth_hz / 2.0

    @property
    def nominal_stop_frequency_hz(self) -> float:
        return self.center_frequency_hz + self.nominal_bandwidth_hz / 2.0

    @property
    def bandwidth_match_score(self) -> float:
        return video_bandwidth_match_score(self.observed_bandwidth_hz / 1e6, self.nominal_bandwidth_mhz)

    @property
    def bandwidth_coverage_ratio(self) -> float:
        nominal = self.nominal_bandwidth_hz
        return min(self.observed_bandwidth_hz / nominal, 1.0) if nominal > 0 else 0.0

    @property
    def peak_hit(self) -> FrameHit:
        return max(self.frame_hits, key=lambda hit: hit.peak_level_dbuv)

    @property
    def peak_level_dbuv(self) -> float:
        return self.peak_hit.peak_level_dbuv

    @property
    def max_bandwidth_hz(self) -> float:
        return max(hit.bandwidth_hz for hit in self.frame_hits)

    @property
    def median_bandwidth_hz(self) -> float:
        return safe_median(hit.bandwidth_hz for hit in self.frame_hits)

    @property
    def average_peak_margin_db(self) -> float:
        return safe_mean(hit.peak_margin_db for hit in self.frame_hits)

    @property
    def first_hit(self) -> FrameHit:
        return min(self.frame_hits, key=lambda hit: hit.frame_index)

    @property
    def last_hit(self) -> FrameHit:
        return max(self.frame_hits, key=lambda hit: hit.frame_index)

    def eligible_frame_count(self, file_analysis: FileAnalysis) -> int:
        return sum(
            1
            for _, frame_start, frame_stop in file_analysis.frame_ranges
            if frame_stop >= self.start_frequency_hz and frame_start <= self.stop_frequency_hz
        )

    def eligible_hit_ratio(self, file_analysis: FileAnalysis) -> float:
        eligible = self.eligible_frame_count(file_analysis)
        return self.hit_frame_count / eligible if eligible else 0.0


def normalize_label(value: object) -> str:
    return str(value or "").strip().lower().replace(" ", "").replace("-", "_")


def nominal_video_bandwidth_mhz(measured_bandwidth_mhz: float) -> float:
    """Map a measured hot span to the nearest expected video-link channel width."""
    if not math.isfinite(measured_bandwidth_mhz) or measured_bandwidth_mhz <= 0:
        return VIDEO_LINK_BANDWIDTH_MHZ[0]

    previous = VIDEO_LINK_BANDWIDTH_MHZ[0]
    for current in VIDEO_LINK_BANDWIDTH_MHZ[1:]:
        boundary = (previous + current) / 2.0
        if measured_bandwidth_mhz <= boundary:
            return previous
        previous = current
    return VIDEO_LINK_BANDWIDTH_MHZ[-1]


def video_bandwidth_match_score(measured_bandwidth_mhz: float, nominal_bandwidth_mhz: float) -> float:
    if nominal_bandwidth_mhz <= 0 or not math.isfinite(measured_bandwidth_mhz):
        return 0.0
    return max(0.0, min(1.0, 1.0 - abs(measured_bandwidth_mhz - nominal_bandwidth_mhz) / nominal_bandwidth_mhz))


def parse_file_timestamp(name: str) -> tuple[datetime | None, int | None]:
    match = TIMESTAMP_RE.search(name)
    if not match:
        return None, None
    stamp = datetime.strptime(
        f"{match.group(1)}_{match.group(2)}_{match.group(3)}",
        "%Y%m%d_%H%M%S_%f",
    )
    part_number = int(match.group(4)) if match.group(4) else None
    return stamp, part_number


def validate_header(header: Sequence[str] | None, path: Path) -> list[str]:
    if header is None:
        raise ValueError(f"Missing CSV header in {path.name}")
    normalized = [item.strip() for item in header]
    if list(normalized[: len(REQUIRED_COLUMNS)]) != list(REQUIRED_COLUMNS):
        raise ValueError(f"Unexpected PSC CSV header in {path.name}: {header}")
    return normalized


def row_value(row: Sequence[str], header_index: dict[str, int], name: str) -> str:
    index = header_index.get(name)
    if index is None or index >= len(row):
        return ""
    return row[index].strip()


def update_metadata(metadata: SweepMetadata, row: Sequence[str], header_index: dict[str, int]) -> SweepMetadata:
    values = {
        "drone_id": metadata.drone_id,
        "model": metadata.model,
        "behavior_code": metadata.behavior_code,
        "behavior_name": metadata.behavior_name,
    }
    changed = False
    for key in OPTIONAL_METADATA_COLUMNS:
        if values[key]:
            continue
        value = row_value(row, header_index, key).strip().strip('"')
        if value:
            values[key] = value
            changed = True
    return SweepMetadata(**values) if changed else metadata


def safe_mean(values: Iterable[float]) -> float:
    items = [value for value in values if math.isfinite(value)]
    return float(sum(items) / len(items)) if items else 0.0


def safe_median(values: Iterable[float]) -> float:
    items = [value for value in values if math.isfinite(value)]
    return float(statistics.median(items)) if items else 0.0


def safe_stdev(values: Iterable[float]) -> float:
    items = [value for value in values if math.isfinite(value)]
    return float(statistics.pstdev(items)) if len(items) >= 2 else 0.0


def percentile(values: Sequence[float], fraction: float) -> float:
    items = sorted(value for value in values if math.isfinite(value))
    if not items:
        return 0.0
    if len(items) == 1:
        return float(items[0])
    position = (len(items) - 1) * fraction
    lower = int(math.floor(position))
    upper = int(math.ceil(position))
    if lower == upper:
        return float(items[lower])
    ratio = position - lower
    return float(items[lower] * (1.0 - ratio) + items[upper] * ratio)


def estimate_step_hz(points: Sequence[tuple[float, float]]) -> float:
    diffs = [
        right[0] - left[0]
        for left, right in zip(points, points[1:])
        if right[0] > left[0]
    ]
    if not diffs:
        return 0.0
    if len(diffs) > 128:
        diffs = diffs[:64] + diffs[-64:]
    return float(statistics.median(diffs))


def split_contiguous(
    points: Sequence[tuple[float, float]],
    max_gap_hz: float,
) -> list[list[tuple[float, float]]]:
    if not points:
        return []
    runs: list[list[tuple[float, float]]] = []
    current: list[tuple[float, float]] = [points[0]]
    for point in points[1:]:
        if point[0] - current[-1][0] > max_gap_hz:
            runs.append(current)
            current = [point]
        else:
            current.append(point)
    runs.append(current)
    return runs


def frame_background(points: Sequence[tuple[float, float]]) -> float:
    return safe_median(level for _, level in points)


def frame_threshold(background_dbuv: float) -> float:
    return max(ABSOLUTE_HIT_LEVEL_DB, background_dbuv + RELATIVE_HIT_MARGIN_DB)


def detect_frame_hits(
    frame_index: int,
    points: Sequence[tuple[float, float]],
) -> list[FrameHit]:
    if len(points) < 2:
        return []

    step_hz = estimate_step_hz(points)
    if step_hz <= 0:
        return []

    background_dbuv = frame_background(points)
    threshold_dbuv = frame_threshold(background_dbuv)
    hot_points = [
        (frequency_hz, level_dbuv)
        for frequency_hz, level_dbuv in points
        if level_dbuv >= threshold_dbuv
    ]
    if not hot_points:
        return []

    max_gap_hz = max(step_hz * 1.5, 25_000.0)
    hits: list[FrameHit] = []
    for run in split_contiguous(hot_points, max_gap_hz):
        peak_frequency_hz, peak_level_dbuv = max(run, key=lambda point: point[1])
        if len(run) < MIN_HOT_BINS and peak_level_dbuv < SINGLE_BIN_PEAK_DB:
            continue
        hits.append(
            FrameHit(
                frame_index=frame_index,
                start_hz=run[0][0],
                stop_hz=run[-1][0],
                peak_frequency_hz=peak_frequency_hz,
                peak_level_dbuv=peak_level_dbuv,
                background_dbuv=background_dbuv,
                hot_bin_count=len(run),
                step_hz=step_hz,
            )
        )
    return hits


def summarize_frame(frame_index: int, points: Sequence[tuple[float, float]]) -> FrameStats | None:
    if not points:
        return None
    levels = [level for _, level in points]
    background_dbuv = frame_background(points)
    threshold_dbuv = frame_threshold(background_dbuv)
    peak_level = max(levels)
    hot_count = sum(1 for level in levels if level >= threshold_dbuv)
    return FrameStats(
        frame_index=frame_index,
        start_hz=points[0][0],
        stop_hz=points[-1][0],
        bin_count=len(points),
        background_dbuv=background_dbuv,
        mean_level_dbuv=safe_mean(levels),
        peak_level_dbuv=peak_level,
        peak_margin_db=peak_level - background_dbuv,
        hot_bin_ratio=hot_count / len(points) if points else 0.0,
        hot_bin_count=hot_count,
        level_std_db=safe_stdev(levels),
    )


def read_file_analysis(path: Path, include_frequency_features: bool = False) -> FileAnalysis:
    timestamp, part_number = parse_file_timestamp(path.name)
    frame_hits: list[FrameHit] = []
    frame_ranges: list[tuple[int, float, float]] = []
    frame_stats: list[FrameStats] = []
    valid_rows = 0
    invalid_rows = 0
    min_frequency_hz = math.inf
    max_frequency_hz = -math.inf
    frame_indices: list[int] = []
    step_samples: list[float] = []
    metadata = SweepMetadata()

    current_frame: int | None = None
    points: list[tuple[float, float]] = []

    def flush_frame() -> None:
        nonlocal points, current_frame
        if current_frame is None:
            return
        if points:
            frame_ranges.append((current_frame, points[0][0], points[-1][0]))
            step_hz = estimate_step_hz(points)
            if step_hz > 0:
                step_samples.append(step_hz)
            stats = summarize_frame(current_frame, points)
            if stats is not None:
                frame_stats.append(stats)
            frame_hits.extend(detect_frame_hits(current_frame, points))
        frame_indices.append(current_frame)
        points = []

    with path.open("r", newline="", encoding="utf-8-sig") as handle:
        reader = csv.reader(handle)
        header = validate_header(next(reader, None), path)
        header_index = {name: index for index, name in enumerate(header)}

        for row in reader:
            if len(row) < len(REQUIRED_COLUMNS):
                invalid_rows += 1
                continue
            metadata = update_metadata(metadata, row, header_index)
            try:
                frame_index = int(row_value(row, header_index, "frame_index"))
                frequency_hz = float(row_value(row, header_index, "frequency_hz"))
                level_dbuv = float(row_value(row, header_index, "level_dbuv"))
            except ValueError:
                invalid_rows += 1
                continue

            if current_frame is None:
                current_frame = frame_index
            elif frame_index != current_frame:
                flush_frame()
                current_frame = frame_index

            if (
                not math.isfinite(frequency_hz)
                or not math.isfinite(level_dbuv)
                or frequency_hz <= 0
                or level_dbuv >= INVALID_LEVEL_DB
            ):
                invalid_rows += 1
                continue

            valid_rows += 1
            min_frequency_hz = min(min_frequency_hz, frequency_hz)
            max_frequency_hz = max(max_frequency_hz, frequency_hz)
            points.append((frequency_hz, level_dbuv))

        flush_frame()

    if not frame_indices or not frame_ranges:
        raise ValueError(f"No valid sweep frames found in {path.name}")

    analysis = FileAnalysis(
        path=path,
        timestamp=timestamp,
        part_number=part_number,
        metadata=metadata,
        total_frames=len(frame_indices),
        first_frame_index=min(frame_indices),
        last_frame_index=max(frame_indices),
        valid_rows=valid_rows,
        invalid_rows=invalid_rows,
        min_frequency_hz=min_frequency_hz,
        max_frequency_hz=max_frequency_hz,
        step_hz=float(statistics.median(step_samples)) if step_samples else 0.0,
        frame_ranges=frame_ranges,
        frame_stats=frame_stats,
        hits=frame_hits,
    )
    analysis.events = merge_hits(analysis)
    analysis.features = build_file_features(analysis, include_frequency_features)
    return analysis


def infer_cycle_length(
    frame_ranges: Sequence[tuple[int, float, float]],
    step_hz: float,
) -> int:
    if len(frame_ranges) < 4:
        return 1
    starts = [frame_start for _, frame_start, _ in frame_ranges]
    tolerance_hz = max(step_hz * 2.0, 100_000.0)
    max_period = min(32, len(starts) // 2)
    for period in range(1, max_period + 1):
        comparisons = len(starts) - period
        matches = sum(
            abs(starts[index] - starts[index - period]) <= tolerance_hz
            for index in range(period, len(starts))
        )
        if comparisons > 0 and matches / comparisons >= 0.95:
            return period
    return 1


def frequency_gap_hz(left_start: float, left_stop: float, right_start: float, right_stop: float) -> float:
    if right_start > left_stop:
        return right_start - left_stop
    if left_start > right_stop:
        return left_start - right_stop
    return 0.0


def merge_hits(file_analysis: FileAnalysis) -> list[CandidateEvent]:
    hits = sorted(file_analysis.hits, key=lambda hit: (hit.frame_index, hit.start_hz))
    if not hits:
        return []

    frame_gap_limit = max(2, file_analysis.cycle_length * MAX_FRAME_GAP_CYCLES)
    frequency_tolerance_hz = max(file_analysis.step_hz * 2.0, 250_000.0)
    events: list[CandidateEvent] = []

    for hit in hits:
        matching: list[tuple[float, CandidateEvent]] = []
        for event in events:
            if hit.frame_index - event.last_frame_index > frame_gap_limit:
                continue
            gap = frequency_gap_hz(
                event.start_frequency_hz,
                event.stop_frequency_hz,
                hit.start_hz,
                hit.stop_hz,
            )
            if gap <= frequency_tolerance_hz:
                matching.append((gap, event))

        if matching:
            _, event = min(matching, key=lambda item: item[0])
            event.add(hit)
        else:
            events.append(
                CandidateEvent(
                    source_file=file_analysis.path.name,
                    timestamp=file_analysis.timestamp,
                    part_number=file_analysis.part_number,
                    total_frames=file_analysis.total_frames,
                    first_frame_index=hit.frame_index,
                    last_frame_index=hit.frame_index,
                    frame_hits=[hit],
                )
            )

    return [
        event
        for event in events
        if event.hit_frame_count >= 2
    ]


def clamp(value: float, lower: float, upper: float) -> float:
    return max(lower, min(upper, value))


def classify_event(event: CandidateEvent, file_analysis: FileAnalysis) -> tuple[str, float, str]:
    eligible_frames = event.eligible_frame_count(file_analysis)
    eligible_ratio = event.hit_frame_count / eligible_frames if eligible_frames else 0.0
    peak = event.peak_level_dbuv
    width_mhz = event.observed_bandwidth_hz / 1e6
    margin = event.average_peak_margin_db
    bandwidth_match = event.bandwidth_match_score

    strength_score = clamp((peak - ABSOLUTE_HIT_LEVEL_DB) / 45.0, 0.0, 1.0)
    persistence_score = clamp(eligible_ratio / 0.60, 0.0, 1.0)
    width_score = 0.70 * bandwidth_match + 0.30 * clamp(width_mhz / 10.0, 0.0, 1.0)
    margin_score = clamp(margin / 30.0, 0.0, 1.0)
    score = 100.0 * (
        0.35 * strength_score
        + 0.30 * persistence_score
        + 0.20 * width_score
        + 0.15 * margin_score
    )

    if event.hit_frame_count >= 8 and eligible_ratio >= 0.20 and peak >= 35.0 and bandwidth_match >= 0.35:
        return "A", score, f"强度、持续性明显，匹配约{event.nominal_bandwidth_mhz:.0f}MHz图传带宽"
    if (
        event.hit_frame_count >= 5
        and eligible_ratio >= 0.10
        and peak >= 30.0
        and bandwidth_match >= 0.20
    ) or (
        event.hit_frame_count >= 3
        and peak >= 45.0
        and width_mhz >= 1.0
    ):
        return "B", score, f"存在重复命中或强峰特征，估计为{event.nominal_bandwidth_mhz:.0f}MHz图传带宽"
    return "C", score, "命中次数或持续性有限，需人工复核"


def event_sort_key(item: tuple[CandidateEvent, FileAnalysis]) -> tuple[int, float, float]:
    event, file_analysis = item
    grade, score, _ = classify_event(event, file_analysis)
    grade_rank = {"A": 0, "B": 1, "C": 2}[grade]
    return grade_rank, -score, -event.peak_level_dbuv


def top_event(file_analysis: FileAnalysis) -> CandidateEvent | None:
    if not file_analysis.events:
        return None
    return sorted(((event, file_analysis) for event in file_analysis.events), key=event_sort_key)[0][0]


def build_file_features(file_analysis: FileAnalysis, include_frequency_features: bool = False) -> dict[str, float]:
    stats = file_analysis.frame_stats
    events = file_analysis.events
    top = top_event(file_analysis)
    qualified_events = [event for event in events if classify_event(event, file_analysis)[0] in {"A", "B"}]
    grades = Counter(classify_event(event, file_analysis)[0] for event in events)

    peak_levels = [item.peak_level_dbuv for item in stats]
    backgrounds = [item.background_dbuv for item in stats]
    margins = [item.peak_margin_db for item in stats]
    hot_ratios = [item.hot_bin_ratio for item in stats]
    stds = [item.level_std_db for item in stats]
    event_bandwidths = [event.observed_bandwidth_hz / 1e6 for event in events]
    event_nominal_bandwidths = [event.nominal_bandwidth_mhz for event in events]
    event_bandwidth_matches = [event.bandwidth_match_score for event in events]
    event_scores = [classify_event(event, file_analysis)[1] for event in events]
    frequency_span_hz = max(0.0, file_analysis.max_frequency_hz - file_analysis.min_frequency_hz)

    top_eligible_frames = top.eligible_frame_count(file_analysis) if top else 0
    top_hit_ratio = top.eligible_hit_ratio(file_analysis) if top else 0.0
    top_score = classify_event(top, file_analysis)[1] if top else 0.0

    features = {
        "total_frames": float(file_analysis.total_frames),
        "valid_rows": float(file_analysis.valid_rows),
        "invalid_row_ratio": file_analysis.invalid_rows / max(file_analysis.valid_rows + file_analysis.invalid_rows, 1),
        "frequency_span_mhz": frequency_span_hz / 1e6,
        "step_khz": file_analysis.step_hz / 1e3,
        "cycle_length": float(file_analysis.cycle_length),
        "frame_background_median": safe_median(backgrounds),
        "frame_background_std": safe_stdev(backgrounds),
        "frame_peak_max": max(peak_levels) if peak_levels else 0.0,
        "frame_peak_median": safe_median(peak_levels),
        "frame_peak_p95": percentile(peak_levels, 0.95),
        "frame_peak_margin_max": max(margins) if margins else 0.0,
        "frame_peak_margin_median": safe_median(margins),
        "frame_peak_margin_p95": percentile(margins, 0.95),
        "frame_hot_ratio_max": max(hot_ratios) if hot_ratios else 0.0,
        "frame_hot_ratio_mean": safe_mean(hot_ratios),
        "frame_hot_ratio_p95": percentile(hot_ratios, 0.95),
        "frame_level_std_mean": safe_mean(stds),
        "frame_level_std_p95": percentile(stds, 0.95),
        "hit_count": float(len(file_analysis.hits)),
        "event_count": float(len(events)),
        "qualified_event_count": float(len(qualified_events)),
        "grade_a_count": float(grades.get("A", 0)),
        "grade_b_count": float(grades.get("B", 0)),
        "top_event_score": top_score,
        "top_event_peak_level": top.peak_level_dbuv if top else 0.0,
        "top_event_peak_margin": top.average_peak_margin_db if top else 0.0,
        "top_event_bandwidth_mhz": top.observed_bandwidth_hz / 1e6 if top else 0.0,
        "top_event_nominal_bandwidth_mhz": top.nominal_bandwidth_mhz if top else 0.0,
        "top_event_bandwidth_match_score": top.bandwidth_match_score if top else 0.0,
        "top_event_bandwidth_coverage_ratio": top.bandwidth_coverage_ratio if top else 0.0,
        "top_event_median_bandwidth_mhz": top.median_bandwidth_hz / 1e6 if top else 0.0,
        "top_event_hit_frames": float(top.hit_frame_count if top else 0),
        "top_event_eligible_frames": float(top_eligible_frames),
        "top_event_hit_ratio": top_hit_ratio,
        "event_bandwidth_mean_mhz": safe_mean(event_bandwidths),
        "event_bandwidth_std_mhz": safe_stdev(event_bandwidths),
        "event_nominal_bandwidth_mean_mhz": safe_mean(event_nominal_bandwidths),
        "event_bandwidth_match_mean": safe_mean(event_bandwidth_matches),
        "event_bandwidth_match_max": max(event_bandwidth_matches) if event_bandwidth_matches else 0.0,
        "event_score_mean": safe_mean(event_scores),
        "event_score_max": max(event_scores) if event_scores else 0.0,
    }
    if include_frequency_features:
        features.update(
            {
                "min_frequency_mhz": file_analysis.min_frequency_hz / 1e6,
                "max_frequency_mhz": file_analysis.max_frequency_hz / 1e6,
                "top_event_center_frequency_mhz": top.center_frequency_hz / 1e6 if top else 0.0,
            }
        )
    return features


def format_duration(seconds: float) -> str:
    seconds = max(0.0, seconds)
    minutes = int(seconds // 60)
    seconds -= minutes * 60
    return f"{minutes:02d}:{seconds:05.2f}"


def estimate_event_times(
    event: CandidateEvent,
    file_analysis: FileAnalysis,
) -> tuple[str, str, float, float, str]:
    span = max(1, file_analysis.last_frame_index - file_analysis.first_frame_index)
    start_offset_s = (
        (event.first_frame_index - file_analysis.first_frame_index) / span
    ) * file_analysis.duration_s
    end_offset_s = (
        (event.last_frame_index - file_analysis.first_frame_index) / span
    ) * file_analysis.duration_s
    start_relative = format_duration(start_offset_s)
    end_relative = format_duration(end_offset_s)

    if file_analysis.timestamp is None:
        return start_relative, end_relative, start_offset_s, end_offset_s, "仅有片段内相对时间"

    chunk_end = file_analysis.timestamp
    chunk_start = chunk_end - timedelta(seconds=file_analysis.duration_s)
    start_local = chunk_start + timedelta(seconds=start_offset_s)
    end_local = chunk_start + timedelta(seconds=end_offset_s)
    return (
        start_local.isoformat(sep=" "),
        end_local.isoformat(sep=" "),
        start_offset_s,
        end_offset_s,
        "文件名时间戳视为片段结束，按约10秒线性估计",
    )


def event_record(
    event_id: str,
    event: CandidateEvent,
    file_analysis: FileAnalysis,
    image_name: str = "",
) -> dict[str, object]:
    grade, score, reason = classify_event(event, file_analysis)
    start_local, end_local, start_offset_s, end_offset_s, time_basis = estimate_event_times(
        event,
        file_analysis,
    )
    eligible_frames = event.eligible_frame_count(file_analysis)
    eligible_ratio = event.hit_frame_count / eligible_frames if eligible_frames else 0.0
    return {
        "candidate_id": event_id,
        "source_file": event.source_file,
        "model": file_analysis.metadata.model,
        "signal_noise_label": file_analysis.metadata.signal_noise_label,
        "type_label": file_analysis.metadata.type_label,
        "part_number": event.part_number if event.part_number is not None else "",
        "start_time_local_est": start_local,
        "end_time_local_est": end_local,
        "start_offset_s": round(start_offset_s, 3),
        "end_offset_s": round(end_offset_s, 3),
        "start_frequency_hz": round(event.nominal_start_frequency_hz, 3),
        "end_frequency_hz": round(event.nominal_stop_frequency_hz, 3),
        "center_frequency_hz": round(event.center_frequency_hz, 3),
        "start_frequency_mhz": round(event.nominal_start_frequency_hz / 1e6, 6),
        "end_frequency_mhz": round(event.nominal_stop_frequency_hz / 1e6, 6),
        "center_frequency_mhz": round(event.center_frequency_hz / 1e6, 6),
        "bandwidth_mhz": round(event.nominal_bandwidth_mhz, 6),
        "nominal_bandwidth_mhz": round(event.nominal_bandwidth_mhz, 6),
        "bandwidth_match_score": round(event.bandwidth_match_score, 6),
        "bandwidth_coverage_ratio": round(event.bandwidth_coverage_ratio, 6),
        "observed_start_frequency_hz": round(event.start_frequency_hz, 3),
        "observed_end_frequency_hz": round(event.stop_frequency_hz, 3),
        "observed_start_frequency_mhz": round(event.start_frequency_hz / 1e6, 6),
        "observed_end_frequency_mhz": round(event.stop_frequency_hz / 1e6, 6),
        "observed_bandwidth_mhz": round(event.observed_bandwidth_hz / 1e6, 6),
        "peak_frequency_hz": round(event.peak_hit.peak_frequency_hz, 3),
        "peak_frequency_mhz": round(event.peak_hit.peak_frequency_hz / 1e6, 6),
        "peak_level_dbuv": round(event.peak_level_dbuv, 2),
        "average_peak_margin_db": round(event.average_peak_margin_db, 2),
        "sustained_frame_count": event.hit_frame_count,
        "eligible_frame_count": eligible_frames,
        "eligible_hit_ratio": round(eligible_ratio, 6),
        "max_frame_bandwidth_mhz": round(event.max_bandwidth_hz / 1e6, 6),
        "median_frame_bandwidth_mhz": round(event.median_bandwidth_hz / 1e6, 6),
        "peak_frame_index": event.peak_hit.frame_index,
        "candidate_grade": grade,
        "candidate_score": round(score, 2),
        "grade_reason": reason,
        "time_basis": time_basis,
        "waterfall_image": image_name,
    }


def color_map(value: float) -> tuple[int, int, int]:
    if not math.isfinite(value):
        return 8, 90, 64
    ratio = clamp(
        (value - WATERFALL_COLOR_LOW_DB)
        / max(WATERFALL_COLOR_HIGH_DB - WATERFALL_COLOR_LOW_DB, 1e-9),
        0.0,
        1.0,
    )
    stops = (
        (0.00, (8, 90, 64)),
        (0.30, (18, 145, 78)),
        (0.58, (52, 190, 74)),
        (0.78, (220, 210, 55)),
        (1.00, (255, 70, 34)),
    )
    for (left_ratio, left_color), (right_ratio, right_color) in zip(stops, stops[1:]):
        if ratio <= right_ratio:
            local = (ratio - left_ratio) / (right_ratio - left_ratio)
            return tuple(
                round(left_color[index] + local * (right_color[index] - left_color[index]))
                for index in range(3)
            )
    return stops[-1][1]


def safe_font(size: int) -> ImageFont.ImageFont:
    try:
        return ImageFont.truetype("arial.ttf", size)
    except OSError:
        return ImageFont.load_default()


def select_waterfall_frame_window(
    event: CandidateEvent,
    file_analysis: FileAnalysis,
) -> tuple[int, int]:
    file_start = file_analysis.first_frame_index
    file_stop = file_analysis.last_frame_index
    if file_stop <= file_start:
        return file_start, file_start

    event_start = int(clamp(event.first_frame_index, file_start, file_stop))
    event_stop = int(clamp(event.last_frame_index, event_start, file_stop))
    event_span = event_stop - event_start + 1
    max_rows = min(MAX_WATERFALL_ROWS, file_stop - file_start + 1)

    if event_span >= max_rows:
        window_start = event_start
        window_stop = min(file_stop, window_start + max_rows - 1)
        if window_stop - window_start + 1 < max_rows:
            window_start = max(file_start, window_stop - max_rows + 1)
        return window_start, window_stop

    margin = max(0, (max_rows - event_span) // 2)
    window_start = event_start - margin
    window_stop = window_start + max_rows - 1
    if window_start < file_start:
        window_start = file_start
        window_stop = min(file_stop, window_start + max_rows - 1)
    if window_stop > file_stop:
        window_stop = file_stop
        window_start = max(file_start, window_stop - max_rows + 1)
    return window_start, window_stop


def render_waterfall(
    event: CandidateEvent,
    file_analysis: FileAnalysis,
    output_path: Path,
    candidate_id: str,
) -> None:
    plot_width = 900
    plot_height = 480
    left = 86
    top = 64
    right = 112
    bottom = 76
    image_width = left + plot_width + right
    image_height = top + plot_height + bottom

    window_start, window_stop = select_waterfall_frame_window(event, file_analysis)
    window_frame_count = max(1, window_stop - window_start + 1)
    frequency_padding = max(file_analysis.step_hz * 8.0, event.nominal_bandwidth_hz * 0.35, event.max_bandwidth_hz * 0.8, 1_000_000.0)
    frequency_low = max(file_analysis.min_frequency_hz, event.nominal_start_frequency_hz - frequency_padding)
    frequency_high = min(file_analysis.max_frequency_hz, event.nominal_stop_frequency_hz + frequency_padding)
    if frequency_high <= frequency_low:
        frequency_low = file_analysis.min_frequency_hz
        frequency_high = file_analysis.max_frequency_hz

    matrix = [
        [WATERFALL_COLOR_LOW_DB for _ in range(plot_width)]
        for _ in range(plot_height)
    ]

    with file_analysis.path.open("r", newline="", encoding="utf-8-sig") as handle:
        reader = csv.reader(handle)
        header = validate_header(next(reader, None), file_analysis.path)
        header_index = {name: index for index, name in enumerate(header)}
        current_frame: int | None = None
        points: list[tuple[float, float]] = []

        def flush_plot_frame() -> None:
            nonlocal points, current_frame
            if current_frame is None or not points:
                points = []
                return
            if not (window_start <= current_frame <= window_stop):
                points = []
                return

            row_index = current_frame - window_start
            row_start = int(round(row_index * plot_height / window_frame_count))
            row_stop = int(round((row_index + 1) * plot_height / window_frame_count)) - 1
            row_start = int(clamp(row_start, 0, plot_height - 1))
            row_stop = int(clamp(max(row_start, row_stop), 0, plot_height - 1))
            bin_width_hz = estimate_step_hz(points) or file_analysis.step_hz
            if bin_width_hz <= 0.0:
                bin_width_hz = max((frequency_high - frequency_low) / max(plot_width, 1), 1.0)
            for frequency_hz, level_dbuv in points:
                if not (frequency_low <= frequency_hz <= frequency_high):
                    continue
                x_start = int(
                    math.floor(
                        (frequency_hz - bin_width_hz * 0.5 - frequency_low)
                        / max(frequency_high - frequency_low, 1.0)
                        * plot_width
                    )
                )
                x_stop = int(
                    math.ceil(
                        (frequency_hz + bin_width_hz * 0.5 - frequency_low)
                        / max(frequency_high - frequency_low, 1.0)
                        * plot_width
                    )
                )
                x_start = int(clamp(x_start, 0, plot_width - 1))
                x_stop = int(clamp(x_stop, x_start + 1, plot_width))
                for y in range(row_start, row_stop + 1):
                    row_values = matrix[y]
                    for x in range(x_start, x_stop):
                        row_values[x] = max(row_values[x], level_dbuv)
            points = []

        for row in reader:
            if len(row) < len(REQUIRED_COLUMNS):
                continue
            try:
                frame_index = int(row_value(row, header_index, "frame_index"))
                frequency_hz = float(row_value(row, header_index, "frequency_hz"))
                level_dbuv = float(row_value(row, header_index, "level_dbuv"))
            except ValueError:
                continue
            if current_frame is None:
                current_frame = frame_index
            elif frame_index != current_frame:
                flush_plot_frame()
                current_frame = frame_index
            if (
                frequency_hz <= 0
                or level_dbuv >= INVALID_LEVEL_DB
                or not math.isfinite(frequency_hz)
                or not math.isfinite(level_dbuv)
                or current_frame is None
                or not (window_start <= current_frame <= window_stop)
            ):
                continue
            points.append((frequency_hz, level_dbuv))
        flush_plot_frame()

    image = Image.new("RGB", (image_width, image_height), (7, 25, 29))
    draw = ImageDraw.Draw(image)
    title_font = safe_font(18)
    label_font = safe_font(13)
    small_font = safe_font(11)
    draw.rectangle((left, top, left + plot_width - 1, top + plot_height - 1), fill=(8, 90, 64))

    for y, row in enumerate(matrix):
        for x, value in enumerate(row):
            draw.point((left + x, top + y), fill=color_map(value))

    draw.rectangle((left, top, left + plot_width, top + plot_height), outline=(222, 232, 240), width=1)

    candidate_x1 = left + round(
        (event.nominal_start_frequency_hz - frequency_low) / max(frequency_high - frequency_low, 1.0) * plot_width
    )
    candidate_x2 = left + round(
        (event.nominal_stop_frequency_hz - frequency_low) / max(frequency_high - frequency_low, 1.0) * plot_width
    )
    observed_x1 = left + round(
        (event.start_frequency_hz - frequency_low) / max(frequency_high - frequency_low, 1.0) * plot_width
    )
    observed_x2 = left + round(
        (event.stop_frequency_hz - frequency_low) / max(frequency_high - frequency_low, 1.0) * plot_width
    )
    draw.rectangle(
        (candidate_x1, top, max(candidate_x1 + 1, candidate_x2), top + plot_height),
        outline=(255, 255, 255),
        width=2,
    )
    draw.rectangle(
        (observed_x1, top + 3, max(observed_x1 + 1, observed_x2), top + plot_height - 3),
        outline=(126, 210, 255),
        width=1,
    )

    grade, score, _ = classify_event(event, file_analysis)
    title = (
        f"{candidate_id} [{grade}] {event.nominal_start_frequency_hz / 1e6:.6f}-"
        f"{event.nominal_stop_frequency_hz / 1e6:.6f} MHz "
        f"({event.nominal_bandwidth_mhz:.0f}M) peak {event.peak_level_dbuv:.1f} dBuV"
    )
    draw.text((left, 18), title, fill=(245, 248, 252), font=title_font)
    draw.text(
        (left, 42),
        f"{file_analysis.path.name} | hits {event.hit_frame_count} | score {score:.1f} | "
        f"observed {event.observed_bandwidth_hz / 1e6:.2f}M | frames {window_start}-{window_stop} | sweep CSV only",
        fill=(183, 197, 214),
        font=small_font,
    )

    x_labels = (
        (frequency_low, f"{frequency_low / 1e6:.3f}"),
        ((frequency_low + frequency_high) / 2.0, f"{(frequency_low + frequency_high) / 2e6:.3f}"),
        (frequency_high, f"{frequency_high / 1e6:.3f}"),
    )
    for frequency_hz, label in x_labels:
        x = left + round((frequency_hz - frequency_low) / max(frequency_high - frequency_low, 1.0) * plot_width)
        draw.line((x, top + plot_height, x, top + plot_height + 5), fill=(222, 232, 240))
        draw.text((x - 22, top + plot_height + 10), label, fill=(222, 232, 240), font=small_font)
    draw.text((left + plot_width // 2 - 52, image_height - 30), "Frequency (MHz)", fill=(222, 232, 240), font=label_font)

    def frame_offset_s(frame_index: int) -> float:
        frame_span = max(1, file_analysis.last_frame_index - file_analysis.first_frame_index)
        return (frame_index - file_analysis.first_frame_index) / frame_span * file_analysis.duration_s

    for ratio in (0.0, 0.5, 1.0):
        frame_index = round(window_start + ratio * (window_stop - window_start))
        y = top + round(ratio * (plot_height - 1))
        draw.line((left - 5, y, left, y), fill=(222, 232, 240))
        draw.text((14, y - 7), f"{frame_offset_s(frame_index):.2f}s", fill=(222, 232, 240), font=small_font)
    draw.text((left - 38, top - 25), "Old", fill=(222, 232, 240), font=small_font)
    draw.text((left - 38, top + plot_height - 18), "New", fill=(222, 232, 240), font=small_font)

    colorbar_x = left + plot_width + 30
    for y in range(plot_height):
        ratio = 1.0 - y / max(plot_height - 1, 1)
        value = WATERFALL_COLOR_LOW_DB + ratio * (WATERFALL_COLOR_HIGH_DB - WATERFALL_COLOR_LOW_DB)
        draw.line((colorbar_x, top + y, colorbar_x + 18, top + y), fill=color_map(value))
    draw.rectangle((colorbar_x, top, colorbar_x + 18, top + plot_height), outline=(222, 232, 240))
    draw.text((colorbar_x - 4, top - 22), "dBuV", fill=(222, 232, 240), font=small_font)
    draw.text((colorbar_x + 25, top - 3), f"{WATERFALL_COLOR_HIGH_DB:.1f}", fill=(222, 232, 240), font=small_font)
    draw.text((colorbar_x + 25, top + plot_height - 12), f"{WATERFALL_COLOR_LOW_DB:.1f}", fill=(222, 232, 240), font=small_font)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    image.save(output_path, format="PNG", optimize=True)


def render_overview(image_paths: Sequence[Path], output_path: Path) -> None:
    if not image_paths:
        return
    display_paths = image_paths[:OVERVIEW_IMAGE_COUNT]
    thumb_width = 620
    thumb_height = 380
    columns = 2
    rows = math.ceil(len(display_paths) / columns)
    overview = Image.new("RGB", (columns * thumb_width, rows * thumb_height), (7, 25, 29))
    for index, path in enumerate(display_paths):
        with Image.open(path) as image:
            image = image.convert("RGB")
            image.thumbnail((thumb_width - 12, thumb_height - 12))
            x = (index % columns) * thumb_width + (thumb_width - image.width) // 2
            y = (index // columns) * thumb_height + (thumb_height - image.height) // 2
            overview.paste(image, (x, y))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    overview.save(output_path, format="PNG", optimize=True)


def write_csv(path: Path, rows: Sequence[dict[str, object]], fieldnames: Sequence[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8-sig") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def feature_rows(file_analyses: Sequence[FileAnalysis]) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for analysis in file_analyses:
        row: dict[str, object] = {
            "source_file": analysis.path.name,
            "drone_id": analysis.metadata.drone_id,
            "model": analysis.metadata.model,
            "signal_noise_label": analysis.metadata.signal_noise_label,
            "type_label": analysis.metadata.type_label,
        }
        row.update({key: round(value, 8) for key, value in analysis.features.items()})
        rows.append(row)
    return rows


def feature_columns(file_analyses: Sequence[FileAnalysis]) -> list[str]:
    keys: set[str] = set()
    for analysis in file_analyses:
        keys.update(analysis.features)
    return sorted(keys)


def build_feature_matrix(file_analyses: Sequence[FileAnalysis], columns: Sequence[str]) -> list[list[float]]:
    return [
        [float(analysis.features.get(column, 0.0)) for column in columns]
        for analysis in file_analyses
    ]


def heuristic_signal_probability(analysis: FileAnalysis) -> float:
    score = analysis.features.get("top_event_score", 0.0)
    hit_ratio = analysis.features.get("top_event_hit_ratio", 0.0)
    margin = analysis.features.get("top_event_peak_margin", 0.0)
    width = analysis.features.get("top_event_bandwidth_mhz", 0.0)
    bandwidth_match = analysis.features.get("top_event_bandwidth_match_score", 0.0)
    width_value = 0.5 * clamp(width / 10.0, 0.0, 1.0) + 0.5 * clamp(bandwidth_match, 0.0, 1.0)
    value = (
        0.45 * clamp(score / 100.0, 0.0, 1.0)
        + 0.25 * clamp(hit_ratio / 0.6, 0.0, 1.0)
        + 0.20 * clamp(margin / 30.0, 0.0, 1.0)
        + 0.10 * width_value
    )
    return round(value, 8)


def label_distribution(labels: Sequence[str]) -> dict[str, int]:
    return dict(sorted(Counter(labels).items()))


def can_train(labels: Sequence[str], min_class_count: int = 2) -> tuple[bool, str]:
    counts = Counter(label for label in labels if label)
    if len(counts) < 2:
        return False, "少于两个类别，无法训练监督分类器"
    too_small = {label: count for label, count in counts.items() if count < min_class_count}
    if too_small:
        return False, f"类别样本不足：{too_small}"
    return True, ""


def train_classifier(
    file_analyses: Sequence[FileAnalysis],
    label_getter,
    feature_names: Sequence[str],
    min_class_count: int = 2,
) -> tuple[object | None, dict[str, object]]:
    labeled = [analysis for analysis in file_analyses if label_getter(analysis)]
    labels = [label_getter(analysis) for analysis in labeled]
    ok, reason = can_train(labels, min_class_count)
    report: dict[str, object] = {
        "enabled": False,
        "reason": reason,
        "labeled_file_count": len(labeled),
        "label_distribution": label_distribution(labels),
        "validation": {},
    }
    if not ok:
        return None, report

    try:
        from sklearn.ensemble import RandomForestClassifier
        from sklearn.metrics import accuracy_score, balanced_accuracy_score, classification_report, confusion_matrix
        from sklearn.model_selection import GroupShuffleSplit, train_test_split
    except Exception as exc:  # pragma: no cover - depends on local environment
        report["reason"] = f"scikit-learn 不可用：{exc}"
        return None, report

    X = build_feature_matrix(labeled, feature_names)
    y = labels
    groups = [session_group_key(analysis) for analysis in labeled]
    train_indices: list[int]
    test_indices: list[int]
    split_method = "stratified"

    unique_groups = sorted(set(groups))
    if len(unique_groups) >= 4:
        splitter = GroupShuffleSplit(n_splits=8, test_size=0.25, random_state=42)
        for train_idx, test_idx in splitter.split(X, y, groups=groups):
            train_labels = {y[index] for index in train_idx}
            test_labels = {y[index] for index in test_idx}
            if train_labels == set(Counter(y)) and test_labels == set(Counter(y)):
                train_indices = list(train_idx)
                test_indices = list(test_idx)
                split_method = "group_shuffle"
                break
        else:
            train_indices, test_indices = train_test_split(
                list(range(len(y))),
                test_size=0.25,
                random_state=42,
                stratify=y,
            )
    else:
        train_indices, test_indices = train_test_split(
            list(range(len(y))),
            test_size=0.25,
            random_state=42,
            stratify=y,
        )

    train_X = [X[index] for index in train_indices]
    test_X = [X[index] for index in test_indices]
    train_y = [y[index] for index in train_indices]
    test_y = [y[index] for index in test_indices]
    classifier = RandomForestClassifier(
        n_estimators=250,
        random_state=42,
        class_weight="balanced",
        min_samples_leaf=1,
    )
    classifier.fit(train_X, train_y)
    predictions = classifier.predict(test_X)
    labels_order = sorted(Counter(y))
    report.update(
        {
            "enabled": True,
            "reason": "",
            "model": "RandomForestClassifier",
            "feature_count": len(feature_names),
            "split_method": split_method,
            "train_count": len(train_indices),
            "validation_count": len(test_indices),
            "validation": {
                "accuracy": round(float(accuracy_score(test_y, predictions)), 6),
                "balanced_accuracy": round(float(balanced_accuracy_score(test_y, predictions)), 6),
                "labels": labels_order,
                "confusion_matrix": confusion_matrix(test_y, predictions, labels=labels_order).tolist(),
                "classification_report": classification_report(
                    test_y,
                    predictions,
                    labels=labels_order,
                    zero_division=0,
                    output_dict=True,
                ),
            },
        }
    )

    final_classifier = RandomForestClassifier(
        n_estimators=250,
        random_state=42,
        class_weight="balanced",
        min_samples_leaf=1,
    )
    final_classifier.fit(X, y)
    return final_classifier, report


def deep_input_for_analysis(file_analysis: FileAnalysis):
    """Build a frequency-relative sweep image from detected CSV energy hits.

    The horizontal axis is centered on the strongest candidate and spans 50 MHz,
    while the vertical axis is normalized frame time.  This avoids feeding the
    absolute 5.8 GHz tuning point to the network and leaves room for 40 MHz
    channels plus frequency offset.
    """
    import numpy as np

    image = np.zeros(
        (DEEP_INPUT_CHANNELS, DEEP_INPUT_HEIGHT, DEEP_INPUT_WIDTH),
        dtype=np.float32,
    )
    event = top_event(file_analysis)
    if event is None:
        return image

    center_hz = event.center_frequency_hz
    half_span_hz = DEEP_CROP_HALF_BANDWIDTH_MHZ * 1e6
    low_hz = center_hz - half_span_hz
    high_hz = center_hz + half_span_hz
    frame_span = max(1, file_analysis.last_frame_index - file_analysis.first_frame_index)

    for hit in file_analysis.hits:
        if hit.stop_hz < low_hz or hit.start_hz > high_hz:
            continue
        row = int(
            round(
                (hit.frame_index - file_analysis.first_frame_index)
                / frame_span
                * (DEEP_INPUT_HEIGHT - 1)
            )
        )
        row = int(clamp(row, 0, DEEP_INPUT_HEIGHT - 1))
        col_start = int(
            math.floor((max(hit.start_hz, low_hz) - low_hz) / (2.0 * half_span_hz) * DEEP_INPUT_WIDTH)
        )
        col_stop = int(
            math.ceil((min(hit.stop_hz, high_hz) - low_hz) / (2.0 * half_span_hz) * DEEP_INPUT_WIDTH)
        )
        col_start = int(clamp(col_start, 0, DEEP_INPUT_WIDTH - 1))
        col_stop = int(clamp(max(col_start + 1, col_stop), col_start + 1, DEEP_INPUT_WIDTH))
        margin_value = clamp(hit.peak_margin_db / 40.0, 0.0, 1.0)
        peak_value = clamp((hit.peak_level_dbuv - ABSOLUTE_HIT_LEVEL_DB) / 50.0, 0.0, 1.0)
        image[0, row, col_start:col_stop] = np.maximum(
            image[0, row, col_start:col_stop],
            margin_value,
        )
        image[1, row, col_start:col_stop] = 1.0
        image[2, row, col_start:col_stop] = np.maximum(
            image[2, row, col_start:col_stop],
            peak_value,
        )
    return image


class SweepCNN:
    """Lazy wrapper around the optional PyTorch CNN implementation."""

    def __init__(self, model, classes: Sequence[str], device: str):
        self.model = model
        self.classes_ = list(classes)
        self.device = device

    def _tensor(self, rows):
        import numpy as np
        import torch

        array = np.asarray(rows, dtype=np.float32)
        return torch.as_tensor(array, dtype=torch.float32, device=self.device)

    def predict_proba(self, rows):
        import torch

        self.model.eval()
        with torch.no_grad():
            logits = self.model(self._tensor(rows))
            return torch.softmax(logits, dim=1).detach().cpu().numpy()

    def predict(self, rows):
        import numpy as np

        probabilities = self.predict_proba(rows)
        return np.asarray(
            [self.classes_[index] for index in probabilities.argmax(axis=1)],
            dtype=object,
        )


def _make_sweep_cnn(num_classes: int):
    import torch.nn as nn

    class _Network(nn.Module):
        def __init__(self):
            super().__init__()
            self.features = nn.Sequential(
                nn.Conv2d(DEEP_INPUT_CHANNELS, 16, kernel_size=5, padding=2),
                nn.BatchNorm2d(16),
                nn.ReLU(inplace=True),
                nn.MaxPool2d(2),
                nn.Conv2d(16, 32, kernel_size=3, padding=1),
                nn.BatchNorm2d(32),
                nn.ReLU(inplace=True),
                nn.MaxPool2d(2),
                nn.Conv2d(32, 64, kernel_size=3, padding=1),
                nn.BatchNorm2d(64),
                nn.ReLU(inplace=True),
                nn.MaxPool2d(2),
                nn.Conv2d(64, 96, kernel_size=3, padding=1),
                nn.BatchNorm2d(96),
                nn.ReLU(inplace=True),
                nn.AdaptiveAvgPool2d((1, 1)),
            )
            self.classifier = nn.Sequential(
                nn.Flatten(),
                nn.Linear(96, 48),
                nn.ReLU(inplace=True),
                nn.Dropout(p=0.25),
                nn.Linear(48, num_classes),
            )

        def forward(self, inputs):
            return self.classifier(self.features(inputs))

    return _Network()


def _augment_deep_batch(batch):
    import torch

    augmented = batch.clone()
    shifts = torch.randint(
        -DEEP_MAX_SHIFT_PIXELS,
        DEEP_MAX_SHIFT_PIXELS + 1,
        (augmented.shape[0],),
        device=augmented.device,
    )
    for index, shift_value in enumerate(shifts.tolist()):
        if shift_value == 0:
            continue
        shifted = torch.roll(augmented[index], shifts=shift_value, dims=-1)
        if shift_value > 0:
            shifted[:, :, :shift_value] = 0.0
        else:
            shifted[:, :, shift_value:] = 0.0
        augmented[index] = shifted
    gain = 0.85 + 0.30 * torch.rand(
        (augmented.shape[0], 1, 1, 1),
        device=augmented.device,
    )
    noise = torch.randn_like(augmented) * 0.015
    return torch.clamp(augmented * gain + noise, 0.0, 1.0)


def _deep_split_indices(labels: Sequence[str], groups: Sequence[str], seed: int) -> tuple[list[int], list[int], str]:
    try:
        from sklearn.model_selection import GroupShuffleSplit, train_test_split
    except Exception:
        indices = list(range(len(labels)))
        split = max(1, int(len(indices) * 0.25))
        return indices[split:], indices[:split], "simple_split"

    unique_groups = sorted(set(groups))
    if len(unique_groups) >= 4:
        splitter = GroupShuffleSplit(n_splits=8, test_size=0.25, random_state=seed)
        all_labels = set(labels)
        for train_idx, test_idx in splitter.split(list(range(len(labels))), labels, groups=groups):
            train_labels = {labels[index] for index in train_idx}
            test_labels = {labels[index] for index in test_idx}
            if train_labels == all_labels and test_labels == all_labels:
                return list(train_idx), list(test_idx), "group_shuffle"

    train_idx, test_idx = train_test_split(
        list(range(len(labels))),
        test_size=0.25,
        random_state=seed,
        stratify=labels,
    )
    return list(train_idx), list(test_idx), "stratified"


def _fit_deep_network(
    model,
    inputs,
    targets,
    sample_indices: Sequence[int],
    epochs: int,
    batch_size: int,
    criterion,
    optimizer,
    device: str,
    seed: int,
) -> None:
    import torch
    from torch.utils.data import DataLoader, TensorDataset

    selected_inputs = inputs[list(sample_indices)]
    selected_targets = targets[list(sample_indices)]
    dataset = TensorDataset(selected_inputs, selected_targets)
    generator = torch.Generator()
    generator.manual_seed(seed)
    loader = DataLoader(
        dataset,
        batch_size=max(1, min(batch_size, len(dataset))),
        shuffle=True,
        generator=generator,
        num_workers=0,
    )
    model.train()
    for _ in range(max(1, epochs)):
        for batch_inputs, batch_targets in loader:
            batch_inputs = _augment_deep_batch(batch_inputs.to(device))
            batch_targets = batch_targets.to(device)
            optimizer.zero_grad(set_to_none=True)
            loss = criterion(model(batch_inputs), batch_targets)
            loss.backward()
            optimizer.step()


def train_deep_classifier(
    file_analyses: Sequence[FileAnalysis],
    label_getter,
    deep_inputs: dict[str, object],
    task_name: str,
    output_path: Path,
    min_class_count: int = 2,
    epochs: int = DEEP_DEFAULT_EPOCHS,
    batch_size: int = DEEP_DEFAULT_BATCH_SIZE,
    seed: int = 42,
) -> tuple[object | None, dict[str, object]]:
    labeled = [analysis for analysis in file_analyses if label_getter(analysis)]
    labels = [label_getter(analysis) for analysis in labeled]
    ok, reason = can_train(labels, min_class_count)
    report: dict[str, object] = {
        "enabled": False,
        "reason": reason,
        "task": task_name,
        "model": "SweepCNN",
        "labeled_file_count": len(labeled),
        "label_distribution": label_distribution(labels),
        "input_shape": [DEEP_INPUT_CHANNELS, DEEP_INPUT_HEIGHT, DEEP_INPUT_WIDTH],
        "crop_bandwidth_mhz": DEEP_CROP_HALF_BANDWIDTH_MHZ * 2.0,
        "frequency_offset_augmentation_mhz": (
            DEEP_MAX_SHIFT_PIXELS
            / DEEP_INPUT_WIDTH
            * DEEP_CROP_HALF_BANDWIDTH_MHZ
            * 2.0
        ),
        "validation": {},
    }
    if not ok:
        return None, report

    try:
        import numpy as np
        import torch
        import torch.nn as nn
        from sklearn.metrics import (
            accuracy_score,
            balanced_accuracy_score,
            classification_report,
            confusion_matrix,
        )
    except Exception as exc:  # pragma: no cover - depends on local environment
        report["reason"] = f"深度学习依赖不可用：{exc}"
        return None, report

    torch.manual_seed(seed)
    np.random.seed(seed)
    if torch.cuda.is_available():
        torch.cuda.manual_seed_all(seed)
    device = "cuda" if torch.cuda.is_available() else "cpu"
    classes = sorted(set(labels))
    label_to_index = {label: index for index, label in enumerate(classes)}
    inputs = np.stack([deep_inputs[analysis.path.name] for analysis in labeled]).astype(np.float32)
    targets_np = np.asarray([label_to_index[label] for label in labels], dtype=np.int64)
    groups = [session_group_key(analysis) for analysis in labeled]
    train_indices, test_indices, split_method = _deep_split_indices(labels, groups, seed)

    input_tensor = torch.as_tensor(inputs, dtype=torch.float32)
    target_tensor = torch.as_tensor(targets_np, dtype=torch.long)
    class_counts = Counter(targets_np.tolist())
    weights = torch.tensor(
        [
            len(targets_np) / (len(classes) * max(1, class_counts[index]))
            for index in range(len(classes))
        ],
        dtype=torch.float32,
        device=device,
    )
    model = _make_sweep_cnn(len(classes)).to(device)
    criterion = nn.CrossEntropyLoss(weight=weights)
    optimizer = torch.optim.AdamW(model.parameters(), lr=0.001, weight_decay=1e-4)

    best_state = None
    best_balanced_accuracy = -1.0
    best_epoch = 1
    no_improvement = 0
    validation_inputs = input_tensor[test_indices].to(device)
    validation_targets = target_tensor[test_indices].to(device)
    history: list[dict[str, float]] = []
    for epoch in range(max(1, epochs)):
        _fit_deep_network(
            model,
            input_tensor,
            target_tensor,
            train_indices,
            1,
            batch_size,
            criterion,
            optimizer,
            device,
            seed + epoch,
        )
        model.eval()
        with torch.no_grad():
            validation_logits = model(validation_inputs)
            validation_loss = float(criterion(validation_logits, validation_targets).item())
            validation_predictions = validation_logits.argmax(dim=1).cpu().numpy()
        balanced = float(
            balanced_accuracy_score(
                targets_np[test_indices],
                validation_predictions,
            )
        )
        history.append(
            {
                "epoch": float(epoch + 1),
                "validation_loss": round(validation_loss, 8),
                "balanced_accuracy": round(balanced, 8),
            }
        )
        if balanced > best_balanced_accuracy:
            best_balanced_accuracy = balanced
            best_epoch = epoch + 1
            best_state = {
                key: value.detach().cpu().clone()
                for key, value in model.state_dict().items()
            }
            no_improvement = 0
        else:
            no_improvement += 1
            if no_improvement >= DEEP_EARLY_STOPPING_PATIENCE and epoch + 1 >= DEEP_MIN_EPOCHS:
                break

    if best_state is None:
        best_state = {
            key: value.detach().cpu().clone()
            for key, value in model.state_dict().items()
        }
    model.load_state_dict(best_state)
    model.eval()
    with torch.no_grad():
        validation_logits = model(validation_inputs)
        validation_predictions = validation_logits.argmax(dim=1).cpu().numpy()
    labels_order = classes
    report.update(
        {
            "enabled": True,
            "reason": "",
            "device": device,
            "classes": classes,
            "feature_count": DEEP_INPUT_CHANNELS * DEEP_INPUT_HEIGHT * DEEP_INPUT_WIDTH,
            "split_method": split_method,
            "train_count": len(train_indices),
            "validation_count": len(test_indices),
            "best_epoch": best_epoch,
            "history": history,
            "validation": {
                "accuracy": round(float(accuracy_score(targets_np[test_indices], validation_predictions)), 6),
                "balanced_accuracy": round(
                    float(balanced_accuracy_score(targets_np[test_indices], validation_predictions)),
                    6,
                ),
                "labels": labels_order,
                "confusion_matrix": confusion_matrix(
                    targets_np[test_indices],
                    validation_predictions,
                    labels=list(range(len(labels_order))),
                ).tolist(),
                "classification_report": classification_report(
                    targets_np[test_indices],
                    validation_predictions,
                    labels=list(range(len(labels_order))),
                    target_names=labels_order,
                    zero_division=0,
                    output_dict=True,
                ),
            },
        }
    )

    final_model = _make_sweep_cnn(len(classes)).to(device)
    final_criterion = nn.CrossEntropyLoss(weight=weights)
    final_optimizer = torch.optim.AdamW(final_model.parameters(), lr=0.001, weight_decay=1e-4)
    _fit_deep_network(
        final_model,
        input_tensor,
        target_tensor,
        list(range(len(labeled))),
        max(DEEP_MIN_EPOCHS, best_epoch),
        batch_size,
        final_criterion,
        final_optimizer,
        device,
        seed + 1000,
    )
    output_path.parent.mkdir(parents=True, exist_ok=True)
    torch.save(
        {
            "state_dict": final_model.state_dict(),
            "classes": classes,
            "task": task_name,
            "input_shape": [DEEP_INPUT_CHANNELS, DEEP_INPUT_HEIGHT, DEEP_INPUT_WIDTH],
            "crop_bandwidth_mhz": DEEP_CROP_HALF_BANDWIDTH_MHZ * 2.0,
            "frequency_offset_augmentation_mhz": report["frequency_offset_augmentation_mhz"],
        },
        output_path,
    )
    report["model_path"] = str(output_path)
    return SweepCNN(final_model, classes, device), report


def session_group_key(analysis: FileAnalysis) -> str:
    timestamp = parse_file_timestamp(analysis.path.name)[0]
    if timestamp is None:
        return analysis.path.stem
    return timestamp.strftime("%Y%m%d_%H%M")


def classifier_confidence(classifier: object, row: list[float], label: str) -> float:
    if not hasattr(classifier, "predict_proba"):
        return 1.0
    probabilities = classifier.predict_proba([row])[0]
    classes = list(classifier.classes_)
    try:
        index = classes.index(label)
    except ValueError:
        return 0.0
    return float(probabilities[index])


def signal_noise_prediction_rows(
    file_analyses: Sequence[FileAnalysis],
    classifier: object | None,
    feature_names: Sequence[str],
) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for analysis in file_analyses:
        true_label = analysis.metadata.signal_noise_label
        features = [float(analysis.features.get(name, 0.0)) for name in feature_names]
        if classifier is not None:
            predicted = str(classifier.predict([features])[0])
            confidence = classifier_confidence(classifier, features, predicted)
            prob_signal = classifier_confidence(classifier, features, "signal")
        else:
            prob_signal = heuristic_signal_probability(analysis)
            predicted = "signal" if prob_signal >= SIGNAL_PROBABILITY_THRESHOLD else "noise"
            confidence = prob_signal if predicted == "signal" else 1.0 - prob_signal
        top = top_event(analysis)
        rows.append(
            {
                "source_file": analysis.path.name,
                "true_signal_noise_label": true_label,
                "predicted_signal_noise_label": predicted,
                "prob_signal": round(prob_signal, 8),
                "prediction_confidence": round(confidence, 8),
                "classifier_model": "RandomForestClassifier" if classifier is not None else "heuristic",
                "model": analysis.metadata.model,
                "drone_id": analysis.metadata.drone_id,
                "top_candidate_center_mhz": round(top.center_frequency_hz / 1e6, 6) if top else "",
                "top_candidate_start_mhz": round(top.nominal_start_frequency_hz / 1e6, 6) if top else "",
                "top_candidate_end_mhz": round(top.nominal_stop_frequency_hz / 1e6, 6) if top else "",
                "top_candidate_nominal_bandwidth_mhz": round(top.nominal_bandwidth_mhz, 6) if top else "",
                "top_candidate_observed_bandwidth_mhz": round(top.observed_bandwidth_hz / 1e6, 6) if top else "",
                "top_candidate_bandwidth_match_score": round(top.bandwidth_match_score, 6) if top else "",
                "top_candidate_score": round(analysis.features.get("top_event_score", 0.0), 4),
                "top_candidate_hit_ratio": round(analysis.features.get("top_event_hit_ratio", 0.0), 6),
                "qualified_event_count": int(analysis.features.get("qualified_event_count", 0)),
            }
        )
    return rows


def deep_signal_noise_prediction_rows(
    file_analyses: Sequence[FileAnalysis],
    classifier: SweepCNN,
    deep_inputs: dict[str, object],
) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for analysis in file_analyses:
        input_row = deep_inputs[analysis.path.name]
        predicted = str(classifier.predict([input_row])[0])
        confidence = classifier_confidence(classifier, input_row, predicted)
        prob_signal = classifier_confidence(classifier, input_row, "signal")
        top = top_event(analysis)
        rows.append(
            {
                "source_file": analysis.path.name,
                "true_signal_noise_label": analysis.metadata.signal_noise_label,
                "predicted_signal_noise_label": predicted,
                "prob_signal": round(prob_signal, 8),
                "prediction_confidence": round(confidence, 8),
                "classifier_model": "SweepCNN",
                "model": analysis.metadata.model,
                "drone_id": analysis.metadata.drone_id,
                "top_candidate_center_mhz": round(top.center_frequency_hz / 1e6, 6) if top else "",
                "top_candidate_start_mhz": round(top.nominal_start_frequency_hz / 1e6, 6) if top else "",
                "top_candidate_end_mhz": round(top.nominal_stop_frequency_hz / 1e6, 6) if top else "",
                "top_candidate_nominal_bandwidth_mhz": round(top.nominal_bandwidth_mhz, 6) if top else "",
                "top_candidate_observed_bandwidth_mhz": round(top.observed_bandwidth_hz / 1e6, 6) if top else "",
                "top_candidate_bandwidth_match_score": round(top.bandwidth_match_score, 6) if top else "",
                "top_candidate_score": round(analysis.features.get("top_event_score", 0.0), 4),
                "top_candidate_hit_ratio": round(analysis.features.get("top_event_hit_ratio", 0.0), 6),
                "qualified_event_count": int(analysis.features.get("qualified_event_count", 0)),
            }
        )
    return rows


def type_prediction_rows(
    file_analyses: Sequence[FileAnalysis],
    classifier: object | None,
    feature_names: Sequence[str],
    signal_rows: Sequence[dict[str, object]],
) -> list[dict[str, object]]:
    signal_by_file = {str(row["source_file"]): row for row in signal_rows}
    rows: list[dict[str, object]] = []
    for analysis in file_analyses:
        signal_row = signal_by_file.get(analysis.path.name, {})
        features = [float(analysis.features.get(name, 0.0)) for name in feature_names]
        if classifier is not None:
            predicted = str(classifier.predict([features])[0])
            confidence = classifier_confidence(classifier, features, predicted)
        else:
            predicted = ""
            confidence = 0.0
        rows.append(
            {
                "source_file": analysis.path.name,
                "true_type_label": analysis.metadata.type_label,
                "predicted_type_label": predicted,
                "prediction_confidence": round(confidence, 8),
                "predicted_signal_noise_label": signal_row.get("predicted_signal_noise_label", ""),
                "prob_signal": signal_row.get("prob_signal", ""),
                "classifier_model": "RandomForestClassifier" if classifier is not None else "none",
                "model": analysis.metadata.model,
            }
        )
    return rows


def deep_type_prediction_rows(
    file_analyses: Sequence[FileAnalysis],
    classifier: SweepCNN,
    deep_inputs: dict[str, object],
    signal_rows: Sequence[dict[str, object]],
) -> list[dict[str, object]]:
    signal_by_file = {str(row["source_file"]): row for row in signal_rows}
    rows: list[dict[str, object]] = []
    for analysis in file_analyses:
        input_row = deep_inputs[analysis.path.name]
        predicted = str(classifier.predict([input_row])[0])
        confidence = classifier_confidence(classifier, input_row, predicted)
        signal_row = signal_by_file.get(analysis.path.name, {})
        top = top_event(analysis)
        rows.append(
            {
                "source_file": analysis.path.name,
                "true_type_label": analysis.metadata.type_label,
                "predicted_type_label": predicted,
                "prediction_confidence": round(confidence, 8),
                "predicted_signal_noise_label": signal_row.get("predicted_signal_noise_label", ""),
                "prob_signal": signal_row.get("prob_signal", ""),
                "classifier_model": "SweepCNN",
                "model": analysis.metadata.model,
                "predicted_bandwidth_mhz": round(top.nominal_bandwidth_mhz, 6) if top else "",
                "observed_bandwidth_mhz": round(top.observed_bandwidth_hz / 1e6, 6) if top else "",
            }
        )
    return rows


def write_readme(
    output_root: Path,
    input_root: Path,
    file_analyses: Sequence[FileAnalysis],
    result_rows: Sequence[dict[str, object]],
    all_event_count: int,
    plot_count: int,
    signal_report: dict[str, object],
    type_report: dict[str, object],
) -> None:
    grade_counts = {
        grade: sum(row["candidate_grade"] == grade for row in result_rows)
        for grade in ("A", "B", "C")
    }
    signal_counts = Counter(analysis.metadata.signal_noise_label or "unlabeled" for analysis in file_analyses)
    type_counts = Counter(analysis.metadata.type_label or "unlabeled" for analysis in file_analyses)
    text = f"""# Sweep CSV 图传候选检测与弱分类结果

输入目录：`{input_root}`

本次处理文件数：{len(file_analyses)}

正式候选事件数：{len(result_rows)}

全部合并事件数：{all_event_count}

候选等级统计：A={grade_counts["A"]}，B={grade_counts["B"]}，C={grade_counts["C"]}

图传/噪声标签分布：{dict(signal_counts)}

类型标签分布：{dict(type_counts)}

## 输出文件

- `sweep_detection_results.csv`：A/B 级疑似图传候选频段，UTF-8 with BOM，可直接用 Excel 打开。
- `sweep_detection_results_en.csv`：同一结果的英文列名版本。
- `sweep_detection_all_events_en.csv`：包含 C 级短时事件的完整明细，用于审计和调参。
- `sweep_signal_noise_predictions.csv`：每个 sweep CSV 的图传/噪声弱分类结果。
- `sweep_type_predictions.csv`：当标签足够时，每个 sweep CSV 的类型弱分类结果。
- `sweep_file_features.csv`：只从 sweep CSV 提取的特征表。
- `deep_learning_report.json`：SweepCNN 的训练配置、频偏增强和验证指标。
- `deep_learning/`：训练得到的 SweepCNN 权重；若深度学习不可用则为空。
- `waterfall/`：高等级和排名靠前候选的单独瀑布图，共 {plot_count} 张。
- `waterfall_overview_top_candidates.png`：前 {min(plot_count, OVERVIEW_IMAGE_COUNT)} 个候选的瀑布图总览。
- `run_manifest.json`：本次运行参数、标签分布和分类验证指标。

## 分类状态

图传/噪声分类：{"已训练 " + str(signal_report.get("model", "监督模型")) if signal_report.get("enabled") else "使用启发式评分"}

图传/噪声说明：{signal_report.get("reason") or signal_report.get("validation") or signal_report.get("fallback_reason")}

类型分类：{"已训练 " + str(type_report.get("model", "监督模型")) if type_report.get("enabled") else "未启用或标签不足"}

类型分类说明：{type_report.get("reason") or type_report.get("validation") or type_report.get("fallback_reason")}

## 检测规则

1. 只读取 sweep CSV：`frame_index`、`bin_index`、`frequency_hz`、`level_dbuv`，以及可选的 `drone_id/model/behavior` 标签列。
2. 每帧用中位数估计背景，命中条件为 `level_dbuv >= 20 dBuV` 且高于帧背景至少 `8 dB`。
3. 相邻热频点合并为连续频带；至少两个热频点保留为候选，单频点只有在峰值至少 `45 dBuV` 时保留。
4. 跨帧候选按频率重叠和 PSC 扫频周期合并，再按图传信号常见 `10/20/40 MHz` 三档估计名义信道带宽。
5. 输出的起止频率和 `bandwidth_mhz` 表示名义图传信道；`observed_*` 字段保留扫频热频点实际覆盖范围。
6. 深度模型使用 `3×96×256` 的相对频率能量图，横向输入覆盖 `60 MHz`，训练时随机平移频率轴以模拟约 `±7.5 MHz` 的频偏和中心估计误差。
7. 分类特征和深度模型输入都来自扫频能量形态，不使用 IQ、CAP、NPZ 或 `data/raw`。

## 限制

扫频 CSV 只能反映频率-电平随时间变化，缺少 IQ 相位和细粒度调制信息。因此这里的类型分类是弱分类，可靠性低于 IQ 指纹模型。
"""
    (output_root / "README.md").write_text(text, encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-root", type=Path, default=DEFAULT_INPUT_ROOT)
    parser.add_argument("--output-root", type=Path, default=DEFAULT_OUTPUT_ROOT)
    parser.add_argument("--max-plots", type=int, default=MAX_PLOT_COUNT)
    parser.add_argument("--max-files", type=int, default=0, help="Limit CSV files for a fast smoke run.")
    parser.add_argument("--include-frequency-features", action="store_true", help="Allow absolute frequency features in classifiers.")
    parser.add_argument("--train-signal-noise", dest="train_signal_noise", action="store_true", default=True)
    parser.add_argument("--no-train-signal-noise", dest="train_signal_noise", action="store_false")
    parser.add_argument("--train-type", dest="train_type", action="store_true", default=True)
    parser.add_argument("--no-train-type", dest="train_type", action="store_false")
    parser.add_argument("--min-type-class-count", type=int, default=2)
    parser.add_argument(
        "--classifier-model",
        choices=("deep", "random_forest", "auto"),
        default="deep",
        help="Classifier backend. deep uses PyTorch SweepCNN and falls back to random_forest if unavailable.",
    )
    parser.add_argument("--deep-epochs", type=int, default=DEEP_DEFAULT_EPOCHS)
    parser.add_argument("--deep-batch-size", type=int, default=DEEP_DEFAULT_BATCH_SIZE)
    parser.add_argument("--deep-seed", type=int, default=42)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    input_root = args.input_root.resolve()
    output_root = args.output_root.resolve()
    output_root.mkdir(parents=True, exist_ok=True)

    files = sorted(
        input_root.glob("*.csv"),
        key=lambda path: (
            parse_file_timestamp(path.name)[0] or datetime.min,
            path.name,
        ),
    )
    if args.max_files > 0:
        files = files[: args.max_files]
    if not files:
        raise SystemExit(f"No CSV files found in {input_root}")

    file_analyses: list[FileAnalysis] = []
    event_pairs: list[tuple[CandidateEvent, FileAnalysis]] = []
    for index, path in enumerate(files, start=1):
        print(f"[{index}/{len(files)}] analyzing {path.name}", flush=True)
        analysis = read_file_analysis(path, include_frequency_features=args.include_frequency_features)
        file_analyses.append(analysis)
        event_pairs.extend((event, analysis) for event in analysis.events)

    event_pairs.sort(key=event_sort_key)
    qualified_pairs = [
        (event, analysis)
        for event, analysis in event_pairs
        if classify_event(event, analysis)[0] in ("A", "B")
    ]
    selected_pairs: list[tuple[int, CandidateEvent, FileAnalysis]] = [
        (ordinal, event, analysis)
        for ordinal, (event, analysis) in enumerate(qualified_pairs[: args.max_plots], start=1)
    ]

    waterfall_dir = output_root / "waterfall"
    image_by_event: dict[int, str] = {}
    image_paths: list[Path] = []
    for ordinal, event, analysis in selected_pairs:
        grade, _, _ = classify_event(event, analysis)
        image_name = (
            f"{ordinal:03d}_{grade}_{event.nominal_start_frequency_hz / 1e6:.3f}-"
            f"{event.nominal_stop_frequency_hz / 1e6:.3f}MHz_{event.nominal_bandwidth_mhz:.0f}M_waterfall.png"
        )
        image_path = waterfall_dir / image_name
        print(f"  rendering {image_name}", flush=True)
        render_waterfall(event, analysis, image_path, f"C{ordinal:03d}")
        image_by_event[id(event)] = f"waterfall/{image_name}"
        image_paths.append(image_path)

    rows = [
        event_record(f"C{ordinal:03d}", event, analysis, image_by_event.get(id(event), ""))
        for ordinal, (event, analysis) in enumerate(qualified_pairs, start=1)
    ]
    all_rows = [
        event_record(f"E{ordinal:04d}", event, analysis, image_by_event.get(id(event), ""))
        for ordinal, (event, analysis) in enumerate(event_pairs, start=1)
    ]

    chinese_fields = [
        "candidate_id",
        "source_file",
        "model",
        "signal_noise_label",
        "type_label",
        "part_number",
        "start_time_local_est",
        "end_time_local_est",
        "start_offset_s",
        "end_offset_s",
        "start_frequency_mhz",
        "end_frequency_mhz",
        "center_frequency_mhz",
        "bandwidth_mhz",
        "nominal_bandwidth_mhz",
        "bandwidth_match_score",
        "bandwidth_coverage_ratio",
        "observed_start_frequency_mhz",
        "observed_end_frequency_mhz",
        "observed_bandwidth_mhz",
        "peak_frequency_mhz",
        "peak_level_dbuv",
        "average_peak_margin_db",
        "sustained_frame_count",
        "eligible_frame_count",
        "eligible_hit_ratio",
        "max_frame_bandwidth_mhz",
        "median_frame_bandwidth_mhz",
        "peak_frame_index",
        "candidate_grade",
        "candidate_score",
        "grade_reason",
        "time_basis",
        "waterfall_image",
    ]
    english_fields = list(rows[0].keys()) if rows else list(event_record("EMPTY", event_pairs[0][0], event_pairs[0][1]).keys()) if event_pairs else []
    chinese_headers = {
        "candidate_id": "候选编号",
        "source_file": "源文件",
        "model": "原始model标签",
        "signal_noise_label": "图传噪声标签",
        "type_label": "类型标签",
        "part_number": "分片编号",
        "start_time_local_est": "开始时间_本地估计",
        "end_time_local_est": "结束时间_本地估计",
        "start_offset_s": "开始时间_片段相对秒",
        "end_offset_s": "结束时间_片段相对秒",
        "start_frequency_mhz": "起始频率_MHz",
        "end_frequency_mhz": "终止频率_MHz",
        "center_frequency_mhz": "中心频率_MHz",
        "bandwidth_mhz": "名义带宽_MHz",
        "nominal_bandwidth_mhz": "名义图传带宽_MHz",
        "bandwidth_match_score": "带宽匹配评分",
        "bandwidth_coverage_ratio": "热频点覆盖名义带宽比例",
        "observed_start_frequency_mhz": "观测起始频率_MHz",
        "observed_end_frequency_mhz": "观测终止频率_MHz",
        "observed_bandwidth_mhz": "观测热频点带宽_MHz",
        "peak_frequency_mhz": "峰值频率_MHz",
        "peak_level_dbuv": "峰值_dBuV",
        "average_peak_margin_db": "平均峰值裕量_dB",
        "sustained_frame_count": "持续帧数",
        "eligible_frame_count": "覆盖频段总帧数",
        "eligible_hit_ratio": "覆盖帧命中比例",
        "max_frame_bandwidth_mhz": "单帧最大带宽_MHz",
        "median_frame_bandwidth_mhz": "单帧中位带宽_MHz",
        "peak_frame_index": "峰值帧号",
        "candidate_grade": "候选等级",
        "candidate_score": "候选评分",
        "grade_reason": "等级说明",
        "time_basis": "时间依据",
        "waterfall_image": "瀑布图",
    }
    chinese_rows = [
        {chinese_headers[field]: row.get(field, "") for field in chinese_fields}
        for row in rows
    ]
    write_csv(output_root / "sweep_detection_results.csv", chinese_rows, [chinese_headers[field] for field in chinese_fields])
    write_csv(output_root / "sweep_detection_results_en.csv", rows, english_fields)
    write_csv(output_root / "sweep_detection_all_events_en.csv", all_rows, english_fields)
    render_overview(image_paths, output_root / "waterfall_overview_top_candidates.png")

    features = feature_columns(file_analyses)
    deep_inputs: dict[str, object] = {}
    deep_signal_classifier = None
    deep_type_classifier = None
    deep_signal_report: dict[str, object] = {
        "enabled": False,
        "reason": "未请求深度学习模型",
        "task": "signal_noise",
    }
    deep_type_report: dict[str, object] = {
        "enabled": False,
        "reason": "未请求深度学习模型",
        "task": "type",
    }
    if args.classifier_model in ("deep", "auto") and (args.train_signal_noise or args.train_type):
        try:
            import torch  # noqa: F401
            import numpy  # noqa: F401

            deep_inputs = {
                analysis.path.name: deep_input_for_analysis(analysis)
                for analysis in file_analyses
            }
            deep_root = output_root / "deep_learning"
            if args.train_signal_noise:
                try:
                    deep_signal_classifier, deep_signal_report = train_deep_classifier(
                        file_analyses,
                        lambda analysis: analysis.metadata.signal_noise_label,
                        deep_inputs,
                        "signal_noise",
                        deep_root / "signal_noise_sweep_cnn.pt",
                        min_class_count=2,
                        epochs=args.deep_epochs,
                        batch_size=args.deep_batch_size,
                        seed=args.deep_seed,
                    )
                except Exception as exc:  # pragma: no cover - runtime-dependent
                    deep_signal_report = {
                        "enabled": False,
                        "reason": f"深度模型训练失败：{exc}",
                        "task": "signal_noise",
                        "model": "SweepCNN",
                    }
            if args.train_type:
                try:
                    deep_type_classifier, deep_type_report = train_deep_classifier(
                        file_analyses,
                        lambda analysis: analysis.metadata.type_label,
                        deep_inputs,
                        "type",
                        deep_root / "type_sweep_cnn.pt",
                        min_class_count=args.min_type_class_count,
                        epochs=args.deep_epochs,
                        batch_size=args.deep_batch_size,
                        seed=args.deep_seed + 1,
                    )
                except Exception as exc:  # pragma: no cover - runtime-dependent
                    deep_type_report = {
                        "enabled": False,
                        "reason": f"深度模型训练失败：{exc}",
                        "task": "type",
                        "model": "SweepCNN",
                    }
        except Exception as exc:  # pragma: no cover - depends on local environment
            deep_signal_report["reason"] = f"深度学习依赖不可用：{exc}"
            deep_type_report["reason"] = f"深度学习依赖不可用：{exc}"

    signal_classifier = None
    signal_report: dict[str, object] = {"enabled": False, "reason": "训练已关闭"}
    if args.train_signal_noise:
        if deep_signal_classifier is not None:
            signal_report = deep_signal_report
            signal_rows = deep_signal_noise_prediction_rows(
                file_analyses,
                deep_signal_classifier,
                deep_inputs,
            )
        else:
            signal_classifier, signal_report = train_classifier(
                file_analyses,
                lambda analysis: analysis.metadata.signal_noise_label,
                features,
                min_class_count=2,
            )
            if args.classifier_model == "deep":
                signal_report["requested_model"] = "SweepCNN"
                signal_report["fallback_from"] = "SweepCNN"
                signal_report["fallback_reason"] = deep_signal_report.get("reason", "")
            signal_rows = signal_noise_prediction_rows(file_analyses, signal_classifier, features)
    else:
        signal_rows = signal_noise_prediction_rows(file_analyses, None, features)
    signal_fields = list(signal_rows[0].keys()) if signal_rows else []
    write_csv(output_root / "sweep_signal_noise_predictions.csv", signal_rows, signal_fields)

    type_classifier = None
    type_report: dict[str, object] = {"enabled": False, "reason": "训练已关闭"}
    if args.train_type:
        if deep_type_classifier is not None:
            type_report = deep_type_report
            type_rows = deep_type_prediction_rows(
                file_analyses,
                deep_type_classifier,
                deep_inputs,
                signal_rows,
            )
        else:
            type_classifier, type_report = train_classifier(
                file_analyses,
                lambda analysis: analysis.metadata.type_label,
                features,
                min_class_count=args.min_type_class_count,
            )
            if args.classifier_model == "deep":
                type_report["requested_model"] = "SweepCNN"
                type_report["fallback_from"] = "SweepCNN"
                type_report["fallback_reason"] = deep_type_report.get("reason", "")
            type_rows = type_prediction_rows(file_analyses, type_classifier, features, signal_rows)
    else:
        type_rows = type_prediction_rows(file_analyses, None, features, signal_rows)
    type_fields = list(type_rows[0].keys()) if type_rows else []
    write_csv(output_root / "sweep_type_predictions.csv", type_rows, type_fields)

    rows_for_features = feature_rows(file_analyses)
    feature_fields = ["source_file", "drone_id", "model", "signal_noise_label", "type_label"] + features
    write_csv(output_root / "sweep_file_features.csv", rows_for_features, feature_fields)

    deep_learning_report = {
        "requested_classifier_model": args.classifier_model,
        "signal_noise": deep_signal_report,
        "type": deep_type_report,
    }
    (output_root / "deep_learning_report.json").write_text(
        json.dumps(deep_learning_report, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )

    manifest = {
        "input_root": str(input_root),
        "output_root": str(output_root),
        "input_file_count": len(file_analyses),
        "candidate_event_count": len(rows),
        "all_event_count": len(all_rows),
        "waterfall_count": len(image_paths),
        "requested_classifier_model": args.classifier_model,
        "signal_noise_classification": signal_report,
        "type_classification": type_report,
        "deep_learning": deep_learning_report,
        "label_distribution": {
            "signal_noise": label_distribution([analysis.metadata.signal_noise_label or "unlabeled" for analysis in file_analyses]),
            "type": label_distribution([analysis.metadata.type_label or "unlabeled" for analysis in file_analyses]),
        },
        "parameters": {
            "invalid_level_db": INVALID_LEVEL_DB,
            "absolute_hit_level_dbuv": ABSOLUTE_HIT_LEVEL_DB,
            "relative_hit_margin_db": RELATIVE_HIT_MARGIN_DB,
            "minimum_hot_bins": MIN_HOT_BINS,
            "single_bin_peak_db": SINGLE_BIN_PEAK_DB,
            "max_frame_gap_cycles": MAX_FRAME_GAP_CYCLES,
            "video_link_bandwidth_mhz": list(VIDEO_LINK_BANDWIDTH_MHZ),
            "deep_input_shape": [DEEP_INPUT_CHANNELS, DEEP_INPUT_HEIGHT, DEEP_INPUT_WIDTH],
            "deep_crop_bandwidth_mhz": DEEP_CROP_HALF_BANDWIDTH_MHZ * 2.0,
            "deep_epochs": args.deep_epochs,
            "deep_batch_size": args.deep_batch_size,
            "deep_seed": args.deep_seed,
            "assumed_chunk_duration_s": 10.0,
            "waterfall_max_rows": MAX_WATERFALL_ROWS,
            "waterfall_color_low_dbuv": WATERFALL_COLOR_LOW_DB,
            "waterfall_color_high_dbuv": WATERFALL_COLOR_HIGH_DB,
            "overview_image_count": OVERVIEW_IMAGE_COUNT,
            "include_frequency_features": bool(args.include_frequency_features),
        },
        "files": [
            {
                "name": analysis.path.name,
                "timestamp": analysis.timestamp.isoformat(sep=" ") if analysis.timestamp else "",
                "part_number": analysis.part_number,
                "frames": analysis.total_frames,
                "valid_rows": analysis.valid_rows,
                "invalid_rows": analysis.invalid_rows,
                "model": analysis.metadata.model,
                "signal_noise_label": analysis.metadata.signal_noise_label,
                "type_label": analysis.metadata.type_label,
                "frequency_range_mhz": [
                    round(analysis.min_frequency_hz / 1e6, 6),
                    round(analysis.max_frequency_hz / 1e6, 6),
                ],
                "step_khz": round(analysis.step_hz / 1e3, 6),
                "estimated_cycle_length": analysis.cycle_length,
            }
            for analysis in file_analyses
        ],
    }
    (output_root / "run_manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    write_readme(
        output_root,
        input_root,
        file_analyses,
        rows,
        len(all_rows),
        len(image_paths),
        signal_report,
        type_report,
    )

    print(
        f"completed: {len(rows)} qualified candidate events, "
        f"{len(all_rows)} total events, {len(image_paths)} waterfall plots"
    )
    print(f"results: {output_root / 'sweep_detection_results.csv'}")
    print(f"signal/noise predictions: {output_root / 'sweep_signal_noise_predictions.csv'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
