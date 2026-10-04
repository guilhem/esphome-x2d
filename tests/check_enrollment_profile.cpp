#include "enrollment_profile.h"
#include <cassert>
#include <cstdio>
#include <cstring>

namespace {
struct Radio {
  x2d::radio::Waveform emitted;
  uint32_t now = 0, starts = 0;
  bool running = false;
  bool available() const { return true; }
  bool start_burst(const x2d::radio::Waveform &wave, uint32_t, uint32_t &started) {
    assert(!running);
    emitted = wave;
    running = true;
    started = now;
    ++starts;
    return true;
  }
  x2d::radio::FrameState poll_burst(uint8_t &completed) {
    assert(running);
    completed = emitted.copies();
    running = false;
    return x2d::radio::FrameState::complete;
  }
  void request_stop() {}
  void end_burst() { assert(!running); }
};
struct Observer {
  const char *last = "";
  unsigned emitted = 0;
  uint32_t random_u32() { return 123; }
  void status(const char *message, uint8_t) { last = message; }
  void tx_result(const x2d::radio::TxEvent &event) {
    if (event.job.request_id && !strcmp(event.outcome, "emitted")) ++emitted;
  }
};

void mismatched_profile_blocks_enrollment_only() {
  using namespace x2d::journal;
  const auto profile = esphome::x2d::enrollment_profile();
  const uint8_t stored_suffix = profile.identity_suffix ^ 1;
  MemoryFlash flash;
  Journal journal(flash);
  assert(journal.open() == StorageState::empty);
  assert(journal.initialize(123, 456, stored_suffix) == Status::ok);
  while (journal.state() == StorageState::initializing) assert(journal.maintain() == Status::ok);
  assert(journal.enrollment_profile_matches(stored_suffix));
  assert(!journal.enrollment_profile_matches(profile.identity_suffix));

  Radio radio;
  Observer observer;
  x2d::Controller<Radio, Observer> enrollment(journal, radio, observer);
  assert(enrollment.begin(true, true, 208500, profile));
  const auto programs = flash.programs(), erases = flash.erases();
  assert(!enrollment.associate(0));
  assert(!strcmp(observer.last, "association_profile_unqualified"));
  assert(!enrollment.pending_slot() && !enrollment.busy());
  assert(journal.shutter(1).state == SlotState::unused);
  uint32_t identity = 0;
  assert(!journal.identity(1, &identity, true));
  assert(flash.programs() == programs && flash.erases() == erases && !radio.starts);

  // A binding made under the stored profile remains usable after a build change.
  assert(journal.allocate_candidate(1, false) == Status::ok);
  assert(journal.shutter(1).logical_id == 2); // rejected enrollment burnt no ID
  assert(journal.claim_attempt(1, false) == Status::ok);
  const auto epoch = journal.incarnation(1, true);
  Reservation reserved;
  assert(journal.reserve(1, 0, false, &reserved, epoch, true) == Status::ok);
  assert(journal.reserve(1, 0, false, &reserved, epoch, true) == Status::ok);
  const auto retry_programs = flash.programs();
  assert(!enrollment.retry(1, 0));
  assert(!strcmp(observer.last, "association_profile_unqualified"));
  uint32_t next = 0;
  assert(journal.next_counter(1, &next, true) && next == 2);
  assert(journal.shutter(1).attempts == 1);
  assert(flash.programs() == retry_programs && flash.erases() == erases && !radio.starts);
  assert(enrollment.confirm(1));
  assert(journal.identity(1, &identity) && (identity & 255) == stored_suffix);

  Journal reboot(flash);
  x2d::Controller<Radio, Observer> commands(reboot, radio, observer);
  assert(commands.begin(true, false, 208500, profile));
  assert(!reboot.enrollment_profile_matches(profile.identity_suffix));
  assert(commands.paired(1) && reboot.shutter(1).in_service);
  assert(!commands.busy() && !radio.starts); // boot never replays
  assert(commands.command(1, x2d::Action::open, 0));
  for (uint32_t now = 0; commands.busy() && now < 100; ++now) {
    radio.now = now;
    commands.tick(now);
  }
  assert(!commands.busy() && radio.starts == 1 && observer.emitted == 1);
  assert(reboot.next_counter(1, &next) && next == 3);
  uint32_t restored = 0;
  assert(reboot.identity(1, &restored) && restored == identity);
  assert(reboot.incarnation(1) == epoch && !commands.pending_slot());
  x2d::radio::Body body;
  x2d::radio::Waveform expected;
  assert(x2d::radio::make_command_body(identity, x2d::Action::open, 2, &body));
  assert(x2d::radio::encode_burst(body, 25, &expected));
  assert(radio.emitted.chips() == expected.chips());
  assert(!memcmp(radio.emitted.packed(), expected.packed(), expected.packed_bytes()));
}
}  // namespace

// Use the actual consumer profile with the actual journal, including suffix 0.
int main() {
  const auto profile = esphome::x2d::enrollment_profile();
  static_assert(esphome::x2d::enrollment_profile().identity_suffix == X2D_TRIAL_IDENTITY_SUFFIX);
  x2d::journal::MemoryFlash flash;
  x2d::journal::Journal journal(flash);
  assert(journal.open() == x2d::journal::StorageState::empty);
  assert(journal.initialize(123, 456, profile.identity_suffix) == x2d::journal::Status::ok);
  while (journal.state() == x2d::journal::StorageState::initializing)
    assert(journal.maintain() == x2d::journal::Status::ok);
  assert(journal.allocate_candidate(1, false) == x2d::journal::Status::ok);
  uint32_t identity = 0;
  assert(journal.identity(1, &identity, true));
  assert((identity & 255) == profile.identity_suffix);
  x2d::journal::Journal reboot(flash);
  assert(reboot.open() == x2d::journal::StorageState::ready);
  uint32_t restored = 0;
  assert(reboot.identity(1, &restored, true) && restored == identity);
  assert(reboot.cancel_candidate(1) == x2d::journal::Status::ok);
  assert(reboot.allocate_candidate(2, false) == x2d::journal::Status::ok);
  assert(reboot.identity(2, &restored, true) && restored != identity);
  assert((restored & 255) == profile.identity_suffix);
  mismatched_profile_blocks_enrollment_only();
  puts("consumer profile: persistence, mismatch admission and stored commands-only waveform passed");
}
