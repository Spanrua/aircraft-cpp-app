"""Strict open-set baseline for 3943B DJI RF fingerprinting."""

from __future__ import annotations

import argparse
import csv
import json
import os
import sys
from array import array
from collections import defaultdict
from pathlib import Path
from typing import Sequence

import matplotlib
import numpy as np
import torch
import torch.nn as nn
from sklearn.metrics import average_precision_score, f1_score, roc_auc_score, roc_curve
from torch.utils.data import DataLoader, Dataset
from tqdm import tqdm

matplotlib.use("Agg")
import matplotlib.pyplot as plt

import CNNTRANS_v5_3943B as cnntrans
from rf_fingerprint_id.open_set_protocol import (
    OPEN_SET_ROLES,
    TUNING_ROLES,
    SourceItem,
    build_source_balanced_prototypes,
    build_split_manifest,
    classification_scores,
    paired_class_names,
    source_balanced_percentile,
    source_balanced_percentiles_by_label,
    split_individual_name,
    validate_split_manifest,
)


DEFAULT_DATA_ROOT = "/root/autodl-tmp/3943B_58g_dataset"
DEFAULT_OUTPUT_DIR = "/root/autodl-tmp/open_set_baseline"
DEFAULT_CACHE_DIR = (
    "/root/autodl-tmp/"
    "cnntrans_stitch8_spec_cache_complex_signed_fullofficial_20260812"
)
DEFAULT_SEEDS = (20260818, 20260819)
_cancel_event_emitted = False


def cancellation_requested() -> bool:
    flag_path = os.environ.get("AIRCRAFT_CANCEL_FLAG", "")
    return bool(flag_path and Path(flag_path).exists())


def raise_if_cancelled() -> None:
    global _cancel_event_emitted
    if cancellation_requested():
        if not _cancel_event_emitted:
            print(
                json.dumps(
                    {
                        "type": "cancelled",
                        "message": "Cancellation requested by Qt workflow",
                    },
                    ensure_ascii=False,
                ),
                flush=True,
            )
            _cancel_event_emitted = True
        raise KeyboardInterrupt("Cancellation requested by Qt workflow")


class OpenSetDataset(Dataset):
    """Manifest dataset view with source filtering and known-label remapping."""

    def __init__(
        self,
        base: Dataset,
        source_groups: Sequence[str],
        label_map: dict[int, int],
    ) -> None:
        self.base = base
        self.source_groups = set(source_groups)
        self.label_map = {int(key): int(value) for key, value in label_map.items()}
        self.paths = base.paths
        self.path_to_group = path_to_source_group(base)
        self.indices: list[int] = []
        self.record_path_ids = array("I")
        self.record_local_idxs = array("I")
        self.record_labels = array("B")
        self.record_source_groups: list[str] = []
        self.record_original_labels = array("B")

        for record_idx in range(base.record_count):
            path_id = int(base.record_path_ids[record_idx])
            source_group = self.path_to_group[path_id]
            original_label = int(base.record_labels[record_idx])
            if source_group not in self.source_groups:
                continue
            if original_label not in self.label_map:
                continue
            self.indices.append(record_idx)
            self.record_path_ids.append(path_id)
            self.record_local_idxs.append(int(base.record_local_idxs[record_idx]))
            self.record_labels.append(self.label_map[original_label])
            self.record_original_labels.append(original_label)
            self.record_source_groups.append(source_group)

        self.record_count = len(self.indices)
        self.labels = self.record_labels
        self.selected_source_groups = {
            self.record_source_groups[index]
            for index in range(len(self.record_source_groups))
        }
        self.selected_source_files = {
            source_file_from_group(group) for group in self.selected_source_groups
        }
        if self.record_count == 0:
            raise RuntimeError("Open-set dataset view has no records.")

    def __len__(self) -> int:
        return self.record_count

    def __getitem__(self, index: int):
        base_index = self.indices[index]
        inputs, _label = self.base[base_index]
        return inputs, torch.tensor(int(self.record_labels[index]), dtype=torch.long)


def source_file_from_group(source_group: str) -> str:
    return source_group.split("::", 1)[1] if "::" in source_group else source_group


def class_from_group(source_group: str) -> str:
    return source_group.split("::", 1)[0]


def manifest_backing_dataset(dataset: Dataset) -> Dataset:
    return cnntrans.unwrap_dataset(dataset)


def path_to_source_group(dataset: Dataset) -> dict[int, str]:
    dataset = manifest_backing_dataset(dataset)
    mapping: dict[int, str] = {}
    for source_group, paths in dataset.source_group_to_paths.items():
        for raw_path in paths:
            path_id = dataset.path_to_id.get(str(raw_path))
            if path_id is None:
                continue
            existing = mapping.get(path_id)
            if existing is not None and existing != source_group:
                raise ValueError(
                    f"NPZ path belongs to multiple source groups: {raw_path}"
                )
            mapping[path_id] = source_group
    missing = sorted(set(dataset.record_path_ids) - set(mapping))
    if missing:
        raise ValueError(f"Missing source-group mapping for path ids: {missing[:5]}")
    return mapping


