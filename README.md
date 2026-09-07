# GE Fit Plus LN → ESPHome

ESPHome integration for a GE Fit Plus LN body scale over Bluetooth Low Energy.
The production target is an ESP32-C6 running ESPHome and publishing entities to Home Assistant through the ESPHome native API.

## Compatibility

Validated hardware capture:

- Device name: `Fit Plus`
- Firmware: `V16.0`
- BLE service: `FFF0`
- Notify characteristic: `FFF1`
- Write characteristic: `FFF2`

Other GE Fit models and firmware versions are not assumed compatible.

## Features

The component exposes:

- Weight in kilograms and pounds
- BMI
- Body-fat percentage
- Body-water percentage
- Protein percentage
- Bone-mass percentage
- Muscle-mass percentage
- Skeletal-muscle percentage
- Fat-free mass
- Whole-body impedance
- Eight segmental impedance channels
- Measurement source and subject diagnostics
- A measurement sequence ID so repeated readings remain distinguishable in Home Assistant

The scale MAC address, Wi-Fi credentials, API key, OTA password, and personal profile belong in a local `secrets.yaml`. Do not commit that file.

## Safe defaults

- Display write-back is disabled.
- Guest filtering is disabled; every decoded reading is retained as the primary stream.
- Body-composition values are estimates intended for wellness trend tracking, not medical diagnosis.
- The first deployment should compare several readings against the Fit Profile app before enabling any display write-back behavior.

## Local setup

Install the pinned ESPHome version used by CI:

```bash
python3 -m venv .venv
. .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install esphome==2026.8.2
```

Create the local secrets file:

```bash
cp secrets.yaml.example secrets.yaml
```

Replace the placeholder values, especially:

- `ge_scale_mac`
- `ge_scale_height_m`
- `ge_scale_sex`
- `ge_scale_birthday`
- `ge_scale_age`
- Wi-Fi/API/OTA credentials

Validate and compile:

```bash
esphome config ge-fit-plus-ln.yaml
esphome compile ge-fit-plus-ln.yaml
```

The temporary discovery configuration can be used before the scale MAC is known:

```bash
esphome run ge-fit-plus-ln-discovery.yaml
```

Keep the Fit Profile app disconnected from the scale while testing so it does not compete for the BLE connection.

## Add to an existing ESPHome deployment

The component can be used from an existing ESPHome configuration in either of two ways.

### Local component copy

Copy the `components/ge_scale/` directory into the existing ESPHome configuration directory:

```bash
cp -a components/ge_scale /path/to/your/esphome-config/components/
```

Then add the BLE client and component blocks to the existing device YAML. Reuse the device's existing `esp32_ble_tracker` and `ble_client` blocks when they already exist.

### Pinned Git component

Alternatively, reference the published repository at a fixed release instead of `main`:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/rgnv/HAOS-ESPHome-GE-Fit-Plus-LN
      ref: v0.1.0
    components: [ge_scale]
```

Add the component to the device configuration:

```yaml
time:
  - platform: sntp
    id: ge_scale_time

esp32_ble_tracker:

ble_client:
  - mac_address: !secret ge_scale_mac
    id: ge_scale_client
    auto_connect: true

ge_scale:
  id: ge_scale_data
  ble_client_id: ge_scale_client
  time_id: ge_scale_time
  height: !secret ge_scale_height_m
  sex: !secret ge_scale_sex
  birthday: !secret ge_scale_birthday
  age: !secret ge_scale_age
  filter_guests: false
  write_back: false
  weight:
    name: "GE Fit Plus LN Weight"
  measurement_id:
    name: "GE Fit Plus LN Measurement ID"
```

Validate and compile the existing device as usual:

```bash
esphome config your-device.yaml
esphome compile your-device.yaml
```

A tagged GitHub release contains CI-built validation binaries and checksums. Those binaries use CI placeholder credentials and are not configured for a specific household. For a real installation, compile the YAML with the local `secrets.yaml`, or use the existing ESPHome dashboard to build and install it.

## Secondary Linux Bluetooth adapter path

The ESP32-C6 is the primary production bridge. A Linux Bluetooth adapter on a separate BlueZ-enabled host is a secondary diagnostic and fallback path. The adapter tools live in `tools/ble_adapter/` and use BlueZ through Bleak.

On Debian 13 or another BlueZ-enabled Linux host:

```bash
sudo apt-get update
sudo apt-get install -y bluez python3-bleak
sudo systemctl enable --now bluetooth
bluetoothctl show
```

Alternatively, use the pinned Python dependency in an isolated environment:

```bash
python3 -m venv .venv-ble-adapter
. .venv-ble-adapter/bin/activate
python -m pip install -r tools/ble_adapter/requirements.txt
```

Scan for the scale while it is awake:

```bash
python tools/ble_adapter/ge_fit_plus_ln_probe.py scan --seconds 30 --name "Fit Plus"
```

Inspect GATT without pairing or writing:

```bash
python tools/ble_adapter/ge_fit_plus_ln_probe.py inspect \\
  --address FF:05:00:16:79:2D
```

Capture notifications without writing anything:

```bash
python tools/ble_adapter/ge_fit_plus_ln_probe.py capture \\
  --address FF:05:00:16:79:2D \\
  --seconds 90 \\
  --output captures/fit-plus-ln-read-only.jsonl
```

If the scale is awake but does not emit result frames passively, explicitly request the protocol unlock sequence:

```bash
python tools/ble_adapter/ge_fit_plus_ln_probe.py capture \\
  --address FF:05:00:16:79:2D \\
  --seconds 90 \\
  --handshake \\
  --output captures/fit-plus-ln-handshake.jsonl
```

The `--handshake` mode sends only the four protocol unlock/control frames. It never sends display-result write-back frames. Do not run it while Fit Profile is connected to the scale.

The address above is the currently observed device address; rescan if the scale advertises a different address.

## Home Assistant behavior

The C6 connects directly to the scale and sends the decoded entities to Home Assistant over the ESPHome API. No separate Linux Bluetooth adapter is required for production operation.

The component sends four unlock/control frames required by the BLE protocol. Display-result write-back remains disabled by the device configuration.

## Continuous delivery

GitHub Actions validates the YAML and compiles the ESP32-C6 firmware on pull requests and pushes. Pushing a tag such as `v0.1.0` creates a GitHub release containing:

- Application firmware binary
- ELF/debug image
- Bootloader binary
- Partition table binary
- SHA-256 checksums

## License

This project is released under the MIT License; see [LICENSE](LICENSE).
