# Repository audit — 2026-09-09

Reviewed baseline: `d515b030fa5218d766f7cdc15326875b04ab8f91` (`main`, `v0.1.17`).
Also reviewed and retained concurrent commit `5c31fc5`, which ignores private HA override files. No runtime code changed in that commit.

**Result: targeted fixes pass host checks and firmware compilation; open findings prevent a clean production-readiness verdict.** This review covers the repository, not deployed devices or external Google/HA services. Existing production reports are historical evidence, not tests performed during this audit.

## Scope and method

Reviewed ESPHome schema, both firmware configurations, BLE state machines and callers, metric calculations, NVS serialization/replay, both Linux tools, systemd template, all HA assets, Android Kotlin/manifest/build configuration, event schema, workflow, public secrets/profile examples, license, and documentation. All 44 baseline tracked files were included in inventory and credential-pattern scanning. Gradle launcher scripts were inspected; the wrapper binary and third-party dependency implementations were not exhaustively audited.

Traced BLE notification → finalization → entity publication/outbox → HA consumer and Android polling → Health Connect insertion. Ran existing checks, added focused regression tests, compiled firmware with public CI placeholders, and checked dependency advisories. No physical scale, production credentials, HA instance, Health Connect device, firmware flashing, or NVS erasure was used.

Priority: **P1** = data loss, wrong attribution, duplicate health records, or a normal supported workflow failing; **P2** = conditional correctness, reliability, security exposure, or portability defect. Open items below remain unfixed unless explicitly listed under completed fixes.

## Completed fixes

| Finding | Change | Evidence |
| --- | --- | --- |
| P1: REST fallback retained previous impedance | Publish `unknown` for missing metrics and all absent impedance channels, with the current measurement attributes. | BIA followed by weight-only regression checks all nine impedance states. |
| P1: REST measurement ID preceded source and omitted subject | Publish primary subject and source before the final ID event. | Publication-order regression; failed metric writes cannot announce a completed ID. |
| P2: REST sample timestamp was connection time | Stamp result notifications when received; fallback uses the last live-weight notification time. | Simulated delayed notifications cover both result and fallback paths. |
| P2: malformed profiles passed validation | Reject nonfinite/nonpositive height and age; reject impossible/future birthdays in ESPHome and Linux configuration. | CLI schema rejection tests and JSON-profile regressions. |
| P2: Linux display tail wrapped above 255 | Saturate the one-byte field instead of truncating its high bits. | Synthetic coefficient overflow checks the field and checksum. |
| P2: CI installed conflicting Bleak versions together | Run adapter installation/tests in their own venv, including `pip check`. ESPHome 2026.8.2 requires Bleak 2.1.1; the adapter pins 0.22.3. | Both dependency environments and adapter tests checked separately. |
| P2: README overstated implemented features and supplied broken multiline commands | Correct Linux/QN and Android scope, fix doubled shell continuations, remove display write-back from the handshake-only example, and link this audit. | Compared examples and claims with actual implementations. |

Configuration tests now fail explicitly when ESPHome is unavailable, instead of returning success after skipping schema validation. Firmware C++ behavior and Android code were not changed by these targeted fixes.

## Open P1 findings

1. **Android scale failures block independent glucose sync.** [SyncWorker.kt](health-connect/android/app/src/main/java/dev/rgnv/gefitplus/healthconnect/SyncWorker.kt), `syncOnce`, calls `syncWeight` before `syncGlucose` without isolating their errors. Production normally disables Wi-Fi, making ESPHome weight unavailable; `toDouble()` then throws before glucose is reached. Missing weight permissions also blocks glucose through the combined permission gate. Handle availability, permissions, and retry results per stream. Reproduce with unavailable scale weight and a valid glucose entity.

