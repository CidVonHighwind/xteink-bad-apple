#pragma once

#include <cmath>
#include <cstring>
#include <string>

#include "demo.h"
#include "esp_timer.h"

struct Ball {
  int x, y;    // centre position
  int dx, dy;  // direction: each ±1
  int radius;
  int speed;
};

class BallDemo : public IDemo {
 public:
  static constexpr int kNumBalls = 20;

  BallDemo() {
    reset();
  }

  void activate(EInkDisplay& epd) override {
    memset(epd.frameBufferActive, 0x00, EInkDisplay::BUFFER_SIZE);
    epd.writeRedRam(epd.frameBufferActive, EInkDisplay::BUFFER_SIZE);
  }

  void reset() override {
    memcpy(balls_, kInitial_, sizeof(balls_));
    bar_x_ = -kBarWidth;
    bar2_y_ = -kBarWidth;  // just off top-left, same timing as bar1
  }
  void forward() override {
    step(1);
  }
  void backward() override {
    step(-1);
  }

  RefreshMode show(EInkDisplay& epd, microreader::ILogger& logger, RefreshMode mode) override {
    epd.clearScreen(0xFF);
    for (int i = 0; i < kNumBalls; ++i)
      drawFilledCircle(epd.frameBuffer, balls_[i].x, balls_[i].y, balls_[i].radius, true);

    drawBar(epd.frameBuffer, bar_x_, true);
    drawBarV(epd.frameBuffer, bar2_y_, true);

    const int64_t t0 = esp_timer_get_time();
    mode = epd.writeBuffers(mode);
    const int64_t t_ms = (esp_timer_get_time() - t0) / 1000;
    logger.log(microreader::LogLevel::Info, "upload: " + std::to_string(t_ms) + " ms");

    epd.refreshDisplay(mode, false);
    return mode;
  }

  bool isDone() const override {
    return false;
  }
  const char* helpText() const override {
    return "BALLS  Up=fwd   Dn=back  B0=auto        B1=restart  B2=mode  B3=video";
  }

 private:
  Ball balls_[kNumBalls];
  int bar_x_ = 0;
  int bar2_y_ = 0;

  static constexpr int kBarWidth = 120;
  static constexpr int kBarSpeed = 8;
  static constexpr int kBarSlope = 1;  // bar shifts 1 pixel right per kBarSlope rows

  static constexpr Ball kInitial_[kNumBalls] = {
      {80,  60,  1,  1,  10, 4 },
      {200, 120, 1,  -1, 20, 6 },
      {400, 240, -1, 1,  35, 6 },
      {600, 80,  -1, -1, 15, 8 },
      {150, 350, 1,  1,  25, 10},
      {700, 400, -1, 1,  12, 12},
      {300, 420, 1,  -1, 40, 5 },
      {500, 300, -1, -1, 18, 14},
      {650, 200, 1,  1,  28, 9 },
      {100, 180, -1, 1,  8,  14},
      {720, 440, -1, -1, 10, 4 },
      {350, 50,  -1, 1,  20, 6 },
      {130, 380, 1,  -1, 35, 6 },
      {450, 430, 1,  1,  15, 8 },
      {620, 260, -1, -1, 25, 10},
      {50,  120, 1,  1,  12, 12},
      {760, 160, -1, 1,  40, 5 },
      {230, 290, 1,  -1, 18, 14},
      {540, 460, -1, 1,  28, 9 },
      {380, 140, 1,  -1, 8,  14},
  };

  void step(int direction) {
    for (int i = 0; i < kNumBalls; ++i)
      move(balls_[i], direction);
    const int offscreen_left = -(kBarWidth + EInkDisplay::DISPLAY_HEIGHT / kBarSlope);
    bar_x_ += kBarSpeed * direction;
    if (bar_x_ > EInkDisplay::DISPLAY_WIDTH)
      bar_x_ = offscreen_left;
    else if (bar_x_ < offscreen_left)
      bar_x_ = EInkDisplay::DISPLAY_WIDTH;
    const int offscreen_top = -kBarWidth;
    bar2_y_ += kBarSpeed * direction;
    if (bar2_y_ > EInkDisplay::DISPLAY_HEIGHT + EInkDisplay::DISPLAY_WIDTH)
      bar2_y_ = offscreen_top;
    else if (bar2_y_ < offscreen_top)
      bar2_y_ = EInkDisplay::DISPLAY_HEIGHT + EInkDisplay::DISPLAY_WIDTH;
  }

  static void move(Ball& b, int dir) {
    b.x += b.dx * dir * b.speed;
    if (b.x - b.radius < 0) {
      b.x = b.radius;
      b.dx = -b.dx;
    } else if (b.x + b.radius > EInkDisplay::DISPLAY_WIDTH - 1) {
      b.x = EInkDisplay::DISPLAY_WIDTH - 1 - b.radius;
      b.dx = -b.dx;
    }
    b.y += b.dy * dir * b.speed;
    if (b.y - b.radius < 0) {
      b.y = b.radius;
      b.dy = -b.dy;
    } else if (b.y + b.radius > EInkDisplay::DISPLAY_HEIGHT - 1) {
      b.y = EInkDisplay::DISPLAY_HEIGHT - 1 - b.radius;
      b.dy = -b.dy;
    }
  }

  static inline void setPixel(uint8_t* buf, int x, int y, bool black) {
    if (x < 0 || x >= EInkDisplay::DISPLAY_WIDTH || y < 0 || y >= EInkDisplay::DISPLAY_HEIGHT)
      return;
    const int byte_idx = y * EInkDisplay::DISPLAY_WIDTH_BYTES + x / 8;
    const uint8_t mask = 0x80u >> (x & 7);
    if (black)
      buf[byte_idx] &= ~mask;
    else
      buf[byte_idx] |= mask;
  }

  static void drawBar(uint8_t* buf, int x, bool black) {
    for (int by = 0; by < EInkDisplay::DISPLAY_HEIGHT; ++by) {
      const int row_x = x + by / kBarSlope;
      for (int bx = row_x; bx < row_x + kBarWidth; ++bx)
        setPixel(buf, bx, by, black);
    }
  }

  static void drawBarV(uint8_t* buf, int y, bool black) {
    for (int bx = 0; bx < EInkDisplay::DISPLAY_WIDTH; ++bx) {
      const int col_y = y - bx / kBarSlope;  // negative slope: perpendicular to drawBar
      for (int by = col_y; by < col_y + kBarWidth; ++by)
        setPixel(buf, bx, by, black);
    }
  }

  static void drawFilledCircle(uint8_t* buf, int cx, int cy, int r, bool black) {
    for (int dy = -r; dy <= r; ++dy) {
      const int half_w = static_cast<int>(sqrtf(static_cast<float>(r * r - dy * dy)));
      for (int dx = -half_w; dx <= half_w; ++dx)
        setPixel(buf, cx + dx, cy + dy, black);
    }
  }
};
