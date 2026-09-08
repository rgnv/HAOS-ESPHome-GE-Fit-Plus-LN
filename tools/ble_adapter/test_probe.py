#!/usr/bin/env python3
"""Regression tests for the profile-aware Linux adapter decoder."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import unittest


MODULE_PATH = Path(__file__).with_name("ge_fit_plus_ln_probe.py")
_spec = importlib.util.spec_from_file_location("ge_fit_plus_ln_probe", MODULE_PATH)
if _spec is None or _spec.loader is None:
    raise RuntimeError("unable to load adapter module")
module = importlib.util.module_from_spec(_spec)
sys.modules["ge_fit_plus_ln_probe"] = module
_spec.loader.exec_module(module)


class WeightOnlyFallbackTests(unittest.TestCase):
    def test_fallback_contains_derived_metrics_without_impedance(self) -> None:
        profile = module.Profile("example", 1.75, "male", "1990-01-01", 36.0)
        metrics = module.compute_weight_only_metrics(80.0, profile)

        self.assertEqual(metrics["source"], "estimate")
        self.assertEqual(metrics["impedance_ohm"], [])
        self.assertIsNone(metrics["whole_body_impedance_ohm"])
        self.assertEqual(metrics["weight_kg"], 80.0)
        self.assertIn("body_fat_percent", metrics)
        self.assertIn("body_water_percent", metrics)
        self.assertIn("fat_free_mass_kg", metrics)


if __name__ == "__main__":
    unittest.main()
