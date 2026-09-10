#include "components/ge_scale/recording.h"
#include <cassert>
#include <iostream>
using namespace esphome::ge_scale;
int main() {
  RecordingQueue q;
  q.epoch = 42; q.capacity = 2;
  std::vector<uint8_t> disk;
  unsigned writes = 0;
  auto save = [&](const auto &b) { disk = b; ++writes; return true; };
  auto fail = [](const auto &) { return false; };
  Recording a; a.metrics[0] = 80; a.metrics[1] = 176;
  assert(q.append(a, save));
  const auto first = q.id(a.sequence);
  Recording b; b.metrics[0] = 80; b.bia = true; b.measurement = a.measurement;
  for (size_t i = 2; i < b.metrics.size(); ++i) b.metrics[i] = 123.25f;
  b.metrics[3] = 25;
  assert(q.append(b, save));
  assert(a.measurement == b.measurement && a.sequence != b.sequence);
  Recording c; c.metrics[0] = 81;
  assert(!q.append(c, save)); assert(writes == 2);
  assert(!q.acknowledge(q.id(b.sequence), save));
  assert(!q.acknowledge(first, fail)); assert(q.count == 2);
  RecordingQueue reboot;
  assert(reboot.decode(disk)); assert(reboot.id(reboot.front()->sequence) == first);
  assert(recording_json(q, *q.front()) == recording_json(reboot, *reboot.front()));
  assert(recording_json(q, *q.front()).find("\"whole_body_impedance_ohm\":null") != std::string::npos);
  assert(recording_json(q, *q.front()).find("\"timestamp\":null") != std::string::npos);
  for (size_t i = 0; i < disk.size(); ++i) {
    auto corrupt = disk; corrupt[i] ^= 1; assert(!reboot.decode(corrupt));
    auto truncated = std::vector<uint8_t>(disk.begin(), disk.begin() + i);
    assert(!reboot.decode(truncated));
  }
  assert(q.acknowledge(first, save)); assert(!q.acknowledge(first, save));
  assert(!q.append(c, fail)); assert(q.count == 1 && q.next == 3);
  assert(c.sequence == 0 && c.measurement == 0);
  assert(q.append(c, save));  // wraps physical tail
  assert(reboot.decode(disk)); assert(reboot.front()->sequence == b.sequence);
  for (size_t i = 0; i < b.metrics.size(); ++i)
    assert(reboot.records[0].metrics[i] == b.metrics[i] ||
           (std::isnan(reboot.records[0].metrics[i]) && std::isnan(b.metrics[i])));
  assert(q.acknowledge(q.id(b.sequence), save));
  assert(q.acknowledge(q.id(c.sequence), save));
  assert(reboot.decode(disk)); assert(!reboot.front()); assert(reboot.next == 4);
  for (unsigned i = 0; i < 100; ++i) {
    Recording r; r.metrics[0] = 90;
    assert(q.append(r, save)); assert(q.acknowledge(q.id(r.sequence), save));
  }
  // Online capture persists identity even when offline queue is full, without replay.
  q.head = 0; q.capacity = 1;
  Recording online; online.metrics[0] = 80;
  assert(q.append(online, save));
  auto previous_id = online.sequence;
  Recording repeated; repeated.metrics[0] = 80;
  assert(q.append(repeated, save, false));
  assert(q.count == 1 && repeated.measurement > previous_id);
  assert(reboot.decode(disk) && reboot.next == q.next);
  auto before = q.encode();
  auto token = q.id(q.front()->sequence);
  assert(!q.response(token, false, save));
  assert(q.encode() == before);
  assert(!q.response("wrong", true, save));
  assert(!q.response(token, true, fail));
  assert(q.encode() == before);
  assert(q.response(token, true, save));
  assert(!q.response(token, true, save));  // late/duplicate response
  for (uint32_t capacity : {0u, 33u, UINT32_MAX}) {
    q.capacity = capacity;
    assert(!q.append(c, save));
    assert(!q.acknowledge(token, save));
  }
  q.capacity = 32;
  Recording nullable; nullable.metrics[0] = 80;
  assert(nullable.valid() && replayable(nullable) && !replay_body_fat(nullable));
  nullable.timestamp = 1788825600;
  assert(replayable(nullable));
  nullable.metrics[3] = 24;
  assert(replay_body_fat(nullable));  // valid recorded estimate is supported
  Recording scale_metrics;
  scale_metrics.metrics[0] = 80;
  scale_metrics.metrics[2] = 22;
  scale_metrics.metrics[3] = 24;
  scale_metrics.scale_metrics = true;
  assert(scale_metrics.valid());  // direct 0x02/0xfe burst has no whole-body Z field
  assert(recording_json(q, scale_metrics).find("\"source\":\"scale\"") != std::string::npos);
  nullable.bia = true;
  assert(!replay_body_fat(nullable));  // missing impedance cannot support BIA
  nullable.metrics[10] = 400;
  assert(replay_body_fat(nullable));
  nullable.metrics[3] = NAN;
  assert(!replay_body_fat(nullable));
  nullable.guest = true;
  assert(!replayable(nullable));
  nullable.metrics[0] = NAN;
  assert(!q.append(nullable, save));
  assert(!q.append(nullable, save, false));
  nullable.metrics[0] = 80;
  nullable.bia = false;
  nullable.metrics[10] = 400;
  assert(!q.append(nullable, save));  // cannot fabricate impedance
  for (unsigned i = 0; i < 32; ++i) {
    Recording r; r.metrics[0] = 80;
    assert(q.append(r, save));
  }
  assert(disk.size() == 3100 && !q.append(c, save));
  assert(reboot.decode(disk));
  before = reboot.encode();
  auto malformed = disk;
  malformed[16] = 33;  // valid checksum, invalid capacity
  auto crc = RecordingQueue::checksum(malformed.data(), malformed.size() - 4);
  for (int i = 0; i < 4; ++i) malformed[malformed.size() - 4 + i] = crc >> (8 * i);
  assert(!reboot.decode(malformed) && reboot.encode() == before);
  q.next = 16777215;
  Recording last; last.metrics[0] = 80;
  assert(q.append(last, save, false) && last.measurement == 16777215);
  assert(!q.append(c, save, false));  // numeric sensor would lose integer precision
  q.next = UINT32_MAX; assert(!q.append(c, save, false));
  std::cout << "recording serialization, corruption, FIFO, wrap, ACK and crash tests passed\n";
}
