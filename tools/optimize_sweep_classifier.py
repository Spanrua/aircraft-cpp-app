#!/usr/bin/env python3
"""Optimize small-sample UAV sweep classifiers with chronological evaluation."""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
from collections import Counter
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Iterable

from sklearn.ensemble import ExtraTreesClassifier, RandomForestClassifier
from sklearn.impute import SimpleImputer
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import (
    accuracy_score,
    balanced_accuracy_score,
    classification_report,
    confusion_matrix,
    f1_score,
)
from sklearn.pipeline import Pipeline
from sklearn.preprocessing import StandardScaler
from sklearn.svm import SVC


LABELS = ("air3s", "min5pro", "yu4pro")
NON_FEATURE_COLUMNS = {
    "source_file",
    "drone_id",
    "model",
    "signal_noise_label",
    "type_label",
    "total_frames",
    "valid_rows",
}
STRUCTURAL_FEATURES = {
    "cycle_length",
    "frequency_span_mhz",
    "step_khz",
    "invalid_row_ratio",
    "top_event_eligible_frames",
    "top_event_hit_frames",
}
TIMESTAMP_PATTERN = re.compile(
    r"_(\d{8})_(\d{6})_(\d{3})(?:_part\d+)?\.csv$",
    re.IGNORECASE,
)


@dataclass(frozen=True)
class Record:
    source_file: str
    label: str
    timestamp: datetime
    values: tuple[float, ...]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--features",
        type=Path,
        default=Path("data/sweep/analysis_results_deep/sweep_file_features.csv"),
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("data/sweep/analysis_results_optimized_20261011"),
    )
    parser.add_argument(
        "--feature-set",
        choices=("robust", "all"),
        default="robust",
        help="robust excludes structural acquisition features; all keeps them.",
    )
    parser.add_argument("--seed", type=int, default=20261011)
    return parser.parse_args()


def parse_timestamp(source_file: str) -> datetime:
    match = TIMESTAMP_PATTERN.search(source_file)
    if match is None:
        raise ValueError(f"无法从文件名解析时间：{source_file}")
    return datetime.strptime(
        f"{match.group(1)}_{match.group(2)}_{match.group(3)}",
        "%Y%m%d_%H%M%S_%f",
    )


def load_records(
    path: Path,
    excluded_features: set[str] | None = None,
) -> tuple[list[Record], list[str]]:
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        rows = list(csv.DictReader(handle))
    if not rows:
        raise ValueError(f"特征表为空：{path}")

    excluded_features = (
        STRUCTURAL_FEATURES if excluded_features is None else excluded_features
    )
    feature_names = [
        name
        for name in rows[0]
        if name not in NON_FEATURE_COLUMNS
        and name not in excluded_features
    ]
    records: list[Record] = []
    for row in rows:
        label = row.get("type_label", "")
        if label not in LABELS:
            continue
        values = tuple(
            float(row[name]) if row.get(name, "") not in ("", None) else math.nan
            for name in feature_names
        )
        records.append(
            Record(
                source_file=row["source_file"],
                label=label,
                timestamp=parse_timestamp(row["source_file"]),
                values=values,
            )
        )
    records.sort(key=lambda record: record.timestamp)
    return records, feature_names


def split_temporal_blocks(
    records: list[Record],
    block_count: int,
) -> list[list[int]]:
    per_label: dict[str, list[int]] = {label: [] for label in LABELS}
    for index, record in enumerate(records):
        per_label[record.label].append(index)

    blocks: list[list[int]] = [[] for _ in range(block_count)]
    for label in LABELS:
        indices = per_label[label]
        for block_index, chunk in enumerate(split_evenly(indices, block_count)):
            blocks[block_index].extend(chunk)
    return blocks


def split_evenly(values: list[int], parts: int) -> list[list[int]]:
    result: list[list[int]] = []
    base, remainder = divmod(len(values), parts)
    cursor = 0
    for part_index in range(parts):
        size = base + (1 if part_index < remainder else 0)
        result.append(values[cursor : cursor + size])
        cursor += size
    return result


