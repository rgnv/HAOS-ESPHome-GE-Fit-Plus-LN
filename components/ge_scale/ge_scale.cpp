#include "ge_scale.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include <cmath>
#include <cstdio>

namespace esphome {
namespace ge_scale {

static const char *const TAG = "ge_scale";

static const double LB = 0.45359237;

// Fixed fractions of fat-free mass (from bodycomp.py, calibrated to the Fit Profile app).
static const float FRAC_WATER = 0.7220f;
static const float FRAC_PROTEIN = 0.2279f;
static const float FRAC_BONE = 0.0502f;
static const float FRAC_SMM = 0.6458f;

// Accept only plausible whole-body impedance values from the scale's BIA path.
// A four-pad scale can provide foot-to-foot BIA; missing/invalid data is unknown.
static const int Z_MIN = 100;
static const int Z_MAX = 1200;

// Timing.
static const uint32_t HANDSHAKE_STEP_MS = 200;
static const uint32_t STABLE_MS = 18000;            // weight steady + no BIA result -> weight-only
static const uint32_t COMPUTING_TIMEOUT_MS = 45000;  // BIA started but no result -> give up
static const uint32_t LEGACY_METRIC_QUIET_MS = 2000; // wait for the 0x02/0xfe burst to finish
static const float MIN_WEIGHT_KG = 5.0f;
static const float WEIGHT_STABLE_DELTA = 0.1f;

// Unlock handshake (shared/handshake-replay.txt).  Sent to fff2, 200 ms apart.
static const uint8_t HS0[] = {0x13, 0x0a, 0xff, 0x02, 0x10, 0x00, 0x00, 0x00, 0xb4, 0xe2};
static const uint8_t HS1[] = {0x20, 0x09, 0xff, 0x39, 0x97, 0xe6, 0x31, 0x90, 0x9f};
static const uint8_t HS2[] = {0xa0, 0x0d, 0x02, 0xfe, 0xff, 0xee, 0x00, 0x19, 0x06, 0xf4, 0x04, 0x02, 0xb3};
static const uint8_t HS3[] = {0x22, 0x06, 0xff, 0x00, 0x01, 0x28};
static const uint8_t *const HS[] = {HS0, HS1, HS2, HS3};
static const uint16_t HS_LEN[] = {sizeof(HS0), sizeof(HS1), sizeof(HS2), sizeof(HS3)};

// FFM = FFM_A*(Ht^2/Z) + FFM_B*W + FFM_C   (bodycomp.py single-subject fit).
static const float FFM_A = 1234.4413142868493f;
static const float FFM_B = 0.40454841017657805f;
static const float FFM_C = 25.48036033753605f;

// Write-back display-frame field coefficients (bodycomp._FIELD_COEF).  Fields 1 and 3
// are overridden with fat%*10 and skeletal-muscle%*10; the rest reproduce the app's
// on-display values.  basis = [Ht^2/Z, W, 1].
static const float FIELD_COEF[8][3] = {
    {0.0f, 0.0f, 16.0f},
    {-14959.216125107047f, 5.811746385808269f, -189.21668315058827f},
    {-2379.8752926210254f, 3.197323288650493f, 523.1246185896813f},
    {4419.768400600015f, -3.080743250352583f, 748.7685654763195f},
    {524592.5109326105f, 26.64716651585372f, 806.5305023047146f},
    {-403218.8710085171f, 10.289345762924421f, 2055.113949622363f},
    {517792.86723941576f, 32.92523305485731f, -4012.113444582079f},
    {2039.8931079686508f, 0.11658003829882707f, 0.8931840659954299f},
};

#ifdef USE_SENSOR
static inline void publish_(sensor::Sensor *s, float v) {
  if (s != nullptr)
    s->publish_state(v);
}
#endif

void GEScale::reset_session_() {
  this->session_generation_++;
  this->session_impedances_.fill(NAN);
  this->had_live_ = false;
  this->saw_computing_ = false;
  this->got_result_ = false;
  this->weightonly_published_ = false;
  this->measurement_id_published_ = false;
  this->last_weight_ = 0.0f;
  this->last_weight_change_ms_ = 0;
  this->computing_since_ms_ = 0;
  this->handshake_started_ = false;
  this->hs_next_ = 0;
  this->legacy_mode_seen_ = false;
  this->legacy_stable_ = false;
  this->legacy_metric_seen_ = false;
  this->legacy_result_active_ = false;
  this->legacy_stable_since_ms_ = 0;
  this->legacy_last_metric_ms_ = 0;
  this->legacy_bmi_ = NAN;
  this->legacy_fat_pct_ = NAN;
  this->legacy_water_pct_ = NAN;
  this->legacy_bone_pct_ = NAN;
  this->legacy_muscle_pct_ = NAN;
  this->qn_mode_seen_ = false;
  this->qn_config_sent_ = false;
  this->qn_ready_sent_ = false;
  this->qn_history_sent_ = false;
  this->qn_protocol_type_ = 0;
}

void GEScale::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                  esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_OPEN_EVT:
      ESP_LOGI(TAG, "GATT open: status=%u addr_type=%u", param->open.status,
               static_cast<unsigned>(this->parent()->get_remote_addr_type()));
      break;
    case ESP_GATTC_SEARCH_CMPL_EVT: {
      ESP_LOGI(TAG, "GATT discovery: status=%u", param->search_cmpl.status);
      this->reset_session_();
      auto *notify_chr = this->parent()->get_characteristic((uint16_t) 0xFFF0, (uint16_t) 0xFFF1);
      auto *write_chr = this->parent()->get_characteristic((uint16_t) 0xFFF0, (uint16_t) 0xFFF2);
      if (notify_chr == nullptr || write_chr == nullptr) {
        ESP_LOGW(TAG, "Scale GATT characteristics fff1/fff2 not found");
        break;
      }
      this->notify_handle_ = notify_chr->handle;
      this->write_handle_ = write_chr->handle;
      auto status = esp_ble_gattc_register_for_notify(this->parent()->get_gattc_if(),
                                                      this->parent()->get_remote_bda(), this->notify_handle_);
      if (status)
        ESP_LOGW(TAG, "register_for_notify failed, status=%d", status);
      break;
    }
    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      if (param->reg_for_notify.handle != this->notify_handle_)
        break;
      ESP_LOGI(TAG, "GATT notify registration: status=%u", param->reg_for_notify.status);
      this->node_state = espbt::ClientState::ESTABLISHED;
      ESP_LOGI(TAG, "Notifications on; sending unlock handshake");
      this->start_handshake_();
      break;
    }
    case ESP_GATTC_WRITE_CHAR_EVT: {
      if (param->write.handle == this->write_handle_)
        ESP_LOGI(TAG, "GATT write: status=%u", param->write.status);
      // A handshake write was acknowledged: chain the next step immediately instead of
      // waiting out the wall-clock timer (the timers stay armed as a backstop).
      if (param->write.handle == this->write_handle_ && this->handshake_started_ && !this->qn_mode_seen_ && this->hs_next_ < 4 &&
          param->write.status == ESP_GATT_OK)
        this->send_handshake_step_(this->hs_next_);
      break;
    }
    case ESP_GATTC_NOTIFY_EVT: {
      if (param->notify.handle != this->notify_handle_ || param->notify.value_len == 0)
        break;
      this->handle_frame_(param->notify.value, param->notify.value_len);
      break;
    }
    case ESP_GATTC_DISCONNECT_EVT: {
      ESP_LOGI(TAG, "GATT disconnect: reason=%u", param->disconnect.reason);
      if (this->had_live_ && !this->got_result_ && !this->weightonly_published_) {
        ESP_LOGI(TAG, "Disconnected before a result; finalizing weight-only");
        this->finalize_result_(this->last_weight_, -1);
      }
      this->reset_session_();
      break;
    }
    default:
      break;
  }
}

