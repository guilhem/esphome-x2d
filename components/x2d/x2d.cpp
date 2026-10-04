#include "x2d.h"
#include "enrollment_profile.h"

#include "esphome/core/application.h"
#include "esphome/components/api/api_server.h"
#include <esp_random.h>
#include <cmath>
#include <cstdio>

namespace esphome::x2d {
static const char *const TAG = "x2d";

cover::CoverTraits X2DCover::get_traits() {
  cover::CoverTraits traits;
  traits.set_is_assumed_state(true);
  traits.set_supports_stop(true);
  traits.set_supports_position(false);
  traits.set_supports_tilt(false);
  return traits;
}

void X2DCover::set_assumed_position(float value) {
  position = value;
  current_operation = cover::COVER_OPERATION_IDLE;
  publish_state(false);  // A command is not a measured state to restore from NVS.
}

void X2DCover::control(const cover::CoverCall &call) {
  if (!parent_) return;
  if (call.get_stop()) parent_->command(slot_, ::x2d::Action::stop);
  else if (call.get_position().has_value()) {
    const auto position = *call.get_position();
    if (position == cover::COVER_OPEN) parent_->command(slot_, ::x2d::Action::open);
    else if (position == cover::COVER_CLOSED) parent_->command(slot_, ::x2d::Action::close);
  }
}

void X2DButton::press_action() {
  if (!parent_) return;
  if (confirm_) parent_->confirm();
  else parent_->associate();
}

void X2DComponent::add_cover(X2DCover *cover, uint8_t slot) {
  if (!cover || slot < 1 || slot > ::x2d::MAX_SHUTTERS) return;
  covers_[slot - 1] = cover;
  cover->set_parent(this);
  cover->set_slot(slot);
}

void X2DComponent::setup() {
  const bool storage_found = flash_.open();
  const bool radio_ok = radio_.setup(storage_found && transmit_enabled_);
  const bool storage_ok = controller_.begin(transmit_enabled_, enrollment_enabled_, chip_ns_,
                                           enrollment_profile());
  for (uint8_t slot = 1; slot <= ::x2d::MAX_SHUTTERS; ++slot) {
    if (!covers_[slot - 1]) continue;
    covers_[slot - 1]->set_internal(!controller_.paired(slot));
    covers_[slot - 1]->set_assumed_position(NAN);
  }
  if (!storage_ok) {
    // Before AFTER_WIFI: a failed API never sends a misleading empty inventory.
    // HA retains the previously discovered entities, now unavailable.
    api::global_api_server->mark_failed();
    ESP_LOGE(TAG, "Journal unavailable or corrupt; RF and native API disabled. No automatic reset.");
    mark_failed();
    return;
  }
  if (!radio_ok) {
    ESP_LOGE(TAG, "Radio unavailable: %s", radio_.error());
    status_set_warning("Radio unavailable");
  }
#ifdef USE_OTA
  ota::get_global_ota_callback()->add_global_state_listener(this);
#endif
}

void X2DComponent::loop() { controller_.tick(millis()); }

void X2DComponent::associate() {
  const uint8_t slot = controller_.pending_slot();
  if (slot) controller_.retry(slot, millis());
  else controller_.associate(millis());
}

void X2DComponent::confirm() {
  // The core only records the association. A new entity needs a reboot, so the
  // adapter pauses RF and owns the restart; the controller refuses until then.
  const uint8_t slot = controller_.pending_slot();
  if (!controller_.confirm(slot)) return;
  restarting_ = true;
  controller_.pause("restarting");
  status("paired_restarting", slot);
  set_timeout(200, [] { App.safe_reboot(); });
}

uint32_t X2DComponent::random_u32() { return esp_random(); }

void X2DComponent::status(const char *message, uint8_t slot) {
  char text[112];
  if (slot) snprintf(text, sizeof(text), "shutter %u: %s", slot, message);
  else snprintf(text, sizeof(text), "%s", message);
  ESP_LOGI(TAG, "%s", text);  // Never export the radio identity, suffix or journal.
  if (status_sensor_) status_sensor_->publish_state(text);
}

void X2DComponent::tx_result(const ::x2d::radio::TxEvent &event) {
  if (event.job.enrollment || !event.job.shutter_id || event.job.shutter_id > ::x2d::MAX_SHUTTERS) return;
  auto *cover = covers_[event.job.shutter_id - 1];
  if (!cover) return;
  if (!strcmp(event.outcome, "emitted")) {
    cover->set_assumed_position(event.job.action == ::x2d::Action::open ? cover::COVER_OPEN :
                               event.job.action == ::x2d::Action::close ? cover::COVER_CLOSED : NAN);
  } else if (strcmp(event.outcome, "rejected")) {
    cover->set_assumed_position(NAN);
  }
}

void X2DComponent::quiesce_(const char *reason) {
  controller_.pause(reason);
  const uint32_t start = millis();
  while (controller_.active() && millis() - start < 250) {
    controller_.tick(millis());
    App.feed_wdt();
    delay(1);
  }
  if (controller_.active()) {
    radio_.force_abort();
    controller_.tick(millis());  // Reports unknown, keeps all reservations consumed.
  }
}

void X2DComponent::on_shutdown() {
  restarting_ = true;
  quiesce_("restarting");
}

#ifdef USE_OTA
void X2DComponent::on_ota_global_state(ota::OTAState state, float, uint8_t, ota::OTAComponent *) {
  if (state == ota::OTA_STARTED) {
    quiesce_("update_in_progress");  // Native OTA invokes this synchronously before backend.begin().
    status("update_in_progress", 0);
  } else if (state == ota::OTA_ABORT || state == ota::OTA_ERROR) {
    if (!restarting_) controller_.resume();
    status("update_failed", 0);
  }
}
#endif

void X2DComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "X2D experimental connector, ESP32-S3 / CC1101, 16 slots");
  ESP_LOGCONFIG(TAG, "  Chip duration: %u ns", static_cast<unsigned>(chip_ns_));
  ESP_LOGCONFIG(TAG, "  Transmission: %s; association: %s", YESNO(transmit_enabled_), YESNO(enrollment_enabled_));
  ESP_LOGCONFIG(TAG, "  Journal: 64 KiB at 0x%08X", static_cast<unsigned>(X2D_JOURNAL_ADDRESS));
}

}  // namespace esphome::x2d
