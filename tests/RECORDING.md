# Recording outbox contract

Production keeps its existing live measurement-ID route. A finalized online
reading publishes metrics, subject/source, and guest state before its numeric ID;
`api.batch_delay: 0ms` avoids normal delayed state batching. Transport failure
and reconnect still require the live-automation safeguards described below. Weight-only readings clear stale
trusted BIA/impedance states; estimates remain separate. Repeated weights in new
BLE sessions get new IDs. One session emits one event: a late BIA result can
update sensors but does not create a second queued/provider write.

Offline finalized readings instead enter a durable FIFO. They do not publish a
new live measurement ID, including during replay. Each record stores capture UTC
(or zero when unsynchronized), uptime, source/subject flags, and nullable metrics.
Values are frozen at first finalization, never rebuilt from current sensor state.

## Radio lifecycle

Production starts with Wi-Fi disabled and the BLE tracker scanning continuously
in passive mode. The exact target address matches its advertisement without
waiting for a scan response; ESPHome keeps the advertised public/random address
type for GATT open. Discovery firmware still uses active scanning for metadata.
A finalized record is written to NVS before Wi-Fi is enabled. On boot, a pending
record and disables Wi-Fi again. After successful live delivery or after
all replay records are acknowledged, the device waits ten minutes for queued API
messages and to keep the fresh HA reading available, then disables Wi-Fi again. A failed connection attempt retains records,
disables Wi-Fi after 90 seconds, and retries after a 60-second backoff. Discovery
keeps Wi-Fi enabled so provisioning remains accessible.

## Provider setup and replay

No extra provider entry-ID configuration is required. The production ESPHome
integration must allow the device to perform Home Assistant actions, and HA must
support ESPHome action status responses. The existing Google Health Scale Sync
service receives the replay; no replay-ready automation or invented service is
needed. Discovery has no queue, provider actions, or scale component.

Replay calls `google_health_scale_sync.log_body_measurements` with `weight_kg`,
optional UTC ISO-8601 `measured_at`, and persistent `measurement_id`.
`body_fat_percent` is included only for a finite, in-range metric captured by the
scale path, including a valid estimate; impedance and other metrics remain local.
The provider chooses its configured entry as it does for the existing service.

Only the oldest record is offered. One request may be outstanding; responses
expire after 60 seconds. Success durably removes that exact head record. Errors,
missing responses, disconnects, and failed ACK writes retain it. Retry spacing is
at least 60 seconds after each response/timeout. Late responses cannot ACK another
record. Timed-out callbacks are removed through APIServer's response handler.
This component reserves call IDs 0x80000000..0xffffffff; do not add other custom
callers using that range. Exhaustion stops replay until reboot.

Guest records and invalid metric records block FIFO replay. A zero capture
timestamp is accepted; the HA service then assigns the replay time. Pending count,
storage health, and optional oldest-record ID expose queue state. Recover the NVS
blob offline before using the production `discard_recording(record_id)` action for
an exact head ID; discarding permanently removes that record. No discard occurs
during an outstanding request. Corrupt blobs are retained and require offline
recovery; this change supplies no automatic salvage or erase action.

## Delivery limits

This is a bounded outbox with at-least-once retry, **not exactly-once delivery**.
HA success means the service returned successfully. It proves Google acceptance
only if the provider awaits all writes and raises on every rejected/failed write.
Its implementation was not available in this repository for verification. Merely
logging an unsuccessful HTTP status while returning normally would lose retries.

Google may accept a write before HA/device disconnects, before a response is lost,
or before the ACK reaches flash. Retrying can duplicate it; a multi-metric write
can partially succeed. `measurement_id` is stable but does not itself implement
provider/Google deduplication. Multiple HA action subscribers can also receive the
same request. Use one intended HA subscriber. Provider-side durable deduplication
and reconciliation are required for stronger guarantees. In-flight calls from a
previous boot cannot be reconciled by this device.

Connected live delivery intentionally remains outside this ACK contract. An API
state subscriber is the connection heuristic; a non-HA subscriber can satisfy it.
A connection lost just after capture can lose the live automation event. HA state
restoration/reconnect triggers must be filtered by the existing automation to
avoid reprocessing an old live ID. No live service or existing HA automation was
changed here.

## Storage and wear

NVS namespace/key: `ge_recordings` / `queue`. GQE1 uses a 28-byte header/checksum
and 96 bytes per record: 1,564 bytes at default 16, 3,100 bytes at maximum 32.
These fit ESP-IDF's multipage blob support; the inspected generated partition has
0x70000 bytes of NVS, shared with other components. Actual free space can still
cause writes to fail. There is no assumption that a successful enqueue follows
from free logical queue capacity.

Each capture persists the sequence (online too); offline ACKs also write one
snapshot. Retries do not write flash. Snapshot writes are O(capacity), acceptable
for infrequent weigh-ins, not high-rate telemetry. Full queues reject new offline
records rather than overwrite old ones; online identity reservation still works.
Failed append/ACK writes do not change the queue or caller record in RAM. An
ambiguous NVS commit failure stops further writes/replay until reboot. Boot checks
length, schema, CRC, sequence, flags, and metric validity before replacing RAM;
invalid blobs remain on flash. Capacity changes preserve records; shrinking below
pending count stops storage until configuration is corrected.

The installation epoch is initialized once and persisted with the sequence.
Numeric persisted and fallback live IDs stop before 2^24 to retain exact float sensor
representation; replay IDs include the epoch and sequence. Reboots do not reset
persisted counters. An erased/replaced NVS partition starts a new epoch and resets
the numeric live counter. Storage failure/exhaustion suppresses only an offline
record; online sensor states still publish using a bounded RAM-only fallback ID.

## Focused validation

Run from repository root (Python environment needs pinned ESPHome 2026.8.2):

```sh
g++ -std=c++17 -Wall -Wextra -Werror -I. tests/recording_test.cpp -o /tmp/recording-test
/tmp/recording-test
python3 tests/config_test.py
```

Host checks cover FIFO/full capacity, wrap, corruption/truncation, invalid schema,
append/ACK write failure, unsuccessful/stale/duplicate responses, nullable metrics,
replay eligibility, online identity reservation, and reboot round trips. Config
checks use only public CI placeholders in a temporary directory. They validate
production/discovery and reject capacities 0 and 33. No framework is needed.

Official references inspected:

- [ESPHome native API actions and responses](https://esphome.io/components/api/).
- Installed ESPHome 2026.8.2 `api_server.{h,cpp}`, `api_connection.{h,cpp}`,
  `homeassistant_service.h`, `api_pb2.h`, and `string_ref.h` under
  `/var/lib/hermes/.venv-esphome/lib/python3.13/site-packages/esphome/`.
  `send_homeassistant_action` is void; status callbacks use nonzero `call_id` and
  `USE_API_HOMEASSISTANT_ACTION_RESPONSES`. Request strings are serialized during
  send; local owning strings remain alive until it returns.
- [ESP-IDF NVS limits, wear leveling, and commit API](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-reference/storage/nvs_flash.html).

Review validation used cached ESP32-C6 compiler/SDK for `-fsyntax-only` on both
component translation units, then a full ESPHome 2026.8.2 compile on a detached
build host. The resulting image was flashed through the controlled serial upload
path (without opening a log console), and the device/API plus new HA outbox
entities were read back successfully. Hardware NVS power-loss behavior, offline
BLE capture, and end-to-end HA/Google replay remain untested.
