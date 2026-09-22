"""Open-set protocol helpers for DJI RF fingerprint experiments."""

from __future__ import annotations

import hashlib
import math
from collections import defaultdict
from dataclasses import dataclass
from typing import Iterable, Mapping, Sequence

import numpy as np


OPEN_SET_ROLES = (
    "train",
    "selection",
    "calibration",
    "known_test",
    "unknown_test",
)
KNOWN_ROLE_FRACTIONS = {
    "train": 0.625,
    "selection": 0.125,
    "calibration": 0.125,
    "known_test": 0.125,
}
TUNING_ROLES = {"train", "selection", "calibration"}


@dataclass(frozen=True)
class SourceItem:
    """One source recording in the open-set split plan."""

    class_name: str
    source_group: str
    source_file: str
    label: int | None = None


def stable_hash_float(text: str, seed: int = 0) -> float:
    payload = f"{int(seed)}::{text}".encode("utf-8")
    digest = hashlib.sha1(payload).hexdigest()
    return int(digest[:12], 16) / float(0xFFFFFFFFFFFF)


def split_individual_name(class_name: str) -> tuple[str, str]:
    if "_" not in class_name:
        raise ValueError(f"Class name does not contain an individual suffix: {class_name}")
    family, suffix = class_name.rsplit("_", 1)
    return family, suffix


def paired_class_names(
    class_names: Sequence[str],
    known_suffix: str = "01",
    unknown_suffix: str = "02",
) -> tuple[list[str], list[str]]:
    by_family: dict[str, dict[str, str]] = defaultdict(dict)
    for name in class_names:
        family, suffix = split_individual_name(name)
        by_family[family][suffix] = name

    known = []
    unknown = []
    for family in sorted(by_family):
        suffixes = by_family[family]
        if known_suffix in suffixes and unknown_suffix in suffixes:
            known.append(suffixes[known_suffix])
            unknown.append(suffixes[unknown_suffix])

    if not known:
        raise ValueError(
            f"No paired classes found for suffixes {known_suffix!r}/{unknown_suffix!r}."
        )
    if len(known) != len(unknown):
        raise ValueError("Known/unknown class pairing is incomplete.")
    return known, unknown


def label_remap(class_names: Sequence[str], known_names: Sequence[str]) -> dict[int, int]:
    name_to_index = {name: index for index, name in enumerate(class_names)}
    missing = [name for name in known_names if name not in name_to_index]
    if missing:
        raise ValueError(f"Known classes are missing from class_names: {missing}")
    return {name_to_index[name]: index for index, name in enumerate(known_names)}


def allocate_known_roles(
    source_groups: Sequence[str],
    class_name: str,
    seed: int,
) -> dict[str, list[str]]:
    ordered = sorted(
        set(source_groups),
        key=lambda group: stable_hash_float(f"{class_name}::{group}", seed),
    )
    if len(ordered) < len(KNOWN_ROLE_FRACTIONS):
        raise ValueError(
            f"{class_name} has {len(ordered)} non-official known sources; "
            f"need at least {len(KNOWN_ROLE_FRACTIONS)}."
        )

    raw_counts = {
        role: len(ordered) * fraction
        for role, fraction in KNOWN_ROLE_FRACTIONS.items()
    }
    counts = {role: int(math.floor(value)) for role, value in raw_counts.items()}
    for role in KNOWN_ROLE_FRACTIONS:
        counts[role] = max(1, counts[role])

    while sum(counts.values()) > len(ordered):
        role = max(counts, key=lambda item: (counts[item], raw_counts[item]))
        if counts[role] <= 1:
            break
        counts[role] -= 1
    while sum(counts.values()) < len(ordered):
        role = max(
            KNOWN_ROLE_FRACTIONS,
            key=lambda item: raw_counts[item] - counts[item],
        )
        counts[role] += 1

    offset = 0
    roles = {}
    for role in KNOWN_ROLE_FRACTIONS:
        count = counts[role]
        roles[role] = ordered[offset : offset + count]
        offset += count
    return roles