2. **Android deduplication is neither concurrent nor crash safe.** Both foreground service and WorkManager invoke `syncOnce`. Each can read the same previous key, insert `Metadata.manualEntry()` without `clientRecordId`, then save the key. A crash after insertion but before preferences are saved produces the same duplicate on restart. Use stable, source-namespaced client record IDs at the Health Connect boundary; a mutex alone does not fix crash recovery. Verify simultaneous polls and an insertion-success/preferences-failure restart. [Health Connect upsert semantics](https://developer.android.com/health-and-fitness/health-connect/sync-data).

3. **Android accepts invalid IDs and reads a torn measurement snapshot.** In `syncWeight`, `unknown`/`unavailable` are nonempty and can be treated as new IDs. ID and weight arrive through separate REST calls, so a new weight can be saved under an old ID. Missing `measurement_time` falls back to poll time, including the native ESPHome path. Validate state sentinels, require coherent identity/metrics/timestamp, and retry an inconsistent snapshot. Reading the ID twice helps but cannot by itself detect metrics that change before the publisher updates its ID; a single finalized payload is stronger.

4. **Android silently assumes measurement units.** `syncWeight` always uses kilograms; `syncGlucose` always uses mg/dL and never inspects `unit_of_measurement`. An HA glucose entity configured in mmol/L is recorded at the wrong magnitude. Convert only recognized units and reject unsupported/nonfinite values before insertion. Cover kg/lb and mg/dL/mmol/L inputs. No deployed unit setting was inspected.

5. **Android 15 foreground-service lifecycle contradicts continuous-sync claims.** Target SDK is 35, `SyncService` uses `dataSync`, holds a wake lock indefinitely, and lacks `onTimeout`. Android 15 limits background data-sync foreground services to six hours per 24 hours and disallows launching them from `BOOT_COMPLETED`. `BootReceiver` catches startup failure but provides no replacement scheduling there; imported ADB configuration also bypasses `saveAndSchedule`, so its periodic fallback may never exist. Use persistent WorkManager scheduling for all configuration paths and handle service shutdown explicitly. [Android 15 service rules](https://developer.android.com/about/versions/15/behavior-changes-15#fgs-boot-completed).

6. **Android provider selection breaks the older supported platform path.** `MainActivity` permission contract and `SyncEngine.getSdkStatus` hard-code `com.google.android.healthconnect.controller`, while `getOrCreate` uses SDK defaults. Android 9–13 uses the standalone `com.google.android.apps.healthdata` provider. Use the SDK's consistent provider defaults and test both pre-14 and integrated Health Connect. [Official client source](https://android.googlesource.com/platform/frameworks/support/+/d40efbfcc8684651783df15bccc0fb2e42d0d3c0/health/connect/connect-client/src/main/java/androidx/health/connect/client/HealthConnectClient.kt).

7. **Linux fallback does not implement the production QN dialect.** [Probe](tools/ble_adapter/ge_fit_plus_ln_probe.py), `decode_frame`, treats every `0x23` as computing and ignores `0x12`/`0x14`/`0x21`. Both callers send only the fixed legacy handshake. The matching C++ handlers decode stored results instead. README now states this limitation. Implement one shared Linux protocol handler and test captured QN frames before claiming fallback compatibility with that firmware.

8. **Linux discovery can select another person's scale despite a configured MAC.** [Publisher](tools/ble_adapter/ge_fit_plus_ln_ha_publisher.py), `resolve`, accepts `address_match or name_match` from the first advertisement. The same-name neighbor can win before the configured device; the configured personal profile then attributes those readings to the primary user. Make name-based fallback explicit and refuse ambiguous matches. Test competing same-name advertisements and rotating-address policy separately.

9. **Offline delivery requires an external provider that generic setup does not supply.** [recording.cpp](components/ge_scale/recording.cpp), `replay_`, always calls `google_health_scale_sync.log_body_measurements`. Neither this service nor an online health-write automation is implemented in the repository. Without the external service and ESPHome action permission, pending records remain queued and eventually fill capacity. The Android companion does not consume that queue. Also, online captures reserve identity with `retain=false`: connection loss after finalization can lose the event; a lost offline service ACK can duplicate it. These are partly documented design limits, not a claim that NVS FIFO is broken. Make provider prerequisites prominent and test the full delivery contract before changing the online/offline policy.

10. **HA reconnects can fabricate a weigh-in for today.** [Last-metrics package](homeassistant/ge_fit_plus_ln_last_metrics.yaml), snapshot automation, triggers on numeric weight becoming available for five seconds as well as measurement-ID changes, then unconditionally writes `now()` as recorded time. An old restored reading after restart/reconnect can dismiss today's reminder. Its numeric-only ID condition also rejects the Linux publisher's string IDs. Persist and compare a finalized identity with its original timestamp; distinguish availability restoration from a new measurement. Test reboot/reconnect without weighing and two identical-weight new measurements.

## Open P2 findings

11. **Firmware impedance publication bypasses shared finalization.** In [ge_scale.cpp](components/ge_scale/ge_scale.cpp), `publish_diagnostics_` is called only from the B1 branch, before the primary/guest check. QN valid impedance is stored but never published to those sensors; a filtered B1 guest can overwrite primary impedance. Publish accepted diagnostics from shared primary finalization, clearing unavailable channels and keeping repeated/guest frames out. Test B1, QN, guest, and fallback transitions.

12. **Legacy BMI differs between HA and recorded payload.** `finalize_result_` publishes calculated BMI before replacing its local `bmi` with `legacy_bmi_`. The queue receives the scale value while the sensor keeps the calculated value. Publish after algorithm selection. Also, the legacy `source=scale` path mixes supplied metrics with calculated missing fields; consumers cannot infer that every composition field came directly from the scale.

13. **BLE handshake records success before transport success.** Notify registration enters ESTABLISHED regardless of asynchronous status. `send_handshake_step_` increments its index before the write; `write_char_` ignores immediate return status, and QN flags are set before successful writes. A transient write/registration failure can prevent retries for the whole session. Check status, keep failed steps retryable, and validate CCCD/notification/write ordering against real hardware. QN and B1 decoding also lack full frame-length/checksum validation comparable to the legacy seven-byte dialect; confirm each dialect's framing contract before adding checks.

14. **Firmware fallback IDs can collide with the last persisted live ID.** [recording.cpp](components/ge_scale/recording.cpp), `record_`, starts the RAM fallback counter at zero independently of persisted `recordings_.next`. A storage failure after live ID 1 can publish fallback ID 1 again, suppressing change-based consumers. Preserve monotonic identity across persisted/fallback transitions and test the first append failure after a successful live capture. Reboot/NVS reset identity policy also needs explicit treatment.

15. **Wi-Fi has no total replay deadline once API connects.** A connected API clears `wifi_request_started_ms_`; any pending record prevents idle shutdown. Missing action subscribers, repeated service rejection, or a guest at the FIFO head can keep Wi-Fi on indefinitely. Manual discard of the last record does not arm the idle deadline either. Bound total delivery attempts without dropping records, and test failure/backoff plus discard-to-empty behavior.

16. **Linux capture has session and task-lifetime limits.** `capture_once` sleeps for the entire five-minute window, emits at most one result, and only checks weight-only stability at the end. Multiple weigh-ins in one connection are lost; a short stable step-on near the end can be discarded at teardown. Probe display-write tasks are untracked; publisher tasks are awaited only on the successful capture path, so an intervening BLE failure can leave an unobserved task. Use explicit session completion and owned task cleanup; add disconnect, late-result, and publish-failure tests. No Linux durable retry outbox exists, so a transient HA failure can permanently lose a reading.

17. **Bearer-token transport and local storage need deployment hardening.** Linux defaults to a household-specific HTTP URL and uses redirect-following `urllib`; a redirect can forward the Authorization header to another origin. Android allows cleartext to one fixed private IP. Encrypted preferences protect stored Android credentials, not plaintext network traffic. Require an explicit HA endpoint, prefer HTTPS, and reject cross-origin/downgrade redirects. The systemd service runs as root; capture/profile file permissions follow the caller's umask. Health measurements and identifiers are logged on both platforms. These are code/config exposures; no network interception or credential theft was demonstrated.

18. **Dependency audit is not clean for the build host.** Adapter requirements returned no known vulnerabilities. The isolated ESPHome/build-tool environment returned 14 advisory entries across pip 25.1.1 and Starlette 0.52.1, including duplicate aliases; that is not 14 distinct proven exploits. Starlette arrives via PlatformIO, not the ESP32 binary. Upstream confirms host-header URL confusion and form-limit issues in affected versions. Audit server exposure and select a compatible patched toolchain; blindly overriding ESPHome/PlatformIO constraints is not a validated fix. pip is environment tooling rather than a project pin. [Host-header advisory](https://github.com/Kludex/starlette/security/advisories/GHSA-86qp-5c8j-p5mr), [form-limit advisory](https://github.com/Kludex/starlette/security/advisories/GHSA-82w8-qh3p-5jfq).

19. **Contract and deployment assets remain inconsistent.** [measurement.schema.json](health-connect/measurement.schema.json) only permits integer IDs and excludes `source=scale`; Linux and queued records use string identities. It is not a validator for the current NVS JSON payload. HA notification actions name private mobile-app services and stop on missing services. Android README claims a CI build, but the only workflow builds firmware. The root README's pinned component example still references an older release, and firmware project version stays `0.1.0`. Align the supported event contract, examples, release metadata, and deployment prerequisites without silently repurposing historical reports.

20. **Firmware display math differs from published values.** `send_display_frames_` computes fat-free mass without the clamps used by `finalize_result_` and the Python display builder. At supported range extremes, display fat/skeletal-muscle values can diverge from published metrics. Share or consistently apply the clamp while retaining calibration coefficients; verify against captured display frames before enabling write-back. Public write-back remains disabled.

## Validation and limits

| Check | Result |
| --- | --- |
| Native C++ outbox tests with `-Wall -Wextra -Werror -fsanitize=address,undefined` | Passed: serialization, corruption/truncation, FIFO, capacity, wrap, persistence failure, ACK/retry identity. Does not execute the GEScale BLE/API/Wi-Fi callbacks. |
| Adapter regression suite | Passed under pinned Bleak 0.22.3 and ESPHome's Bleak 2.1.1. New regression cases also failed against original decoder/publisher sources, confirming detection of prior defects. No Bluetooth radio or HA HTTP endpoint used. |
| ESPHome 2026.8.2 production/discovery config validation | Passed with temporary public secrets, invalid-capacity tests, invalid-date tests, and zero/nonfinite profile rejection. Existing source-contract assertions are static checks, not runtime simulations. |
| Full ESP32-C6 firmware compilation | Passed with public CI placeholders and ESP-IDF 5.5.5. Firmware uses about 88.4% of the application partition. No flash performed. |
| Android assemble/lint | Attempted; blocked before source compilation by missing accepted SDK licenses for `platforms;android-35` and `build-tools;34.0.0`. No Android build, lint, or on-device success claimed. |
| Credential-pattern review | No private-key, GitHub-token, AWS-key, or JWT pattern found in tracked files. Public CI credentials are explicit placeholders. This is a limited current-tree scan, not a comprehensive history/entropy scan or proof of no secrets. |
| Dependency advisories | Adapter clean in the queried database; build environment findings remain open as item 18. Android transitive dependencies and firmware C/C++ dependencies were not independently vulnerability-scanned. |
| Diff hygiene | `git diff --check` passed. |
| HA YAML / Android XML syntax | Parsed successfully; this does not execute templates, automations, or Android components. |

Reproduce the supported local checks from the repository root with separate environments:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install --upgrade pip esphome==2026.8.2
.venv/bin/python tests/config_test.py

python3 -m venv .venv-ble-adapter
.venv-ble-adapter/bin/python -m pip install -r tools/ble_adapter/requirements.txt
.venv-ble-adapter/bin/python -m pip check
.venv-ble-adapter/bin/python -m unittest tools/ble_adapter/test_probe.py

g++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I. tests/recording_test.cpp -o /tmp/ge-recording-test
/tmp/ge-recording-test
```

Compile firmware with a local secrets file, or a fresh disposable checkout populated only with `secrets.ci.yaml` placeholders. Do not overwrite deployment secrets to reproduce an audit build.

Additional limits: no HA template execution, end-to-end replay/provider acknowledgement, Android unit conversion/insertion, power-loss fault injection, protocol fuzzing, or physical-scale timing validation. Passing host tests does not resolve the open findings above.
