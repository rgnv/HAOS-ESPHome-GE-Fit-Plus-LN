#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"

#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif

#ifdef USE_ESP32

#include <esp_gattc_api.h>
#include <cmath>
#include <nvs.h>
#include "recording.h"

namespace esphome {
namespace ge_scale {

namespace espbt = esphome::esp32_ble_tracker;

// GE "Fit Plus" body scale BLE bridge.
//
// Connects to the scale as a central, replays the captured unlock handshake, parses
// the legacy 0xb1 result frame and the Fit Plus 0x02/0xfe metric burst, reproduces
// the Fit Profile app's body-composition numbers, writes them back to the scale's own
// display (the "measurement done" signal), and publishes everything to Home Assistant
// over the ESPHome native API.
//
// If no valid impedance is reported, it still finalizes a weight-only reading with a
// BMI-based body-fat estimate so data keeps flowing.
class GEScale : public Component, public ble_client::BLEClientNode {
 public:
  void setup() override;
  void loop() override;
  void set_recording_capacity(uint32_t n) { this->recordings_.capacity = n; }
  uint32_t pending_recordings() const { return this->recordings_.count; }
  bool recording_storage_ok() const { return this->recording_ok_; }
  std::string oldest_recording() const {
    return this->recordings_.front() ? this->recordings_.id(this->recordings_.front()->sequence) : "";
  }
  void discard_recording(std::string token);
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }
  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;

  // --- profile / config ---
  void set_height(float m) { this->height_m_ = m; }
  void set_sex_male(bool m) { this->sex_male_ = m; }
  void set_birthday(int y, int mo, int d) {
    this->birth_year_ = y;
    this->birth_month_ = mo;
    this->birth_day_ = d;
  }
  void set_age_fallback(float years) { this->age_fallback_ = years; }
  void set_expected_weight(float kg) { this->expected_weight_kg_ = kg; }
  void set_weight_tolerance(float kg) { this->weight_tol_kg_ = kg; }
  void set_write_back(bool wb) { this->write_back_ = wb; }
  void set_filter_guests(bool filter) { this->filter_guests_ = filter; }
#ifdef USE_TIME
  void set_time(time::RealTimeClock *t) { this->time_ = t; }
#endif