def build_split_manifest(
    items: Iterable[SourceItem],
    class_names: Sequence[str],
    official_source_groups: Iterable[str] = (),
    known_suffix: str = "01",
    unknown_suffix: str = "02",
    seed: int = 20260818,
) -> dict:
    known_names, unknown_names = paired_class_names(
        class_names,
        known_suffix=known_suffix,
        unknown_suffix=unknown_suffix,
    )
    known_set = set(known_names)
    unknown_set = set(unknown_names)
    official_set = set(official_source_groups)

    known_by_class: dict[str, list[SourceItem]] = {name: [] for name in known_names}
    unknown_by_class: dict[str, list[SourceItem]] = {name: [] for name in unknown_names}
    for item in items:
        if item.source_group in official_set:
            continue
        if item.class_name in known_set:
            known_by_class[item.class_name].append(item)
        elif item.class_name in unknown_set:
            unknown_by_class[item.class_name].append(item)

    roles = {role: [] for role in OPEN_SET_ROLES}
    sources = {role: [] for role in OPEN_SET_ROLES}
    for class_name in known_names:
        class_groups = [item.source_group for item in known_by_class[class_name]]
        class_roles = allocate_known_roles(class_groups, class_name, seed)
        item_by_group = {item.source_group: item for item in known_by_class[class_name]}
        for role, groups in class_roles.items():
            for group in groups:
                item = item_by_group[group]
                roles[role].append(group)
                sources[role].append(
                    {
                        "class_name": item.class_name,
                        "source_group": item.source_group,
                        "source_file": item.source_file,
                        "label": item.label,
                    }
                )

    for class_name in unknown_names:
        for item in sorted(
            unknown_by_class[class_name],
            key=lambda value: stable_hash_float(
                f"{value.class_name}::{value.source_group}", seed
            ),
        ):
            roles["unknown_test"].append(item.source_group)
            sources["unknown_test"].append(
                {
                    "class_name": item.class_name,
                    "source_group": item.source_group,
                    "source_file": item.source_file,
                    "label": item.label,
                }
            )

    manifest = {
        "protocol_version": 1,
        "seed": int(seed),
        "known_suffix": known_suffix,
        "unknown_suffix": unknown_suffix,
        "class_names": list(class_names),
        "known_classes": known_names,
        "unknown_classes": unknown_names,
        "known_label_map": label_remap(class_names, known_names),
        "roles": {role: sorted(groups) for role, groups in roles.items()},
        "sources": sources,
        "official_source_groups": sorted(official_set),
        "known_role_fractions_of_non_official": KNOWN_ROLE_FRACTIONS,
    }
    validate_split_manifest(manifest)
    return manifest


def validate_split_manifest(manifest: Mapping) -> dict:
    roles = {role: set(manifest["roles"].get(role, [])) for role in OPEN_SET_ROLES}
    official = set(manifest.get("official_source_groups", []))
    errors = []

    role_items = list(roles.items())
    source_file_roles: dict[str, str] = {}
    for index, (left_name, left_groups) in enumerate(role_items):
        overlap = sorted(left_groups & official)
        if overlap:
            errors.append(f"{left_name} overlaps official groups: {overlap[:5]}")
        for right_name, right_groups in role_items[index + 1 :]:
            overlap = sorted(left_groups & right_groups)
            if overlap:
                errors.append(
                    f"{left_name}/{right_name} source_group overlap: {overlap[:5]}"
                )

    for role in OPEN_SET_ROLES:
        for item in manifest["sources"].get(role, []):
            source_file = str(item["source_file"])
            previous_role = source_file_roles.get(source_file)
            if previous_role is not None and previous_role != role:
                errors.append(
                    f"source_file appears in multiple roles: {source_file}"
                )
            source_file_roles[source_file] = role

    known_classes = set(manifest["known_classes"])
    unknown_classes = set(manifest["unknown_classes"])
    for role in ("train", "selection", "calibration", "known_test"):
        classes = {item["class_name"] for item in manifest["sources"].get(role, [])}
        bad = sorted(classes - known_classes)
        if bad:
            errors.append(f"{role} contains unknown classes: {bad}")
        missing = sorted(known_classes - classes)
        if missing:
            errors.append(f"{role} is missing known classes: {missing}")

    unknown_classes_present = {
        item["class_name"] for item in manifest["sources"].get("unknown_test", [])
    }
    bad_unknown = sorted(unknown_classes_present - unknown_classes)
    if bad_unknown:
        errors.append(f"unknown_test contains known classes: {bad_unknown}")
    missing_unknown = sorted(unknown_classes - unknown_classes_present)
    if missing_unknown:
        errors.append(f"unknown_test is missing unknown classes: {missing_unknown}")

    status = "error" if errors else "ok"
    report = {
        "status": status,
        "errors": errors,
        "role_source_group_counts": {
            role: len(groups) for role, groups in roles.items()
        },
        "role_source_file_counts": {
            role: len(
                {
                    str(item["source_file"])
                    for item in manifest["sources"].get(role, [])
                }
            )
            for role in OPEN_SET_ROLES
        },
        "official_source_group_count": len(official),
        "tuning_roles": sorted(TUNING_ROLES),
    }
    if errors:
        raise ValueError("Open-set split manifest is invalid: " + "; ".join(errors))
    return report


