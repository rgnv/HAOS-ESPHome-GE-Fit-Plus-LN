# Production failure investigation — 2026-09-09

## Deployed passive-discovery fix — continuation

The earlier "no production fix" and "no flash" statements below describe prior
attempts, not this deployment.

The address-type hypothesis was traced through the installed ESPHome 2026.8.2
`BLEClientBase::parse_device()` and `connect()`, and ESP-IDF 5.5.5
`esp_ble_gattc_open()`: the advertisement's address type is preserved throughout.
The exact-target comparison does not require a service UUID or local name.

A separate gate exists before that comparison: IDF's
`btm_ble_update_inq_result()` suppresses connectable advertisement reports while
active scanning until the scan response arrives. The production SDK config has
`CONFIG_BT_BLE_ACT_SCAN_REP_ADV_SCAN` disabled. A peripheral whose scan response
is absent or missed can therefore advertise without reaching ESPHome discovery.
This mechanism fits the real-versus-emulator differential, but its occurrence
in the supplied physical incident is not yet proven by a C6 packet trace.

The production YAML now uses passive scanning, which removes this gate. It
retains continuous scanning, exact secret-bound target, auto-connect, and the
advertised address type. Target diagnostics contain only RSSI, address type, and
client state. Component diagnostics expose GATT open/discovery/registration/write
statuses, disconnect reasons, and notification type/length without payloads.
Existing outbox readiness/backoff fixes were reviewed and retained.

Validation: local configuration/schema checks and `git diff --check` passed.
Detached `nohup setsid` runner on Linux host passed C++ recording tests and the production
firmware build. No compiler ran under the Hermes gateway. The temporary build
secrets were removed. App0 alone was flashed at `0x10000` with transfer verification;
its read-back header matched the build and NVS hashes before/after were identical.
Controlled serial reset changed ACTIVE to PASSIVE, with scanner RUNNING and a
320 ms idle scan window (30 ms connection scan window).

Deployed application SHA-256:
`f4868304db58de8627dc69a65178560e3fa57471b4fc951cbeb5e9b341f040a2`.
ELF SHA-256 from the flashed app descriptor:
`5707cbde087b339ec72c19a375b3fd6d2c6dcf058db57c694ebfebe44254f6c3`.

## Continued incident: coordinated physical retry

The user supplied a subsequent coordinated Linux host scan that repeatedly detected the
configured physical scale during a real weigh-in, with the expected local name,
service metadata, and RSSI around -68 to -79 dBm. No phone app was installed.
This supersedes the earlier uncoordinated scans' absence of the target. It proves
advertising at the Linux host Bluetooth receiver, but does not yet prove reception or
connection at the C6 antenna.

Continuation checks:

- Initial HEAD is `8642d69` (`v0.1.12`). Existing edits to `recording.cpp`,
  `tests/config_test.py`, and `tests/RECORDING.md` were preserved.
- Linux host access and C6 serial access work; the Linux publisher remains inactive.
- Installed ESPHome 2026.8.2 passes the advertised address type through
  `BLEClientBase::parse_device()` to `esp_ble_gattc_open()`. A public/random
  address override is not justified without observing the physical failure.
- A simultaneous Linux host Bleak scan and C6 serial monitor were started. Output is
  filtered before storage: no raw notification payload, measurement, profile,
  Wi-Fi configuration, or scale address is retained in the diagnostic output.
- Do not count startup configuration lines `Connecting: 0, ... active: 0` or
  `Connected: NO` as connection events. The initial broad substring filter
  matched these; inspection of redacted messages corrected that interpretation.
- Local `python3 tests/config_test.py` and `git diff --check` passed.
- Existing detached Linux host runner has successful C++ test and compile exit statuses.
  No new compiler was started in the gateway process.
- The previously saved flash app descriptor identifies ESPHome 2026.8.2,
  IDF v5.5.5, and ELF SHA-256
  `8bc3dabf02989f77177c94f6259ea9849ddcb69fe03b8ec60366360403597b88`.
  Both available cached build images have different ELF hashes. This historical
  header does not independently establish the current active OTA partition or
  an exact source revision for the restored image.

The final uninterrupted 300-second capture completed with no configured-target
advertisement, no established GATT session, and no notification/finalization.
No physical weigh-in confirmation arrived during this capture. Serial monitor
closed at completion. This is another unconfirmed-wake observation, not a
reproduction of the supplied coordinated physical failure.

No production fix is established by these checks. A confirmed physical retry
must show C6 target discovery, connection outcome, service discovery, notification
registration, and protocol progress before changing address/security/MTU behavior.
The sections below retain earlier investigation history; their negative scans
and older completion statements are not findings from the coordinated retry.

## Hardware follow-up (same day)

- Recovered Linux host access and confirmed the C6 USB device is attached. Linux
  publisher remains disabled and inactive. HA production ESPHome options still
  have `allow_service_calls: true` and `subscribe_logs: false`.
