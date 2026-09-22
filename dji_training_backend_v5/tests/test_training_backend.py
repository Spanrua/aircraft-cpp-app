import json
import tempfile
import unittest
from pathlib import Path

import numpy as np

from training_backend import (
    TRAINER_SCRIPT,
    TrainingConfig,
    audit_manifest,
    build_command,
    build_open_set_command,
    validate_manifest,
)


class TrainingBackendTests(unittest.TestCase):
    def _dataset(self, root: Path, *, valid: bool = True) -> None:
        sample = root / "samples" / "sample.npz"
        sample.parent.mkdir(parents=True, exist_ok=True)
        if valid:
            np.savez_compressed(sample, iq=np.zeros((2, 2, 4096), dtype=np.float32))
        (root / "manifest.jsonl").write_text(
            json.dumps(
                {
                    "path": "samples/sample.npz",
                    "label": "air2_01",
                    "window_index": 1,
                }
            )
            + "\n",
            encoding="utf-8",
        )

    def test_validate_manifest_audits_npz_and_indices(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self._dataset(root)
            report = validate_manifest(root)
            self.assertEqual(report["records"], 1)
            self.assertEqual(report["classes"], 1)
            self.assertEqual(report["npz_files"], 1)
            self.assertEqual(report["status"], "ok")

    def test_audit_reports_bad_shape_and_out_of_range_index(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            sample = root / "sample.npz"
            np.savez_compressed(sample, iq=np.zeros((1, 2, 128), dtype=np.float32))
            (root / "manifest.jsonl").write_text(
                json.dumps({"path": "sample.npz", "label": 0, "window_index": 4}) + "\n",
                encoding="utf-8",
            )
            report = audit_manifest(root)
            self.assertEqual(report["status"], "error")
            self.assertTrue(report["errors"]["malformed_npz"])

    def test_build_command_uses_v5_best_defaults_and_stitch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self._dataset(root)
            config = TrainingConfig(root, root / "run", epochs=2, num_workers=0)
            command = build_command(config, python_executable="python-test")
            self.assertEqual(command[0:3], ["python-test", "-u", str(TRAINER_SCRIPT)])
            self.assertIn("--stitch-windows", command)
            self.assertIn("8", command)
            self.assertIn("--model-arch", command)
            self.assertIn("axis_profile_xlarge", command)
            self.assertIn("--train-data-mode", command)
            self.assertIn("raw", command)
            self.assertIn("--json-events", command)

    def test_build_open_set_command(self):
        command = build_open_set_command(
            Path("/data/version"),
            Path("/data/run"),
            action="train",
            checkpoint=Path("/data/run/best_checkpoint.pth"),
            python_executable="python-test",
            stitch_windows=1,
        )
        self.assertEqual(command[0:3], ["python-test", "-u", command[2]])
        self.assertIn("train", command)
        self.assertIn("1", command)
        self.assertIn("--feature-mode", command)
        self.assertIn("complex_signed_mag", command)

    def test_build_command_routes_open_set_to_baseline(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self._dataset(root)
            config = TrainingConfig(
                root,
                root / "run",
                model_mode="open_set",
                stitch_windows=8,
                stitch_stride=4,
            )
            command = build_command(config, python_executable="python-test")
            self.assertIn("open_set_baseline.py", str(command[2]))
            self.assertIn("train", command)

    def test_build_command_routes_cache_precompute(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self._dataset(root)
            config = TrainingConfig(
                root,
                root / "run",
                cache_dir=root / "cache",
                precompute_cache_only=True,
                stitch_windows=8,
                stitch_stride=4,
            )
            command = build_command(config, python_executable="python-test")
            self.assertIn("--precompute-cache", command)
            self.assertIn("--precompute-batch-size", command)
            self.assertIn("--stitch-windows", command)
            self.assertIn("8", command)

    def test_raw_mode_does_not_pass_cache_dir(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self._dataset(root)
            config = TrainingConfig(
                root,
                root / "run",
                train_data_mode="raw",
                cache_dir=root / "cache",
            )
            command = build_command(config, python_executable="python-test")
            self.assertNotIn("--cache-dir", command)

    def test_cache_mode_requires_and_passes_cache_dir(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self._dataset(root)
            cache_dir = root / "cache"
            cache_dir.mkdir()
            (cache_dir / "cache_summary.json").write_text("{}", encoding="utf-8")
            config = TrainingConfig(
                root,
                root / "run",
                train_data_mode="cache",
                cache_dir=cache_dir,
            )
            command = build_command(config, python_executable="python-test")
            self.assertIn("--cache-dir", command)
            self.assertIn(str(cache_dir), command)

    def test_open_set_preserves_extra_args(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self._dataset(root)
            config = TrainingConfig(
                root,
                root / "run",
                model_mode="open_set",
                extra_args=["--max-files", "1"],
            )
            command = build_command(config, python_executable="python-test")
            self.assertIn("--max-files", command)
            self.assertIn("1", command)

    def test_require_cuda_is_forwarded_to_both_backends(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self._dataset(root)
            closed = build_command(
                TrainingConfig(root, root / "closed", require_cuda=True),
                python_executable="python-test",
            )
            opened = build_command(
                TrainingConfig(root, root / "open", model_mode="open_set", require_cuda=True),
                python_executable="python-test",
            )
            self.assertIn("--require-cuda", closed)
            self.assertIn("--require-cuda", opened)

    def test_manifest_split_counts_are_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            sample = root / "samples" / "sample.npz"
            sample.parent.mkdir(parents=True, exist_ok=True)
            np.savez_compressed(sample, iq=np.zeros((3, 2, 4096), dtype=np.float32))
            (root / "manifest.jsonl").write_text(
                "\n".join(
                    json.dumps(
                        {
                            "path": "samples/sample.npz",
                            "label": "air2_01",
                            "window_index": index,
                            "split": split,
                        }
                    )
                    for index, split in enumerate(("train", "val", "test"))
                )
                + "\n",
                encoding="utf-8",
            )
            report = audit_manifest(root)
            self.assertEqual(report["split_source"], "manifest")
            self.assertEqual(report["split_counts"]["test"], 1)

    def test_explicit_manifest_split_is_not_recomputed(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            sample = root / "samples" / "sample.npz"
            sample.parent.mkdir(parents=True, exist_ok=True)
            np.savez_compressed(sample, iq=np.zeros((3, 2, 4096), dtype=np.float32))
            (root / "manifest.jsonl").write_text(
                "\n".join(
                    json.dumps(
                        {
                            "path": "samples/sample.npz",
                            "label": "air2_01",
                            "window_index": index,
                            "split": split,
                        }
                    )
                    for index, split in enumerate(("train", "val", "test"))
                )
                + "\n",
                encoding="utf-8",
            )
            from CNNTRANS_v5_3943B import IqrawNpzDataset

            lengths = {
                split: len(
                    IqrawNpzDataset(
                        root,
                        split=split,
                        val_ratio=0.9,
                        internal_dev_ratio=0.9,
                        is_train=False,
                    )
                )
                for split in ("train", "val", "test")
            }
            self.assertEqual(lengths, {"train": 1, "val": 1, "test": 1})


if __name__ == "__main__":
    unittest.main()
