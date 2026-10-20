import importlib.util
import sys
import unittest
from importlib.machinery import SourceFileLoader
from pathlib import Path


SCRIPT_PATH = Path(__file__).resolve().parents[1] / "tools" / "signal_noise_binary.pyw"
LOADER = SourceFileLoader("signal_noise_binary", str(SCRIPT_PATH))
SPEC = importlib.util.spec_from_loader(LOADER.name, LOADER)
assert SPEC is not None
signal_noise = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = signal_noise
LOADER.exec_module(signal_noise)


class _FakeModel:
    def to(self, device):
        self.device = device
        return self

    def load_state_dict(self, state_dict, strict=True):
        self.state_dict = state_dict
        self.strict = strict

    def eval(self):
        self.evaluated = True
        return self


class SignalNoiseBinaryTests(unittest.TestCase):
    def test_load_model_uses_safe_checkpoint_format(self):
        original_load = signal_noise.torch.load
        original_model = signal_noise.SignalNoiseCNN
        calls = []

        def fake_load(*args, **kwargs):
            calls.append(kwargs)
            return {"model": {}, "threshold": 0.5}

        signal_noise.torch.load = fake_load
        signal_noise.SignalNoiseCNN = _FakeModel
        try:
            model, threshold = signal_noise._load_model(Path("model.pt"), signal_noise.torch.device("cpu"))
            self.assertIsInstance(model, _FakeModel)
            self.assertEqual(threshold, 0.5)
            self.assertTrue(calls[0]["weights_only"])

            signal_noise.torch.load = lambda *args, **kwargs: {"model": {}, "threshold": 1.5}
            with self.assertRaisesRegex(ValueError, "threshold"):
                signal_noise._load_model(Path("model.pt"), signal_noise.torch.device("cpu"))
        finally:
            signal_noise.torch.load = original_load
            signal_noise.SignalNoiseCNN = original_model


if __name__ == "__main__":
    unittest.main()