def build_model(model_name: str, seed: int) -> Pipeline:
    if model_name == "logistic_regression":
        estimator = LogisticRegression(
            C=1.0,
            class_weight="balanced",
            max_iter=4000,
            random_state=seed,
            solver="lbfgs",
        )
        return Pipeline(
            [
                ("imputer", SimpleImputer(strategy="median")),
                ("scaler", StandardScaler()),
                ("model", estimator),
            ]
        )
    if model_name == "rbf_svm":
        estimator = SVC(
            C=2.0,
            class_weight="balanced",
            gamma="scale",
            probability=False,
            random_state=seed,
        )
        return Pipeline(
            [
                ("imputer", SimpleImputer(strategy="median")),
                ("scaler", StandardScaler()),
                ("model", estimator),
            ]
        )
    if model_name == "random_forest":
        estimator = RandomForestClassifier(
            class_weight="balanced_subsample",
            max_features="sqrt",
            min_samples_leaf=2,
            n_estimators=700,
            n_jobs=-1,
            random_state=seed,
        )
        return Pipeline(
            [
                ("imputer", SimpleImputer(strategy="median")),
                ("model", estimator),
            ]
        )
    if model_name == "extra_trees":
        estimator = ExtraTreesClassifier(
            class_weight="balanced",
            max_features="sqrt",
            min_samples_leaf=2,
            n_estimators=700,
            n_jobs=-1,
            random_state=seed,
        )
        return Pipeline(
            [
                ("imputer", SimpleImputer(strategy="median")),
                ("model", estimator),
            ]
        )
    raise ValueError(f"未知模型：{model_name}")


def evaluate(
    records: list[Record],
    train_indices: Iterable[int],
    test_indices: Iterable[int],
    model_name: str,
    seed: int,
) -> dict[str, object]:
    train_indices = list(train_indices)
    test_indices = list(test_indices)
    train_x = [records[index].values for index in train_indices]
    test_x = [records[index].values for index in test_indices]
    train_y = [records[index].label for index in train_indices]
    test_y = [records[index].label for index in test_indices]
    model = build_model(model_name, seed)
    model.fit(train_x, train_y)
    predictions = model.predict(test_x)
    labels = list(LABELS)
    return {
        "model": model_name,
        "accuracy": float(accuracy_score(test_y, predictions)),
        "balanced_accuracy": float(
            balanced_accuracy_score(test_y, predictions)
        ),
        "macro_f1": float(
            f1_score(
                test_y,
                predictions,
                labels=labels,
                average="macro",
                zero_division=0,
            )
        ),
        "labels": labels,
        "confusion_matrix": confusion_matrix(
            test_y,
            predictions,
            labels=labels,
        ).tolist(),
        "classification_report": classification_report(
            test_y,
            predictions,
            labels=labels,
            output_dict=True,
            zero_division=0,
        ),
        "test_count": len(test_indices),
        "test_distribution": dict(Counter(test_y)),
    }


def summarize(results: list[dict[str, object]]) -> dict[str, dict[str, float]]:
    grouped: dict[str, list[dict[str, object]]] = {}
    for result in results:
        grouped.setdefault(str(result["model"]), []).append(result)
    summary: dict[str, dict[str, float]] = {}
    for model_name, model_results in grouped.items():
        summary[model_name] = {}
        for metric in ("accuracy", "balanced_accuracy", "macro_f1"):
            values = [float(result[metric]) for result in model_results]
            mean = sum(values) / len(values)
            variance = sum((value - mean) ** 2 for value in values) / max(
                1, len(values) - 1
            )
            summary[model_name][f"{metric}_mean"] = mean
            summary[model_name][f"{metric}_std"] = math.sqrt(variance)
    return summary


