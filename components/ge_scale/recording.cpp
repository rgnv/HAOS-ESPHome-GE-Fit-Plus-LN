#include "ge_scale.h"
#ifdef USE_ESP32
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/api/api_server.h"
#include <esp_random.h>
#include "esphome/components/api/homeassistant_service.h"

namespace esphome::ge_scale {
static const char *const TAG = "ge_scale.recording";
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 90000;
static constexpr uint32_t WIFI_RETRY_DELAY_MS = 60000;
static constexpr uint32_t WIFI_IDLE_GRACE_MS = 5000;

#ifdef USE_WIFI
void GEScale::request_wifi_() {
  auto *wifi = wifi::global_wifi_component;
  // BLEClient also calls node loops during setup, before WiFi's lower priority.
  if (wifi == nullptr || !wifi->is_ready()) return;
  const uint32_t now = millis();
  if (wifi->is_disabled() && this->wifi_retry_after_ms_ &&
      static_cast<int32_t>(now - this->wifi_retry_after_ms_) < 0)
    return;
  this->wifi_retry_after_ms_ = 0;
  if (wifi->is_disabled()) {
    ESP_LOGI(TAG, "Enabling WiFi for queued measurement delivery");
    wifi->enable();
  }
  this->wifi_delivery_requested_ = true;
  this->wifi_idle_deadline_ms_ = 0;
  if (!this->wifi_request_started_ms_)
    this->wifi_request_started_ms_ = now;
}

void GEScale::maybe_disable_wifi_() {
  auto *wifi = wifi::global_wifi_component;
  if (wifi == nullptr || !this->wifi_delivery_requested_) return;
  const uint32_t now = millis();
  auto *server = api::global_api_server;
  const bool api_ready = server != nullptr && server->is_connected_with_state_subscription();
  if (!api_ready && this->wifi_request_started_ms_ && now - this->wifi_request_started_ms_ > WIFI_CONNECT_TIMEOUT_MS) {
    ESP_LOGW(TAG, "WiFi/API delivery timeout; retaining queued recordings and returning to BLE-only");
    wifi->disable();
    this->wifi_delivery_requested_ = false;
    this->wifi_request_started_ms_ = 0;
    this->wifi_idle_deadline_ms_ = 0;
    this->wifi_retry_after_ms_ = now + WIFI_RETRY_DELAY_MS;
    this->status_set_warning("WiFi delivery timeout; recording retained");
    return;
  }
  if (api_ready)
    this->wifi_request_started_ms_ = 0;
  if (this->pending_call_ || this->recordings_.count || !this->wifi_idle_deadline_ms_ ||
      static_cast<int32_t>(now - this->wifi_idle_deadline_ms_) < 0)
    return;
  ESP_LOGI(TAG, "Queued delivery complete; disabling WiFi until the next recording");
  wifi->disable();
  this->wifi_delivery_requested_ = false;
  this->wifi_idle_deadline_ms_ = 0;
}
#endif

void GEScale::setup() {
  if (this->recordings_.capacity < 1 || this->recordings_.capacity > RecordingQueue::MAX_RECORDS) {
    this->status_set_warning("Invalid recording capacity");
    return;
  }
  const uint32_t configured_capacity = this->recordings_.capacity;
  if (nvs_open("ge_recordings", NVS_READWRITE, &this->recording_nvs_) != ESP_OK) {
    this->status_set_warning("Recording storage unavailable");
    return;
  }
  size_t size = 0;
  auto err = nvs_get_blob(this->recording_nvs_, "queue", nullptr, &size);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    do { this->recordings_.epoch = (uint64_t(esp_random()) << 32) | esp_random(); } while (!this->recordings_.epoch);
    this->recording_ok_ = true;
    this->save_recordings_(this->recordings_.encode());
  } else if (err == ESP_OK && size >= 28 && size <= 28 + RecordingQueue::MAX_RECORDS * 96) {
    std::vector<uint8_t> bytes(size);
    this->recording_ok_ = nvs_get_blob(this->recording_nvs_, "queue", bytes.data(), &size) == ESP_OK &&
                          this->recordings_.decode(bytes);
  }
  if (this->recording_ok_ && this->recordings_.capacity != configured_capacity) {
    // Never erase or silently shrink a persisted outbox on configuration changes.
    if (this->recordings_.count > configured_capacity) {
      this->recording_ok_ = false;
    } else {
      auto candidate = std::make_unique<RecordingQueue>(this->recordings_);
      candidate->capacity = configured_capacity;  // decode normalized head to zero
      if (this->save_recordings_(candidate->encode())) this->recordings_ = *candidate;
    }
  }
  if (!this->recording_ok_)
    this->status_set_warning("Recording storage invalid; retained for recovery");
  // replay_() requests delivery once WiFi setup has finished.
}

bool GEScale::save_recordings_(const std::vector<uint8_t> &bytes) {
  if (nvs_set_blob(this->recording_nvs_, "queue", bytes.data(), bytes.size()) == ESP_OK &&
      nvs_commit(this->recording_nvs_) == ESP_OK)
    return true;
  // Commit failure is ambiguous: stop writes/replay until reboot reloads NVS.
  this->recording_ok_ = false;
  this->status_set_warning("Recording write failed; replay stopped");
  ESP_LOGE(TAG, "Recording write failed; live capture continues without durability");
  return false;
}