def build_base_dataset(
    args: argparse.Namespace,
    is_train: bool,
    use_cache: bool = True,
) -> cnntrans.Dataset:
    cache_dir = Path(args.cache_dir) if getattr(args, "cache_dir", "") else None
    if use_cache and cache_dir is not None and cache_dir.exists():
        return cnntrans.PrecomputedSpecDataset(
            args.data_root,
            cache_dir,
            split="train",
            val_ratio=args.val_ratio,
            is_train=is_train,
            max_files=args.max_files,
            max_files_per_class=args.max_files_per_class,
            cache_size=args.cache_size,
            index_cache_dir=args.index_cache_dir,
            internal_dev_ratio=0.0,
            feature_mode=args.feature_mode,
            cache_data_root=args.cache_data_root or args.data_root,
            train_exclude_sources=set(),
            stitch_windows=args.stitch_windows,
            stitch_stride=args.stitch_stride,
        )
    return cnntrans.IqrawNpzDataset(
        args.data_root,
        split="train",
        val_ratio=args.val_ratio,
        is_train=is_train,
        max_files=args.max_files,
        max_files_per_class=args.max_files_per_class,
        cache_size=args.cache_size,
        return_raw_iq=not args.cpu_stft,
        index_cache_dir=args.index_cache_dir,
        internal_dev_ratio=0.0,
        raw_iq_cache_dir="",
        train_exclude_sources=set(),
        stitch_windows=args.stitch_windows,
        stitch_stride=args.stitch_stride,
    )


def source_items_from_dataset(dataset: Dataset) -> list[SourceItem]:
    dataset = manifest_backing_dataset(dataset)
    items = []
    for source_group in sorted(dataset.selected_source_groups):
        class_name = class_from_group(source_group)
        try:
            label = cnntrans.CLASS_NAMES.index(class_name)
        except ValueError:
            label = None
        items.append(
            SourceItem(
                class_name=class_name,
                source_group=source_group,
                source_file=source_file_from_group(source_group),
                label=label,
            )
        )
    return items


