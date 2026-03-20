#include <cstring>
#include <string>

#include "ball_demo.h"
#include "demo.h"
#include "epd.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input.h"
#include "logger.h"
#include "sdcard.h"
#include "serial_lut.h"
#include "video_player.h"

extern "C" void app_main(void) {
  serial_lut_start();

  static Esp32Logger logger;
  static Esp32InputSource input;
  static EInkDisplay epd;

  // Wait for the serial monitor to connect before logging anything.
  vTaskDelay(pdMS_TO_TICKS(2000));

  epd.begin();

  // ---- SD card init ----
  const bool sd_ok = sd_init();
  const int kTotalFrames = sd_ok ? sd_count_frames() : 0;
  if (sd_ok)
    logger.log(microreader::LogLevel::Info, "SD OK  frames: " + std::to_string(kTotalFrames));
  else
    logger.log(microreader::LogLevel::Info, "SD mount failed — ball demo only");

  // ---- Demos ----
  static BallDemo ballDemo;
  static VideoPlayer videoPlayer(kTotalFrames);

  static constexpr int kNumDemos = 2;
  IDemo* demos[kNumDemos] = {&ballDemo, &videoPlayer};
  int currentDemo = (kTotalFrames > 0) ? 1 : 0;
  IDemo* demo = demos[currentDemo];

  static constexpr int kCustomLutRefreshCount = 8;

  bool playing = true;

  // ---- Pending refresh state + helpers ----
  int pending_refreshes = 0;

  auto redraw = [&]() {
    demo->show(epd, logger, FAST_REFRESH);
    pending_refreshes = kCustomLutRefreshCount;
  };

  // Turn the screen on with a HALF_REFRESH before loading the custom LUT
  // (HALF_REFRESH reloads OTP LUT, so custom LUT must come after).
  logger.log(microreader::LogLevel::Info, demo->helpText());
  epd.clearScreen(0xFF);
  epd.displayBuffer(HALF_REFRESH);
  demo->activate(epd);
  epd.setCustomLUT(true);
  redraw();

  while (true) {
    const int64_t loop_start = esp_timer_get_time();

    static uint8_t lut_buf[112];
    if (serial_lut_take(lut_buf)) {
      epd.setCustomLUT(true, lut_buf);
      redraw();
      logger.log(microreader::LogLevel::Info, "LUT applied");
    }

    const microreader::ButtonState state = input.poll_buttons();

    // Button0: play/pause toggle
    if (state.is_pressed(microreader::Button::Button0)) {
      playing = !playing;
      if (!playing) {
        pending_refreshes = kCustomLutRefreshCount;
      }
      logger.log(microreader::LogLevel::Info, playing ? "playing" : "paused");
    }

    // Button1: restart
    if (state.is_pressed(microreader::Button::Button1)) {
      playing = false;
      demo->reset();
      redraw();
      logger.log(microreader::LogLevel::Info, "restart");
    }

    // Button2: switch demo
    if (state.is_pressed(microreader::Button::Button2)) {
      playing = false;
      currentDemo = (currentDemo + 1) % kNumDemos;
      demo = demos[currentDemo];
      demo->activate(epd);
      logger.log(microreader::LogLevel::Info, demo->helpText());
      redraw();
    }

    // Button3: (unused)
    if (state.is_pressed(microreader::Button::Button3)) {}

    // Up: forward
    if (!playing && state.is_pressed(microreader::Button::Up)) {
      demo->forward();
      redraw();
    }

    // Down: backward
    if (!playing && state.is_pressed(microreader::Button::Down)) {
      demo->backward();
      redraw();
    }

    // Auto-advance
    if (playing) {
      demo->forward();
      demo->show(epd, logger, FAST_REFRESH);
      if (demo->isDone()) {
        playing = false;
        pending_refreshes = kCustomLutRefreshCount;
        logger.log(microreader::LogLevel::Info, "done");
      }
    }

    // Cleanup refresh
    if (pending_refreshes > 0) {
      const int64_t t_refresh0 = esp_timer_get_time();
      epd.refreshDisplay(FAST_REFRESH, false);
      const int64_t t_refresh_ms = (esp_timer_get_time() - t_refresh0) / 1000;
      --pending_refreshes;
      logger.log(microreader::LogLevel::Info,
                 "refresh: " + std::to_string(t_refresh_ms) + " ms  remaining: " + std::to_string(pending_refreshes));
    }

    constexpr int64_t kIdleLoopMs = 20;
    const int64_t elapsed_ms = (esp_timer_get_time() - loop_start) / 1000;
    const int64_t target_ms = playing ? demo->targetFrameMs() : kIdleLoopMs;
    if (elapsed_ms < target_ms)
      vTaskDelay(pdMS_TO_TICKS(target_ms - elapsed_ms));
  }
}
