#pragma once

// Pure waveform encoder for the ESP32-S3 RMT backend (radio.cpp). It has no ESP-IDF or ESPHome includes, so the same
// code runs in the RMT ISR and in tests/check_radio_encoder.cpp.
//
// What the hardware path guarantees, and what it does not:
//  * One RMT transaction per burst, 10 MHz, kMemWords words of memory used as two ping-pong halves. The driver calls
//    encode() from the ISR whenever a half has been consumed. Words are never padded or reordered, so copies are
//    continuous as long as every refill arrives before the transmitter reaches the end of the valid data.
//  * The RMT has no underrun flag: a late refill sends stale memory. encode() therefore checks every refill against the
//    time the transmitter reaches the end of the valid data and ends the stream immediately when less than
//    kMinSlackUs remain (fault late). That detects, it does not prevent: data sent after a very late refill is lost.
//  * STOP is decided only when the encoder is about to take the first run of the next copy. Everything already in
//    the RMT memory (the prefetch) is still sent, so the burst ends at the first copy boundary that was not prefetched.
//  * Counts while busy are a lower bound (frames_lower_bound). Only a clean transaction end (judge_done() == ok) makes
//    frames_done() exact.

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "radio_codec.h"

#if defined(ESP_PLATFORM)
// Fold header helpers into the IRAM callbacks. Separate weak/COMDAT IRAM
// methods put Xtensa literal pools out of l32r range at link time.
#define X2D_ISR inline __attribute__((always_inline))
#else
#define X2D_ISR
#endif

