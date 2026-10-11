"""Exercise the production alias resolver without requiring an ESPHome install."""
import ast
import types
import unittest
from pathlib import Path


class ReferenceConfigTests(unittest.TestCase):
    def setUp(self):
        source = Path(__file__).resolve().parents[1] / "src/esphome/aec_speexdsp/__init__.py"
        tree = ast.parse(source.read_text())
        function = next(node for node in tree.body if isinstance(node, ast.FunctionDef)
                        and node.name == "_resolve_reference_activity")
        namespace = {"cv": types.SimpleNamespace(Invalid=ValueError), "CONF_AGC": "agc"}
        exec(compile(ast.Module(body=[function], type_ignores=[]), str(source), "exec"), namespace)
        self.resolve = namespace["_resolve_reference_activity"]

    def test_defaults_and_legacy(self):
        config = {"agc": {"gate": {}}, "near_end": {}}
        self.resolve(config)
        self.assertEqual(config["reference_activity"],
                         dict(open_rms=200, close_rms=100, tail_ms=250, startup_ms=200))
        self.assertEqual(config["near_end"]["startup_ms"], 200)
        config = {"agc": {"gate": {"startup_guard_ms": 96, "reference_open_rms": 300}}, "near_end": {}}
        self.resolve(config)
        self.assertEqual(config["near_end"]["startup_ms"], 96)
        self.assertEqual(config["reference_activity"]["open_rms"], 300)

    def test_canonical_and_equal_aliases(self):
        config = {"agc": {"gate": {"tail_ms": 400}}, "near_end": {"startup_ms": 100},
                  "reference_activity": {"tail_ms": 400, "startup_ms": 100}}
        self.resolve(config)
        self.assertEqual(config["agc"]["gate"]["startup_guard_ms"], 100)

    def test_conflicts_and_hysteresis(self):
        for config in [
            {"agc": {"gate": {"tail_ms": 400}}, "reference_activity": {"tail_ms": 250}},
            {"agc": {"gate": {"startup_guard_ms": 200}}, "near_end": {"startup_ms": 100}},
            {"agc": {"gate": {}}, "reference_activity": {"open_rms": 100, "close_rms": 200}},
        ]:
            with self.assertRaises(ValueError):
                self.resolve(config)
