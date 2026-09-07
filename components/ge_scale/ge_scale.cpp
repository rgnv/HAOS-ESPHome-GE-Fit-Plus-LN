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

// Whole-body impedance is only produced when the hand bars are held.  Anything
// outside this band (0, foot-to-foot only, garbage) is treated as "no bars".
static const int Z_MIN = 100;
static const int Z_MAX = 1200;

// Timing.
static const uint32_t HANDSHAKE_STEP_MS = 200;
static const uint32_t STABLE_MS = 18000;            // weight steady + no bars -> weight-only
static const uint32_t COMPUTING_TIMEOUT_MS = 45000;  // bars engaged but no result -> give up
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
}

void GEScale::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                  esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_SEARCH_CMPL_EVT: {
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
      this->node_state = espbt::ClientState::ESTABLISHED;
      ESP_LOGI(TAG, "Notifications on; sending unlock handshake");
      this->start_handshake_();
      break;
    }
    case ESP_GATTC_WRITE_CHAR_EVT: {
      // A handshake write was acknowledged: chain the next step immediately instead of
      // waiting out the wall-clock timer (the timers stay armed as a backstop).
      if (param->write.handle == this->write_handle_ && this->handshake_started_ && this->hs_next_ < 4 &&
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
  this->send_handshake_step_(0);
  const uint32_t generation = this->session_generation_;
  // Backstop timers only: normally each write's ack (ESP_GATTC_WRITE_CHAR_EVT) chains
  // the next step within tens of ms, and these arrive as no-ops.
  this->set_timeout("hs1", HANDSHAKE_STEP_MS,
                    [this, generation]() {
                      if (generation == this->session_generation_)
                        this->send_handshake_step_(1);
                    });
  this->set_timeout("hs2", 2 * HANDSHAKE_STEP_MS,
                    [this, generation]() {
                      if (generation == this->session_generation_)
                        this->send_handshake_step_(2);
                    });
  this->set_timeout("hs3", 3 * HANDSHAKE_STEP_MS,
                    [this, generation]() {
                      if (generation == this->session_generation_)
                        this->send_handshake_step_(3);
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

void GEScale::handle_frame_(const uint8_t *b, uint16_t len) {
  ESP_LOGD(TAG, "notify[%u]: %s", len, format_hex_pretty(b, len).c_str());
  const uint8_t type = b[0];
  if (type == 0x10 && len > 6) {  // live weight (big-endian at bytes 5-6)
    float w = ((b[5] << 8) | b[6]) / 100.0f;
    if (w >= MIN_WEIGHT_KG && w <= 300.0f) {
      this->had_live_ = true;
      if (fabsf(w - this->last_weight_) > WEIGHT_STABLE_DELTA)
        this->last_weight_change_ms_ = millis();
      this->last_weight_ = w;
    }
    return;
  }
  if (type == 0x23 || type == 0xb4) {  // "computing" -> hand bars engaged, impedance sweep running
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
    ESP_LOGI(TAG, "Result: %.2f kg, Z=%d (%s)", w, z, z_valid ? "with bars" : "no bars");
    this->publish_diagnostics_(imps, z_valid ? z : -1);
    // Write the computed numbers back to the scale display FIRST -- that is the
    // "measurement complete" signal to the person standing on it.
    if (this->write_back_ && z_valid && this->is_main_(w))
      this->send_display_frames_(w, z);
    this->finalize_result_(w, z_valid ? z : -1);
  }
}

void GEScale::loop() {
  if (this->node_state != espbt::ClientState::ESTABLISHED)
    return;
  if (!this->had_live_ || this->got_result_ || this->weightonly_published_ ||
      this->last_weight_ < MIN_WEIGHT_KG)
    return;
  const uint32_t now = millis();
  // No hand bars: weight held steady long enough and no impedance sweep started.
  if (!this->saw_computing_ && (now - this->last_weight_change_ms_) > STABLE_MS) {
    ESP_LOGI(TAG, "Stable weight, no hand bars -> weight-only reading");
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
    return;  // no impedance data (feet-only / no bars) -> nothing to report
  for (int i = 0; i < 8; i++)
    publish_(this->impedance_sensors_[i], impedances[i]);
  publish_(this->z_whole_sensor_, (float) z_whole);
#endif
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
  // Dynamic algorithm selection: full segmental BIA when the hand bars gave us
  // impedance, otherwise the anthropometric (BMI) estimate from weight alone.
  const char *source = z_valid ? "bia" : "estimate";

#ifdef USE_SENSOR
  if (!this->measurement_id_published_) {
    this->measurement_id_++;
    publish_(this->measurement_id_sensor_, (float) this->measurement_id_);
    this->measurement_id_published_ = true;
  }
#endif

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
  const float bmi = weight_kg / (h * h);

  if (!main) {  // guest -> hidden weight_guest entity only; the main entities stay untouched
#ifdef USE_SENSOR
    publish_(this->weight_guest_sensor_, (float) (weight_kg / LB));
#endif
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
  } else {  // no hand bars: BMI-based body-fat estimate (Deurenberg 1991 anthropometric)
    fat_pct = 1.20f * bmi + 0.23f * this->compute_age_() - 10.8f * (this->sex_male_ ? 1.0f : 0.0f) - 5.4f;
    fat_pct = fmaxf(3.0f, fminf(60.0f, fat_pct));
    ffm = weight_kg * (1.0f - fat_pct / 100.0f);
  }

  const float water = FRAC_WATER * ffm;
  const float protein = FRAC_PROTEIN * ffm;
  const float bone = FRAC_BONE * ffm;
  const float smm = FRAC_SMM * ffm;
  const float bone_pct = 100.0f * bone / weight_kg;
  const float muscle_mass_pct = 100.0f - fat_pct - bone_pct;
  const float water_pct = 100.0f * water / weight_kg;
  const float protein_pct = 100.0f * protein / weight_kg;
  const float smm_pct = 100.0f * smm / weight_kg;

#ifdef USE_SENSOR
  if (z_valid) {  // impedance-based -> the real, trusted body-comp on the MAIN entities
    publish_(this->body_fat_sensor_, fat_pct);
    publish_(this->body_water_sensor_, water_pct);
    publish_(this->protein_sensor_, protein_pct);
    publish_(this->bone_mass_sensor_, bone_pct);
    publish_(this->muscle_mass_sensor_, muscle_mass_pct);
    publish_(this->skeletal_muscle_sensor_, smm_pct);
    publish_(this->fat_free_mass_sensor_, ffm);
  } else {  // no impedance -> BMI estimate goes to HIDDEN entities; main ones untouched
    publish_(this->body_fat_estimate_sensor_, fat_pct);
    publish_(this->body_water_estimate_sensor_, water_pct);
    publish_(this->protein_estimate_sensor_, protein_pct);
    publish_(this->bone_mass_estimate_sensor_, bone_pct);
    publish_(this->muscle_mass_estimate_sensor_, muscle_mass_pct);
    publish_(this->skeletal_muscle_estimate_sensor_, smm_pct);
    publish_(this->fat_free_mass_estimate_sensor_, ffm);
  }
#endif
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