void GEScale::start_handshake_() {
  if (this->handshake_started_)
    return;
  this->handshake_started_ = true;
  this->hs_next_ = 0;
  const uint32_t generation = this->session_generation_;
  // QN firmware emits 0x12 as soon as FFF1 notifications are enabled. Give
  // that notification-driven state machine a chance before using the legacy
  // fixed handshake for older scales that never send 0x12.
  this->set_timeout("legacy_hs", 2 * 1000,
                    [this, generation]() {
                      if (generation != this->session_generation_ || this->qn_mode_seen_)
                        return;
                      this->send_handshake_step_(0);
                      this->set_timeout("hs1", HANDSHAKE_STEP_MS,
                                        [this, generation]() {
                                          if (generation == this->session_generation_ && !this->qn_mode_seen_)
                                            this->send_handshake_step_(1);
                                        });
                      this->set_timeout("hs2", 2 * HANDSHAKE_STEP_MS,
                                        [this, generation]() {
                                          if (generation == this->session_generation_ && !this->qn_mode_seen_)
                                            this->send_handshake_step_(2);
                                        });
                      this->set_timeout("hs3", 3 * HANDSHAKE_STEP_MS,
                                        [this, generation]() {
                                          if (generation == this->session_generation_ && !this->qn_mode_seen_)
                                            this->send_handshake_step_(3);
                                        });
                    });
}

