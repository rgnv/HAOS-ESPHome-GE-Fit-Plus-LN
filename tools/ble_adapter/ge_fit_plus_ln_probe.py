#!/usr/bin/env python3
"""BLE discovery and read-only protocol capture for GE Fit Plus LN.

The default operations only scan, inspect services, or subscribe to notifications.
The optional --handshake flag sends the four unlock/control frames required by the
scale protocol. It never sends display-result write-back frames.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import sys
import time
from collections import Counter
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
        item = advertisement_dict(device, adv)
        found[device.address] = item

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
            service_row = {
                "type": "service",
                "uuid": service.uuid,
                "description": service.description,
            }
            print(json.dumps(service_row, sort_keys=True))
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


async def read_device_info(client: BleakClient) -> None:
    for name, uuid in DEVICE_INFO_UUIDS.items():
        try:
            value = await client.read_gatt_char(uuid)
            text = value.decode("utf-8", "replace")
            print(json.dumps({"type": "device_info", "name": name, "value": text, "hex": value.hex()}))
        except Exception as exc:  # diagnostic output should identify unavailable optional fields
            print(json.dumps({"type": "device_info_error", "name": name, "error": str(exc)}))


async def capture(address: str, seconds: float, send_handshake: bool, output: Path | None) -> int:
    device = await resolve(address, 20)
    client = BleakClient(device, timeout=20)
    stream = output.open("w", encoding="utf-8") if output else None
    counts: Counter[str] = Counter()

    def emit(row: dict[str, Any]) -> None:
        text = json.dumps(row, sort_keys=True)
        print(text, flush=True)
        if stream:
            stream.write(text + "\n")
            stream.flush()

    def callback(_: Any, data: bytearray) -> None:
        payload = bytes(data)
        key = payload.hex()
        counts[key] += 1
        emit({"type": "notification", "monotonic": time.monotonic(), "data": key})

    try:
        await client.connect()
        emit({"type": "connected", "address": address})
        await read_device_info(client)
        await client.start_notify(NOTIFY_UUID, callback)
        emit({"type": "notifications_started", "characteristic": NOTIFY_UUID})
        if send_handshake:
            for index, frame in enumerate(HANDSHAKE):
                await client.write_gatt_char(WRITE_UUID, frame, response=True)
                emit({"type": "handshake_sent", "index": index, "data": frame.hex()})
                await asyncio.sleep(0.2)
            emit({"type": "handshake_complete"})
        await asyncio.sleep(seconds)
    finally:
        try:
            if client.is_connected:
                await client.stop_notify(NOTIFY_UUID)
        except Exception:
            pass
        if client.is_connected:
            await client.disconnect()
        if stream:
            stream.close()
        print(
            json.dumps(
                {
                    "type": "summary",
                    "unique_frames": len(counts),
                    "total_notifications": sum(counts.values()),
                    "frame_counts": dict(counts),
                },
                sort_keys=True,
            ),
            flush=True,
        )
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    scan_parser = subparsers.add_parser("scan", help="scan for BLE devices")
    scan_parser.add_argument("--seconds", type=float, default=30)
    scan_parser.add_argument("--name", default="Fit Plus", help="case-insensitive name filter; empty for all")

    inspect_parser = subparsers.add_parser("inspect", help="connect and enumerate GATT read-only")
    inspect_parser.add_argument("--address", required=True)

    capture_parser = subparsers.add_parser("capture", help="capture FFF1 notifications")
    capture_parser.add_argument("--address", required=True)
    capture_parser.add_argument("--seconds", type=float, default=60)
    capture_parser.add_argument("--output", type=Path)
    capture_parser.add_argument(
        "--handshake",
        action="store_true",
        help="send unlock/control frames; never sends display write-back frames",
    )
    return parser


async def async_main(args: argparse.Namespace) -> int:
    if args.command == "scan":
        return await scan(args.seconds, args.name or None)
    if args.command == "inspect":
        return await inspect(args.address)
    if args.command == "capture":
        return await capture(args.address, args.seconds, args.handshake, args.output)
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
