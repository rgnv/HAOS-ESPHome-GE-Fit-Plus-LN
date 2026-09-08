#!/usr/bin/env python3
"""Publish GE Fit Plus LN BLE readings to Home Assistant over its REST API.

This is the Linux-adapter fallback path. It intentionally keeps all credentials and
profile data outside the repository. The process scans/connects to the configured scale,
replays the protocol handshake, derives the same metrics as the ESPHome component, and
creates/updates the ``sensor.ge_fit_plus_ln_*`` state entities in Home Assistant.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import os
import sys
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from bleak import BleakClient, BleakScanner

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from ge_fit_plus_ln_probe import (  # noqa: E402
    HANDSHAKE,
    NOTIFY_UUID,
    Profile,
    WRITE_UUID,
    decode_frame,
    compute_weight_only_metrics,
)

LOGGER = logging.getLogger("ge_fit_plus_ln_ha_publisher")
MIN_WEIGHT_KG = 5.0
MAX_CAPTURE_SECONDS = 300.0
STABLE_SECONDS = 18.0
COMPUTING_TIMEOUT_SECONDS = 45.0
WEIGHT_STABLE_DELTA_KG = 0.1

METRIC_ENTITIES: dict[str, tuple[str, str | None, str | None, str | None]] = {
    "weight_kg": ("sensor.ge_fit_plus_ln_weight", "kg", "weight", "GE Fit Plus LN Weight"),
    "weight_lb": ("sensor.ge_fit_plus_ln_weight_lb", "lb", "weight", "GE Fit Plus LN Weight (lb)"),
    "bmi": ("sensor.ge_fit_plus_ln_bmi", None, None, "GE Fit Plus LN BMI"),
    "body_fat_percent": ("sensor.ge_fit_plus_ln_body_fat", "%", None, "GE Fit Plus LN Body Fat"),
    "body_water_percent": ("sensor.ge_fit_plus_ln_body_water", "%", None, "GE Fit Plus LN Body Water"),
    "protein_percent": ("sensor.ge_fit_plus_ln_protein", "%", None, "GE Fit Plus LN Protein"),
    "bone_mass_percent": ("sensor.ge_fit_plus_ln_bone_mass", "%", None, "GE Fit Plus LN Bone Mass"),
    "muscle_mass_percent": ("sensor.ge_fit_plus_ln_muscle_mass", "%", None, "GE Fit Plus LN Muscle Mass"),
    "skeletal_muscle_percent": (
        "sensor.ge_fit_plus_ln_skeletal_muscle",
        "%",
        None,
        "GE Fit Plus LN Skeletal Muscle",
    ),
    "fat_free_mass_kg": (
        "sensor.ge_fit_plus_ln_fat_free_mass",
        "kg",
        "weight",
        "GE Fit Plus LN Fat Free Mass",
    ),
    "whole_body_impedance_ohm": (
        "sensor.ge_fit_plus_ln_whole_body_impedance",
        "Ω",
        None,
        "GE Fit Plus LN Whole Body Impedance",
    ),
}


def utc_now() -> datetime:
    return datetime.now(timezone.utc)


class HomeAssistantPublisher:
    """Small synchronous REST client used from an asyncio worker thread."""

    def __init__(self, base_url: str, token: str) -> None:
        self.base_url = base_url.rstrip("/")
        self.token = token

    def publish(self, entity_id: str, state: Any, attributes: dict[str, Any]) -> None:
        import urllib.error
        import urllib.request

        payload = json.dumps({"state": state, "attributes": attributes}).encode()
        request = urllib.request.Request(
            f"{self.base_url}/api/states/{entity_id}",
            data=payload,
            method="POST",
            headers={
                "Authorization": f"Bearer {self.token}",
                "Content-Type": "application/json",
            },
        )
        try:
            with urllib.request.urlopen(request, timeout=15) as response:
                if response.status not in (200, 201):
                    raise RuntimeError(f"HTTP {response.status}")
        except urllib.error.HTTPError as exc:
            body = exc.read(500).decode("utf-8", "replace")
            raise RuntimeError(f"HTTP {exc.code}: {body}") from exc

    def publish_measurement(self, metrics: dict[str, Any], measurement_id: str, measured_at: str) -> None:
        common = {
            "measurement_id": measurement_id,
            "measurement_time": measured_at,
            "source": metrics.get("source", "estimate"),
            "profile": metrics.get("profile"),
            "friendly_name": "GE Fit Plus LN",
        }
        for key, (entity_id, unit, device_class, friendly_name) in METRIC_ENTITIES.items():
            value = metrics.get(key)
            if value is None:
                continue
            attrs = dict(common)
            attrs["friendly_name"] = friendly_name
            if unit is not None:
                attrs["unit_of_measurement"] = unit
            if device_class is not None:
                attrs["device_class"] = device_class
            attrs["state_class"] = "measurement"
            self.publish(entity_id, value, attrs)

        fat_free_mass_kg = metrics.get("fat_free_mass_kg")
        if fat_free_mass_kg is not None:
            self.publish(
                "sensor.ge_fit_plus_ln_fat_free_mass_lb",
                round(float(fat_free_mass_kg) * 2.20462262185, 3),
                {
                    **common,
                    "friendly_name": "GE Fit Plus LN Fat Free Mass (lb)",
                    "unit_of_measurement": "lb",
                    "device_class": "weight",
                    "state_class": "measurement",
                },
            )

        impedance = metrics.get("impedance_ohm") or []
        for index, value in enumerate(impedance, start=1):
            self.publish(
                f"sensor.ge_fit_plus_ln_impedance_{index}",
                value,
                {
                    **common,
                    "friendly_name": f"GE Fit Plus LN Impedance {index}",
                    "unit_of_measurement": "Ω",
                    "state_class": "measurement",
                },
            )
        self.publish(
            "sensor.ge_fit_plus_ln_measurement_id",
            measurement_id,
            {**common, "friendly_name": "GE Fit Plus LN Measurement ID"},
        )
        self.publish(
            "sensor.ge_fit_plus_ln_measurement_source",
            metrics.get("source", "estimate"),
            {**common, "friendly_name": "GE Fit Plus LN Measurement Source"},
        )


async def resolve(address: str | None, name: str, timeout: float = 20.0) -> Any:
    """Resolve the scale by its configured address, then by advertised name.

    Some Android/BlueZ combinations expose the scale with a rotating address. The
    configured address remains the preferred path, while the name fallback avoids
    treating that privacy-address change as a permanently offline scale.
    """
    if address:
        device = await BleakScanner.find_device_by_address(address, timeout=min(timeout, 8.0))
        if device is not None:
            return device

    advertisements = await BleakScanner.discover(timeout=timeout, return_adv=True)
    needle = name.lower().replace(" ", "")
    for device, advertisement in advertisements.values():
        advertised_name = (advertisement.local_name or device.name or "").lower()
        if needle in advertised_name.replace(" ", ""):
            LOGGER.info("resolved scale by advertised name as %s", device.address)
            return device
    raise RuntimeError("scale was not advertising; wake it and retry")


async def capture_once(
    address: str | None,
    name: str,
    profile: Profile,
    publisher: HomeAssistantPublisher,
    capture_seconds: float,
) -> bool:
    device = await resolve(address, name)
    client = BleakClient(device, timeout=20)
    last_live_weight: float | None = None
    last_live_change: float | None = None
    computing_started: float | None = None
    result_seen = False
    notification_count = 0
    publish_task: asyncio.Task[None] | None = None
    published = False
    measured_at = utc_now().isoformat()

    async def publish_metrics(metrics: dict[str, Any]) -> None:
        nonlocal published
        if published:
            return
        try:
            measurement_id = f"{datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')}-{uuid.uuid4().hex[:8]}"
            await asyncio.to_thread(publisher.publish_measurement, metrics, measurement_id, measured_at)
        except Exception:
            LOGGER.exception("failed to publish measurement to Home Assistant")
            raise
        published = True
        LOGGER.info("published measurement %s (%.3f kg, source=%s)", measurement_id, metrics["weight_kg"], metrics["source"])

    def on_notification(_: Any, data: bytearray) -> None:
        nonlocal last_live_weight, last_live_change, computing_started, result_seen, notification_count, publish_task
        notification_count += 1
        decoded = decode_frame(bytes(data), profile)
        if decoded is None:
            return
        now = time.monotonic()
        kind = decoded.get("frame_type")
        if kind == "live_weight":
            weight = float(decoded["weight_kg"])
            if last_live_weight is None or abs(weight - last_live_weight) > WEIGHT_STABLE_DELTA_KG:
                last_live_change = now
            last_live_weight = weight
        elif kind == "computing" and computing_started is None:
            computing_started = now
        elif kind == "result":
            result_seen = True
            metrics = decoded.get("metrics")
            if isinstance(metrics, dict) and publish_task is None:
                publish_task = asyncio.create_task(publish_metrics(metrics))

    try:
        await client.connect()
        LOGGER.info("connected to scale")
        await client.start_notify(NOTIFY_UUID, on_notification)
        for frame in HANDSHAKE:
            await client.write_gatt_char(WRITE_UUID, frame, response=True)
            await asyncio.sleep(0.2)
        await asyncio.sleep(capture_seconds)
        if not result_seen and last_live_weight is not None and last_live_change is not None:
            now = time.monotonic()
            stable_for = now - last_live_change
            computing_for = now - computing_started if computing_started is not None else None
            if stable_for >= STABLE_SECONDS and (
                computing_for is None or computing_for >= COMPUTING_TIMEOUT_SECONDS
            ):
                await publish_metrics(compute_weight_only_metrics(last_live_weight, profile))
        if publish_task is not None:
            await publish_task
        LOGGER.info(
            "capture complete (notifications=%d, live_weight=%s, result=%s, published=%s)",
            notification_count,
            last_live_weight,
            result_seen,
            published,
        )
        return published
    finally:
        if client.is_connected:
            try:
                await client.stop_notify(NOTIFY_UUID)
            except Exception:
                pass
            await client.disconnect()


async def run_forever(
    address: str | None,
    name: str,
    profile: Profile,
    publisher: HomeAssistantPublisher,
    retry_seconds: float,
) -> None:
    while True:
        try:
            await capture_once(address, name, profile, publisher, MAX_CAPTURE_SECONDS)
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            LOGGER.info("scale capture unavailable: %s", exc)
        await asyncio.sleep(retry_seconds)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--address", default=os.environ.get("GE_SCALE_MAC"))
    parser.add_argument("--name", default=os.environ.get("GE_SCALE_NAME", "Fit Plus"))
    parser.add_argument("--profile", type=Path, default=Path(os.environ.get("GE_SCALE_PROFILE", "profile.json")))
    parser.add_argument("--ha-url", default=os.environ.get("HA_URL", "http://10.0.0.123:8123"))
    parser.add_argument("--ha-token", default=os.environ.get("HASS_TOKEN"))
    parser.add_argument("--retry-seconds", type=float, default=5.0)
    parser.add_argument("--log-level", default=os.environ.get("LOG_LEVEL", "INFO"))
    args = parser.parse_args()
    if not args.ha_token:
        parser.error("HASS_TOKEN or --ha-token is required")
    logging.basicConfig(level=getattr(logging, args.log_level.upper()), format="%(asctime)s %(levelname)s %(message)s")
    profile = Profile.from_json(args.profile)
    publisher = HomeAssistantPublisher(args.ha_url, args.ha_token)
    try:
        asyncio.run(run_forever(args.address, args.name, profile, publisher, args.retry_seconds))
    except KeyboardInterrupt:
        return 130
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
