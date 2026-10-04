#pragma once

#include "esphome/core/component.h"
#include "esphome/components/button/button.h"
#include "esphome/components/cover/cover.h"
#include "esphome/components/text_sensor/text_sensor.h"
#ifdef USE_OTA
#include "esphome/components/ota/ota_backend.h"
#endif

#include <x2d/controller.h>
#include "radio.h"
#include "storage.h"

namespace esphome::x2d {

class X2DComponent;

class X2DCover : public cover::Cover {
 public:
  void set_parent(X2DComponent *parent) { parent_ = parent; }
  void set_slot(uint8_t slot) { slot_ = slot; }
  cover::CoverTraits get_traits() override;
  void set_assumed_position(float value);
 protected:
  void control(const cover::CoverCall &call) override;
  X2DComponent *parent_ = nullptr;
  uint8_t slot_ = 0;
};

class X2DButton : public button::Button {
 public:
  void set_parent(X2DComponent *parent) { parent_ = parent; }
  void set_confirm(bool confirm) { confirm_ = confirm; }
 protected:
  void press_action() override;
  X2DComponent *parent_ = nullptr;
  bool confirm_ = false;
};

class X2DComponent : public Component, public RadioBus
#ifdef USE_OTA
    , public ota::OTAGlobalStateListener
#endif
{
 public:
  X2DComponent() : journal_(flash_), radio_(*this), controller_(journal_, radio_, *this) {}
  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::DATA; }
  void set_transmit_enabled(bool enabled) { transmit_enabled_ = enabled; }
  void set_enrollment_enabled(bool enabled) { enrollment_enabled_ = enabled; }
  void set_chip_ns(uint32_t value) { chip_ns_ = value; }
  void set_status_sensor(text_sensor::TextSensor *sensor) { status_sensor_ = sensor; }
  void add_cover(X2DCover *cover, uint8_t slot);
  void command(uint8_t slot, ::x2d::Action action) { controller_.command(slot, action, millis()); }
  void associate();
  void confirm();
  uint32_t random_u32();
  void status(const char *message, uint8_t slot);
  void tx_result(const ::x2d::radio::TxEvent &event);
#ifdef USE_OTA
  void on_ota_global_state(ota::OTAState state, float progress, uint8_t error,
                           ota::OTAComponent *component) override;
#endif
 protected:
  void quiesce_(const char *reason);
  JournalFlash flash_;
  ::x2d::journal::Journal journal_;
  RadioOutput radio_;
  ::x2d::Controller<RadioOutput, X2DComponent> controller_;
  X2DCover *covers_[::x2d::MAX_SHUTTERS]{};
  text_sensor::TextSensor *status_sensor_ = nullptr;
  uint32_t chip_ns_ = 208500;
  bool transmit_enabled_ = false, enrollment_enabled_ = false;
  bool restarting_ = false;  // Once set, only the reboot may follow: never resume RF.
};

}  // namespace esphome::x2d
