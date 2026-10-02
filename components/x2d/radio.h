#pragma once

#include "esphome/components/spi/spi.h"
#include "esphome/core/gpio.h"
#include <cc1101.h>
#include <radio_runtime.h>
#include <driver/rmt_tx.h>

namespace esphome::x2d {

class RadioBus : public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW,
                                      spi::CLOCK_PHASE_LEADING, spi::DATA_RATE_1MHZ> {
 public:
  void set_data_pin(InternalGPIOPin *pin) { data_pin_ = pin; }
  void set_miso_pin(InternalGPIOPin *pin) { miso_pin_ = pin; }
  bool setup_bus();
  void begin_spi() { enable(); }
  void end_spi() { disable(); }
  void select(bool active) { cs_->digital_write(!active); }
  bool miso_high() { return miso_pin_->digital_read(); }
  uint8_t transfer(uint8_t value) { return transfer_byte(value); }
  uint32_t now_us() { return micros(); }
  void delay_us(uint32_t value) { delayMicroseconds(value); }
  void data_write(bool high) { data_pin_->digital_write(high); }
  void data_output(bool output);
  int data_gpio() const { return data_pin_->get_pin(); }
  void force_low();
 protected:
  InternalGPIOPin *data_pin_ = nullptr;
  InternalGPIOPin *miso_pin_ = nullptr;
};

class RadioOutput {
 public:
  explicit RadioOutput(RadioBus &bus) : bus_(bus), cc1101_(bus) {}
  bool setup(bool transmit_enabled);
  bool available() const { return available_ && cc1101_.configured(); }
  const char *error() const { return error_; }
  bool start_burst(const ha_x2d::radio::Waveform &wave, uint32_t chip_ns, uint32_t &started_ms);
  ha_x2d::radio::FrameState poll_burst(uint8_t &completed);
  void request_stop();
  void end_burst();
  void force_abort();
 private:
  struct State;
  static size_t encode_callback_(const void *, size_t, size_t, size_t free,
                                           rmt_symbol_word_t *symbols, bool *done, void *arg);
  static bool done_callback_(rmt_channel_handle_t, const rmt_tx_done_event_data_t *event, void *arg);
  bool fail_(const char *error);
  RadioBus &bus_;
  ha_x2d::cc1101::Driver<RadioBus> cc1101_;
  State *state_ = nullptr;  // Internal SRAM, lifetime is the component/firmware.
  rmt_channel_handle_t channel_ = nullptr;
  rmt_encoder_handle_t encoder_ = nullptr;
  const char *error_ = "not_initialized";
  bool available_ = false, enabled_ = false, active_ = false;
};

}  // namespace esphome::x2d
