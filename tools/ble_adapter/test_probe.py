#!/usr/bin/env python3
"""Regression tests for the profile-aware Linux adapter decoder."""

from __future__ import annotations

import importlib.util
import json
from datetime import datetime, timezone
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import AsyncMock, Mock, patch


MODULE_PATH = Path(__file__).with_name("ge_fit_plus_ln_probe.py")
_spec = importlib.util.spec_from_file_location("ge_fit_plus_ln_probe", MODULE_PATH)
if _spec is None or _spec.loader is None:
    raise RuntimeError("unable to load adapter module")
module = importlib.util.module_from_spec(_spec)
sys.modules["ge_fit_plus_ln_probe"] = module
_spec.loader.exec_module(module)
sys.path.insert(0, str(MODULE_PATH.parent))
import ge_fit_plus_ln_ha_publisher as publisher

PROFILE = module.Profile("example", 1.75, "male", "1990-01-01", 36.0)


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

    def test_profile_rejects_nonfinite_values_and_future_birthday(self) -> None:
        valid = dict(name="example", height_m=1.75, sex="male", birthday="1990-01-01", age=36)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "profile.json"
            for key, value in [(key, value) for key in ("height_m", "age")
                               for value in (float("nan"), float("inf"), 0, -1)] + [
                                   ("birthday", "2999-01-01"), ("birthday", "2000-02-30")]:
                with self.subTest(key=key, value=value):
                    path.write_text(json.dumps({**valid, key: value}))
                    with self.assertRaises(ValueError):
                        module.Profile.from_json(path)
            path.write_text(json.dumps(valid))
            self.assertEqual(module.Profile.from_json(path), PROFILE)

    def test_display_tail_saturates_instead_of_wrapping(self) -> None:
        coefficients = (*module.FIELD_COEF[:7], (0, 0, 300))
        with patch.object(module, "FIELD_COEF", coefficients):
            frame, commit = module.build_display_frames(80, 400, PROFILE)
        self.assertEqual(len(frame), 19)
        self.assertEqual(frame[-2], 255)
        self.assertEqual(frame[-1], sum(frame[:-1]) & 255)
        self.assertEqual(commit, bytes.fromhex("1f05ff1033"))


class PublisherTests(unittest.TestCase):
    def test_measurement_id_is_last_after_source_and_subject(self) -> None:
        client = publisher.HomeAssistantPublisher("https://ha.example", "test-token")
        client.publish = Mock()
        client.publish_measurement(module.compute_weight_only_metrics(80, PROFILE), "one", "2026-01-01T12:00:00Z")
        entities = [call.args[0] for call in client.publish.call_args_list]
        self.assertIn("sensor.ge_fit_plus_ln_subject", entities)
        self.assertIn("sensor.ge_fit_plus_ln_measurement_source", entities)
        self.assertEqual(entities[-1], "sensor.ge_fit_plus_ln_measurement_id")

    def test_fallback_clears_old_impedance_and_publishes_id_last(self) -> None:
        client = publisher.HomeAssistantPublisher("https://ha.example", "test-token")
        states = {}
        events = []

        def publish(entity, state, attributes):
            states[entity] = (state, attributes)
            events.append(entity)

        client.publish = publish
        client.publish_measurement(module.compute_metrics(80, [200] * 8, PROFILE), "one", "2026-01-01T12:00:00Z")
        client.publish_measurement(module.compute_weight_only_metrics(80, PROFILE), "two", "2026-01-02T12:00:00Z")
        for entity in ["sensor.ge_fit_plus_ln_whole_body_impedance"] + [
            f"sensor.ge_fit_plus_ln_impedance_{i}" for i in range(1, 9)
        ]:
            self.assertEqual(states[entity][0], "unknown")
            self.assertEqual(states[entity][1]["measurement_id"], "two")
        self.assertEqual(states["sensor.ge_fit_plus_ln_measurement_source"][0], "estimate")
        self.assertEqual(states["sensor.ge_fit_plus_ln_subject"][0], "primary")
        self.assertEqual(events[-1], "sensor.ge_fit_plus_ln_measurement_id")

    def test_failed_metric_publish_does_not_announce_complete_measurement(self) -> None:
        client = publisher.HomeAssistantPublisher("https://ha.example", "test-token")
        client.publish = Mock(side_effect=RuntimeError("HTTP 500"))
        with self.assertRaises(RuntimeError):
            client.publish_measurement(module.compute_weight_only_metrics(80, PROFILE), "one", "2026-01-01T12:00:00Z")
        self.assertNotIn("sensor.ge_fit_plus_ln_measurement_id",
                         [call.args[0] for call in client.publish.call_args_list])


class CaptureTests(unittest.IsolatedAsyncioTestCase):
    async def test_timestamp_follows_notification_not_connection(self) -> None:
        for result_frame in (True, False):
            with self.subTest(result_frame=result_frame):
                clock = [datetime(2026, 1, 1, 12, 0, tzinfo=timezone.utc)]
                measured_at = datetime(2026, 1, 1, 12, 3, tzinfo=timezone.utc)
                client = Mock(is_connected=True)
                for name in ("connect", "write_gatt_char", "stop_notify", "disconnect"):
                    setattr(client, name, AsyncMock())
                frame = bytearray(23 if result_frame else 7)
                frame[0] = 0xB1 if result_frame else 0x10
                frame[5:7] = (8000).to_bytes(2, "little" if result_frame else "big")

                async def notify(uuid, callback):
                    clock[0] = measured_at
                    callback(None, frame)

                client.start_notify = notify
                rest = Mock()
                with patch.object(publisher, "resolve", AsyncMock()), \
                     patch.object(publisher, "BleakClient", return_value=client), \
                     patch.object(publisher, "utc_now", side_effect=lambda: clock[0]), \
                     patch.object(publisher, "time", Mock(monotonic=Mock(side_effect=[0, 60]))), \
                     patch.object(publisher.asyncio, "sleep", AsyncMock()):
                    self.assertTrue(await publisher.capture_once(None, "Fit Plus", PROFILE, rest, 300))
                rest.publish_measurement.assert_called_once()
                self.assertEqual(rest.publish_measurement.call_args.args[2], measured_at.isoformat())


if __name__ == "__main__":
    unittest.main()
