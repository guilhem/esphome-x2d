#include "radio.h"
#include "radio_encoder.h"

#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <esp_memory_utils.h>
#include <esp_private/rmt.h>
#include <esp_rom_gpio.h>
#include <esp_timer.h>
#include <hal/gpio_ll.h>
#include <soc/gpio_sig_map.h>
#include <new>

#if !CONFIG_IDF_TARGET_ESP32S3 || !CONFIG_RMT_TX_ISR_CACHE_SAFE
#error "X2D requires ESP32-S3 and CONFIG_RMT_TX_ISR_CACHE_SAFE"
#endif
static_assert(ESP_IDF_VERSION == ESP_IDF_VERSION_VAL(5, 5, 5), "X2D requires the pinned ESP-IDF driver API");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "ISR flags must be lock free");

namespace esphome::x2d {

void RadioBus::data_output(bool output) {
  if (output) {
    // gpio_set_direction(OUTPUT) resets the matrix to plain GPIO in IDF 5.5.5.
    // Change only output-enable here, preserving the RMT signal. INPUT below
    // keeps enable under GPIO control; force_low() explicitly detaches RMT.
    gpio_ll_output_enable(&GPIO, data_gpio());
  } else {
    data_pin_->pin_mode(gpio::FLAG_INPUT);
  }
}

bool RadioBus::setup_bus() {
  if (!data_pin_ || !miso_pin_ || !cs_) return false;
  // SRES may leave CC1101 GDO0 as an output. Do not call its output-pin setup.
  data_output(false);
  spi_setup();
  return delegate_->is_ready();
}

void RadioBus::force_low() {
  // Called only after the CC1101 async input has been verified. Detach RMT so
  // even an unresponsive peripheral/driver cannot keep a carrier asserted.
  gpio_set_level(static_cast<gpio_num_t>(data_gpio()), 0);
  esp_rom_gpio_connect_out_signal(data_gpio(), SIG_GPIO_OUT_IDX, false, false);
  data_output(true);
}

struct RadioOutput::State {
  RadioPlan plan{};
  RadioEncoder encoder;
};

bool RadioOutput::fail_(const char *error) {
  available_ = false;
  error_ = error;
  return false;
}

bool RadioOutput::setup(bool transmit_enabled) {
  if (!bus_.setup_bus()) return fail_("spi_unavailable");
  if (!cc1101_.reset() || !cc1101_.probe().detected) return fail_("cc1101_unavailable");
  error_ = "transmission_disabled";
  if (!transmit_enabled) return true;
  if (!cc1101_.configure_transmitter()) return fail_("cc1101_configuration_failed");
  ::x2d::cc1101::DigitalInput input{};
  if (!cc1101_.digital_input_verified(input)) return fail_("cc1101_data_pin_unverified");
  void *memory = heap_caps_malloc(sizeof(State), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!memory) return fail_("radio_memory_unavailable");
  state_ = new (memory) State{};

  rmt_tx_channel_config_t config{};
  config.gpio_num = static_cast<gpio_num_t>(bus_.data_gpio());
  config.clk_src = RMT_CLK_SRC_APB;
  config.resolution_hz = kRmtHz;
  config.mem_block_symbols = kMemWords;
  config.trans_queue_depth = 1;
  config.intr_priority = 3;
  config.flags.init_level = 0;
  if (rmt_new_tx_channel(&config, &channel_) != ESP_OK) return fail_("rmt_channel_unavailable");
  uint32_t resolution = 0;
  // This inspection API is internal to ESP-IDF; the exact IDF pin above is
  // intentional. Refuse silently rounded clocks rather than distorting X2D.
  if (rmt_get_channel_resolution(channel_, &resolution) != ESP_OK || resolution != kRmtHz)
    return fail_("rmt_clock_mismatch");
  rmt_simple_encoder_config_t encoder_config{};
  encoder_config.callback = encode_callback_;
  encoder_config.arg = state_;
  encoder_config.min_chunk_size = 1;
  if (!esp_ptr_in_iram(reinterpret_cast<const void *>(encode_callback_)) ||
      !esp_ptr_in_iram(reinterpret_cast<const void *>(done_callback_)) || !esp_ptr_internal(state_))
    return fail_("rmt_memory_not_cache_safe");
  if (rmt_new_simple_encoder(&encoder_config, &encoder_) != ESP_OK) return fail_("rmt_encoder_unavailable");
  rmt_tx_event_callbacks_t callbacks{};
  callbacks.on_trans_done = done_callback_;
  if (rmt_tx_register_event_callbacks(channel_, &callbacks, state_) != ESP_OK ||
      rmt_enable(channel_) != ESP_OK) return fail_("rmt_start_failed");
  enabled_ = true;
  // The pin is released while idle. begin_tx() takes it only after rechecking
  // the CC1101 configuration; GPIO direction changes preserve the RMT matrix.
  bus_.data_output(false);
  available_ = true;
  error_ = "none";
  return true;
}

size_t IRAM_ATTR RadioOutput::encode_callback_(const void *, size_t, size_t, size_t free,
                                              rmt_symbol_word_t *symbols, bool *done, void *arg) {
  auto *state = static_cast<State *>(arg);
  const int64_t deadline = state->encoder.refill_deadline_us();
  const auto result = state->encoder.encode(reinterpret_cast<uint32_t *>(symbols), free, esp_timer_get_time());
  *done = result.done;
  if (deadline && esp_timer_get_time() > deadline) {
    state->encoder.set_fault(RadioFault::late);
    *done = true;
  }
  return result.words;
}

bool IRAM_ATTR RadioOutput::done_callback_(rmt_channel_handle_t, const rmt_tx_done_event_data_t *event, void *arg) {
  static_cast<State *>(arg)->encoder.on_done(esp_timer_get_time(), event->num_symbols);
  return false;
}

bool RadioOutput::start_burst(const ::x2d::radio::Waveform &wave, uint32_t chip_ns, uint32_t &started_ms) {
  if (!available() || active_ || chip_ns % 100) return false;
  const auto ticks = chip_ticks_from_ns(chip_ns);
  if (!ticks || !build_plan(wave, ticks, state_->plan)) return false;
  state_->encoder.begin(&state_->plan);
  if (!cc1101_.begin_tx()) return fail_("cc1101_tx_failed");
  rmt_transmit_config_t config{};
  config.flags.queue_nonblocking = 1;
  config.flags.eot_level = 0;
  if (rmt_transmit(channel_, encoder_, &state_->plan, sizeof(state_->plan), &config) != ESP_OK) {
    if (cc1101_.idle()) cc1101_.release_data();
    return fail_("rmt_transmit_failed");
  }
  active_ = true;
  // With an idle, exclusive queue the driver's initial encode and start are
  // synchronous. The observation precedes the first chip by its initial fill.
  started_ms = static_cast<uint32_t>(state_->encoder.start_us() / 1000);
  return true;
}

::x2d::radio::FrameState RadioOutput::poll_burst(uint8_t &completed) {
  using ::x2d::radio::FrameState;
  completed = 0;
  if (!active_ || !state_) return FrameState::unknown;
  completed = state_->encoder.lower_bound();
  if (state_->encoder.fault() != RadioFault::none) {
    completed = 0;  // No exact count is defensible after an underrun.
    force_abort();
    return FrameState::unknown;
  }
  if (state_->encoder.transfer_done()) {
    if (state_->encoder.judge_done() != RadioDone::ok) {
      state_->encoder.set_fault(RadioFault::termination);
      force_abort();
      return FrameState::unknown;
    }
    completed = state_->encoder.frames_done();
    return FrameState::complete;
  }
  if (state_->encoder.watchdog_expired(esp_timer_get_time())) {
    state_->encoder.set_fault(RadioFault::timeout);
    force_abort();
    return FrameState::unknown;
  }
  return FrameState::busy;
}

void RadioOutput::request_stop() { if (active_) state_->encoder.request_stop(); }

void RadioOutput::force_abort() {
  if (!state_) return;
  state_->encoder.set_fault(RadioFault::forced);
  if (enabled_ && rmt_disable(channel_) == ESP_OK) enabled_ = false;
  bus_.force_low();
  cc1101_.hold_data_low();
  fail_("radio_timing_fault");  // Recovery requires a reboot, never a hidden retry.
}

void RadioOutput::end_burst() {
  if (!active_) return;
  if (enabled_ && rmt_tx_wait_all_done(channel_, 0) != ESP_OK) force_abort();
  const bool idle = cc1101_.idle();
  if (idle) cc1101_.release_data();
  else {
    bus_.force_low();
    cc1101_.hold_data_low();
    fail_("cc1101_idle_failed");
  }
  active_ = false;
}

}  // namespace esphome::x2d