  // --- entity setters (all optional) ---
#ifdef USE_SENSOR
  void set_weight_sensor(sensor::Sensor *s) { this->weight_sensor_ = s; }
  void set_weight_lb_sensor(sensor::Sensor *s) { this->weight_lb_sensor_ = s; }
  void set_bmi_sensor(sensor::Sensor *s) { this->bmi_sensor_ = s; }
  void set_measurement_id_sensor(sensor::Sensor *s) { this->measurement_id_sensor_ = s; }
  void set_body_fat_sensor(sensor::Sensor *s) { this->body_fat_sensor_ = s; }
  void set_body_water_sensor(sensor::Sensor *s) { this->body_water_sensor_ = s; }
  void set_protein_sensor(sensor::Sensor *s) { this->protein_sensor_ = s; }
  void set_bone_mass_sensor(sensor::Sensor *s) { this->bone_mass_sensor_ = s; }
  void set_muscle_mass_sensor(sensor::Sensor *s) { this->muscle_mass_sensor_ = s; }
  void set_skeletal_muscle_sensor(sensor::Sensor *s) { this->skeletal_muscle_sensor_ = s; }
  void set_fat_free_mass_sensor(sensor::Sensor *s) { this->fat_free_mass_sensor_ = s; }
  void set_whole_body_impedance_sensor(sensor::Sensor *s) { this->z_whole_sensor_ = s; }
  void set_impedance_sensor(int i, sensor::Sensor *s) { this->impedance_sensors_[i] = s; }
  void set_weight_guest_sensor(sensor::Sensor *s) { this->weight_guest_sensor_ = s; }
  // Hidden (disabled-by-default) estimate entities: BMI-based body-comp used when there
  // is no impedance. Main metrics also publish these values
  // with source=estimate; these diagnostic duplicates remain disabled by default.
  void set_body_fat_estimate_sensor(sensor::Sensor *s) { this->body_fat_estimate_sensor_ = s; }
  void set_body_water_estimate_sensor(sensor::Sensor *s) { this->body_water_estimate_sensor_ = s; }
  void set_protein_estimate_sensor(sensor::Sensor *s) { this->protein_estimate_sensor_ = s; }
  void set_bone_mass_estimate_sensor(sensor::Sensor *s) { this->bone_mass_estimate_sensor_ = s; }
  void set_muscle_mass_estimate_sensor(sensor::Sensor *s) { this->muscle_mass_estimate_sensor_ = s; }
  void set_skeletal_muscle_estimate_sensor(sensor::Sensor *s) { this->skeletal_muscle_estimate_sensor_ = s; }
  void set_fat_free_mass_estimate_sensor(sensor::Sensor *s) { this->fat_free_mass_estimate_sensor_ = s; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_subject_sensor(text_sensor::TextSensor *s) { this->subject_sensor_ = s; }
  void set_source_sensor(text_sensor::TextSensor *s) { this->source_sensor_ = s; }
#endif
#ifdef USE_BINARY_SENSOR
  void set_is_guest_sensor(binary_sensor::BinarySensor *s) { this->is_guest_sensor_ = s; }
#endif

 protected:
  RecordingQueue recordings_;
  nvs_handle_t recording_nvs_{0};
  bool recording_ok_{false};
#ifdef USE_WIFI
  bool wifi_delivery_requested_{false};
  uint32_t wifi_request_started_ms_{0};
  uint32_t wifi_retry_after_ms_{0};
  uint32_t wifi_idle_deadline_ms_{0};
#endif
  uint32_t replay_ms_{0}, pending_call_{0}, fallback_measurement_id_{0};
  std::array<float, 8> session_impedances_{};
  bool legacy_mode_seen_{false};
  bool legacy_stable_{false};
  bool legacy_metric_seen_{false};
  bool legacy_result_active_{false};
  uint32_t legacy_stable_since_ms_{0};
  uint32_t legacy_last_metric_ms_{0};
  float legacy_bmi_{NAN};
  float legacy_fat_pct_{NAN};
  float legacy_water_pct_{NAN};
  float legacy_bone_pct_{NAN};
  float legacy_muscle_pct_{NAN};
  bool qn_mode_seen_{false};
  bool qn_config_sent_{false};
  bool qn_ready_sent_{false};
  bool qn_history_sent_{false};
  bool qn_trigger_sent_{false};
  bool qn_stable_ack_sent_{false};
  uint8_t qn_protocol_type_{0};
  uint16_t qn_info_length_{0};
  bool save_recordings_(const std::vector<uint8_t> &bytes);
  void record_(Recording &record);
  void replay_();
#ifdef USE_WIFI
  void request_wifi_();
  void maybe_disable_wifi_();
#endif

  // BLE plumbing
  uint16_t notify_handle_{0};  // fff1 (notify)
  uint16_t write_handle_{0};   // fff2 (write)
  bool handshake_started_{false};
  int hs_next_{0};  // next unlock-handshake step to send (0..4; 4 = done)
  uint32_t session_generation_{0};

  // per-connection measurement state
  bool had_live_{false};        // saw at least one live-weight (0x10) frame
  bool saw_computing_{false};   // saw a 0xb4 "computing" frame -> BIA started
  bool got_result_{false};        // published a full 0xb1 BIA result
  bool weightonly_published_{false};  // published a weight-only reading (can be upgraded)
  bool measurement_id_published_{false};
  float last_weight_{0.0f};
  uint32_t last_weight_change_ms_{0};
  uint32_t computing_since_ms_{0};

  void reset_session_();
  void start_handshake_();
  void send_handshake_step_(int i);
  void write_char_(uint16_t handle, const uint8_t *data, uint16_t len, esp_gatt_write_type_t wt);

  void handle_frame_(const uint8_t *b, uint16_t len);
  void handle_legacy_frame_(const uint8_t *b, uint16_t len);
  void handle_qn_scale_info_(const uint8_t *b, uint16_t len);
  void handle_qn_ready_();
  void handle_qn_config_request_();
  void handle_qn_stored_result_(const uint8_t *b, uint16_t len);
  void send_qn_measurement_trigger_(float weight_kg);
  void finalize_legacy_result_();
  void finalize_result_(float weight_kg, int z_whole);  // z_whole <= 0 -> weight-only fallback
  void send_display_frames_(float weight_kg, int z_whole);
  void publish_diagnostics_(const float *impedances, int z_whole);

  bool is_main_(float weight_kg) const {
    return !this->filter_guests_ || fabsf(weight_kg - this->expected_weight_kg_) <= this->weight_tol_kg_;
  }
  float compute_age_();

  // config
  float height_m_{1.78f};
  bool sex_male_{true};
  int birth_year_{0}, birth_month_{0}, birth_day_{0};
  float age_fallback_{25.0f};
  float expected_weight_kg_{74.0f};
  float weight_tol_kg_{6.0f};
  bool write_back_{false};
  bool filter_guests_{false};
#ifdef USE_TIME
  time::RealTimeClock *time_{nullptr};
#endif

#ifdef USE_SENSOR
  sensor::Sensor *weight_sensor_{nullptr};
  sensor::Sensor *weight_lb_sensor_{nullptr};
  sensor::Sensor *bmi_sensor_{nullptr};
  sensor::Sensor *measurement_id_sensor_{nullptr};
  sensor::Sensor *body_fat_sensor_{nullptr};
  sensor::Sensor *body_water_sensor_{nullptr};
  sensor::Sensor *protein_sensor_{nullptr};
  sensor::Sensor *bone_mass_sensor_{nullptr};
  sensor::Sensor *muscle_mass_sensor_{nullptr};
  sensor::Sensor *skeletal_muscle_sensor_{nullptr};
  sensor::Sensor *fat_free_mass_sensor_{nullptr};
  sensor::Sensor *z_whole_sensor_{nullptr};
  sensor::Sensor *impedance_sensors_[8]{};
  sensor::Sensor *weight_guest_sensor_{nullptr};
  sensor::Sensor *body_fat_estimate_sensor_{nullptr};
  sensor::Sensor *body_water_estimate_sensor_{nullptr};
  sensor::Sensor *protein_estimate_sensor_{nullptr};
  sensor::Sensor *bone_mass_estimate_sensor_{nullptr};
  sensor::Sensor *muscle_mass_estimate_sensor_{nullptr};
  sensor::Sensor *skeletal_muscle_estimate_sensor_{nullptr};
  sensor::Sensor *fat_free_mass_estimate_sensor_{nullptr};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *subject_sensor_{nullptr};
  text_sensor::TextSensor *source_sensor_{nullptr};
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *is_guest_sensor_{nullptr};
#endif
};

}  // namespace ge_scale
}  // namespace esphome

#endif  // USE_ESP32
