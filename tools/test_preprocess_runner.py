from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path


def load_runner_module():
    runner_path = Path(__file__).with_name("preprocess_runner.py")
    spec = importlib.util.spec_from_file_location("preprocess_runner", runner_path)
    if spec is None or spec.loader is None:
        raise RuntimeError("无法加载 preprocess_runner.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class PreprocessRunnerPathTests(unittest.TestCase):
    def test_finds_preprocess_integration_above_nested_worktree_tools_directory(self):
        runner = load_runner_module()
        with tempfile.TemporaryDirectory(dir="D:/Temp") as temporary_dir:
            project_root = Path(temporary_dir) / "aircraft_project"
            integration_dir = project_root / "preprocess_integration"
            integration_dir.mkdir(parents=True)
            nested_script = project_root / "cpp_app" / ".worktrees" / "demo" / "tools" / "preprocess_runner.py"
            nested_script.parent.mkdir(parents=True)
            nested_script.touch()

            self.assertEqual(runner.find_preprocess_integration_dir(nested_script), integration_dir)


if __name__ == "__main__":
    unittest.main()