def write_csv(path: Path, rows: list[dict[str, object]]) -> None:
    if not rows:
        return
    fields = list(rows[0])
    with path.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    args = parse_args()
    excluded_features = STRUCTURAL_FEATURES if args.feature_set == "robust" else set()
    records, feature_names = load_records(args.features, excluded_features)
    args.output_dir.mkdir(parents=True, exist_ok=True)

    blocks = split_temporal_blocks(records, 5)
    holdout_indices = blocks[-1]
    development_indices = [index for block in blocks[:-1] for index in block]
    development_blocks = blocks[:-1]
    model_names = (
        "logistic_regression",
        "rbf_svm",
        "random_forest",
        "extra_trees",
    )

    inner_results: list[dict[str, object]] = []
    for fold_index in range(len(development_blocks)):
        validation_indices = development_blocks[fold_index]
        train_indices = [
            index
            for block_index, block in enumerate(development_blocks)
            if block_index != fold_index
            for index in block
        ]
        for model_index, model_name in enumerate(model_names):
            result = evaluate(
                records,
                train_indices,
                validation_indices,
                model_name,
                args.seed + fold_index * 100 + model_index,
            )
            result["fold"] = fold_index + 1
            result["protocol"] = "inner_temporal_block_cv"
            inner_results.append(result)

    inner_summary = summarize(inner_results)
    selected_model = max(
        inner_summary,
        key=lambda name: (
            inner_summary[name]["macro_f1_mean"],
            inner_summary[name]["balanced_accuracy_mean"],
        ),
    )
    final_result = evaluate(
        records,
        development_indices,
        holdout_indices,
        selected_model,
        args.seed + 999,
    )
    final_result["protocol"] = "last_temporal_block_holdout"
    final_result["selected_from"] = "inner_temporal_block_cv_macro_f1"

    all_block_results: list[dict[str, object]] = []
    for fold_index, validation_indices in enumerate(blocks):
        train_indices = [
            index
            for block_index, block in enumerate(blocks)
            if block_index != fold_index
            for index in block
        ]
        all_block_results.append(
            evaluate(
                records,
                train_indices,
                validation_indices,
                selected_model,
                args.seed + 2000 + fold_index,
            )
            | {
                "fold": fold_index + 1,
                "protocol": "five_temporal_block_cv",
            }
        )

    block_distribution = [
        {
            "fold": index + 1,
            "count": len(block),
            "labels": dict(
                Counter(records[item].label for item in block)
            ),
            "start": min(records[item].timestamp for item in block).isoformat(),
            "end": max(records[item].timestamp for item in block).isoformat(),
        }
        for index, block in enumerate(blocks)
    ]
    report = {
        "dataset": {
            "feature_table": str(args.features),
            "feature_set": args.feature_set,
            "labeled_file_count": len(records),
            "class_distribution": dict(
                Counter(record.label for record in records)
            ),
            "feature_count": len(feature_names),
            "features": feature_names,
        },
        "protocol": {
            "block_definition": (
                "按每个型号的时间顺序分别切成 5 个连续块；"
                "每个 fold 从三个型号各取一个时间块"
            ),
            "final_holdout": "每个型号最后一个时间块",
            "block_distribution": block_distribution,
            "leakage_control": [
                "最终测试块不参与模型选择",
                "训练和验证均按连续时间块划分",
                "不按独立 CSV 随机打散",
            ],
        },
        "model_selection": {
            "candidates": list(model_names),
            "selection_metric": "inner macro_f1_mean then balanced_accuracy_mean",
            "inner_summary": inner_summary,
            "selected_model": selected_model,
        },
        "final_holdout": final_result,
        "selected_model_five_block_cv": {
            "folds": all_block_results,
            "summary": summarize(all_block_results),
        },
    }
    (args.output_dir / "optimized_experiment_report.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    write_csv(args.output_dir / "inner_model_results.csv", inner_results)
    write_csv(args.output_dir / "five_block_results.csv", all_block_results)

    print(json.dumps({
        "selected_model": selected_model,
        "inner_summary": inner_summary,
        "final_holdout": {
            key: final_result[key]
            for key in (
                "accuracy",
                "balanced_accuracy",
                "macro_f1",
                "confusion_matrix",
                "test_distribution",
            )
        },
        "five_block_cv": report["selected_model_five_block_cv"]["summary"],
        "output_dir": str(args.output_dir),
    }, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