def l2_normalize(values: np.ndarray, axis: int = 1, eps: float = 1e-12) -> np.ndarray:
    array = np.asarray(values, dtype=np.float32)
    denom = np.linalg.norm(array, axis=axis, keepdims=True)
    return array / np.maximum(denom, eps)


def build_source_balanced_prototypes(
    embeddings: np.ndarray,
    labels: np.ndarray,
    source_groups: Sequence[str],
    num_classes: int,
) -> np.ndarray:
    embeddings = l2_normalize(np.asarray(embeddings, dtype=np.float32), axis=1)
    labels = np.asarray(labels, dtype=np.int64)
    if len(embeddings) != len(labels) or len(labels) != len(source_groups):
        raise ValueError("embeddings, labels, and source_groups must have equal length.")

    per_source: dict[tuple[int, str], list[np.ndarray]] = defaultdict(list)
    for embedding, label, source_group in zip(embeddings, labels, source_groups):
        per_source[(int(label), str(source_group))].append(embedding)

    source_vectors_by_class: dict[int, list[np.ndarray]] = defaultdict(list)
    for (label, _source_group), rows in per_source.items():
        source_mean = l2_normalize(np.mean(rows, axis=0, dtype=np.float32)[None, :])[0]
        source_vectors_by_class[label].append(source_mean)

    if set(source_vectors_by_class) != set(range(num_classes)):
        missing = sorted(set(range(num_classes)) - set(source_vectors_by_class))
        raise ValueError(f"Cannot build prototypes; missing classes: {missing}")

    prototypes = []
    for label in range(num_classes):
        class_mean = np.mean(source_vectors_by_class[label], axis=0, dtype=np.float32)
        prototypes.append(l2_normalize(class_mean[None, :])[0])
    return np.asarray(prototypes, dtype=np.float32)


def source_balanced_percentile(
    scores: np.ndarray,
    source_groups: Sequence[str],
    percentile: float,
) -> float:
    scores = np.asarray(scores, dtype=np.float64)
    if len(scores) != len(source_groups):
        raise ValueError("scores and source_groups must have equal length.")
    per_source: dict[str, list[float]] = defaultdict(list)
    for score, source_group in zip(scores, source_groups):
        per_source[str(source_group)].append(float(score))
    means = [float(np.mean(values)) for values in per_source.values()]
    if not means:
        raise ValueError("Cannot compute threshold from an empty calibration set.")
    return float(np.percentile(np.asarray(means, dtype=np.float64), percentile))


def source_balanced_percentiles_by_label(
    scores: np.ndarray,
    labels: np.ndarray,
    source_groups: Sequence[str],
    num_classes: int,
    percentile: float,
) -> np.ndarray:
    scores = np.asarray(scores, dtype=np.float64)
    labels = np.asarray(labels, dtype=np.int64)
    if len(scores) != len(labels) or len(labels) != len(source_groups):
        raise ValueError("scores, labels, and source_groups must have equal length.")

    per_class_sources: dict[int, dict[str, list[float]]] = defaultdict(
        lambda: defaultdict(list)
    )
    for score, label, source_group in zip(scores, labels, source_groups):
        per_class_sources[int(label)][str(source_group)].append(float(score))

    thresholds = np.empty(int(num_classes), dtype=np.float64)
    for label in range(int(num_classes)):
        source_map = per_class_sources.get(label, {})
        means = [float(np.mean(values)) for values in source_map.values()]
        if not means:
            raise ValueError(
                f"Cannot compute threshold for class {label} from an empty calibration set."
            )
        thresholds[label] = np.percentile(
            np.asarray(means, dtype=np.float64),
            percentile,
        )
    return thresholds.astype(np.float32)


def classification_scores(
    logits: np.ndarray,
    embeddings: np.ndarray,
    prototypes: np.ndarray,
) -> dict[str, tuple[np.ndarray, np.ndarray]]:
    logits = np.asarray(logits, dtype=np.float32)
    embeddings = np.asarray(embeddings, dtype=np.float32)
    shifted = logits - logits.max(axis=1, keepdims=True)
    probabilities = np.exp(shifted)
    probabilities /= probabilities.sum(axis=1, keepdims=True)

    normalized_embeddings = l2_normalize(embeddings, axis=1)
    similarities = normalized_embeddings @ np.asarray(prototypes, dtype=np.float32).T
    return {
        "msp": (
            probabilities.max(axis=1).astype(np.float32),
            probabilities.argmax(axis=1).astype(np.int64),
        ),
        "energy": (
            np.logaddexp.reduce(logits, axis=1).astype(np.float32),
            logits.argmax(axis=1).astype(np.int64),
        ),
        "prototype_cosine": (
            similarities.max(axis=1).astype(np.float32),
            similarities.argmax(axis=1).astype(np.int64),
        ),
    }