namespace esphome::x2d {

constexpr uint32_t kRmtHz = 10000000;
constexpr uint32_t kTicksPerUs = kRmtHz / 1000000;
constexpr uint32_t kMemWords = 96;  // two 48-word ESP32-S3 blocks, 48-word halves
constexpr uint32_t kHalfWords = kMemWords / 2;
constexpr uint32_t kMaxHalfTicks = 0x7FFF;  // 15-bit RMT duration
constexpr uint32_t kMinSlackUs = 1000;      // refill must finish this long before the valid data runs out
constexpr uint32_t kDoneEarlyUs = 1000;     // transaction end earlier than expected by more than this: mismatch
constexpr uint32_t kDoneLateUs = 5000;      // later than expected by more than this: mismatch (stale data was sent)
constexpr uint32_t kWatchdogUs = 50000;     // no transaction end this long after the expected end: timeout
constexpr size_t kMaxRuns = ha_x2d::radio::MAX_CHIPS;  // a run is at least one chip
constexpr uint16_t kNoBoundary = 0xFFFF;
static_assert(kMaxRuns < kNoBoundary, "run indexes are 16 bit");

// Rounded chip duration in 10 MHz ticks; 0 when it cannot be sent (a 2-chip run must fit a 15-bit half-symbol and a
// half-symbol must be splittable). 208500 ns is exactly 2085 ticks.
inline uint16_t chip_ticks_from_ns(uint32_t chip_ns) {
  const uint64_t ticks = (uint64_t{chip_ns} * kRmtHz + 500000000ull) / 1000000000ull;
  return ticks < 2 || ticks > kMaxHalfTicks / 2 ? 0 : static_cast<uint16_t>(ticks);
}

// One RMT word: first half (level0, duration0) then second half (level1, duration1), same layout as rmt_symbol_word_t.
X2D_ISR constexpr uint32_t rmt_word(bool level0, uint32_t duration0, bool level1, uint32_t duration1) {
  return (duration0 & 0x7FFF) | (uint32_t{level0} << 15) | ((duration1 & 0x7FFF) << 16) | (uint32_t{level1} << 31);
}

// Burst as run lengths, built once by the main thread and then only read by the ISR. Runs never cross a copy
// boundary, so a copy always ends on a run boundary.
struct RadioPlan {
  uint8_t run[kMaxRuns];                               // bit 7 level, bits 0..6 chips
  uint16_t frame_end_run[ha_x2d::radio::MAX_COPIES];   // run index just after each copy
  uint16_t frame_end_word[ha_x2d::radio::MAX_COPIES];  // words that must be sent to complete each copy
  uint32_t total_ticks;
  uint16_t runs;
  uint16_t chip_ticks;
  uint8_t copies;
};

inline bool build_plan(const ha_x2d::radio::Waveform &wave, uint16_t chip_ticks, RadioPlan &plan) {
  using namespace ha_x2d::radio;
  const size_t chips = wave.chips();
  const uint8_t copies = wave.copies();
  if (chip_ticks < 2 || chip_ticks > kMaxHalfTicks / 2 || !copies || copies > MAX_COPIES || !chips || chips > MAX_CHIPS)
    return false;
  size_t previous = 0;
  for (uint8_t copy = 0; copy < copies; ++copy) {
    const size_t end = wave.frame_end(copy);
    if (end <= previous || end > chips)
      return false;
    previous = end;
  }
  if (previous != chips)
    return false;
  size_t cap = kMaxHalfTicks / chip_ticks;
  if (cap > 127)
    cap = 127;
  size_t runs = 0, i = 0;
  uint8_t copy = 0;
  uint32_t total = 0;
  while (i < chips) {
    const size_t end = wave.frame_end(copy);
    const bool level = wave.chip(i);
    size_t length = 1;
    while (i + length < end && length < cap && wave.chip(i + length) == level)
      ++length;
    plan.run[runs++] = static_cast<uint8_t>((level ? 0x80 : 0) | length);
    total += static_cast<uint32_t>(length) * chip_ticks;
    i += length;
    if (i == end) {
      plan.frame_end_run[copy] = static_cast<uint16_t>(runs);
      plan.frame_end_word[copy] = static_cast<uint16_t>((runs + 1) / 2);
      ++copy;
    }
  }
  plan.total_ticks = total;
  plan.runs = static_cast<uint16_t>(runs);
  plan.chip_ticks = chip_ticks;
  plan.copies = copies;
  return true;
}

// Whole copies that were certainly sent when "words" words have been encoded. The encoder never overwrites unread
// words, so at most kMemWords of them are still waiting and one more may be mid-send.
inline uint8_t frames_lower_bound(const RadioPlan &plan, uint32_t words) {
  if (words <= kMemWords + 1)
    return 0;
  const uint32_t sent = words - kMemWords - 1;
  uint8_t frames = 0;
  while (frames < plan.copies && plan.frame_end_word[frames] <= sent)
    ++frames;
  return frames;
}

enum class RadioFault : uint32_t { none = 0, late, termination, timeout, forced };
enum class RadioDone : uint8_t { ok, not_encoded, symbols, early, late };

struct EncodeResult {
  size_t words;
  bool done;
};

// Owner: main thread calls begin(), request_stop(), set_fault() and the getters. The RMT driver calls encode() (task
// context for the first call, ISR afterwards) and on_done() (ISR). Everything shared is a 32 bit atomic or is written
// before the release store that publishes it. Only loads and stores are used: no read-modify-write atomics in the ISR.
class RadioEncoder {
 public:
  void begin(const RadioPlan *plan) {
    this->plan_ = plan;
    this->stop_.store(0, std::memory_order_relaxed);
    this->fault_.store(0, std::memory_order_relaxed);
    this->published_.store(0, std::memory_order_relaxed);
    this->encoded_.store(0, std::memory_order_relaxed);
    this->finished_.store(0, std::memory_order_relaxed);
    this->words_ = 0;
    this->ticks_ = 0;
    this->valid_words_ = 0;
    this->pos_ = 0;
    this->frames_ = 0;
    this->boundary_ = plan->frame_end_run[0];
    this->start_us_ = 0;
    this->done_us_ = 0;
    this->num_symbols_ = 0;
  }

  void request_stop() { this->stop_.store(1, std::memory_order_release); }

  // Benign race with the ISR: whichever fault is stored first or last, the burst is a failure either way.
  X2D_ISR void set_fault(RadioFault fault) {
    if (this->fault_.load(std::memory_order_relaxed) == 0)
      this->fault_.store(static_cast<uint32_t>(fault), std::memory_order_release);
  }
  RadioFault fault() const { return static_cast<RadioFault>(this->fault_.load(std::memory_order_acquire)); }

  X2D_ISR int64_t refill_deadline_us() const {
    return this->words_ ? this->start_us_ + this->ticks_ / kTicksPerUs - kMinSlackUs : 0;
  }

