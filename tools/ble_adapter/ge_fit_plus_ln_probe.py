#!/usr/bin/env python3
"""BLE discovery and read-only capture for GE Fit Plus LN.

The default operations only scan, inspect services, or subscribe to notifications.
The optional --handshake flag sends the four unlock/control frames required by the
scale protocol. Display-result write-back requires the separate --write-back flag.

A local JSON profile can be supplied to decode result frames into the same BMI and
body-composition metrics exposed by the ESPHome component. Profiles are intentionally
kept outside the repository by the project's .gitignore.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import math
import sys
import time
from collections import Counter
from dataclasses import dataclass
from datetime import date
from pathlib import Path
from typing import Any

from bleak import BleakClient, BleakScanner

SERVICE_UUID = "0000fff0-0000-1000-8000-00805f9b34fb"
NOTIFY_UUID = "0000fff1-0000-1000-8000-00805f9b34fb"
WRITE_UUID = "0000fff2-0000-1000-8000-00805f9b34fb"
DEVICE_INFO_UUIDS = {
    "device_name": "00002a00-0000-1000-8000-00805f9b34fb",
    "manufacturer": "00002a29-0000-1000-8000-00805f9b34fb",
    "firmware": "00002a26-0000-1000-8000-00805f9b34fb",
    "serial": "00002a25-0000-1000-8000-00805f9b34fb",
    "battery": "00002a19-0000-1000-8000-00805f9b34fb",
}
HANDSHAKE = (
    bytes.fromhex("130aff0210000000b4e2"),
    bytes.fromhex("2009ff3997e631909f"),
    bytes.fromhex("a00d02feffee001906f40402b3"),
    bytes.fromhex("2206ff000128"),
)

LB_PER_KG = 1.0 / 0.45359237
FRAC_WATER = 0.7220
FRAC_PROTEIN = 0.2279
FRAC_BONE = 0.0502
FRAC_SMM = 0.6458
FFM_A = 1234.4413142868493
FFM_B = 0.40454841017657805
FFM_C = 25.48036033753605
FIELD_COEF = (
    (0.0, 0.0, 16.0),
    (-14959.216125107047, 5.811746385808269, -189.21668315058827),
    (-2379.8752926210254, 3.197323288650493, 523.1246185896813),
    (4419.768400600015, -3.080743250352583, 748.7685654763195),
    (524592.5109326105, 26.64716651585372, 806.5305023047146),
    (-403218.8710085171, 10.289345762924421, 2055.113949622363),
    (517792.86723941576, 32.92523305485731, -4012.113444582079),
    (2039.8931079686508, 0.11658003829882707, 0.8931840659954299),
)
Z_MIN = 100
Z_MAX = 1200
MIN_WEIGHT_KG = 5.0
MAX_WEIGHT_KG = 300.0
STABLE_SECONDS = 18.0
COMPUTING_TIMEOUT_SECONDS = 45.0
WEIGHT_STABLE_DELTA_KG = 0.1


@dataclass(frozen=True)
class Profile:
    name: str
    height_m: float
    sex: str
    birthday: str
    age: float

    @classmethod
    def from_json(cls, path: Path) -> "Profile":
        raw = json.loads(path.read_text(encoding="utf-8"))
        name = str(raw.get("name", path.stem))
        height_m = float(raw["height_m"])
        sex = str(raw["sex"]).lower()
        birthday = str(raw["birthday"])
        if sex not in {"male", "female"}:
            raise ValueError("profile sex must be male or female")
        if not math.isfinite(height_m) or height_m <= 0:
            raise ValueError("profile height_m must be finite and positive")
        year, month, day = (int(part) for part in birthday.split("-"))
        birthday_date = date(year, month, day)
        if birthday_date > date.today():
            raise ValueError("profile birthday must not be in the future")
        age_value = raw.get("age")
        if age_value is None:
            today = date.today()
            age_value = today.year - birthday_date.year - (
                (today.month, today.day) < (birthday_date.month, birthday_date.day)
            )
        age = float(age_value)
        if not math.isfinite(age) or age <= 0:
            raise ValueError("profile age must be finite and positive")
        return cls(name, height_m, sex, birthday, age)


def advertisement_dict(device: Any, adv: Any) -> dict[str, Any]:
    return {
        "address": device.address,
        "name": adv.local_name or device.name or "",
        "rssi": adv.rssi,
        "service_uuids": sorted(adv.service_uuids or []),
        "manufacturer_data": {
            str(key): value.hex() for key, value in (adv.manufacturer_data or {}).items()
        },
        "service_data": {
            str(key): value.hex() for key, value in (adv.service_data or {}).items()
        },
    }


async def scan(seconds: float, name_filter: str | None) -> int:
    found: dict[str, dict[str, Any]] = {}

    def callback(device: Any, adv: Any) -> None:
        found[device.address] = advertisement_dict(device, adv)

    scanner = BleakScanner(detection_callback=callback)
    await scanner.start()
    try:
        await asyncio.sleep(seconds)
    finally:
        await scanner.stop()

    needle = name_filter.lower() if name_filter else None
    rows = list(found.values())
    if needle:
        rows = [row for row in rows if needle in row["name"].lower()]
    for row in sorted(rows, key=lambda row: (row["name"].lower(), row["address"])):
        print(json.dumps(row, sort_keys=True))
    return 0 if rows else 1


async def resolve(address: str, timeout: float) -> Any:
    device = await BleakScanner.find_device_by_address(address, timeout=timeout)
    if device is None:
        raise RuntimeError(
            f"device {address} was not found; wake the scale and retry scan/capture"
        )
    return device


async def inspect(address: str) -> int:
    device = await resolve(address, 20)
    client = BleakClient(device, timeout=20)
    try:
        await client.connect()
        print(json.dumps({"connected": True, "address": address}))
        for service in client.services:
            print(
                json.dumps(
                    {
                        "type": "service",
                        "uuid": service.uuid,
                        "description": service.description,
                    },
                    sort_keys=True,
                )
            )
            for characteristic in service.characteristics:
                print(
                    json.dumps(
                        {
                            "type": "characteristic",
                            "service_uuid": service.uuid,
                            "uuid": characteristic.uuid,
                            "properties": sorted(characteristic.properties),
                            "description": characteristic.description,
                        },
                        sort_keys=True,
                    )
                )
    finally:
        if client.is_connected:
            await client.disconnect()
    return 0


async def read_device_info(client: BleakClient, emit: Any) -> None:
    for name, uuid in DEVICE_INFO_UUIDS.items():
        try:
            value = await client.read_gatt_char(uuid)
            text = value.decode("utf-8", "replace")
            emit({"type": "device_info", "name": name, "value": text, "hex": value.hex()})
        except Exception as exc:  # optional standard fields may be unavailable
            emit({"type": "device_info_error", "name": name, "error": str(exc)})


def compute_metrics(weight_kg: float, impedances: list[float], profile: Profile) -> dict[str, Any]:
    z_whole = round(sum(impedances) / 4.0)
    z_valid = Z_MIN <= z_whole <= Z_MAX
    bmi = weight_kg / (profile.height_m * profile.height_m)
    if z_valid:
        ffm = FFM_A * (profile.height_m**2 / z_whole) + FFM_B * weight_kg + FFM_C
        ffm = min(ffm, weight_kg * 0.97)
        ffm = max(ffm, 1.0)
    else:
        fat_pct = (
            1.20 * bmi
            + 0.23 * profile.age
            - 10.8 * (1.0 if profile.sex == "male" else 0.0)
            - 5.4
        )
        fat_pct = max(3.0, min(60.0, fat_pct))
        ffm = weight_kg * (1.0 - fat_pct / 100.0)

    fat_pct = 100.0 * (weight_kg - ffm) / weight_kg
    water = FRAC_WATER * ffm
    protein = FRAC_PROTEIN * ffm
    bone = FRAC_BONE * ffm
    smm = FRAC_SMM * ffm
    bone_pct = 100.0 * bone / weight_kg
    muscle_pct = 100.0 - fat_pct - bone_pct
    water_pct = 100.0 * water / weight_kg
    protein_pct = 100.0 * protein / weight_kg
    smm_pct = 100.0 * smm / weight_kg

    return {
        "profile": profile.name,
        "birthday": profile.birthday,
        "age": profile.age,
        "height_m": profile.height_m,
        "sex": profile.sex,
        "weight_kg": round(weight_kg, 3),
        "weight_lb": round(weight_kg * LB_PER_KG, 3),
        "bmi": round(bmi, 3),
        "body_fat_percent": round(fat_pct, 3),
        "body_water_percent": round(water_pct, 3),
        "protein_percent": round(protein_pct, 3),
        "bone_mass_percent": round(bone_pct, 3),
        "muscle_mass_percent": round(muscle_pct, 3),
        "skeletal_muscle_percent": round(smm_pct, 3),
        "fat_free_mass_kg": round(ffm, 3),
        "whole_body_impedance_ohm": z_whole if z_valid else None,
        "impedance_ohm": [round(value, 1) for value in impedances],
        "source": "bia" if z_valid else "estimate",
    }



def compute_weight_only_metrics(weight_kg: float, profile: Profile) -> dict[str, Any]:
    """Compute the anthropometric fallback when no result frame arrives."""
    if not MIN_WEIGHT_KG <= weight_kg <= MAX_WEIGHT_KG:
        raise ValueError("weight_kg is outside the supported range")
    bmi = weight_kg / (profile.height_m * profile.height_m)
    fat_pct = (
        1.20 * bmi
        + 0.23 * profile.age
        - 10.8 * (1.0 if profile.sex == "male" else 0.0)
        - 5.4
    )
    fat_pct = max(3.0, min(60.0, fat_pct))
    ffm = weight_kg * (1.0 - fat_pct / 100.0)
    water = FRAC_WATER * ffm
    protein = FRAC_PROTEIN * ffm
    bone = FRAC_BONE * ffm
    smm = FRAC_SMM * ffm
    bone_pct = 100.0 * bone / weight_kg
    return {
        "profile": profile.name,
        "birthday": profile.birthday,
        "age": profile.age,
        "height_m": profile.height_m,
        "sex": profile.sex,
        "weight_kg": round(weight_kg, 3),
        "weight_lb": round(weight_kg * LB_PER_KG, 3),
        "bmi": round(bmi, 3),
        "body_fat_percent": round(fat_pct, 3),
        "body_water_percent": round(100.0 * water / weight_kg, 3),
        "protein_percent": round(100.0 * protein / weight_kg, 3),
        "bone_mass_percent": round(bone_pct, 3),
        "muscle_mass_percent": round(100.0 - fat_pct - bone_pct, 3),
        "skeletal_muscle_percent": round(100.0 * smm / weight_kg, 3),
        "fat_free_mass_kg": round(ffm, 3),
        "whole_body_impedance_ohm": None,
        "impedance_ohm": [],
        "source": "estimate",
    }


def build_display_frames(weight_kg: float, z_whole: int, profile: Profile) -> tuple[bytes, bytes]:
    if not Z_MIN <= z_whole <= Z_MAX:
        raise ValueError(f"whole-body impedance {z_whole} is outside the valid range")
    basis0 = profile.height_m**2 / z_whole
    ffm = FFM_A * basis0 + FFM_B * weight_kg + FFM_C
    ffm = max(1.0, min(ffm, weight_kg * 0.97))
    fat_pct = 100.0 * (weight_kg - ffm) / weight_kg
    smm_pct = 100.0 * (FRAC_SMM * ffm) / weight_kg

    fields = [int(round(a * basis0 + b * weight_kg + c)) for a, b, c in FIELD_COEF]
    fields[1] = int(round(fat_pct * 10.0))
    fields[3] = int(round(smm_pct * 10.0))
    fields = [max(0, min(value, 0xFFFF)) for value in fields]

    frame = bytearray((0x1C, 0x13, 0xFF))
    for value in fields[:7]:
        frame.extend((value & 0xFF, (value >> 8) & 0xFF))
    frame.append(min(fields[7], 0xFF))
    frame.append(sum(frame) & 0xFF)
    return bytes(frame), bytes.fromhex("1f05ff1033")


def decode_frame(payload: bytes, profile: Profile | None) -> dict[str, Any] | None:
    if not payload:
        return None
    frame_type = payload[0]
    if frame_type == 0x10 and len(payload) > 6:
        weight_kg = ((payload[5] << 8) | payload[6]) / 100.0
        if MIN_WEIGHT_KG <= weight_kg <= MAX_WEIGHT_KG:
            return {"frame_type": "live_weight", "weight_kg": round(weight_kg, 3)}
        return None
    if frame_type in (0x23, 0xB4):
        return {"frame_type": "computing"}
    if frame_type != 0xB1 or len(payload) <= 22:
        return None

    weight_kg = (payload[5] | (payload[6] << 8)) / 100.0
    if not MIN_WEIGHT_KG <= weight_kg <= MAX_WEIGHT_KG:
        return None
    impedances = [
        (payload[7 + 2 * index] | (payload[8 + 2 * index] << 8)) / 10.0
        for index in range(8)
    ]
    if any(value > 5000.0 for value in impedances):
        return None
    result: dict[str, Any] = {
        "frame_type": "result",
        "weight_kg": round(weight_kg, 3),
        "impedance_ohm": [round(value, 1) for value in impedances],
    }
    if profile is not None:
        result["metrics"] = compute_metrics(weight_kg, impedances, profile)
    return result


async def capture(
    address: str,
    seconds: float,
    send_handshake: bool,
    output: Path | None,
    profile_path: Path | None,
    write_back: bool,
) -> int:
    device = await resolve(address, 20)
    client = BleakClient(device, timeout=20)
    profile = Profile.from_json(profile_path) if profile_path else None
    if write_back and not send_handshake:
        raise ValueError("--write-back requires --handshake")
    if write_back and profile is None:
        raise ValueError("--write-back requires --profile")
    if output:
        output.parent.mkdir(parents=True, exist_ok=True)
        stream = output.open("w", encoding="utf-8")
    else:
        stream = None
    counts: Counter[str] = Counter()
    write_back_sent = False
    last_live_weight: float | None = None
    last_live_change: float | None = None
    computing_started: float | None = None
    result_seen = False

    def emit(row: dict[str, Any]) -> None:
        text = json.dumps(row, sort_keys=True)
        print(text, flush=True)
        if stream:
            stream.write(text + "\n")
            stream.flush()

    async def display_writeback(weight_kg: float, z_whole: int) -> None:
        assert profile is not None
        frame, commit = build_display_frames(weight_kg, z_whole, profile)
        await client.write_gatt_char(WRITE_UUID, frame, response=False)
        await asyncio.sleep(0.15)
        await client.write_gatt_char(WRITE_UUID, commit, response=False)
        emit({"type": "display_writeback_complete", "frame": frame.hex(), "commit": commit.hex()})

    def callback(_: Any, data: bytearray) -> None:
        nonlocal write_back_sent, last_live_weight, last_live_change, computing_started, result_seen
        payload = bytes(data)
        key = payload.hex()
        counts[key] += 1
        emit({"type": "notification", "monotonic": time.monotonic(), "data": key})
        decoded = decode_frame(payload, profile)
        if decoded is not None:
            now = time.monotonic()
            frame_type = decoded.get("frame_type")
            if frame_type == "live_weight":
                weight = float(decoded["weight_kg"])
                if last_live_weight is None or abs(weight - last_live_weight) > WEIGHT_STABLE_DELTA_KG:
                    last_live_change = now
                last_live_weight = weight
            elif frame_type == "computing" and computing_started is None:
                computing_started = now
            elif frame_type == "result":
                result_seen = True
            emit({"type": "decoded", "monotonic": now, **decoded})
            if write_back and not write_back_sent and frame_type == "result":
                write_back_sent = True
                impedances = decoded["impedance_ohm"]
                z_whole = round(sum(impedances) / 4.0)
                asyncio.create_task(display_writeback(decoded["weight_kg"], z_whole))
                emit({"type": "display_writeback_scheduled", "z_whole": z_whole})

    try:
        await client.connect()
        emit({"type": "connected", "address": address})
        await read_device_info(client, emit)
        await client.start_notify(NOTIFY_UUID, callback)
        emit({"type": "notifications_started", "characteristic": NOTIFY_UUID})
        if send_handshake:
            for index, frame in enumerate(HANDSHAKE):
                await client.write_gatt_char(WRITE_UUID, frame, response=True)
                emit({"type": "handshake_sent", "index": index, "data": frame.hex()})
                await asyncio.sleep(0.2)
            emit({"type": "handshake_complete"})
        await asyncio.sleep(seconds)
        if not result_seen and last_live_weight is not None and last_live_change is not None:
            now = time.monotonic()
            stable_for = now - last_live_change
            computing_for = now - computing_started if computing_started is not None else None
            if stable_for >= STABLE_SECONDS and (
                computing_for is None or computing_for >= COMPUTING_TIMEOUT_SECONDS
            ):
                fallback: dict[str, Any] = {
                    "frame_type": "weight_only_result",
                    "weight_kg": round(last_live_weight, 3),
                    "source": "estimate",
                    "stable_seconds": round(stable_for, 1),
                }
                if profile is not None:
                    fallback["metrics"] = compute_weight_only_metrics(last_live_weight, profile)
                emit({"type": "decoded", "monotonic": now, **fallback})
    finally:
        try:
            if client.is_connected:
                await client.stop_notify(NOTIFY_UUID)
        except Exception:
            pass
        if client.is_connected:
            await client.disconnect()
        emit(
            {
                "type": "summary",
                "unique_frames": len(counts),
                "total_notifications": sum(counts.values()),
                "frame_counts": dict(counts),
            }
        )
        if stream:
            stream.close()
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    scan_parser = subparsers.add_parser("scan", help="scan for BLE devices")
    scan_parser.add_argument("--seconds", type=float, default=30)
    scan_parser.add_argument(
        "--name", default="Fit Plus", help="case-insensitive name filter; empty for all"
    )

    inspect_parser = subparsers.add_parser("inspect", help="connect and enumerate GATT read-only")
    inspect_parser.add_argument("--address", required=True)

    capture_parser = subparsers.add_parser("capture", help="capture FFF1 notifications")
    capture_parser.add_argument("--address", required=True)
    capture_parser.add_argument("--seconds", type=float, default=60)
    capture_parser.add_argument("--output", type=Path)
    capture_parser.add_argument("--profile", type=Path, help="local JSON profile for derived metrics")
    capture_parser.add_argument(
        "--handshake",
        action="store_true",
        help="send unlock/control frames; never enables display write-back by itself",
    )
    capture_parser.add_argument(
        "--write-back",
        action="store_true",
        help="write computed display frames; requires --handshake and --profile",
    )
    return parser


async def async_main(args: argparse.Namespace) -> int:
    if args.command == "scan":
        return await scan(args.seconds, args.name or None)
    if args.command == "inspect":
        return await inspect(args.address)
    if args.command == "capture":
        return await capture(args.address, args.seconds, args.handshake, args.output, args.profile, args.write_back)
    raise AssertionError(args.command)


def main() -> int:
    args = build_parser().parse_args()
    try:
        return asyncio.run(async_main(args))
    except KeyboardInterrupt:
        return 130
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
