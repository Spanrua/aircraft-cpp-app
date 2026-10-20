from __future__ import annotations

import sys
import unittest
import csv
import json
import subprocess
from datetime import datetime, timedelta
from pathlib import Path
from tempfile import TemporaryDirectory


ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
TEST_TEMP_ROOT = ROOT / "data" / "sweep" / "test_tmp"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import optimize_sweep_classifier as classifier


def make_records() -> list[object]:
    rows: list[object] = []
    samples = {
        "air3s": ((0.0, 0.0), (0.1, 0.0)),
        "min5pro": ((1.0, 1.0), (1.1, 1.0)),
        "yu4pro": ((2.0, 2.0), (2.1, 2.0)),
    }
    index = 0
    for label, values in samples.items():
        for value in values:
            rows.append(
                classifier.Record(
                    source_file=f"3943b_psc_sweep_20261007_1200{index:02d}_000.csv",
                    label=label,
                    timestamp=datetime(2026, 10, 7) + timedelta(seconds=index),
                    values=value,
                )
            )
            index += 1
    return rows


class SweepDeploymentModelTests(unittest.TestCase):
    def test_trains_deployment_artifact_with_feature_contract(self) -> None:
        artifact = classifier.train_deployment_model(
            make_records(),
            ["event_score_max", "frame_peak_p95"],
            seed=7,
        )

        self.assertEqual(artifact["schema_version"], "sweep_file_classifier_v1")
        self.assertEqual(
            artifact["feature_names"],
            ["event_score_max", "frame_peak_p95"],
        )
        self.assertEqual(artifact["class_names"], list(classifier.LABELS))
        self.assertEqual(artifact["training_sample_count"], 6)

    def test_predicts_from_named_features_and_returns_probabilities(self) -> None:
        artifact = classifier.train_deployment_model(
            make_records(),
            ["event_score_max", "frame_peak_p95"],
            seed=7,
        )

        prediction = classifier.predict_deployment_model(
            artifact,
            {"event_score_max": 1.05, "frame_peak_p95": 1.0},
        )

        self.assertEqual(prediction["label"], "min5pro")
        self.assertGreater(prediction["confidence"], 0.0)
        self.assertLessEqual(prediction["confidence"], 1.0)
        self.assertEqual(set(prediction["probabilities"]), set(classifier.LABELS))
        self.assertAlmostEqual(sum(prediction["probabilities"].values()), 1.0)

    def test_saved_artifact_reloads_with_the_same_prediction(self) -> None:
        artifact = classifier.train_deployment_model(
            make_records(),
            ["event_score_max", "frame_peak_p95"],
            seed=7,
        )
        features = {"event_score_max": 1.05, "frame_peak_p95": 1.0}
        TEST_TEMP_ROOT.mkdir(parents=True, exist_ok=True)
        with TemporaryDirectory(dir=TEST_TEMP_ROOT) as tmp:
            path = Path(tmp) / "model.pkl"
            classifier.save_deployment_model(artifact, path)
            restored = classifier.load_deployment_model(path)

        self.assertEqual(
            classifier.predict_deployment_model(restored, features),
            classifier.predict_deployment_model(artifact, features),
        )

    def test_parser_accepts_a_deployment_model_output_path(self) -> None:
        args = classifier.parse_args(["--export-model", "models/sweep_model.pkl"])

        self.assertEqual(args.export_model, Path("models/sweep_model.pkl"))

    def test_parser_accepts_csv_classification_arguments(self) -> None:
        args = classifier.parse_args(
            [
                "--classify-csv",
                "sweeps/window.csv",
                "--model-path",
                "models/sweep_model.pkl",
            ]
        )

        self.assertEqual(args.classify_csv, Path("sweeps/window.csv"))
        self.assertEqual(args.model_path, Path("models/sweep_model.pkl"))

    def test_classifies_one_sweep_csv_from_its_extracted_features(self) -> None:
        artifact = classifier.train_deployment_model(
            make_records(),
            ["top_event_score", "frame_peak_p95"],
            seed=7,
        )
        TEST_TEMP_ROOT.mkdir(parents=True, exist_ok=True)
        with TemporaryDirectory(dir=TEST_TEMP_ROOT) as tmp:
            root = Path(tmp)
            model_path = root / "model.pkl"
            csv_path = root / "3943b_psc_sweep_20261007_120000_001.csv"
            classifier.save_deployment_model(artifact, model_path)
            with csv_path.open("w", encoding="utf-8", newline="") as handle:
                writer = csv.writer(handle)
                writer.writerow(["frame_index", "bin_index", "frequency_hz", "level_dbuv"])
                for frame_index in range(3):
                    for bin_index, level_dbuv in enumerate([0, 1, 35, 36, 35, 1, 0]):
                        writer.writerow(
                            [
                                frame_index,
                                bin_index,
                                5_725_000_000 + bin_index * 5_000,
                                level_dbuv,
                            ]
                        )

            result = classifier.classify_sweep_csv(csv_path, model_path)

        self.assertEqual(result["source_file"], csv_path.name)
        self.assertEqual(result["feature_count"], 2)
        self.assertEqual(set(result["prediction"]["probabilities"]), set(classifier.LABELS))

    def test_rejects_csv_outside_the_model_frequency_contract(self) -> None:
        artifact = classifier.train_deployment_model(
            make_records(),
            ["top_event_score", "frame_peak_p95"],
            seed=7,
        )
        TEST_TEMP_ROOT.mkdir(parents=True, exist_ok=True)
        with TemporaryDirectory(dir=TEST_TEMP_ROOT) as tmp:
            root = Path(tmp)
            model_path = root / "model.pkl"
            csv_path = root / "3943b_psc_sweep_20261007_120000_002.csv"
            classifier.save_deployment_model(artifact, model_path)
            with csv_path.open("w", encoding="utf-8", newline="") as handle:
                writer = csv.writer(handle)
                writer.writerow(["frame_index", "bin_index", "frequency_hz", "level_dbuv"])
                for frame_index in range(3):
                    for bin_index, level_dbuv in enumerate([0, 1, 35, 36, 35, 1, 0]):
                        writer.writerow(
                            [
                                frame_index,
                                bin_index,
                                2_400_000_000 + bin_index * 5_000,
                                level_dbuv,
                            ]
                        )

            with self.assertRaisesRegex(ValueError, "frequency contract"):
                classifier.classify_sweep_csv(csv_path, model_path)

    def test_cli_reports_frequency_contract_rejection_as_json(self) -> None:
        artifact = classifier.train_deployment_model(
            make_records(),
            ["top_event_score", "frame_peak_p95"],
            seed=7,
        )
        TEST_TEMP_ROOT.mkdir(parents=True, exist_ok=True)
        with TemporaryDirectory(dir=TEST_TEMP_ROOT) as tmp:
            root = Path(tmp)
            model_path = root / "model.pkl"
            csv_path = root / "3943b_psc_sweep_20261007_120000_003.csv"
            classifier.save_deployment_model(artifact, model_path)
            with csv_path.open("w", encoding="utf-8", newline="") as handle:
                writer = csv.writer(handle)
                writer.writerow(["frame_index", "bin_index", "frequency_hz", "level_dbuv"])
                for frame_index in range(3):
                    for bin_index, level_dbuv in enumerate([0, 1, 35, 36, 35, 1, 0]):
                        writer.writerow(
                            [
                                frame_index,
                                bin_index,
                                2_400_000_000 + bin_index * 5_000,
                                level_dbuv,
                            ]
                        )

            completed = subprocess.run(
                [
                    sys.executable,
                    str(TOOLS / "optimize_sweep_classifier.py"),
                    "--classify-csv",
                    str(csv_path),
                    "--model-path",
                    str(model_path),
                ],
                capture_output=True,
                text=True,
                check=False,
            )

        self.assertEqual(completed.returncode, 2)
        self.assertIn("frequency contract", json.loads(completed.stdout)["error"])
        self.assertNotIn("Traceback", completed.stderr)


if __name__ == "__main__":
    unittest.main()
