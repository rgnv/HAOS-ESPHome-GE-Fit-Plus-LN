#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <cstdio>
#include <ctime>

namespace esphome::ge_scale {

// Values are frozen at capture, never recomputed with a later profile.
struct Recording {
  uint32_t sequence{0}, measurement{0}, timestamp{0}, uptime_ms{0};
  bool bia{false}, guest{false};
  // kg, lb, BMI, fat%, water%, protein%, bone%, muscle%, skeletal%, FFM kg,
  // whole-body ohms, then eight segmental ohms. Unavailable values are NaN.
  std::array<float, 19> metrics;
  Recording() { metrics.fill(NAN); }
  bool valid() const {
    if (!std::isfinite(metrics[0]) || metrics[0] < 5 || metrics[0] > 300) return false;
    if (timestamp && (timestamp < 1577836800 || timestamp > 4102444799u)) return false;
    for (float f : metrics) if (std::isinf(f)) return false;
    if (std::isfinite(metrics[3]) && (metrics[3] <= 0 || metrics[3] >= 100)) return false;
    if (!bia) for (size_t i = 10; i < metrics.size(); ++i) if (!std::isnan(metrics[i])) return false;
    if (bia && (!std::isfinite(metrics[10]) || metrics[10] < 100 || metrics[10] > 1200)) return false;
    for (size_t i = 11; i < metrics.size(); ++i)
      if (std::isfinite(metrics[i]) && (metrics[i] < 0 || metrics[i] > 5000)) return false;
    return true;
  }
};

struct RecordingQueue {
  static constexpr uint32_t MAX_RECORDS = 32;
  uint64_t epoch{0};
  uint32_t next{1}, head{0}, count{0}, capacity{16};
  std::array<Recording, MAX_RECORDS> records;

  bool valid_config() const { return epoch && next && capacity >= 1 && capacity <= MAX_RECORDS && count <= capacity && head < capacity; }

  const Recording *front() const { return valid_config() && count ? &records[head] : nullptr; }
  std::string id(uint32_t sequence) const {
    char out[32];
    snprintf(out, sizeof(out), "%016llx-%08x", static_cast<unsigned long long>(epoch), static_cast<unsigned>(sequence));
    return out;
  }