- Local and Linux host production secrets agree on the target; the cached generated
  source uses that target. The cached build is older (Wi-Fi enabled at boot,
  five-minute watchdogs). Its ELF identity differs from the app0 header read
  from flash, so cached source alone cannot establish the running configuration.
  A subsequent controlled-reset serial check confirmed that the running BLE
  client reports the configured target and starts scanning. The same target
  also occurs in the earlier saved real-scale capture. This rules out a simple
  production target-copy mismatch, not a later peripheral address change.
- Filtered serial observation saw boot/scan messages but no scale notification
  or finalization. Serial content, measurements, and identifiers were not dumped.
- A working independent Bleak scan observed 41 nearby advertisers in 55 seconds,
  with zero configured-target or FFF0 advertisements. This is stronger evidence
  than the earlier empty `btmgmt` output, but still needs a confirmed physical
  wake-up during observation. A real weigh-in was requested for that purpose.
  A subsequent 275-second scan saw 51 nearby advertisers and zero configured
  target, `Fit Plus` name, or FFF0 service matches. No physical wake-up was
  confirmed during either scan.
- Read and verified the NVS partition without erasing it. The recording blob has
  valid NVS and application checksums, capacity 16, pending count 0, and next
  sequence 1. No successful finalized capture has advanced this outbox since its
  initialization. The backup stays private on Linux host; it is not a repository asset.
- Default esptool bulk reads failed with `Packet content transfer stopped`.
  A stub read using 1,024-byte blocks and one packet in flight successfully
  recovered NVS and verified the transfer digest. This is a diagnostic transport
  workaround, not evidence of a firmware defect.
  A full-chip read later stalled and was interrupted; no complete full-chip
  backup was obtained. The C6 was explicitly reset and runtime scanning verified
  afterward. The verified NVS backup was preserved.
- Inspected the installed HA provider: the service awaits weight and enabled
  body-fat writes; its HTTP client raises on unsuccessful HTTP responses.
  A successful service ACK therefore follows those writes. This does not supply
  provider deduplication or prove a new physical measurement was delivered.
- Configuration tests and `git diff --check` passed locally. Existing C++ outbox
  tests and a production-target firmware build passed on the designated build
  host through a detached `nohup setsid` runner. Generated build settings confirm
  Wi-Fi disabled at boot and both reboot watchdogs zero. Runner removed its
  temporary secrets file. No firmware was compiled under the gateway process.
- Final controlled-reset observation lasted 100 seconds: running BLE target
  matched, scanning started, and no notification, finalization, storage error,
  or panic marker was observed. Serial monitor closed afterward. Existing
  firmware remains installed; no flash, NVS erase, HA configuration change,
  commit, push, tag, or release was performed in this follow-up. Earlier working
  tree source/test edits remain intact; this investigation added this report.

The earlier analysis below predates this follow-up. Physical discovery/capture
remains unresolved; the readiness fixes are not established as its cause.

## Conclusion

The physical failure's root cause is not established without a physical-scale
advertisement/session trace or post-weigh-in outbox state. The supplied emulator
results prove that this C6 could discover that target, handshake, parse a result,
persist records across reboot, and receive a successful HA action response. They
do not prove discovery or protocol timing for the real scale. HA unavailable is
normal while production Wi-Fi is disabled; it is not evidence that BLE stopped.

First investigate the physical peripheral's advertised address against the
production target, whether it advertises during the weigh-in, RF reach, and any
phone/app or other central holding its connection. No private identifiers were
read or compared in this investigation. The configured secret binding is intact;
its value and the restored firmware's embedded target remain unverified.

## Trace and findings

- **Discovery:** production uses `ble_client.mac_address: !secret ge_scale_mac`
  and `auto_connect: true`. Installed `BLEClientBase::parse_device` requires an
  exact address match and IDLE state; FFF0 or device name is not a fallback match.
  It obtains address type from the advertisement, so a random/public type alone
  is not grounds for hard-coding a different type. A changing address would break
  the fixed target. Continuous scanning repeats scans, but scan window/interval
  also matter. Installed tracker schema conditionally increases the default
  window under compatible IDF/coexistence settings; do not assume a fixed duty
  cycle from `continuous: true` alone. Tracker setup/loop has no Wi-Fi-ready gate.
- **Handshake:** FFF1 registration starts four FFF2 writes, chained by write ACKs
  with timer backstops. Installed BLE client base writes the CCCD during notify
  registration handling; adding a second CCCD write blindly is not justified.
  Remaining protocol risks: component does not check asynchronous registration
  status, starts handshake before CCCD write completion, ignores immediate write
  errors, and advances handshake index before success. An accommodating emulator
  cannot exclude those timing differences on the physical scale. No protocol
  sequencing change was made without a physical trace.
- **Finalization:** valid B1 result finalizes directly. Valid live weight falls
  back after 18 seconds steady with no computing frame, after 45 seconds of
  computing, or on disconnect. No valid live/result frame means no finalization
  and therefore no Wi-Fi request. Guest filtering is disabled in production.
  A late BIA upgrade updates sensors but intentionally does not enqueue another
  record for the same session. Display write-back is scheduled before capture;
  Wi-Fi startup can overlap its delayed commit. This remains hardware-unverified.
