import csv
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT_PATH = Path(__file__).resolve().parents[1] / "tools" / "analyze_sweep_candidates.py"
SPEC = importlib.util.spec_from_file_location("analyze_sweep_candidates", SCRIPT_PATH)
assert SPEC is not None and SPEC.loader is not None
sweep = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = sweep
SPEC.loader.exec_module(sweep)


def write_sweep_csv(path: Path, rows, with_metadata: bool = False) -> None:
    header = ["frame_index", "bin_index", "frequency_hz", "level_dbuv"]
    if with_metadata:
        header += ["drone_id", "model", "behavior_code", "behavior_name"]
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(header)
        for row in rows:
            writer.writerow(row)


def frame_rows(frame_index: int, levels, model: str | None = None, step_hz: int = 5_000):
    rows = []
    for bin_index, level in enumerate(levels):
        row = [frame_index, bin_index, 5_725_000_000 + bin_index * step_hz, level]
        if model is not None:
            row += ["D01", model, "UNKNOWN", "未设置"]
        rows.append(row)
    return rows


class SweepCandidateAnalysisTests(unittest.TestCase):
    def test_nominal_video_bandwidth_uses_expected_10_20_40_mhz_bins(self):
        self.assertEqual(sweep.nominal_video_bandwidth_mhz(8.0), 10.0)
        self.assertEqual(sweep.nominal_video_bandwidth_mhz(19.0), 20.0)
        self.assertEqual(sweep.nominal_video_bandwidth_mhz(34.0), 40.0)

    def test_reads_legacy_four_column_csv_without_label(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "legacy.csv"
            rows = []
            rows += frame_rows(0, [0, 1, 35, 36, 35, 1, 0])
            rows += frame_rows(1, [0, 2, 34, 37, 36, 1, 0])
            write_sweep_csv(path, rows)

            analysis = sweep.read_file_analysis(path)

        self.assertEqual(analysis.metadata.model, "")
        self.assertEqual(analysis.metadata.signal_noise_label, "")
        self.assertEqual(len(analysis.events), 1)
        self.assertGreater(analysis.features["top_event_score"], 0)

    def test_reads_eight_column_csv_and_infers_signal_label(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "signal.csv"
            rows = []
            rows += frame_rows(0, [0, 1, 35, 36, 35, 1, 0], model="yu4pro")
            rows += frame_rows(1, [0, 2, 34, 37, 36, 1, 0], model="yu4pro")
            write_sweep_csv(path, rows, with_metadata=True)

            analysis = sweep.read_file_analysis(path)

        self.assertEqual(analysis.metadata.normalized_model, "yu4pro")
        self.assertEqual(analysis.metadata.signal_noise_label, "signal")
        self.assertEqual(analysis.metadata.type_label, "yu4pro")

    def test_reads_noise_label_from_model_column(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "noise.csv"
            rows = []
            rows += frame_rows(0, [0, 1, 2, 3, 2, 1, 0], model="noise")
            rows += frame_rows(1, [0, 1, 2, 3, 2, 1, 0], model="noise")
            write_sweep_csv(path, rows, with_metadata=True)

            analysis = sweep.read_file_analysis(path)

        self.assertEqual(analysis.metadata.signal_noise_label, "noise")
        self.assertEqual(analysis.metadata.type_label, "")
        self.assertEqual(len(analysis.events), 0)

    def test_single_frame_spike_is_not_promoted_to_persistent_event(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "spike.csv"
            rows = []
            rows += frame_rows(0, [0, 1, 50, 1, 0])
            rows += frame_rows(1, [0, 1, 2, 1, 0])
            rows += frame_rows(2, [0, 1, 2, 1, 0])
            write_sweep_csv(path, rows)

            analysis = sweep.read_file_analysis(path)

        self.assertEqual(len(analysis.events), 0)
        self.assertEqual(analysis.features["qualified_event_count"], 0)

    def test_event_record_reports_nominal_video_bandwidth_and_observed_span(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "wide.csv"
            rows = []
            levels = [0] * 50
            for index in range(10, 30):
                levels[index] = 35
            rows += frame_rows(0, levels, step_hz=1_000_000)
            rows += frame_rows(1, levels, step_hz=1_000_000)
            write_sweep_csv(path, rows)

            analysis = sweep.read_file_analysis(path)
            record = sweep.event_record("C001", analysis.events[0], analysis)

        self.assertEqual(record["bandwidth_mhz"], 20.0)
        self.assertEqual(record["nominal_bandwidth_mhz"], 20.0)
        self.assertEqual(record["observed_bandwidth_mhz"], 19.0)
        self.assertAlmostEqual(record["bandwidth_match_score"], 0.95)

    def test_deep_input_is_fixed_shape_and_contains_relative_energy(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "deep_input.csv"
            rows = []
            rows += frame_rows(0, [0, 1, 35, 36, 35, 1, 0], model="yu4pro")
            rows += frame_rows(1, [0, 2, 34, 37, 36, 1, 0], model="yu4pro")
            write_sweep_csv(path, rows, with_metadata=True)

            analysis = sweep.read_file_analysis(path)
            image = sweep.deep_input_for_analysis(analysis)

        self.assertEqual(tuple(image.shape), (3, 96, 256))
        self.assertGreater(float(image.max()), 0.0)

    def test_deep_classifier_can_train_on_small_labeled_set(self):
        try:
            import torch  # noqa: F401
        except ImportError:
            self.skipTest("PyTorch is not installed")

        with tempfile.TemporaryDirectory() as tmp:
            analyses = []
            deep_inputs = {}
            for index, model in enumerate(["yu4pro"] * 4 + ["noise"] * 4):
                path = Path(tmp) / f"deep_{index}.csv"
                rows = []
                levels = [0, 1, 35, 36, 35, 1, 0] if model != "noise" else [0, 1, 2, 3, 2, 1, 0]
                rows += frame_rows(0, levels, model=model)
                rows += frame_rows(1, levels, model=model)
                write_sweep_csv(path, rows, with_metadata=True)
                analysis = sweep.read_file_analysis(path)
                analyses.append(analysis)
                deep_inputs[path.name] = sweep.deep_input_for_analysis(analysis)

            classifier, report = sweep.train_deep_classifier(
                analyses,
                lambda analysis: analysis.metadata.signal_noise_label,
                deep_inputs,
                "signal_noise",
                Path(tmp) / "signal_noise.pt",
                epochs=2,
                batch_size=4,
            )

        self.assertIsNotNone(classifier)
        self.assertTrue(report["enabled"])
        self.assertEqual(report["model"], "SweepCNN")


    def test_train_classifier_requires_two_classes(self):
        with tempfile.TemporaryDirectory() as tmp:
            analyses = []
            for index in range(3):
                path = Path(tmp) / f"signal_{index}.csv"
                rows = []
                rows += frame_rows(0, [0, 1, 35, 36, 35, 1, 0], model="air3s")
                rows += frame_rows(1, [0, 2, 34, 37, 36, 1, 0], model="air3s")
                write_sweep_csv(path, rows, with_metadata=True)
                analyses.append(sweep.read_file_analysis(path))

            classifier, report = sweep.train_classifier(
                analyses,
                lambda analysis: analysis.metadata.signal_noise_label,
                sweep.feature_columns(analyses),
            )

        self.assertIsNone(classifier)
        self.assertFalse(report["enabled"])
        self.assertIn("少于两个类别", report["reason"])


if __name__ == "__main__":
    unittest.main()
