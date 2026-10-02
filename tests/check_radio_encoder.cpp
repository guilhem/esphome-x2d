#include "radio_encoder.h"
#include <cassert>
#include <cstdio>
#include <vector>

using namespace esphome::x2d;
using namespace ha_x2d::radio;

static uint32_t duration(uint32_t word) { return (word & 0x7FFF) + ((word >> 16) & 0x7FFF); }

// Model the driver's 96-word initial fill followed by 48-word consumed halves.
// Decode the resulting waveform independently of the encoder's run index.
static void transmission(const Waveform &wave, int stop_after_refill, RadioEncoder &encoder) {
  RadioPlan plan{};
  assert(build_plan(wave, chip_ticks_from_ns(208500), plan));
  encoder.begin(&plan);
  assert(encoder.fault() == RadioFault::none && !encoder.encoded() && !encoder.transfer_done());
  std::vector<uint32_t> output;
  uint32_t buffer[kMemWords];
  uint64_t consumed_ticks = 0;
  size_t consumed = 0;
  unsigned refill = 0;
  bool done = false;
  while (!done) {
    if (static_cast<int>(refill) == stop_after_refill) encoder.request_stop();
    auto result = encoder.encode(buffer, refill ? kHalfWords : kMemWords,
                                 1000000 + consumed_ticks / kTicksPerUs);
    assert(result.words || result.done);
    output.insert(output.end(), buffer, buffer + result.words);
    done = result.done;
    assert(encoder.fault() == RadioFault::none);
    if (!done) {
      for (size_t i = 0; i < kHalfWords; ++i) consumed_ticks += duration(output.at(consumed++));
      assert(encoder.lower_bound() <= wave.copies());
    }
    assert(++refill < 1000);
  }
  size_t chips = 0;
  uint64_t ticks = 0;
  bool level = false;
  uint32_t run_ticks = 0;
  auto finish_run = [&] {
    if (!run_ticks) return;
    assert(run_ticks % plan.chip_ticks == 0);
    for (uint32_t chip = 0; chip < run_ticks / plan.chip_ticks; ++chip) {
      assert(chips < wave.chips() && wave.chip(chips) == level);
      ++chips;
    }
    run_ticks = 0;
  };
  for (uint32_t word : output) {
    for (unsigned shift : {0u, 16u}) {
      const uint32_t half = word >> shift;
      const uint32_t d = half & 0x7FFF;
      assert(d > 0);
      const bool next = half & 0x8000;
      if (next != level) { finish_run(); level = next; }
      run_ticks += d;
      ticks += d;
    }
  }
  finish_run();
  assert(chips == wave.frame_end(encoder.frames_done() - 1));
  if (stop_after_refill < 0) assert(chips == wave.chips() && encoder.frames_done() == wave.copies());
  encoder.on_done(1000000 + ticks / kTicksPerUs, static_cast<uint32_t>(output.size() + 1));
  assert(encoder.transfer_done() && encoder.judge_done() == RadioDone::ok);
}

int main() {
  assert(chip_ticks_from_ns(208500) == 2085);
  assert(!chip_ticks_from_ns(1) && !chip_ticks_from_ns(4000000));
  RadioEncoder encoder;
  for (uint16_t counter : {0, 1, 2, 999, 65535}) {
    for (uint8_t phase : {0, 1, 2}) {
      Body body{};
      assert(phase < 2 ? make_enrollment_body(0x123456, counter, phase, &body)
                       : make_command_body(0x123456, ha_x2d::Action::stop, counter, &body));
      for (uint8_t copies : {24, 25}) {
        Waveform wave;
        assert(encode_burst(body, copies, &wave));
        transmission(wave, -1, encoder);
        // Every refill point, including before the first prefetch and after the
        // final copy, must terminate on a whole frame with unchanged prefix.
        for (int stop = 0; stop < 60; ++stop) transmission(wave, stop, encoder);
      }
    }
  }
  Body body{};
  Waveform wave;
  assert(make_body(0x123456, 0x04, 123, &body) && encode_burst(body, 25, &wave));
  RadioPlan plan{};
  assert(build_plan(wave, 2085, plan));
  encoder.begin(&plan);
  uint32_t words[kMemWords];
  auto first = encoder.encode(words, kMemWords, 1000000);
  assert(first.words == kMemWords && !first.done);
  auto on_time = encoder.encode(words, kHalfWords, encoder.refill_deadline_us());
  assert(on_time.words == kHalfWords && !on_time.done && encoder.fault() == RadioFault::none);
  auto late = encoder.encode(words, kHalfWords, encoder.refill_deadline_us() + 1);
  assert(late.done && !late.words && encoder.fault() == RadioFault::late);
  encoder.request_stop();
  encoder.on_done(1000000 + encoder.ticks() / kTicksPerUs, encoder.words() + 1);
  assert(encoder.encoded() && encoder.transfer_done());
  // A new burst clears the previous STOP, late fault and completion state.
  transmission(wave, -1, encoder);

  assert(encode_burst(body, 1, &wave) && build_plan(wave, 2085, plan));
  encoder.begin(&plan);
  encoder.encode(words, kMemWords, 1000000);
  assert(encoder.encoded());
  const int64_t expected = 1000000 + encoder.ticks() / kTicksPerUs;
  encoder.on_done(expected, encoder.words());
  assert(encoder.judge_done() == RadioDone::symbols);
  encoder.on_done(expected - kDoneEarlyUs - 1, encoder.words() + 1);
  assert(encoder.judge_done() == RadioDone::early);
  encoder.on_done(expected - kDoneEarlyUs, encoder.words() + 1);
  assert(encoder.judge_done() == RadioDone::ok);
  encoder.on_done(expected + kDoneLateUs, encoder.words() + 1);
  assert(encoder.judge_done() == RadioDone::ok);
  encoder.on_done(expected + kDoneLateUs + 1, encoder.words() + 1);
  assert(encoder.judge_done() == RadioDone::late);
  encoder.on_done(expected, encoder.words() + 1);
  assert(encoder.judge_done() == RadioDone::ok && encoder.frames_done() == 1);
  assert(!encoder.watchdog_expired(expected + kWatchdogUs));
  assert(encoder.watchdog_expired(expected + kWatchdogUs + 1));
  puts("encoder: waveform equivalence, every STOP prefetch boundary and timing-fault checks passed");
}