- **Storage and Wi-Fi:** offline finalization must append successfully before
  requesting Wi-Fi. Full, invalid, or failed NVS can therefore leave Wi-Fi off.
  A healthy pending queue retries via `replay_()` independently of BLE connection
  state. No state subscriber for 90 seconds disables Wi-Fi, then a 60-second
  backoff applies. A snapshot after that timeout can look identical to no capture.
  API-connected but rejected/unreplayable records can keep Wi-Fi on indefinitely:
  connection timeout is not a total delivery deadline. Five-second shutdown grace
  is armed after the last ACK or eligible live delivery; manual discard does not
  arm it. These policy limitations were not changed.
- **API replay:** `is_connected_with_state_subscription()` does not mean HA has
  subscribed to actions. Installed `send_homeassistant_action()` drops an action
  without an action subscriber and only logs a warning. The component keeps the
  head record and expires its callback after 60 seconds, then waits another
  60 seconds before retry. Thus an early replay can take roughly two minutes to
  recover even with HA connected. Missing action permission/responses keeps data
  queued. The supplied successful ACK demonstrates that permission worked for
  that session, not that every subsequent replay succeeded. Online captures use
  the separate unacknowledged live-ID route and retain its delivery limitations.

## Safe fixes

`components/ge_scale/recording.cpp`:

1. Remove Wi-Fi request from NVS setup and guard the shared request function with
   `wifi->is_ready()`. GE scale/BLE client setup priority is 300, Wi-Fi is 250;
   ESPHome initializes higher priorities first. Wi-Fi initially reports OFF,
   while `enable()` only acts on DISABLED, and Wi-Fi setup subsequently applies
   `enable_on_boot: false`. The former early request marked delivery requested
   without starting Wi-Fi. The replay loop normally repaired this afterward,
   so this defect alone does not explain the production symptom. The shared guard
   also covers node loops invoked by BLEClient during application setup waits.
2. Treat zero retry deadline as unarmed and clear an expired deadline before
   delivery. Previously signed subtraction against initial zero blocked the first
   request at uptimes from 2^31 to 2^32 milliseconds (about days 24.9–49.7).
   Clearing expired deadlines also prevents their reuse after a long idle period.
   This does not explain a weigh-in immediately after firmware restoration/reboot.

## Validation and evidence limits

`python3 tests/config_test.py` passed with public CI secrets in temporary files:
production/discovery validation, fixed secret target binding, auto-connect,
radio/API settings, and rejection of recording capacities 0 and 33. Added source
contracts check setup deferral, shared readiness guard, retry deadline handling,
and retained replay wake-up path. Original HEAD fails the new readiness contract.
These checks do not execute C++ or emulate BLE, NVS, Wi-Fi, or API callbacks.

Inspected installed ESPHome sources under
`/var/lib/hermes/.venv-esphome/lib/python3.13/site-packages/esphome/`:
`core/{application.cpp,component.h,component.cpp}`,
`components/wifi/{wifi_component.h,wifi_component.cpp}`,
`components/ble_client/ble_client.cpp`,
`components/esp32_ble_client/{ble_client_base.h,ble_client_base.cpp}`,
`components/esp32_ble_tracker/{__init__.py,esp32_ble_tracker.cpp}`,
and `components/api/{api_server.cpp,api_connection.h,api_connection.cpp}`.
Installed source behavior is evidence for this environment, not proof of the
exact source embedded in the restored production image.

No compilation, flashing, serial access, hardware changes, commits, pushes,
tags, or releases were performed. Existing C++ recording tests were not built
or run. Further hardware work must distinguish discovery, notification capture,
NVS append, Wi-Fi association, API subscription, and provider ACK in that order.

## Verified QN/GE production fix — 2026-09-10

The C6 diagnostic API log captured the real scale session without USB serial:

target advertisement → GATT connect → service discovery → FFF1 registration →
`0x12` (18-byte scale info) → `0x14` ready → `0x21` config request → repeated
`0x23`, `0x24`, and `0x25` result frames. The prior parser ignored the state-machine
frames and treated `0x23` as computing, so it never finalized the stored result.

The production component now:

- waits for `0x12` before using the legacy fixed-handshake fallback;
- selects protocol byte `0x00` for the observed 18-byte scale-info dialect;
- sends the notification-driven config, time-sync, history, and start frames;
- parses the `0x23` stored result at its verified weight/impedance offsets; and
- sends the QN config unit flag for pounds (`0x02`) rather than changing the scale
  display to kilograms.

Verification after flashing the production image: a C6-only weigh-in published
weight `93.35 kg` / `205.80 lb`, BMI, body fat, body water, protein, bone mass,
muscle mass, skeletal muscle, and fat-free mass. HA measurement ID advanced to
`2`; the persistent-metric snapshot automation triggered; the Google Health
automation triggered; and the repository tests plus detached production build
passed. This session's source is `estimate` because the stored result did not
contain a usable whole-body impedance value; no impedance was fabricated.