  // One atomic NVS blob contains both records and cursor; no multi-key transaction.
  // ponytail: snapshot writes are O(N), capped at 32 infrequent weigh-ins; use per-slot
  // journal only if capture frequency or retention grows substantially.
  std::vector<uint8_t> encode() const {
    std::vector<uint8_t> out;
    if (!valid_config()) return out;
    auto put = [&out](uint32_t x) { for (int i = 0; i < 4; ++i) out.push_back(x >> (8 * i)); };
    put(0x31455147);  // GQE1, schema version 1
    put(epoch); put(epoch >> 32); put(next); put(capacity); put(count);
    for (uint32_t i = 0; i < count; ++i) {
      const auto &r = records[(head + i) % capacity];
      put(r.sequence); put(r.measurement); put(r.timestamp); put(r.uptime_ms);
      put((r.bia ? 1u : 0u) | (r.guest ? 2u : 0u));
      for (float f : r.metrics) { uint32_t bits; static_assert(sizeof(f) == sizeof(bits));
        memcpy(&bits, &f, sizeof(bits)); put(bits); }
    }
    uint32_t crc = checksum(out.data(), out.size());
    put(crc);
    return out;
  }
  static uint32_t checksum(const uint8_t *p, size_t n) {
    uint32_t crc = ~0u;
    for (size_t i = 0; i < n; ++i) {
      crc ^= p[i];
      for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
  }
  bool decode(const std::vector<uint8_t> &in) {
    if (in.size() < 28 || in.size() > 28 + MAX_RECORDS * 96) return false;
    size_t pos = 0;
    auto get = [&]() { uint32_t x = 0; for (int i = 0; i < 4; ++i) x |= uint32_t(in[pos++]) << (8 * i); return x; };
    if (get() != 0x31455147) return false;
    auto q = std::make_unique<RecordingQueue>();
    q->epoch = get(); q->epoch |= uint64_t(get()) << 32; q->next = get(); q->capacity = get(); q->count = get();
    if (!q->epoch || !q->next || q->capacity < 1 || q->capacity > MAX_RECORDS ||
        q->count > q->capacity || in.size() != 28 + q->count * 96) return false;
    uint32_t previous = 0;
    for (uint32_t i = 0; i < q->count; ++i) {
      auto &r = q->records[i];
      r.sequence = get(); r.measurement = get(); r.timestamp = get(); r.uptime_ms = get();
      uint32_t flags = get();
      if (flags > 3 || r.sequence <= previous || r.sequence >= q->next ||
          !r.measurement || r.measurement > r.sequence) return false;
      previous = r.sequence; r.bia = flags & 1; r.guest = flags & 2;
      for (float &f : r.metrics) { uint32_t bits = get(); memcpy(&f, &bits, sizeof(f)); if (std::isinf(f)) return false; }
      if (!r.valid()) return false;
    }
    if (get() != checksum(in.data(), in.size() - 4)) return false;
    *this = *q;
    return true;
  }

  template<typename Save> bool append(Recording &r, Save save, bool retain = true) {
    if (!r.valid() || !valid_config() || (retain && count == capacity) || next >= 16777216) return false;  // never overwrite unacknowledged data
    auto q = std::make_unique<RecordingQueue>(*this);
    Recording candidate = r;
    candidate.sequence = q->next++;
    if (!candidate.measurement) candidate.measurement = candidate.sequence;
    if (retain) {
      q->records[(head + count) % capacity] = candidate;
      ++q->count;
    }
    if (candidate.measurement > candidate.sequence) return false;
    if (!save(q->encode())) return false;
    *this = *q;
    r = candidate;
    return true;
  }
  template<typename Save> bool acknowledge(const std::string &token, Save save) {
    if (!valid_config() || !front() || token != id(front()->sequence)) return false;
    auto q = std::make_unique<RecordingQueue>(*this);
    q->records[q->head] = Recording{};
    q->head = (head + 1) % capacity; --q->count;
    if (!save(q->encode())) return false;
    *this = *q;
    return true;
  }
  template<typename Save> bool response(const std::string &token, bool success, Save save) {
    return success && acknowledge(token, save);
  }
};

inline bool replayable(const Recording &r) {
  return !r.guest && r.valid();
}

inline bool replay_body_fat(const Recording &r) {
  return r.valid() && std::isfinite(r.metrics[3]) && r.metrics[3] > 0 && r.metrics[3] < 100;
}

inline std::string recording_json(const RecordingQueue &q, const Recording &r) {
  static const char *const names[] = {"weight_kg", "weight_lb", "bmi", "body_fat_percent",
    "body_water_percent", "protein_percent", "bone_mass_percent", "muscle_mass_percent",
    "skeletal_muscle_percent", "fat_free_mass_kg", "whole_body_impedance_ohm",
    "impedance_1", "impedance_2", "impedance_3", "impedance_4", "impedance_5", "impedance_6", "impedance_7", "impedance_8"};
  std::string out = "{\"schema\":1,\"record_id\":\"" + q.id(r.sequence) + "\",\"measurement_id\":\"" +
    q.id(r.measurement) + "\",\"source\":\"" + (r.bia ? "bia" : "estimate") +
    "\",\"subject\":\"" + (r.guest ? "guest" : "primary") + "\",\"timestamp\":";
  out += r.timestamp ? std::to_string(r.timestamp) : "null";
  out += ",\"uptime_ms\":" + std::to_string(r.uptime_ms);
  char value[48];
  for (size_t i = 0; i < r.metrics.size(); ++i) {
    out += ",\""; out += names[i]; out += "\":";
    if (std::isfinite(r.metrics[i])) { snprintf(value, sizeof(value), "%.9g", static_cast<double>(r.metrics[i])); out += value; }
    else out += "null";
  }
  return out + "}";
}
}  // namespace esphome::ge_scale