void GEScale::record_(Recording &r) {
  // One capture per BLE session, including a late BIA upgrade after weight-only.
  if (this->measurement_id_published_) return;
  this->measurement_id_published_ = true;
  r.uptime_ms = millis();
#ifdef USE_TIME
  if (this->time_ != nullptr && this->time_->now().is_valid())
    r.timestamp = this->time_->now().timestamp;
#endif
  auto *server = api::global_api_server;
  const bool online = server != nullptr && server->is_connected_with_state_subscription();
  bool persisted = false;
  if (this->recording_ok_)
    persisted = this->recordings_.append(r, [this](const auto &b) { return this->save_recordings_(b); }, !online);
  if (!online) {
    if (!persisted) {
      this->status_set_warning("Recording not retained; storage full or failed");
      ESP_LOGE(TAG, "Finalized recording not persisted; HA is offline");
    } else {
#ifdef USE_WIFI
      this->request_wifi_();
#endif
    }
    return;
  }

  // Online captures use the existing live-state automation. Persistence reserves a
  // monotonic sequence when possible; a RAM-only fallback keeps live HA delivery alive
  // if NVS is temporarily unavailable.
  uint32_t measurement = 0;
  if (persisted) {
    measurement = r.measurement;
  } else {
    if (this->fallback_measurement_id_ >= 16777215) {
      this->status_set_warning("Live measurement ID range exhausted; reboot required");
      ESP_LOGE(TAG, "Cannot publish a precise fallback measurement ID");
      return;
    }
    measurement = ++this->fallback_measurement_id_;
  }
  if (!persisted)
    this->status_set_warning("Recording identity not persisted; live measurement delivered");
#ifdef USE_SENSOR
  if (this->measurement_id_sensor_ != nullptr)
    this->measurement_id_sensor_->publish_state(static_cast<float>(measurement));
#endif
#ifdef USE_WIFI
  if (this->wifi_delivery_requested_ && this->recordings_.count == 0)
    this->wifi_idle_deadline_ms_ = millis() + WIFI_IDLE_GRACE_MS;
#endif
}

void GEScale::discard_recording(std::string token) {
  if (!this->recording_ok_ || this->pending_call_) return;
  if (this->recordings_.acknowledge(token, [this](const auto &b) { return this->save_recordings_(b); }))
    this->status_clear_warning();
}

void GEScale::replay_() {
  const uint32_t now = millis();
  auto *server = api::global_api_server;
  if (!this->recording_ok_ || this->pending_call_ || !this->recordings_.front()) return;
  if (server == nullptr || !server->is_connected_with_state_subscription()) {
#ifdef USE_WIFI
    this->request_wifi_();
#endif
    return;
  }
  const auto &r = *this->recordings_.front();
  if (!replayable(r)) {
    this->status_set_warning("Oldest recording needs recovery: guest or invalid timestamp");
    return;
  }
  if (this->replay_ms_ && now - this->replay_ms_ < 60000)
    return;
  this->replay_ms_ = now;
#ifdef USE_WIFI
  this->wifi_request_started_ms_ = 0;
#endif

  // Status-only response: provider must raise on rejected HTTP writes. No claim
  // of Google-level idempotency; a lost response can cause a duplicate on retry.
  // Reserve the high half for this component (one instance); YAML actions use low IDs.
  static uint32_t next_call = 0x80000000u;
  if (next_call == 0) {
    this->status_set_warning("Replay call IDs exhausted; reboot required");
    return;
  }
  const uint32_t call = next_call++;
  const std::string token = this->recordings_.id(r.sequence);
  std::array<std::string, 4> values;
  std::array<const char *, 4> keys{};
  size_t fields = 0;
  values[fields] = std::to_string(r.metrics[0]);
  keys[fields++] = "weight_kg";
  if (r.timestamp) {
    const time_t timestamp = r.timestamp;
    struct tm utc {};
    if (gmtime_r(&timestamp, &utc) != nullptr) {
      char measured_at[32];
      strftime(measured_at, sizeof(measured_at), "%Y-%m-%dT%H:%M:%SZ", &utc);
      values[fields] = measured_at;
      keys[fields++] = "measured_at";
    }
  }
  values[fields] = this->recordings_.id(r.measurement);
  keys[fields++] = "measurement_id";
  if (replay_body_fat(r)) {
    values[fields] = std::to_string(r.metrics[3]);
    keys[fields++] = "body_fat_percent";
  }
  // Owning strings remain alive until synchronous protobuf serialization finishes.
  api::HomeassistantActionRequest request;
  request.service = StringRef("google_health_scale_sync.log_body_measurements");
  request.call_id = call;
  request.data.init(fields);
  for (size_t i = 0; i < fields; ++i) {
    api::HomeassistantServiceMap entry;
    entry.key = StringRef(keys[i]);
    entry.value = StringRef(values[i]);
    request.data.push_back(entry);
  }
  this->pending_call_ = call;
  server->register_action_response_callback(call, [this, call, token](const api::ActionResponse &response) {
    if (this->pending_call_ != call) return;
    this->cancel_timeout("recording_response");
    this->pending_call_ = 0;
    this->replay_ms_ = millis();
    if (this->recording_ok_ && this->recordings_.response(token, response.is_success(),
        [this](const auto &b) { return this->save_recordings_(b); })) {
#ifdef USE_WIFI
      if (this->recordings_.count == 0)
        this->wifi_idle_deadline_ms_ = millis() + WIFI_IDLE_GRACE_MS;
#endif
      this->status_clear_warning();
    } else {
      this->status_set_warning("Replay failed; oldest recording retained");
    }
  });
  // APIServer has no unregister API. Deliver a local failure through its public
  // handler to remove the callback, bounding memory to one outstanding response.
  this->set_timeout("recording_response", 60000, [server, call]() {
    server->handle_action_response(call, false, StringRef("Recording response timeout"));
  });
  server->send_homeassistant_action(request);
}
}  // namespace esphome::ge_scale
#endif
