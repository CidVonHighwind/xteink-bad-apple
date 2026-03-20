#pragma once

#include <cstdint>
#include <cstdlib>

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "input_types.h"

// ---- Button hardware configuration ----
//
// ADC1 channel 1 (GPIO1): 4 buttons via resistor ladder
//   index 0 → Button0 (Back),    threshold ≈ 3470
//   index 1 → Button1 (Confirm), threshold ≈ 2655
//   index 2 → Button2 (Left),    threshold ≈ 1470
//   index 3 → Button3 (Right),   threshold ≈ 3
//
// ADC1 channel 2 (GPIO2): 2 buttons via resistor ladder
//   index 0 → Up   (Vol+), threshold ≈ 2205
//   index 1 → Down (Vol-), threshold ≈ 3
//
// GPIO 3: Power button, digital active-LOW with internal pull-up

class Esp32InputSource final : public microreader::IInputSource {
 public:
  Esp32InputSource() {
    // Configure power button GPIO with internal pull-up
    gpio_config_t cfg{};
    cfg.pin_bit_mask = (1ULL << kPowerPin);
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&cfg);

    // Initialise ADC1 in oneshot mode, 12 dB attenuation (full 0–3.3 V range)
    adc_oneshot_unit_init_cfg_t unit_cfg{};
    unit_cfg.unit_id = ADC_UNIT_1;
    adc_oneshot_new_unit(&unit_cfg, &adc_handle_);

    adc_oneshot_chan_cfg_t ch_cfg{};
    ch_cfg.atten = ADC_ATTEN_DB_12;
    ch_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    adc_oneshot_config_channel(adc_handle_, ADC_CHANNEL_1, &ch_cfg);  // GPIO1
    adc_oneshot_config_channel(adc_handle_, ADC_CHANNEL_2, &ch_cfg);  // GPIO2
  }

  microreader::ButtonState poll_buttons() override {
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    const uint8_t raw = read_raw();

    // Per-button debounce (press side only; release is immediate).
    for (uint8_t i = 0; i < kNumButtons; ++i) {
      const uint8_t mask = static_cast<uint8_t>(1u << i);
      const bool raw_down = (raw & mask) != 0;
      const bool current_down = (debounced_ & mask) != 0;

      if (raw_down != last_raw_[i]) {
        last_raw_[i] = raw_down;
        debounce_time_[i] = now;
      }

      if (raw_down && !current_down) {
        if ((now - debounce_time_[i]) >= kDebounceMs)
          debounced_ |= mask;
      } else if (!raw_down && current_down) {
        debounced_ &= static_cast<uint8_t>(~mask);
      }
    }

    state_.update(debounced_);
    return state_;
  }

 private:
  static constexpr gpio_num_t kPowerPin = GPIO_NUM_3;
  static constexpr int kAdcTol = 400;     // ±tolerance for threshold match
  static constexpr int kAdcNoBtn = 3800;  // ADC value when no key pressed
  static constexpr uint8_t kNumButtons = 7;
  static constexpr uint32_t kDebounceMs = 5;

  static constexpr int kThresh1[4] = {3470, 2655, 1470, 3};  // Button0–3
  static constexpr int kThresh2[2] = {2205, 3};              // Up, Down

  adc_oneshot_unit_handle_t adc_handle_ = nullptr;
  microreader::ButtonState state_{};
  uint8_t debounced_ = 0;
  bool last_raw_[kNumButtons] = {};
  uint32_t debounce_time_[kNumButtons] = {};

  uint8_t read_raw() const {
    uint8_t state = 0;

    // ADC1_CH1 (GPIO1) → Button0..Button3 (bits 0–3)
    int adc1 = 0;
    adc_oneshot_read(adc_handle_, ADC_CHANNEL_1, &adc1);
    if (adc1 <= kAdcNoBtn) {
      for (int i = 0; i < 4; ++i) {
        if (abs(adc1 - kThresh1[i]) < kAdcTol) {
          state |= static_cast<uint8_t>(1u << i);
          break;
        }
      }
    }

    // ADC1_CH2 (GPIO2) → Up (bit 4), Down (bit 5)
    int adc2 = 0;
    adc_oneshot_read(adc_handle_, ADC_CHANNEL_2, &adc2);
    if (adc2 <= kAdcNoBtn) {
      for (int i = 0; i < 2; ++i) {
        if (abs(adc2 - kThresh2[i]) < kAdcTol) {
          state |= static_cast<uint8_t>(1u << (i + 4));
          break;
        }
      }
    }

    // GPIO3, active LOW → Power (bit 6)
    if (gpio_get_level(kPowerPin) == 0)
      state |= static_cast<uint8_t>(1u << 6);

    return state;
  }
};
