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
      ref: v0.1.2
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

## Home Assistant OS integration

ESPHome native API discovery creates the device and sensor entities automatically when the C6 joins Home Assistant. The optional HAOS assets are:

- `homeassistant/ge_fit_plus_ln_package.yaml` — logbook entry on every measurement ID change, including repeated weights.
- `homeassistant/ge_fit_plus_ln_card.yaml` — dashboard card for the primary and diagnostic metrics.

To use them, include the package from the HA configuration and paste the card YAML into a dashboard. Home Assistant Recorder normally records enabled ESPHome sensors automatically; the measurement-ID logbook automation preserves a distinct event for every weigh-in.

The public and CI configurations keep `write_back` disabled. A private deployment may set `ge_scale_write_back: true` in its ignored local `secrets.yaml`.

## Google Health Connect integration (phase 2)

The Health Connect bridge design and event schema are in `health-connect/`. A separate Android companion app will poll the HA measurement ID, deduplicate locally, and initially write WeightRecord and BodyFatRecord with minimal permissions. It is intentionally separate from HAOS because Health Connect is an Android on-device API.

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

Create a local profile file from the public template. The `profiles/` directory is ignored so personal values stay local:

```bash
mkdir -p profiles
cp profile.example.json profiles/my-profile.json
```

Set the profile values to the same values used by the ESPHome device. The adapter calculates age from the birthday when `age` is omitted.

Scan for the scale while it is awake:

```bash
python tools/ble_adapter/ge_fit_plus_ln_probe.py scan --seconds 30 --name "Fit Plus"
```

Inspect GATT without pairing or writing:

```bash
python tools/ble_adapter/ge_fit_plus_ln_probe.py inspect \\
  --address YOUR_SCALE_MAC
```

Capture notifications without writing anything:

```bash
python tools/ble_adapter/ge_fit_plus_ln_probe.py capture \\
  --address YOUR_SCALE_MAC \\
  --seconds 90 \\
  --profile profiles/my-profile.json \\
  --output captures/fit-plus-ln-read-only.jsonl
```

When the scale emits stable `0x10` live-weight frames but no full `0xB1` impedance result, the adapter emits a terminal `weight_only_result` after the documented stability/timeout window. With a profile, that event includes the same explicitly estimated body-composition metrics used by the ESPHome fallback; impedance fields remain empty rather than fabricated.

If the scale is awake but does not emit result frames passively, explicitly request the protocol unlock sequence:

```bash
python tools/ble_adapter/ge_fit_plus_ln_probe.py capture \\
  --address YOUR_SCALE_MAC \\
  --seconds 90 \\
  --profile profiles/my-profile.json \\
  --handshake \\
  --write-back \\
  --output captures/fit-plus-ln-handshake.jsonl
```

The `--handshake` mode sends only the four protocol unlock/control frames. Add `--write-back` only when you explicitly want computed display frames sent to the scale; it requires both `--profile` and `--handshake`. Do not run it while Fit Profile is connected to the scale.

Replace `YOUR_SCALE_MAC` with the address returned by `scan`; rescan if the scale advertises a different address.

### Continuous Home Assistant publisher

`tools/ble_adapter/ge_fit_plus_ln_ha_publisher.py` is the continuous Linux-adapter
fallback. It reconnects to the configured scale, sends the unlock handshake, derives
weight/body-composition metrics, and publishes the `sensor.ge_fit_plus_ln_*` states to
Home Assistant through the REST API. It uses a local profile and token; neither belongs
in Git.

It first resolves the configured address and then falls back to scanning for the
configured advertised name (default `Fit Plus`) so a rotating privacy address does not
make the scale look permanently offline.

The repository includes `systemd/ge-fit-plus-ln.service` as a deployment template. On
the BlueZ host, install the repository under `/opt/ge-fit-plus-ln`, create a root-only
`/etc/ge-fit-plus-ln/ha.env` containing `GE_SCALE_MAC`, `GE_SCALE_PROFILE`, `HA_URL`,
and `HASS_TOKEN`, then enable the service:

```bash
sudo install -d -m 700 /etc/ge-fit-plus-ln
sudo install -m 644 systemd/ge-fit-plus-ln.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now ge-fit-plus-ln.service
```

The REST publisher creates the state entities on its first successful reading, so the
HA dashboard and the measurement-ID automation can use the same entity IDs as the
ESPHome path. It is a fallback publisher, not a second simultaneous BLE client; keep
Fit Profile disconnected while it is capturing.

## Home Assistant behavior

The C6 connects directly to the scale and sends the decoded entities to Home Assistant over the ESPHome API. The Linux adapter publisher is a separate fallback when the scale is serviced by a BlueZ host instead of the C6.

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
