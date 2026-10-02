#pragma once

#include <radio_runtime.h>

namespace esphome::x2d {

// This is a build-time, supervised trial authorization, not a qualified motor
// profile. An ordinary build has no authorization to generate RF identities.
struct PairingAuthorization {
  uint8_t slot = 0;
  uint8_t identity_suffix = 0;
  uint32_t expected_next_counter = 0;
};

// One main-loop owner. The same controller is exercised with the real core's
// MemoryFlash and a simulated radio in the host check; no ESPHome/API mocks.
template<class Radio, class Observer> class Controller {
 public:
  Controller(ha_x2d::journal::Journal &journal, Radio &radio, Observer &observer)
      : journal_(journal), radio_(radio), observer_(observer), runtime_(journal, radio, *this) {}

  bool begin(bool transmit, bool enrollment, uint32_t chip_ns, PairingAuthorization authorization = {}) {
    transmit_ = transmit;
    enrollment_ = transmit && enrollment;
    chip_ns_ = chip_ns;
    authorization_ = authorization;
    valid_ = journal_.open() != ha_x2d::journal::StorageState::corrupt;
    runtime_.set_enabled(valid_ && transmit_ && radio_.available(), enrollment_);
    status(!valid_ ? "storage_corrupt" : !transmit_ ? "transmission_disabled" :
           !radio_.available() ? "radio_unavailable" : pending_slot() ? "association_pending" : "ready");
    return valid_;
  }

  void tick(uint32_t now) {
    if (valid_ && (!paused_ || runtime_.active())) runtime_.tick(now);
  }
  bool paired(uint8_t slot) const {
    return valid_ && journal_.shutter(slot).state == ha_x2d::journal::SlotState::paired;
  }
  bool active() const { return runtime_.active(); }
  bool reboot_requested() const { return reboot_; }
  bool valid() const { return valid_; }

  bool command(uint8_t slot, ha_x2d::Action action, uint32_t now) {
    if (!admit() || !transmit_ || !radio_.available()) {
      if (valid_ && !paused_ && !reboot_) status("transmission_disabled", slot);
      return false;
    }
    return runtime_.submit(job(slot, action, false, now), now);
  }

  bool associate(uint32_t now) {
    if (!admit_enrollment()) return false;
    if (runtime_.active() || runtime_.pending()) return refuse("radio_busy");
    if (journal_.maintenance_due()) return refuse("storage_maintenance");
    uint8_t slot = pending_slot();
    if (slot == MULTIPLE_PENDING) return refuse("multiple_pending_associations");
    if (!slot) {
      for (uint8_t i = 1; i <= ha_x2d::MAX_SHUTTERS; ++i)
        if (journal_.shutter(i).state == ha_x2d::journal::SlotState::unused) { slot = i; break; }
    }
    if (!slot) return refuse("inventory_full");
    if (slot != authorization_.slot) return refuse("association_profile_unqualified", slot);
    if (journal_.shutter(slot).state == ha_x2d::journal::SlotState::unused) {
      if (authorization_.expected_next_counter != 0) return refuse("association_counter_mismatch", slot);
      ha_x2d::journal::NewController fresh;
      for (unsigned attempt = 0; attempt < 64; ++attempt) {
        fresh.identity = ((observer_.random_u32() & 0xFFFF) << 8) | authorization_.identity_suffix;
        if ((fresh.identity & 0xFFFF00) && !journal_.find_identity(fresh.identity)) break;
        fresh.identity = 0;
      }
      if (!fresh.identity) return refuse("identity_generation_failed", slot);
      fresh.generation = (uint64_t{observer_.random_u32()} << 32) | observer_.random_u32();
      if (!fresh.generation) return refuse("identity_generation_failed", slot);
      fresh.first_counter = 0;
      if (!stored(journal_.provision(slot, fresh), slot)) return false;
    }
    uint32_t next;
    if (!journal_.next_counter(slot, &next) || next != authorization_.expected_next_counter)
      return refuse("association_counter_mismatch", slot);
    if (!runtime_.submit(job(slot, ha_x2d::Action::none, true, now), now)) return false;
    status("associating", slot);
    return true;
  }

  bool confirm() {
    if (!admit_enrollment()) return false;
    if (runtime_.active() || runtime_.pending()) return refuse("radio_busy");
    if (journal_.maintenance_due()) return refuse("storage_maintenance");
    const uint8_t slot = pending_slot();
    if (!slot) return refuse("no_pending_association");
    if (slot == MULTIPLE_PENDING) return refuse("multiple_pending_associations");
    if (slot != authorization_.slot) return refuse("association_profile_unqualified", slot);
    uint32_t next;
    // Reservations prove that an attempt was prepared, never motor reception.
    // The button is the human's assertion of the observed motor response.
    if (!journal_.next_counter(slot, &next) || next < 2) return refuse("no_association_attempt", slot);
    if (!stored(journal_.confirm(slot), slot)) return false;
    reboot_ = true;
    pause();
    status("paired_restarting", slot);
    return true;
  }

  void pause() { paused_ = true; runtime_.disconnect(); }
  void resume() { if (!reboot_) paused_ = false; }

  // RadioRuntime hooks. It checks these again before reserving the counters.
  bool profile(const ha_x2d::TxJob &job, ha_x2d::radio::TxProfile &profile) {
    if (!valid_ || paused_ || !transmit_ || !radio_.available()) return false;
    if (job.enrollment) {
      uint32_t next;
      if (!enrollment_ || job.shutter_id != authorization_.slot ||
          !journal_.next_counter(job.shutter_id, &next) || next != authorization_.expected_next_counter)
        return false;
    }
    profile = {static_cast<uint8_t>(job.enrollment ? 24 : 25), chip_ns_, 2001};
    return true;
  }
  bool build(const ha_x2d::TxJob &job, const ha_x2d::journal::Reservation &reserved,
             uint8_t phase, ha_x2d::radio::Body &body) {
    return job.enrollment
        ? ha_x2d::radio::make_enrollment_body(reserved.identity, reserved.counter, phase, &body)
        : ha_x2d::radio::make_command_body(reserved.identity, job.action, reserved.counter, &body);
  }
  void report(const ha_x2d::radio::TxEvent &event) {
    if (event.storage_status == ha_x2d::journal::Status::corrupt ||
        event.storage_status == ha_x2d::journal::Status::io_error) valid_ = false;
    observer_.tx_result(event);
    status(event.job.enrollment && !strcmp(event.outcome, "emitted") ? "awaiting_confirmation" :
           event.error ? event.error : event.outcome, event.job.shutter_id);
  }

 private:
  static constexpr uint8_t MULTIPLE_PENDING = 255;
  uint8_t pending_slot() const {
    uint8_t result = 0;
    for (uint8_t slot = 1; slot <= ha_x2d::MAX_SHUTTERS; ++slot) {
      if (journal_.shutter(slot).state != ha_x2d::journal::SlotState::pending) continue;
      if (result) return MULTIPLE_PENDING;
      result = slot;
    }
    return result;
  }
  void status(const char *message, uint8_t slot = 0) { observer_.status(message, slot); }
  bool refuse(const char *message, uint8_t slot = 0) { status(message, slot); return false; }
  bool admit() {
    if (!valid_) return refuse("storage_corrupt");
    if (reboot_) return refuse("restarting");
    if (paused_) return refuse("update_in_progress");
    return true;
  }
  bool admit_enrollment() {
    if (!admit()) return false;
    if (!enrollment_) return refuse("association_disabled");
    if (!authorization_.slot) return refuse("association_profile_unqualified");
    if (!radio_.available()) return refuse("radio_unavailable");
    return true;
  }
  bool stored(ha_x2d::journal::Status result, uint8_t slot) {
    using ha_x2d::journal::Status;
    if (result == Status::ok) return true;
    if (result == Status::corrupt || result == Status::io_error) valid_ = false;
    return refuse(ha_x2d::journal::protocol_error(result), slot);
  }
  ha_x2d::TxJob job(uint8_t slot, ha_x2d::Action action, bool enrollment, uint32_t now) {
    if (!++request_id_) ++request_id_;
    ha_x2d::TxJob result{};
    result.request_id = request_id_;
    result.shutter_id = slot;
    result.action = action;
    result.enrollment = enrollment;
    result.deadline_ms = now + 3000;
    return result;
  }
  ha_x2d::journal::Journal &journal_;
  Radio &radio_;
  Observer &observer_;
  ha_x2d::radio::RadioRuntime<Radio, Controller> runtime_;
  PairingAuthorization authorization_{};
  uint32_t chip_ns_ = 208500, request_id_ = 0;
  bool transmit_ = false, enrollment_ = false, valid_ = false, paused_ = false, reboot_ = false;
};

}  // namespace esphome::x2d