def write_json(path: Path, payload: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = path.with_name(path.name + ".tmp")
    tmp_path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    os.replace(tmp_path, path)


def atomic_torch_save(payload: object, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = path.with_name(path.name + ".tmp")
    torch.save(payload, tmp_path)
    os.replace(tmp_path, path)


def serializable_args(args: argparse.Namespace) -> dict:
    return {
        key: value
        for key, value in vars(args).items()
        if key != "func"
    }


def load_or_create_manifest(args: argparse.Namespace, dataset: Dataset) -> dict:
    backing_dataset = manifest_backing_dataset(dataset)
    manifest_path = Path(args.output_dir) / "split_manifest.json"
    if manifest_path.exists():
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        validate_split_manifest(manifest)
        return manifest
    manifest = build_split_manifest(
        source_items_from_dataset(backing_dataset),
        cnntrans.CLASS_NAMES,
        official_source_groups=backing_dataset.val_groups,
        known_suffix=args.known_suffix,
        unknown_suffix=args.unknown_suffix,
        seed=args.seed,
    )
    write_json(manifest_path, manifest)
    return manifest


def build_views(args: argparse.Namespace, is_train_base: bool = False) -> tuple[dict, dict[str, OpenSetDataset]]:
    base = build_base_dataset(args, is_train=is_train_base, use_cache=True)
    manifest = load_or_create_manifest(args, base)
    label_map = {int(key): int(value) for key, value in manifest["known_label_map"].items()}
    unknown_labels = {
        cnntrans.CLASS_NAMES.index(name): index
        for index, name in enumerate(manifest["unknown_classes"])
    }
    views = {
        "train": OpenSetDataset(base, manifest["roles"]["train"], label_map),
        "selection": OpenSetDataset(base, manifest["roles"]["selection"], label_map),
        "calibration": OpenSetDataset(base, manifest["roles"]["calibration"], label_map),
        "known_test": OpenSetDataset(base, manifest["roles"]["known_test"], label_map),
        "unknown_test": OpenSetDataset(
            base,
            manifest["roles"]["unknown_test"],
            unknown_labels,
        ),
    }
    return manifest, views


def loader_kwargs(args: argparse.Namespace) -> dict:
    kwargs = {
        "num_workers": args.num_workers,
        "pin_memory": cnntrans.device.type == "cuda" and not args.no_pin_memory,
    }
    if args.num_workers > 0:
        kwargs["persistent_workers"] = not args.no_persistent_workers
        kwargs["prefetch_factor"] = max(1, int(args.prefetch_factor))
    return kwargs


def make_eval_loader(dataset: Dataset, args: argparse.Namespace) -> DataLoader:
    return DataLoader(
        dataset,
        batch_size=args.batch_size,
        shuffle=False,
        **loader_kwargs(args),
    )


def macro_f1_eval(model: nn.Module, loader: DataLoader) -> tuple[float, float]:
    model.eval()
    labels = []
    preds = []
    with torch.no_grad():
        for inputs, batch_labels in loader:
            raise_if_cancelled()
            inputs = cnntrans.move_inputs_to_device(inputs)
            batch_labels = batch_labels.to(cnntrans.device, non_blocking=True)
            with torch.cuda.amp.autocast(enabled=cnntrans.device.type == "cuda"):
                logits = model(inputs)
            labels.extend(batch_labels.cpu().numpy().tolist())
            preds.extend(logits.argmax(dim=1).cpu().numpy().tolist())
    if not labels:
        return 0.0, 0.0
    accuracy = float(100.0 * np.mean(np.asarray(labels) == np.asarray(preds)))
    macro_f1 = float(100.0 * f1_score(labels, preds, average="macro", zero_division=0))
    return accuracy, macro_f1


def train_one_seed(args: argparse.Namespace, seed: int) -> None:
    raise_if_cancelled()
    args.seed = int(seed)
    seed_dir = Path(args.output_dir) / f"seed_{seed}"
    seed_dir.mkdir(parents=True, exist_ok=True)
    args.output_dir = str(seed_dir)
    cnntrans.set_global_seed(seed)

    train_base = build_base_dataset(args, is_train=True, use_cache=True)
    manifest = load_or_create_manifest(args, train_base)
    write_json(
        seed_dir / "split_audit.json",
        validate_split_manifest(manifest),
    )
    label_map = {
        int(key): int(value) for key, value in manifest["known_label_map"].items()
    }
    train_view = OpenSetDataset(train_base, manifest["roles"]["train"], label_map)
    selection_view = OpenSetDataset(
        train_base,
        manifest["roles"]["selection"],
        label_map,
    )

    train_loader = DataLoader(
        train_view,
        batch_sampler=cnntrans.create_batch_sampler(
            train_view,
            batch_size=args.batch_size,
            blocks_per_batch=args.blocks_per_batch,
            batches_per_file_group=args.batches_per_file_group,
            seed=seed,
            num_batches=args.steps_per_epoch,
            path_weight_power=args.path_weight_power,
        ),
        **loader_kwargs(args),
    )
    selection_loader = make_eval_loader(selection_view, args)
    model = cnntrans.IqClassifier(
        num_classes=len(manifest["known_classes"]),
        use_gpu_stft=not args.cpu_stft,
        augment=not args.no_augment,
        dropout=args.dropout,
        model_arch=args.model_arch,
        classifier_head="ce",
        embedding_dim=args.embedding_dim,
        feature_mode=args.feature_mode,
    ).to(cnntrans.device)
    optimizer = torch.optim.AdamW(
        model.parameters(),
        lr=args.lr,
        weight_decay=args.weight_decay,
    )
    steps_per_epoch = args.steps_per_epoch if args.steps_per_epoch > 0 else len(train_loader)
    steps_per_epoch = min(steps_per_epoch, len(train_loader))
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(
        optimizer,
        T_max=max(1, steps_per_epoch * args.epochs),
        eta_min=args.lr * 0.1,
    )
    scaler = torch.cuda.amp.GradScaler(enabled=cnntrans.device.type == "cuda")
    criterion = nn.CrossEntropyLoss(label_smoothing=args.label_smoothing)
    supcon = cnntrans.SupConLoss(temperature=args.supcon_temperature)

    history = []
    best_score = -1.0
    best_epoch = 0
    for epoch in range(args.epochs):
        raise_if_cancelled()
        train_base.is_train = True
        model.train()
        loss_total = 0.0
        correct = 0
        total = 0
        progress = tqdm(
            train_loader,
            desc=f"seed {seed} epoch {epoch + 1}/{args.epochs}",
            total=steps_per_epoch,
            unit="batch",
            mininterval=2.0,
        )
        for batch_idx, (inputs, labels) in enumerate(progress):
            raise_if_cancelled()
            if batch_idx >= steps_per_epoch:
                break
            inputs = cnntrans.move_inputs_to_device(inputs)
            labels = labels.to(cnntrans.device, non_blocking=True)
            optimizer.zero_grad(set_to_none=True)
            with torch.cuda.amp.autocast(enabled=cnntrans.device.type == "cuda"):
                logits, embeddings = model(inputs, labels=labels, return_embedding=True)
                ce_loss = criterion(logits, labels)
                supcon_loss = supcon(embeddings, labels)
                loss = ce_loss + args.supcon_weight * supcon_loss
            scaler.scale(loss).backward()
            scaler.unscale_(optimizer)
            nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            scaler.step(optimizer)
            scaler.update()
            scheduler.step()
            loss_total += float(loss.item())
            predicted = logits.argmax(dim=1)
            correct += int(predicted.eq(labels).sum().item())
            total += int(labels.numel())
            progress.set_postfix(
                loss=f"{loss.item():.4f}",
                acc=f"{100.0 * correct / max(total, 1):.2f}%",
            )

        train_base.is_train = False
        selection_acc, selection_macro_f1 = macro_f1_eval(model, selection_loader)
        raise_if_cancelled()
        record = {
            "epoch": epoch + 1,
            "train_loss": loss_total / max(steps_per_epoch, 1),
            "train_acc": 100.0 * correct / max(total, 1),
            "selection_acc": selection_acc,
            "selection_macro_f1": selection_macro_f1,
            "learning_rate": optimizer.param_groups[0]["lr"],
        }
        history.append(record)
        print(
            f"seed {seed} epoch {epoch + 1}: "
            f"selection macro-F1={selection_macro_f1:.2f}, acc={selection_acc:.2f}",
            flush=True,
        )
        if selection_macro_f1 > best_score:
            best_score = selection_macro_f1
            best_epoch = epoch + 1
            atomic_torch_save(model.state_dict(), seed_dir / "best_model.pth")
            atomic_torch_save(
                {
                    "epoch": best_epoch,
                    "best_selection_score": best_score,
                    "selection_metric": "macro_f1",
                    "class_names": manifest["known_classes"],
                    "model_config": {
                        "num_classes": len(manifest["known_classes"]),
                        "model_arch": args.model_arch,
                        "classifier_head": "ce",
                        "embedding_dim": args.embedding_dim,
                        "feature_mode": args.feature_mode,
                        "dropout": args.dropout,
                    },
                    "model_state_dict": model.state_dict(),
                },
                seed_dir / "best_checkpoint.pth",
            )
        write_json(seed_dir / "history.json", history)

    atomic_torch_save(model.state_dict(), seed_dir / "final_model.pth")
    write_json(
        seed_dir / "run_config.json",
        {
            "args": serializable_args(args),
            "seed": seed,
            "best_epoch": best_epoch,
            "best_selection_macro_f1": best_score,
            "known_classes": manifest["known_classes"],
            "unknown_classes": manifest["unknown_classes"],
        },
    )


def export_embeddings(
    model: nn.Module,
    views: dict[str, OpenSetDataset],
    args: argparse.Namespace,
) -> dict[str, dict[str, np.ndarray | list[str]]]:
    exported = {}
    model.eval()
    for role in OPEN_SET_ROLES:
        dataset = views[role]
        loader = make_eval_loader(dataset, args)
        logits_rows = []
        embedding_rows = []
        label_rows = []
        with torch.no_grad():
            for inputs, labels in tqdm(loader, desc=f"export {role}", mininterval=2.0):
                inputs = cnntrans.move_inputs_to_device(inputs)
                with torch.cuda.amp.autocast(enabled=cnntrans.device.type == "cuda"):
                    logits, embeddings = model(inputs, return_embedding=True)
                logits_rows.append(logits.float().cpu().numpy())
                embedding_rows.append(embeddings.float().cpu().numpy())
                label_rows.append(labels.numpy())
        exported[role] = {
            "logits": np.concatenate(logits_rows).astype(np.float32),
            "embeddings": np.concatenate(embedding_rows).astype(np.float32),
            "labels": np.concatenate(label_rows).astype(np.int64),
            "original_labels": np.asarray(dataset.record_original_labels, dtype=np.int64),
            "source_groups": list(dataset.record_source_groups),
        }
        if len(exported[role]["labels"]) != len(dataset):
            raise RuntimeError(f"Incomplete export for role {role}.")
    return exported


def save_embeddings_npz(path: Path, exported: dict[str, dict[str, np.ndarray | list[str]]]) -> None:
    payload = {}
    for role, values in exported.items():
        payload[f"{role}_logits"] = np.asarray(values["logits"], dtype=np.float16)
        payload[f"{role}_embeddings"] = np.asarray(values["embeddings"], dtype=np.float16)
        payload[f"{role}_labels"] = np.asarray(values["labels"], dtype=np.int16)
        payload[f"{role}_original_labels"] = np.asarray(
            values["original_labels"],
            dtype=np.int16,
        )
        payload[f"{role}_source_groups"] = np.asarray(
            values["source_groups"],
            dtype=object,
        )
    np.savez_compressed(path, **payload)


def load_embeddings_npz(path: Path) -> dict[str, dict[str, np.ndarray | list[str]]]:
    exported = {}
    with np.load(path, allow_pickle=True) as payload:
        for role in OPEN_SET_ROLES:
            exported[role] = {
                "logits": np.asarray(payload[f"{role}_logits"], dtype=np.float32),
                "embeddings": np.asarray(
                    payload[f"{role}_embeddings"],
                    dtype=np.float32,
                ),
                "labels": np.asarray(payload[f"{role}_labels"], dtype=np.int64),
                "original_labels": np.asarray(
                    payload[f"{role}_original_labels"],
                    dtype=np.int64,
                ),
                "source_groups": [
                    str(value) for value in payload[f"{role}_source_groups"].tolist()
                ],
            }
    return exported


def rows_for_split(
    role: str,
    data: dict[str, np.ndarray | list[str]],
    score_payloads: dict[str, tuple[np.ndarray, np.ndarray]],
    manifest: dict,
    thresholds: dict[str, dict],
) -> list[dict]:
    rows = []
    known_classes = manifest["known_classes"]
    unknown_classes = manifest["unknown_classes"]
    for index, source_group in enumerate(data["source_groups"]):
        label = int(data["labels"][index])
        original_label = int(data["original_labels"][index])
        true_class = (
            known_classes[label]
            if role != "unknown_test"
            else unknown_classes[label]
        )
        row = {
            "split": role,
            "row_index": index,
            "source_group": source_group,
            "source_file": source_file_from_group(str(source_group)),
            "true_class": true_class,
            "original_label": original_label,
            "known_label": label if role != "unknown_test" else "",
        }
        for score_name, (scores, predictions) in score_payloads.items():
            prediction = int(predictions[index])
            threshold = threshold_for_prediction(
                thresholds[score_name],
                prediction,
                known_classes,
            )
            accepted = bool(float(scores[index]) >= threshold)
            row[f"{score_name}_score"] = float(scores[index])
            row[f"{score_name}_prediction"] = known_classes[prediction]
            row[f"{score_name}_decision"] = known_classes[prediction] if accepted else "unknown"
            row[f"{score_name}_accepted"] = int(accepted)
        rows.append(row)
    return rows


def threshold_artifact_suffix(threshold_mode: str) -> str:
    return "" if threshold_mode == "global" else f"_{threshold_mode.replace('-', '_')}"


def build_threshold_spec(
    scores: np.ndarray,
    labels: np.ndarray,
    source_groups: Sequence[str],
    known_classes: Sequence[str],
    known_acceptance: float,
    threshold_mode: str,
) -> dict:
    percentile = (1.0 - known_acceptance) * 100.0
    global_threshold = source_balanced_percentile(
        scores,
        source_groups,
        percentile=percentile,
    )
    spec = {
        "mode": threshold_mode,
        "global": float(global_threshold),
        "known_acceptance": float(known_acceptance),
    }
    if threshold_mode == "per-class":
        per_class_values = source_balanced_percentiles_by_label(
            scores,
            labels,
            source_groups,
            num_classes=len(known_classes),
            percentile=percentile,
        )
        spec["per_class_thresholds"] = {
            class_name: float(per_class_values[index])
            for index, class_name in enumerate(known_classes)
        }
    return spec


def threshold_for_prediction(
    threshold_spec: dict,
    prediction: int,
    known_classes: Sequence[str],
) -> float:
    if threshold_spec["mode"] == "global":
        return float(threshold_spec["global"])
    class_name = known_classes[int(prediction)]
    return float(threshold_spec["per_class_thresholds"][class_name])


def acceptance_mask(
    scores: np.ndarray,
    predictions: np.ndarray,
    threshold_spec: dict,
    known_classes: Sequence[str],
) -> np.ndarray:
    scores = np.asarray(scores, dtype=np.float32)
    predictions = np.asarray(predictions, dtype=np.int64)
    if threshold_spec["mode"] == "global":
        return scores >= float(threshold_spec["global"])
    thresholds = np.asarray(
        [
            float(threshold_spec["per_class_thresholds"][class_name])
            for class_name in known_classes
        ],
        dtype=np.float32,
    )
    return scores >= thresholds[predictions]


def write_csv(path: Path, rows: Sequence[dict]) -> None:
    if not rows:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def source_prediction_rows(window_rows: Sequence[dict]) -> list[dict]:
    grouped: dict[tuple[str, str], list[dict]] = defaultdict(list)
    for row in window_rows:
        grouped[(row["split"], row["source_group"])].append(row)
    outputs = []
    score_names = ("msp", "energy", "prototype_cosine")
    for (split, source_group), rows in sorted(grouped.items()):
        output = {
            "split": split,
            "source_group": source_group,
            "source_file": rows[0]["source_file"],
            "true_class": rows[0]["true_class"],
            "windows": len(rows),
        }
        for score_name in score_names:
            accepted = np.asarray(
                [row[f"{score_name}_accepted"] for row in rows],
                dtype=np.float32,
            )
            scores = np.asarray(
                [row[f"{score_name}_score"] for row in rows],
                dtype=np.float32,
            )
            output[f"{score_name}_mean_score"] = float(scores.mean())
            output[f"{score_name}_accepted_rate"] = float(accepted.mean())
        outputs.append(output)
    return outputs


def false_accept_breakdown(
    unknown_labels: np.ndarray,
    predictions: np.ndarray,
    accepted: np.ndarray,
    unknown_classes: Sequence[str],
    known_classes: Sequence[str],
) -> dict:
    result = {}
    for unknown_index, unknown_name in enumerate(unknown_classes):
        mask = unknown_labels == unknown_index
        total = int(mask.sum())
        accepted_mask = mask & accepted
        family, _suffix = split_individual_name(unknown_name)
        same_known = f"{family}_01"
        same_index = known_classes.index(same_known) if same_known in known_classes else -1
        same_count = int((accepted_mask & (predictions == same_index)).sum())
        other_count = int(accepted_mask.sum()) - same_count
        result[unknown_name] = {
            "support": total,
            "rejection_rate": float(1.0 - accepted_mask.sum() / max(total, 1)),
            "same_model_false_accept_rate": float(same_count / max(total, 1)),
            "other_model_false_accept_rate": float(other_count / max(total, 1)),
        }
    return result


def compute_basic_metrics(
    known_labels: np.ndarray,
    known_scores: np.ndarray,
    known_predictions: np.ndarray,
    unknown_labels: np.ndarray,
    unknown_scores: np.ndarray,
    unknown_predictions: np.ndarray,
    threshold_spec: dict,
    manifest: dict,
) -> dict:
    known_classes = manifest["known_classes"]
    known_accept = acceptance_mask(
        known_scores,
        known_predictions,
        threshold_spec,
        known_classes,
    )
    unknown_accept = acceptance_mask(
        unknown_scores,
        unknown_predictions,
        threshold_spec,
        known_classes,
    )
    correct = known_predictions == known_labels
    y_true = np.concatenate(
        (np.ones_like(known_scores, dtype=np.int8), np.zeros_like(unknown_scores, dtype=np.int8))
    )
    y_score = np.concatenate((known_scores, unknown_scores))
    try:
        auroc = float(roc_auc_score(y_true, y_score))
    except ValueError:
        auroc = float("nan")
    try:
        aupr = float(average_precision_score(y_true, y_score))
    except ValueError:
        aupr = float("nan")
    far_threshold = (
        float(np.percentile(known_scores, 5.0))
        if len(known_scores)
        else float(threshold_spec["global"])
    )
    far_at_95 = float((unknown_scores >= far_threshold).mean()) if len(unknown_scores) else 0.0
    result = {
        "threshold_mode": threshold_spec["mode"],
        "threshold": float(threshold_spec["global"]),
        "known_acceptance": float(known_accept.mean()) if len(known_accept) else 0.0,
        "known_false_rejection": float(1.0 - known_accept.mean()) if len(known_accept) else 0.0,
        "known_correct_and_accepted": float((known_accept & correct).mean()) if len(correct) else 0.0,
        "accepted_known_exact": float(correct[known_accept].mean()) if known_accept.any() else 0.0,
        "unknown_rejection": float((~unknown_accept).mean()) if len(unknown_accept) else 0.0,
        "unknown_false_acceptance": float(unknown_accept.mean()) if len(unknown_accept) else 0.0,
        "auroc": auroc,
        "aupr": aupr,
        "unknown_far_at_95_known_tpr": far_at_95,
        "far_at_95_known_tpr_threshold": far_threshold,
        "per_unknown_class": false_accept_breakdown(
            unknown_labels,
            unknown_predictions,
            unknown_accept,
            manifest["unknown_classes"],
            manifest["known_classes"],
        ),
    }
    if threshold_spec["mode"] != "global":
        result["per_class_thresholds"] = threshold_spec["per_class_thresholds"]
    return result


def per_source_metric_table(
    labels: np.ndarray,
    scores: np.ndarray,
    predictions: np.ndarray,
    source_groups: Sequence[str],
    is_known: bool,
    threshold_spec: dict,
    known_classes: Sequence[str],
) -> dict[str, np.ndarray]:
    grouped: dict[str, list[int]] = defaultdict(list)
    for index, source_group in enumerate(source_groups):
        grouped[str(source_group)].append(index)

    support = []
    mean_scores = []
    known_acceptance = []
    known_correct_and_accepted = []
    unknown_rejection = []
    unknown_false_acceptance = []
    for indexes in grouped.values():
        idx = np.asarray(indexes, dtype=np.int64)
        source_scores = scores[idx]
        accepted = acceptance_mask(
            source_scores,
            predictions[idx],
            threshold_spec,
            known_classes,
        )
        support.append(int(len(idx)))
        mean_scores.append(float(source_scores.mean()))
        if is_known:
            correct = predictions[idx] == labels[idx]
            known_acceptance.append(float(accepted.mean()))
            known_correct_and_accepted.append(float((accepted & correct).mean()))
        else:
            unknown_rejection.append(float((~accepted).mean()))
            unknown_false_acceptance.append(float(accepted.mean()))

    return {
        "support": np.asarray(support, dtype=np.int64),
        "mean_score": np.asarray(mean_scores, dtype=np.float64),
        "known_acceptance": np.asarray(known_acceptance, dtype=np.float64),
        "known_correct_and_accepted": np.asarray(
            known_correct_and_accepted,
            dtype=np.float64,
        ),
        "unknown_rejection": np.asarray(unknown_rejection, dtype=np.float64),
        "unknown_false_acceptance": np.asarray(
            unknown_false_acceptance,
            dtype=np.float64,
        ),
    }


def source_cluster_bootstrap_ci(
    known_labels: np.ndarray,
    known_scores: np.ndarray,
    known_predictions: np.ndarray,
    known_source_groups: Sequence[str],
    unknown_labels: np.ndarray,
    unknown_scores: np.ndarray,
    unknown_predictions: np.ndarray,
    unknown_source_groups: Sequence[str],
    threshold_spec: dict,
    known_classes: Sequence[str],
    iterations: int,
    seed: int,
) -> dict:
    if iterations <= 0:
        return {}
    known_table = per_source_metric_table(
        known_labels,
        known_scores,
        known_predictions,
        known_source_groups,
        is_known=True,
        threshold_spec=threshold_spec,
        known_classes=known_classes,
    )
    unknown_table = per_source_metric_table(
        unknown_labels,
        unknown_scores,
        unknown_predictions,
        unknown_source_groups,
        is_known=False,
        threshold_spec=threshold_spec,
        known_classes=known_classes,
    )
    known_count = len(known_table["mean_score"])
    unknown_count = len(unknown_table["mean_score"])
    if known_count == 0 or unknown_count == 0:
        return {}

    rng = np.random.default_rng(seed)
    values: dict[str, list[float]] = defaultdict(list)
    for _ in range(iterations):
        known_idx = rng.integers(known_count, size=known_count)
        unknown_idx = rng.integers(unknown_count, size=unknown_count)
        values["known_acceptance"].append(
            float(known_table["known_acceptance"][known_idx].mean())
        )
        values["known_correct_and_accepted"].append(
            float(known_table["known_correct_and_accepted"][known_idx].mean())
        )
        values["unknown_rejection"].append(
            float(unknown_table["unknown_rejection"][unknown_idx].mean())
        )
        values["unknown_false_acceptance"].append(
            float(unknown_table["unknown_false_acceptance"][unknown_idx].mean())
        )

        y_true = np.concatenate(
            (
                np.ones(known_count, dtype=np.int8),
                np.zeros(unknown_count, dtype=np.int8),
            )
        )
        y_score = np.concatenate(
            (
                known_table["mean_score"][known_idx],
                unknown_table["mean_score"][unknown_idx],
            )
        )
        try:
            values["auroc"].append(float(roc_auc_score(y_true, y_score)))
        except ValueError:
            pass
        try:
            values["aupr"].append(float(average_precision_score(y_true, y_score)))
        except ValueError:
            pass
    return {
        name: {
            "low": float(np.percentile(metric_values, 2.5)),
            "high": float(np.percentile(metric_values, 97.5)),
        }
        for name, metric_values in values.items()
        if metric_values
    }


def plot_roc(path: Path, metrics_inputs: dict[str, tuple[np.ndarray, np.ndarray]]) -> None:
    plt.figure(figsize=(7, 5))
    for score_name, (known_scores, unknown_scores) in metrics_inputs.items():
        y_true = np.concatenate(
            (
                np.ones_like(known_scores, dtype=np.int8),
                np.zeros_like(unknown_scores, dtype=np.int8),
            )
        )
        y_score = np.concatenate((known_scores, unknown_scores))
        if len(np.unique(y_true)) < 2:
            continue
        fpr, tpr, _thresholds = roc_curve(y_true, y_score)
        auc = roc_auc_score(y_true, y_score)
        plt.plot(fpr, tpr, label=f"{score_name} AUC={auc:.3f}")
    plt.plot([0, 1], [0, 1], linestyle="--", color="0.7")
    plt.xlabel("False acceptance rate")
    plt.ylabel("Known acceptance rate")
    plt.legend()
    plt.tight_layout()
    plt.savefig(path, dpi=160)
    plt.close()


def evaluate_seed(args: argparse.Namespace, seed: int) -> None:
    raise_if_cancelled()
    seed_dir = Path(args.output_dir) / f"seed_{seed}"
    args.output_dir = str(seed_dir)
    manifest, views = build_views(args, is_train_base=False)
    write_json(
        seed_dir / "split_audit.json",
        validate_split_manifest(manifest),
    )
    checkpoint_path = Path(args.checkpoint or seed_dir / "best_checkpoint.pth")
    if not checkpoint_path.exists():
        raise FileNotFoundError(f"Checkpoint not found: {checkpoint_path}")
    checkpoint = torch.load(checkpoint_path, map_location=cnntrans.device, weights_only=False)
    state_dict = checkpoint.get("model_state_dict", checkpoint)
    model = cnntrans.IqClassifier(
        num_classes=len(manifest["known_classes"]),
        use_gpu_stft=not args.cpu_stft,
        augment=False,
        dropout=args.dropout,
        model_arch=args.model_arch,
        classifier_head="ce",
        embedding_dim=args.embedding_dim,
        feature_mode=args.feature_mode,
    ).to(cnntrans.device)
    model.load_state_dict(state_dict)

    embeddings_path = seed_dir / "embeddings_fp16.npz"
    if args.reuse_embeddings and embeddings_path.exists():
        exported = load_embeddings_npz(embeddings_path)
    else:
        exported = export_embeddings(model, views, args)
        save_embeddings_npz(embeddings_path, exported)

    prototype_path = seed_dir / "prototypes.npz"
    if args.reuse_embeddings and prototype_path.exists():
        with np.load(prototype_path, allow_pickle=True) as payload:
            prototypes = np.asarray(payload["prototypes"], dtype=np.float32)
    else:
        prototypes = build_source_balanced_prototypes(
            exported["train"]["embeddings"],
            exported["train"]["labels"],
            exported["train"]["source_groups"],
            len(manifest["known_classes"]),
        )
        np.savez_compressed(
            prototype_path,
            prototypes=prototypes.astype(np.float32),
            class_names=np.asarray(manifest["known_classes"], dtype=object),
        )

    score_payloads = {
        role: classification_scores(
            values["logits"],
            values["embeddings"],
            prototypes,
        )
        for role, values in exported.items()
    }
    thresholds = {
        score_name: source_balanced_percentile(
            score_payloads["calibration"][score_name][0],
            exported["calibration"]["source_groups"],
            percentile=(1.0 - args.known_acceptance) * 100.0,
        )
        for score_name in ("msp", "energy", "prototype_cosine")
    }

    window_rows = []
    for role in OPEN_SET_ROLES:
        window_rows.extend(
            rows_for_split(
                role,
                exported[role],
                score_payloads[role],
                manifest,
                thresholds,
            )
        )
    write_csv(seed_dir / "window_predictions.csv", window_rows)
    write_csv(seed_dir / "source_predictions.csv", source_prediction_rows(window_rows))

    metrics = {}
    metrics_rows = []
    roc_inputs = {}
    for score_name in ("msp", "energy", "prototype_cosine"):
        known_scores, known_predictions = score_payloads["known_test"][score_name]
        unknown_scores, unknown_predictions = score_payloads["unknown_test"][score_name]
        roc_inputs[score_name] = (known_scores, unknown_scores)
        score_metrics = compute_basic_metrics(
            exported["known_test"]["labels"],
            known_scores,
            known_predictions,
            exported["unknown_test"]["labels"],
            unknown_scores,
            unknown_predictions,
            thresholds[score_name],
            manifest,
        )
        score_metrics["bootstrap_95ci"] = source_cluster_bootstrap_ci(
            exported["known_test"]["labels"],
            known_scores,
            known_predictions,
            exported["known_test"]["source_groups"],
            exported["unknown_test"]["labels"],
            unknown_scores,
            unknown_predictions,
            exported["unknown_test"]["source_groups"],
            thresholds[score_name],
            args.bootstrap_iterations,
            seed + 17,
        )
        metrics[score_name] = score_metrics
        row = {"score": score_name}
        for key, value in score_metrics.items():
            if isinstance(value, (int, float)):
                row[key] = value
        metrics_rows.append(row)
    write_json(seed_dir / "metrics.json", metrics)
    write_csv(seed_dir / "metrics.csv", metrics_rows)
    plot_roc(seed_dir / "roc_curves.png", roc_inputs)


def command_audit(args: argparse.Namespace) -> None:
    cnntrans.set_global_seed(args.seed)
    base = build_base_dataset(args, is_train=False, use_cache=False)
    manifest = load_or_create_manifest(args, base)
    audit = validate_split_manifest(manifest)
    audit.update(
        {
            "data_root": str(Path(args.data_root).resolve()),
            "output_dir": str(Path(args.output_dir).resolve()),
            "official_validation_indexed_for_audit": True,
            "open_set_roles": list(OPEN_SET_ROLES),
            "tuning_roles": sorted(TUNING_ROLES),
            "known_classes": manifest["known_classes"],
            "unknown_classes": manifest["unknown_classes"],
        }
    )
    write_json(Path(args.output_dir) / "split_audit.json", audit)
    print(json.dumps(audit, ensure_ascii=False, indent=2), flush=True)


def write_open_set_run_summary(root_output: Path, seed_results: list[dict]) -> None:
    completed = [item for item in seed_results if item["status"] == "completed"]
    best = max(completed, key=lambda item: float(item.get("best_metric", 0.0)), default=None)
    write_json(
        root_output / "run_summary.json",
        {
            "status": "completed" if completed and len(completed) == len(seed_results) else "partial",
            "model_mode": "open_set",
            "seeds": seed_results,
            "completed_seed_count": len(completed),
            "failed_seed_count": sum(item["status"] == "failed" for item in seed_results),
            "cancelled_seed_count": sum(item["status"] == "cancelled" for item in seed_results),
            "best_seed": best.get("seed") if best else None,
            "best_metric": best.get("best_metric", 0.0) if best else 0.0,
            "best_seed_output_dir": best.get("output_dir", "") if best else "",
            "best_model_path": best.get("best_model_path", "") if best else "",
            "final_model_path": best.get("final_model_path", "") if best else "",
        },
    )


def command_train(args: argparse.Namespace) -> None:
    root_output = Path(args.output_dir)
    seed_results = []
    for seed in args.seeds:
        seed_value = int(seed)
        seed_dir = root_output / f"seed_{seed_value}"
        seed_args = argparse.Namespace(**vars(args))
        seed_args.output_dir = str(root_output)
        try:
            raise_if_cancelled()
            train_one_seed(seed_args, seed_value)
            run_config = {}
            config_path = seed_dir / "run_config.json"
            if config_path.exists():
                run_config = json.loads(config_path.read_text(encoding="utf-8"))
            seed_results.append(
                {
                    "seed": seed_value,
                    "status": "completed",
                    "output_dir": str(seed_dir),
                    "best_model_path": str(seed_dir / "best_model.pth"),
                    "best_checkpoint_path": str(seed_dir / "best_checkpoint.pth"),
                    "final_model_path": str(seed_dir / "final_model.pth"),
                    "metrics_path": str(seed_dir / "metrics.json"),
                    "best_metric": run_config.get("best_selection_macro_f1", 0.0),
                }
            )
        except KeyboardInterrupt:
            seed_results.append(
                {
                    "seed": seed_value,
                    "status": "cancelled",
                    "output_dir": str(root_output / f"seed_{seed_value}"),
                }
            )
            write_open_set_run_summary(root_output, seed_results)
            raise
        except Exception as exc:
            seed_results.append(
                {
                    "seed": seed_value,
                    "status": "failed",
                    "output_dir": str(root_output / f"seed_{seed_value}"),
                    "error": str(exc),
                }
            )
            write_open_set_run_summary(root_output, seed_results)
            raise
    write_open_set_run_summary(root_output, seed_results)


def command_evaluate(args: argparse.Namespace) -> None:
    root_output = Path(args.output_dir)
    for seed in args.seeds:
        seed_args = argparse.Namespace(**vars(args))
        seed_args.output_dir = str(root_output)
        evaluate_seed(seed_args, int(seed))


def add_common_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--data-root", default=DEFAULT_DATA_ROOT)
    parser.add_argument("--output-dir", default=DEFAULT_OUTPUT_DIR)
    parser.add_argument("--cache-dir", default=DEFAULT_CACHE_DIR)
    parser.add_argument("--cache-data-root", default="")
    parser.add_argument("--known-suffix", default="01")
    parser.add_argument("--unknown-suffix", default="02")
    parser.add_argument("--known-acceptance", type=float, default=0.95)
    parser.add_argument("--val-ratio", type=float, default=0.2)
    parser.add_argument("--seed", type=int, default=DEFAULT_SEEDS[0])
    parser.add_argument("--seeds", type=int, nargs="+", default=list(DEFAULT_SEEDS))
    parser.add_argument("--model-arch", default="axis_profile_xlarge")
    parser.add_argument("--feature-mode", default="complex_signed_mag")
    parser.add_argument("--stitch-windows", type=int, default=8)
    parser.add_argument("--stitch-stride", type=int, default=1)
    parser.add_argument("--batch-size", type=int, default=32)
    parser.add_argument("--num-workers", type=int, default=4)
    parser.add_argument("--prefetch-factor", type=int, default=1)
    parser.add_argument("--no-persistent-workers", action="store_true")
    parser.add_argument("--no-pin-memory", action="store_true")
    parser.add_argument("--cache-size", type=int, default=2)
    parser.add_argument("--index-cache-dir", default="")
    parser.add_argument("--max-files", type=int, default=None)
    parser.add_argument("--max-files-per-class", type=int, default=0)
    parser.add_argument("--cpu-stft", action="store_true")
    parser.add_argument("--dropout", type=float, default=0.6)
    parser.add_argument("--embedding-dim", type=int, default=512)
    parser.add_argument(
        "--require-cuda",
        action="store_true",
        help="Fail immediately instead of silently falling back to CPU.",
    )


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    audit_parser = subparsers.add_parser("audit")
    add_common_args(audit_parser)
    audit_parser.set_defaults(func=command_audit)

    train_parser = subparsers.add_parser("train")
    add_common_args(train_parser)
    train_parser.add_argument("--epochs", type=int, default=6)
    train_parser.add_argument("--steps-per-epoch", type=int, default=0)
    train_parser.add_argument("--lr", type=float, default=1e-4)
    train_parser.add_argument("--weight-decay", type=float, default=0.01)
    train_parser.add_argument("--label-smoothing", type=float, default=0.1)
    train_parser.add_argument("--supcon-weight", type=float, default=0.05)
    train_parser.add_argument("--supcon-temperature", type=float, default=0.10)
    train_parser.add_argument("--blocks-per-batch", type=int, default=2)
    train_parser.add_argument("--batches-per-file-group", type=int, default=4)
    train_parser.add_argument("--path-weight-power", type=float, default=0.5)
    train_parser.add_argument("--no-augment", action="store_true")
    train_parser.set_defaults(func=command_train)

    eval_parser = subparsers.add_parser("evaluate")
    add_common_args(eval_parser)
    eval_parser.add_argument("--checkpoint", default="")
    eval_parser.add_argument("--bootstrap-iterations", type=int, default=200)
    eval_parser.add_argument("--reuse-embeddings", action="store_true")
    eval_parser.set_defaults(func=command_evaluate)
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> None:
    args = parse_args(argv)
    if args.require_cuda and not torch.cuda.is_available():
        raise RuntimeError(
            "CUDA GPU is required for this open-set task, but torch.cuda.is_available() is false"
        )
    if args.stitch_windows < 1:
        raise ValueError("--stitch-windows must be >= 1.")
    if not 0.0 < args.known_acceptance < 1.0:
        raise ValueError("--known-acceptance must be in (0, 1).")
    known_names, unknown_names = paired_class_names(
        cnntrans.CLASS_NAMES,
        known_suffix=args.known_suffix,
        unknown_suffix=args.unknown_suffix,
    )
    print(
        "Open-set classes: "
        + json.dumps(
            {"known": known_names, "unknown": unknown_names},
            ensure_ascii=False,
        ),
        flush=True,
    )
    args.func(args)


if __name__ == "__main__":
    main(sys.argv[1:])