void GEScale::send_handshake_step_(int i) {
  if (i != this->hs_next_)
    return;  // this step already went out (ack beat the timer, or vice versa)
  this->hs_next_++;
  this->write_char_(this->write_handle_, HS[i], HS_LEN[i], ESP_GATT_WRITE_TYPE_RSP);
}

void GEScale::write_char_(uint16_t handle, const uint8_t *data, uint16_t len, esp_gatt_write_type_t wt) {
  if (this->node_state != espbt::ClientState::ESTABLISHED)
    return;
  esp_ble_gattc_write_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(), handle, len,
                           const_cast<uint8_t *>(data), wt, ESP_GATT_AUTH_REQ_NONE);
}

void GEScale::handle_qn_scale_info_(const uint8_t *b, uint16_t len) {
  if (this->qn_config_sent_ || len < 3)
    return;
  this->qn_mode_seen_ = true;
  if (len >= 18 && b[1] == len)
    this->qn_protocol_type_ = len == 18 ? 0x00 : b[2];
  else
    this->qn_protocol_type_ = b[2];

  const uint8_t height_cm = static_cast<uint8_t>(fminf(220.0f, fmaxf(60.0f, lroundf(this->height_m_ * 100.0f))));
  const uint8_t age = static_cast<uint8_t>(fminf(80.0f, fmaxf(6.0f, lroundf(this->compute_age_()))));
  const uint8_t gender = this->sex_male_ ? 0x00 : 0x01;  // QN wire encoding: male=0, female=1
  uint8_t config[] = {0x13, 0x09, this->qn_protocol_type_, 0x02, 0x10, height_cm, age, gender, 0x00};  // 0x02 = lb
  for (int i = 0; i < 8; i++) config[8] = static_cast<uint8_t>(config[8] + config[i]);
  this->qn_config_sent_ = true;
  this->write_char_(this->write_handle_, config, sizeof(config), ESP_GATT_WRITE_TYPE_RSP);
  ESP_LOGI(TAG, "QN scale info received; sent config protocol=0x%02x", this->qn_protocol_type_);
}

void GEScale::handle_qn_ready_() {
  if (this->qn_ready_sent_)
    return;
  this->qn_mode_seen_ = true;
  this->qn_ready_sent_ = true;
  uint32_t seconds = 0;
#ifdef USE_TIME
  if (this->time_ != nullptr) {
    auto now = this->time_->now();
    if (now.is_valid() && now.timestamp >= 946684800)
      seconds = static_cast<uint32_t>(now.timestamp - 946684800);
  }
#endif
  uint8_t sync[] = {0x20, 0x08, this->qn_protocol_type_, 0x00, 0x00, 0x00, 0x00, 0x00};
  sync[3] = seconds & 0xff;
  sync[4] = (seconds >> 8) & 0xff;
  sync[5] = (seconds >> 16) & 0xff;
  sync[6] = (seconds >> 24) & 0xff;
  for (int i = 0; i < 7; i++) sync[7] = static_cast<uint8_t>(sync[7] + sync[i]);
  this->write_char_(this->write_handle_, sync, sizeof(sync), ESP_GATT_WRITE_TYPE_RSP);

  const uint8_t age = static_cast<uint8_t>(fminf(255.0f, fmaxf(1.0f, this->compute_age_())));
  const uint32_t generation = this->session_generation_;
  this->set_timeout("qn_profile", HANDSHAKE_STEP_MS,
                    [this, generation, age]() {
                      if (generation == this->session_generation_) {
                        uint8_t profile[] = {0xa2, 0x06, 0x01, 0x32, age, 0x00};
                        for (int i = 0; i < 5; i++) profile[5] = static_cast<uint8_t>(profile[5] + profile[i]);
                        this->write_char_(this->write_handle_, profile, sizeof(profile), ESP_GATT_WRITE_TYPE_RSP);
                      }
                    });
  ESP_LOGI(TAG, "QN ready frame received; sent time sync and profile");
}