  // Driver callback: write up to "free" words to "out". "now_us" is a monotonic microsecond clock; the first call
  // takes it as the time the transmitter starts (taken just before the hardware starts, so it is early by a few
  // microseconds and the lateness checks lean towards reporting a fault).
  X2D_ISR EncodeResult encode(uint32_t *out, size_t free, int64_t now_us) {
    const RadioPlan &plan = *this->plan_;
    if (this->words_ == 0) {
      this->start_us_ = now_us;
    } else if (now_us > this->start_us_ + static_cast<int64_t>(this->ticks_ / kTicksPerUs) -
                            static_cast<int64_t>(kMinSlackUs)) {
      // The transmitter is about to reach (or already passed) the end of the valid data. Put the end marker there
      // right now so it stops at the last valid word instead of replaying stale memory.
      this->valid_words_ = this->words_;
      this->set_fault(RadioFault::late);
      this->encoded_.store(1, std::memory_order_release);
      return {0, true};
    }
    size_t n = 0;
    bool done = false;
    while (n < free && !done) {
      uint8_t first, second;
      if (this->take_(first) != Step::run) {
        done = true;
        break;
      }
      const uint32_t d0 = static_cast<uint32_t>(first & 0x7F) * plan.chip_ticks;
      if (this->take_(second) == Step::run) {
        const uint32_t d1 = static_cast<uint32_t>(second & 0x7F) * plan.chip_ticks;
        out[n++] = rmt_word(first & 0x80, d0, second & 0x80, d1);
        this->ticks_ += d0 + d1;
      } else {
        // Odd run count at the end: send the last run as two halves of the same level.
        out[n++] = rmt_word(first & 0x80, d0 - d0 / 2, first & 0x80, d0 / 2);
        this->ticks_ += d0;
        done = true;
      }
    }
    if (!done && n == free && this->pos_ == plan.runs) {
      this->frames_ = plan.copies;
      this->boundary_ = kNoBoundary;
      done = true;
    }
    this->words_ += static_cast<uint32_t>(n);
    this->published_.store(this->words_, std::memory_order_release);
    if (done)
      this->encoded_.store(1, std::memory_order_release);
    return {n, done};
  }

  // Transaction-done ISR.
  X2D_ISR void on_done(int64_t now_us, uint32_t num_symbols) {
    this->done_us_ = now_us;
    this->num_symbols_ = num_symbols;
    this->finished_.store(1, std::memory_order_release);
  }

  bool transfer_done() const { return this->finished_.load(std::memory_order_acquire) != 0; }
  bool encoded() const { return this->encoded_.load(std::memory_order_acquire) != 0; }

  // The end of the transaction must match what was encoded: symbol count (+1 end marker) and elapsed time.
  RadioDone judge_done() const {
    if (!this->encoded())
      return RadioDone::not_encoded;
    if (this->num_symbols_ != this->words_ + 1)
      return RadioDone::symbols;
    const int64_t expected = this->ticks_ / kTicksPerUs;
    const int64_t elapsed = this->done_us_ - this->start_us_;
    if (elapsed + static_cast<int64_t>(kDoneEarlyUs) < expected)
      return RadioDone::early;
    if (elapsed > expected + static_cast<int64_t>(kDoneLateUs))
      return RadioDone::late;
    return RadioDone::ok;
  }

  bool watchdog_expired(int64_t now_us) const {
    return now_us - this->start_us_ > static_cast<int64_t>(this->plan_->total_ticks / kTicksPerUs) + kWatchdogUs;
  }

  // Exact only after a clean transaction end (judge_done() == ok); then it is the number of whole copies sent.
  uint8_t frames_done() const { return this->frames_; }
  // Valid at any time: a count that was certainly sent, ignoring data after a late refill.
  uint8_t lower_bound() const {
    const uint32_t words = this->fault() == RadioFault::late ? this->valid_words_
                                                              : this->published_.load(std::memory_order_acquire);
    return frames_lower_bound(*this->plan_, words);
  }

  int64_t start_us() const { return this->start_us_; }
  uint32_t words() const { return this->words_; }
  uint32_t ticks() const { return this->ticks_; }
  uint32_t valid_words() const { return this->valid_words_; }

 protected:
  enum class Step : uint8_t { run, end, stop };

  // Next run of the burst. A copy boundary is where STOP is decided; nothing already written is taken back.
  X2D_ISR Step take_(uint8_t &packed) {
    const RadioPlan &plan = *this->plan_;
    if (this->pos_ == this->boundary_) {
      ++this->frames_;
      if (this->frames_ < plan.copies) {
        this->boundary_ = plan.frame_end_run[this->frames_];
        if (this->stop_.load(std::memory_order_acquire) != 0)
          return Step::stop;
      } else {
        this->boundary_ = kNoBoundary;
      }
    }
    if (this->pos_ == plan.runs)
      return Step::end;
    packed = plan.run[this->pos_++];
    return Step::run;
  }

  const RadioPlan *plan_{nullptr};
  std::atomic<uint32_t> stop_{0}, fault_{0}, published_{0}, encoded_{0}, finished_{0};
  uint32_t words_{0}, ticks_{0}, valid_words_{0}, num_symbols_{0};
  uint16_t pos_{0}, boundary_{0};
  uint8_t frames_{0};
  int64_t start_us_{0}, done_us_{0};
};

}  // namespace esphome::x2d