void GEScale::handle_qn_config_request_() {
  if (this->qn_history_sent_)
    return;
  this->qn_mode_seen_ = true;
  this->qn_history_sent_ = true;
  uint8_t history[] = {0xa0, 0x0d, 0x04, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  for (int i = 0; i < 12; i++) history[12] = static_cast<uint8_t>(history[12] + history[i]);
  this->write_char_(this->write_handle_, history, sizeof(history), ESP_GATT_WRITE_TYPE_RSP);
  const uint32_t generation = this->session_generation_;
  this->set_timeout("qn_history", HANDSHAKE_STEP_MS,
                    [this, generation]() {
                      if (generation == this->session_generation_) {
                        uint8_t history_start[] = {0xa0, 0x0d, 0x02, 0x01, 0x00, 0x08, 0x00, 0x21, 0x06, 0xb8, 0x04, 0x02, 0x00};
                        for (int i = 0; i < 12; i++) history_start[12] = static_cast<uint8_t>(history_start[12] + history_start[i]);
                        this->write_char_(this->write_handle_, history_start, sizeof(history_start), ESP_GATT_WRITE_TYPE_RSP);
                      }
                    });
  this->set_timeout("qn_start", 2 * HANDSHAKE_STEP_MS,
                    [this, generation]() {
                      if (generation == this->session_generation_) {
                        uint8_t start[] = {0x22, 0x06, this->qn_protocol_type_, 0x00, 0x03, 0x00};
                        for (int i = 0; i < 5; i++) start[5] = static_cast<uint8_t>(start[5] + start[i]);
                        this->write_char_(this->write_handle_, start, sizeof(start), ESP_GATT_WRITE_TYPE_RSP);
                      }
                    });
  ESP_LOGI(TAG, "QN config request received; sent history response and start");
}

void GEScale::handle_qn_stored_result_(const uint8_t *b, uint16_t len) {
  if (len < 17 || this->got_result_ || this->weightonly_published_)
    return;
  const float weight = static_cast<float>((b[10] << 8) | b[11]) / 100.0f;
  if (weight < MIN_WEIGHT_KG || weight > 300.0f)
    return;
  const int r1 = b[13] | (b[14] << 8);
  const int r2 = b[15] | (b[16] << 8);
  const int impedance = r1 > 0 ? r1 : r2;
  if (impedance >= Z_MIN && impedance <= Z_MAX) {
    this->session_impedances_[0] = static_cast<float>(impedance);
    this->finalize_result_(weight, impedance);
  } else {
    // The extended 0x23 record is often a zero-filled placeholder. Keep its
    // weight as the pending stable reading and wait for the following 0x10
    // stable frame, which carries the usable foot-to-foot resistance.
    this->had_live_ = true;
    if (fabsf(weight - this->last_weight_) > WEIGHT_STABLE_DELTA)
      this->last_weight_change_ms_ = millis();
    this->last_weight_ = weight;
    ESP_LOGD(TAG, "QN stored result has no BIA; waiting for stable 0x10 resistance");
  }
}

void GEScale::handle_legacy_frame_(const uint8_t *b, uint16_t len) {
  if (len < 7 || b[0] != 0x02)
    return;
  uint32_t sum = 0;
  for (int i = 0; i < 6; i++)
    sum += b[i];
  if (static_cast<uint8_t>(sum - 2u) != b[6]) {
    ESP_LOGD(TAG, "Legacy frame checksum rejected");
    return;
  }

  const uint32_t now = millis();
  if (b[1] == 0xfe) {
    if (b[5] != 0xcb)
      return;
    this->legacy_mode_seen_ = true;
    const uint8_t metric = b[2];
    const float value = static_cast<float>((b[3] << 8) | b[4]) / 10.0f;
    switch (metric) {
      case 0x00:  // weight, repeated in the metric burst
        if (value >= MIN_WEIGHT_KG && value <= 300.0f) {
          this->had_live_ = true;
          this->last_weight_ = value;
        }
        break;
      case 0x01:  // BMI
        if (value >= 5.0f && value <= 80.0f) this->legacy_bmi_ = value;
        else return;
        break;
      case 0x02:  // body fat percentage
        if (value > 0.0f && value < 100.0f) this->legacy_fat_pct_ = value;
        else return;
        break;
      case 0x05:  // total muscle percentage
        if (value > 0.0f && value < 100.0f) this->legacy_muscle_pct_ = value;
        else return;
        break;
      case 0x07:  // bone percentage
        if (value >= 0.0f && value < 100.0f) this->legacy_bone_pct_ = value;
        else return;
        break;
      case 0x08:  // total body water percentage
        if (value > 0.0f && value < 100.0f) this->legacy_water_pct_ = value;
        else return;
        break;
      default:
        return;  // BMR/other fields are not represented by the current HA schema.
    }
    this->legacy_metric_seen_ = true;
    this->legacy_last_metric_ms_ = now;
    ESP_LOGD(TAG, "Legacy scale metric received: id=0x%02x", metric);
    return;
  }

  if (b[5] != 0xce && b[5] != 0xca)
    return;
  this->legacy_mode_seen_ = true;
  const float weight = static_cast<float>((b[1] << 8) | b[2]) / 10.0f;
  if (weight < MIN_WEIGHT_KG || weight > 300.0f)
    return;
  this->had_live_ = true;
  if (fabsf(weight - this->last_weight_) > WEIGHT_STABLE_DELTA)
    this->last_weight_change_ms_ = now;
  this->last_weight_ = weight;
  this->legacy_stable_ = b[5] == 0xca;
  if (this->legacy_stable_)
    this->legacy_stable_since_ms_ = now;
}

void GEScale::handle_frame_(const uint8_t *b, uint16_t len) {
  if (len >= 7 && b[0] == 0x02) {
    this->handle_legacy_frame_(b, len);
    return;
  }
  const uint8_t type = b[0];
  ESP_LOGD(TAG, "GATT frame: type=0x%02x length=%u", type, len);
  if (type == 0x12 && len > 10) {
    this->handle_qn_scale_info_(b, len);
    return;
  }
  if (type == 0x14) {
    this->handle_qn_ready_();
    return;
  }
  if (type == 0x21) {
    this->handle_qn_config_request_();
    return;
  }
  if (type == 0xa1 || type == 0xa3)
    return;
  if (type == 0x23 && len >= 17) {
    this->handle_qn_stored_result_(b, len);
    return;
  }
  if (type == 0x10 && len > 6) {  // live weight (big-endian at bytes 5-6)
    float w = ((b[5] << 8) | b[6]) / 100.0f;
    if (w >= MIN_WEIGHT_KG && w <= 300.0f) {
      this->had_live_ = true;
      if (fabsf(w - this->last_weight_) > WEIGHT_STABLE_DELTA)
        this->last_weight_change_ms_ = millis();
      this->last_weight_ = w;

      // The Fit Plus long-frame variant reports a stable state (0x02) with
      // foot-to-foot BIA in bytes 7-10. The values are deci-ohms on this
      // firmware family; use them internally for the display calculation but
      // never expose them as HA impedance entities.
      if (len >= 11 && b[4] == 0x02) {
        const int r1 = (b[7] << 8) | b[8];
        const int r2 = (b[9] << 8) | b[10];
        const int raw_impedance = r1 > 0 ? r1 : r2;
        const int impedance = static_cast<int>(lroundf(raw_impedance / 10.0f));
        if (impedance >= Z_MIN && impedance <= Z_MAX) {
          this->session_impedances_[0] = static_cast<float>(impedance);
          ESP_LOGI(TAG, "Stable 0x10 foot-BIA received: %.2f kg, internal Z=%d", w, impedance);
          this->finalize_result_(w, impedance);
        }
      }
    }
    return;
  }
  if (type == 0xb4) {  // "computing" -> impedance sweep running
    if (!this->saw_computing_) {
      this->saw_computing_ = true;
      this->computing_since_ms_ = millis();
    }
    return;
  }
  if (type == 0xb1 && len > 22) {  // final result
    float w = (b[5] | (b[6] << 8)) / 100.0f;  // little-endian in the result frame
    if (w < MIN_WEIGHT_KG || w > 300.0f)
      return;
    float imps[8];
    float zsum = 0.0f;
    for (int i = 0; i < 8; i++) {
      imps[i] = (b[7 + 2 * i] | (b[8 + 2 * i] << 8)) / 10.0f;
      if (imps[i] > 5000.0f)
        return;
      zsum += imps[i];
    }
    // Whole-body impedance is not a direct frame field on this scale; the 8 segmental
    // impedances (bytes 7-22) sum/4 reproduces the value the body-comp model was fit to.
    int z = (int) lroundf(zsum / 4.0f);
    bool z_valid = (z >= Z_MIN && z <= Z_MAX);
    ESP_LOGI(TAG, "Result: %.2f kg, Z=%d (%s)", w, z, z_valid ? "with BIA" : "no BIA");
    if (z_valid)
      std::copy(imps, imps + 8, this->session_impedances_.begin());
    this->publish_diagnostics_(imps, z_valid ? z : -1);
    // Write the computed numbers back to the scale display FIRST -- that is the
    // "measurement complete" signal to the person standing on it.
    if (this->write_back_ && z_valid && this->is_main_(w))
      this->send_display_frames_(w, z);
    this->finalize_result_(w, z_valid ? z : -1);
  }
}

void GEScale::loop() {
  this->replay_();
#ifdef USE_WIFI
  this->maybe_disable_wifi_();
#endif
  if (this->node_state != espbt::ClientState::ESTABLISHED)
    return;
  if (!this->had_live_ || this->got_result_ || this->weightonly_published_ ||
      this->last_weight_ < MIN_WEIGHT_KG)
    return;
  const uint32_t now = millis();
  if (this->legacy_mode_seen_) {
    if (!this->legacy_stable_)
      return;
    if (this->legacy_metric_seen_ && this->legacy_last_metric_ms_ &&
        now - this->legacy_last_metric_ms_ > LEGACY_METRIC_QUIET_MS) {
      this->finalize_legacy_result_();
      return;
    }
    if (this->legacy_stable_since_ms_ && now - this->legacy_stable_since_ms_ > COMPUTING_TIMEOUT_MS)
      this->finalize_result_(this->last_weight_, -1);
    return;
  }
  // Weight held steady long enough and no impedance sweep started.
  if (!this->saw_computing_ && (now - this->last_weight_change_ms_) > STABLE_MS) {
    ESP_LOGI(TAG, "Stable weight, no BIA result -> weight-only reading");
    this->finalize_result_(this->last_weight_, -1);
    return;
  }
  // Bars engaged but the result frame never arrived: fall back rather than hang.
  if (this->saw_computing_ && (now - this->computing_since_ms_) > COMPUTING_TIMEOUT_MS) {
    ESP_LOGW(TAG, "Impedance measurement timed out -> weight-only reading");
    this->finalize_result_(this->last_weight_, -1);
  }
}

float GEScale::compute_age_() {
#ifdef USE_TIME
  if (this->time_ != nullptr && this->birth_year_ > 0) {
    auto now = this->time_->now();
    if (now.is_valid()) {
      int age = now.year - this->birth_year_;
      if (now.month < this->birth_month_ ||
          (now.month == this->birth_month_ && now.day_of_month < this->birth_day_))
        age -= 1;
      return (float) age;
    }
  }
#endif
  return this->age_fallback_;
}

void GEScale::publish_diagnostics_(const float *impedances, int z_whole) {
#ifdef USE_SENSOR
  if (z_whole <= 0)
    return;  // no accepted impedance data -> nothing to report
  for (int i = 0; i < 8; i++)
    publish_(this->impedance_sensors_[i], impedances[i]);
  publish_(this->z_whole_sensor_, (float) z_whole);
#endif
}

void GEScale::finalize_legacy_result_() {
  if (!this->legacy_stable_ || this->last_weight_ < MIN_WEIGHT_KG || this->measurement_id_published_)
    return;
  this->legacy_result_active_ = true;
  this->finalize_result_(this->last_weight_, -1);
  this->legacy_result_active_ = false;
}

void GEScale::finalize_result_(float weight_kg, int z_whole) {
  if (weight_kg < MIN_WEIGHT_KG)
    return;
  const bool z_valid = z_whole > 0;
  if (z_valid) {
    if (this->got_result_)
      return;
    this->got_result_ = true;
  } else {
    if (this->got_result_ || this->weightonly_published_)
      return;
    this->weightonly_published_ = true;
  }

  const bool main = this->is_main_(weight_kg);
  const char *subject = main ? "primary" : "guest";
  // Dynamic algorithm selection: full BIA when a valid impedance is available,
  // otherwise the anthropometric (BMI) estimate from weight alone.
  const char *source = z_valid ? "bia" : (this->legacy_result_active_ ? "scale" : "estimate");

#ifdef USE_TEXT_SENSOR
  if (this->subject_sensor_ != nullptr)
    this->subject_sensor_->publish_state(subject);
  if (this->source_sensor_ != nullptr)
    this->source_sensor_->publish_state(source);
#endif
#ifdef USE_BINARY_SENSOR
  if (this->is_guest_sensor_ != nullptr)
    this->is_guest_sensor_->publish_state(!main);
#endif

  const float h = this->height_m_;
  float bmi = weight_kg / (h * h);

  Recording record;
  record.bia = z_valid;
  record.scale_metrics = this->legacy_result_active_;
  record.guest = !main;
  record.metrics[0] = weight_kg;
  record.metrics[1] = static_cast<float>(weight_kg / LB);
  if (z_valid) {
    record.metrics[10] = z_whole;
    std::copy(this->session_impedances_.begin(), this->session_impedances_.end(), record.metrics.begin() + 11);
  }

  if (!main) {  // guest -> hidden weight_guest entity only; the main entities stay untouched
#ifdef USE_SENSOR
    publish_(this->weight_guest_sensor_, (float) (weight_kg / LB));
#endif
    this->record_(record);
    ESP_LOGI(TAG, "Published GUEST weigh-in: %.2f kg -> weight_guest (guest flag set)", weight_kg);
    return;
  }

  // primary user
#ifdef USE_SENSOR
  publish_(this->weight_sensor_, weight_kg);
  publish_(this->weight_lb_sensor_, (float) (weight_kg / LB));
  publish_(this->bmi_sensor_, bmi);
#endif

  float ffm, fat_pct;
  if (z_valid) {  // full BIA (bodycomp model, reproduces the app)
    ffm = FFM_A * (h * h / (float) z_whole) + FFM_B * weight_kg + FFM_C;
    if (ffm > weight_kg * 0.97f)
      ffm = weight_kg * 0.97f;   // physical clamp: FFM can't exceed body weight
    if (ffm < 1.0f)
      ffm = 1.0f;
    fat_pct = 100.0f * (weight_kg - ffm) / weight_kg;
  } else if (this->legacy_result_active_) {  // direct Fit Plus metric burst
    if (std::isfinite(this->legacy_bmi_))
      bmi = this->legacy_bmi_;
    fat_pct = std::isfinite(this->legacy_fat_pct_)
                  ? this->legacy_fat_pct_
                  : 1.20f * bmi + 0.23f * this->compute_age_() - 10.8f * (this->sex_male_ ? 1.0f : 0.0f) - 5.4f;
    fat_pct = fmaxf(3.0f, fminf(60.0f, fat_pct));
    ffm = weight_kg * (1.0f - fat_pct / 100.0f);
  } else {  // no BIA result: BMI-based body-fat estimate (Deurenberg 1991)
    fat_pct = 1.20f * bmi + 0.23f * this->compute_age_() - 10.8f * (this->sex_male_ ? 1.0f : 0.0f) - 5.4f;
    fat_pct = fmaxf(3.0f, fminf(60.0f, fat_pct));
    ffm = weight_kg * (1.0f - fat_pct / 100.0f);
  }

  const float water = FRAC_WATER * ffm;
  const float protein = FRAC_PROTEIN * ffm;
  const float bone = FRAC_BONE * ffm;
  const float smm = FRAC_SMM * ffm;
  const float bone_pct = this->legacy_result_active_ && std::isfinite(this->legacy_bone_pct_)
                             ? this->legacy_bone_pct_ : 100.0f * bone / weight_kg;
  const float muscle_mass_pct = this->legacy_result_active_ && std::isfinite(this->legacy_muscle_pct_)
                                    ? this->legacy_muscle_pct_ : 100.0f - fat_pct - bone_pct;
  const float water_pct = this->legacy_result_active_ && std::isfinite(this->legacy_water_pct_)
                              ? this->legacy_water_pct_ : 100.0f * water / weight_kg;
  const float protein_pct = 100.0f * protein / weight_kg;
  const float smm_pct = 100.0f * smm / weight_kg;
  const float values[] = {bmi, fat_pct, water_pct, protein_pct, bone_pct, muscle_mass_pct, smm_pct, ffm};
  std::copy(values, values + 8, record.metrics.begin() + 2);

#ifdef USE_SENSOR
  if (z_valid) {  // impedance-based -> the real, trusted body-comp on the MAIN entities
    publish_(this->body_fat_sensor_, fat_pct);
    publish_(this->body_water_sensor_, water_pct);
    publish_(this->protein_sensor_, protein_pct);
    publish_(this->bone_mass_sensor_, bone_pct);
    publish_(this->muscle_mass_sensor_, muscle_mass_pct);
    publish_(this->skeletal_muscle_sensor_, smm_pct);
    publish_(this->fat_free_mass_sensor_, ffm);
  } else {  // no impedance -> publish calculated estimates, never fabricate impedance
    publish_(this->body_fat_sensor_, fat_pct);
    publish_(this->body_water_sensor_, water_pct);
    publish_(this->protein_sensor_, protein_pct);
    publish_(this->bone_mass_sensor_, bone_pct);
    publish_(this->muscle_mass_sensor_, muscle_mass_pct);
    publish_(this->skeletal_muscle_sensor_, smm_pct);
    publish_(this->fat_free_mass_sensor_, ffm);
    publish_(this->z_whole_sensor_, NAN);
    for (auto *sensor : this->impedance_sensors_) publish_(sensor, NAN);
    publish_(this->body_fat_estimate_sensor_, fat_pct);
    publish_(this->body_water_estimate_sensor_, water_pct);
    publish_(this->protein_estimate_sensor_, protein_pct);
    publish_(this->bone_mass_estimate_sensor_, bone_pct);
    publish_(this->muscle_mass_estimate_sensor_, muscle_mass_pct);
    publish_(this->skeletal_muscle_estimate_sensor_, smm_pct);
    publish_(this->fat_free_mass_estimate_sensor_, ffm);
  }
#endif
  this->record_(record);  // ID is the final event, after every metric and state.
  ESP_LOGI(TAG, "Published %s: %.2f kg, fat %.1f%%, ffm %.2f kg (%s)", subject, weight_kg, fat_pct, ffm, source);
}

void GEScale::send_display_frames_(float weight_kg, int z_whole) {
  const float h = this->height_m_;
  const float basis0 = h * h / (float) z_whole;
  const float ffm = FFM_A * basis0 + FFM_B * weight_kg + FFM_C;
  const float fat_pct = 100.0f * (weight_kg - ffm) / weight_kg;
  const float smm_pct = 100.0f * (FRAC_SMM * ffm) / weight_kg;

  int fields[8];
  for (int i = 0; i < 8; i++)
    fields[i] = (int) lroundf(FIELD_COEF[i][0] * basis0 + FIELD_COEF[i][1] * weight_kg + FIELD_COEF[i][2]);
  fields[1] = (int) lroundf(fat_pct * 10.0f);   // P1 = fat% x10
  fields[3] = (int) lroundf(smm_pct * 10.0f);   // P3 = skeletal-muscle% x10

  static uint8_t frame[19];
  frame[0] = 0x1c;
  frame[1] = 0x13;
  frame[2] = 0xff;
  int idx = 3;
  for (int i = 0; i < 7; i++) {  // P0..P6 as uint16 LE
    int v = fields[i];
    if (v < 0)
      v = 0;
    if (v > 0xFFFF)
      v = 0xFFFF;
    frame[idx++] = v & 0xFF;
    frame[idx++] = (v >> 8) & 0xFF;
  }
  int tail = fields[7];
  if (tail < 0)
    tail = 0;
  if (tail > 0xFF)
    tail = 0xFF;
  frame[idx++] = tail;  // tail byte
  int sum = 0;
  for (int i = 0; i < 18; i++)
    sum += frame[i];
  frame[18] = sum & 0xFF;  // checksum

  static const uint8_t COMMIT[] = {0x1f, 0x05, 0xff, 0x10, 0x33};
  this->write_char_(this->write_handle_, frame, sizeof(frame), ESP_GATT_WRITE_TYPE_NO_RSP);
  // small gap, then the static commit frame that latches the display
  const uint32_t generation = this->session_generation_;
  this->set_timeout("commit", 150, [this, generation]() {
    if (generation == this->session_generation_)
      this->write_char_(this->write_handle_, COMMIT, sizeof(COMMIT), ESP_GATT_WRITE_TYPE_NO_RSP);
  });
  ESP_LOGD(TAG, "Wrote display frame %02x%02x...%02x back to scale", frame[0], frame[1], frame[18]);
}

void GEScale::dump_config() {
  ESP_LOGCONFIG(TAG,
                "GE Scale:\n"
                "  Height: %.2f m\n"
                "  Sex: %s\n"
                "  Expected weight: %.1f kg (tol %.1f)\n"
                "  Write-back to display: %s",
                this->height_m_, this->sex_male_ ? "male" : "female", this->expected_weight_kg_,
                this->weight_tol_kg_, YESNO(this->write_back_));
}

}  // namespace ge_scale
}  // namespace esphome

#endif  // USE_ESP32
