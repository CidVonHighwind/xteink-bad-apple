#pragma once

#include <cstring>
#include <string>

#include "demo.h"
#include "esp_timer.h"
#include "sdcard.h"

class VideoPlayer : public IDemo {
 public:
  explicit VideoPlayer(int totalFrames) : totalFrames_(totalFrames) {}

  void activate(EInkDisplay& epd) override {
    playbackStartUs_ = esp_timer_get_time();
    // Build RED RAM: 0xFF in border (letterbox) area, 0x00 in video area.
    // Video is centered horizontally: 80px border on each side (10 bytes).
    // static constexpr int kBorderBytes = 10;  // 80 pixels / 8
    // static constexpr int kStride = EInkDisplay::DISPLAY_WIDTH_BYTES;
    // uint8_t* buf = epd.frameBufferActive;
    // for (int y = 0; y < EInkDisplay::DISPLAY_HEIGHT; ++y) {
    //   memset(buf + y * kStride, 0xFF, kBorderBytes);                               // left border
    //   memset(buf + y * kStride + kBorderBytes, 0x00, kStride - 2 * kBorderBytes);  // video area
    //   memset(buf + y * kStride + kStride - kBorderBytes, 0xFF, kBorderBytes);      // right border
    // }
    // epd.writeRedRam(buf, EInkDisplay::BUFFER_SIZE);

    memset(epd.frameBufferActive, 0x00, EInkDisplay::BUFFER_SIZE);
    epd.writeRedRam(epd.frameBufferActive, EInkDisplay::BUFFER_SIZE);
  }

  void reset() override {
    frame_ = 0;
    prefetchFrame_ = -1;
    playbackStartUs_ = esp_timer_get_time();
  }

  void forward() override {
    if (totalFrames_ > 0)
      frame_ = (frame_ + 1) % totalFrames_;
  }

  void backward() override {
    if (totalFrames_ > 0)
      frame_ = (frame_ - 1 + totalFrames_) % totalFrames_;
  }

  RefreshMode show(EInkDisplay& epd, microreader::ILogger& logger, RefreshMode mode) override {
    if (totalFrames_ <= 0)
      return mode;

    const int64_t t0 = esp_timer_get_time();

    // If the frame isn't already prefetched, read it now (e.g. on seeks).
    if (prefetchFrame_ != frame_) {
      if (!sd_read_frame(frame_, prefetchBuf_, EInkDisplay::BUFFER_SIZE)) {
        logger.log(microreader::LogLevel::Info, "read frame " + std::to_string(frame_) + " FAIL");
        return mode;
      }
      prefetchFrame_ = frame_;
    }
    const int64_t t_read = (esp_timer_get_time() - t0) / 1000;

    // Copy prefetched data into the EPD's render buffer.
    memcpy(epd.frameBuffer, prefetchBuf_, EInkDisplay::BUFFER_SIZE);

    // // Toggle border between black and white every 1 second.
    // static constexpr int kBorderBytes = 10;  // 80 pixels / 8
    // static constexpr int kStride = EInkDisplay::DISPLAY_WIDTH_BYTES;
    // const bool whiteBorder = ((esp_timer_get_time() - playbackStartUs_) / 1000000LL) % 2 == 0;
    // const uint8_t borderByte = whiteBorder ? 0xFF : 0x00;
    // for (int y = 0; y < EInkDisplay::DISPLAY_HEIGHT; ++y) {
    //   memset(epd.frameBuffer + y * kStride, borderByte, kBorderBytes);
    //   memset(epd.frameBuffer + y * kStride + kStride - kBorderBytes, borderByte, kBorderBytes);
    // }

    // Send frame to display RAM and fire off the refresh command.
    const int64_t t1 = esp_timer_get_time();
    const RefreshMode effective = epd.writeBuffers(mode);
    epd.triggerRefresh(effective);  // returns immediately; EPD now busy
    const int64_t t_write = (esp_timer_get_time() - t1) / 1000;

    // While the EPD panel is settling, read the next frame.
    const int nextFrame = (frame_ + 1) % totalFrames_;
    const int64_t t2 = esp_timer_get_time();
    if (!sd_read_frame(nextFrame, prefetchBuf_, EInkDisplay::BUFFER_SIZE)) {
      logger.log(microreader::LogLevel::Info, "prefetch frame " + std::to_string(nextFrame) + " FAIL");
      prefetchFrame_ = -1;
    } else {
      prefetchFrame_ = nextFrame;
    }
    const int64_t t_prefetch = (esp_timer_get_time() - t2) / 1000;

    // Wait for the EPD to finish (usually already done by now).
    const int64_t t3 = esp_timer_get_time();
    epd.waitForRefresh();
    const int64_t t_wait = (esp_timer_get_time() - t3) / 1000;

    const int64_t t_total = (esp_timer_get_time() - t0) / 1000;
    logger.log(microreader::LogLevel::Info,
               "frame " + std::to_string(frame_) + "/" + std::to_string(totalFrames_) +
                   "  read=" + std::to_string(t_read) + " write=" + std::to_string(t_write) +
                   " prefetch=" + std::to_string(t_prefetch) + " wait=" + std::to_string(t_wait) +
                   " total=" + std::to_string(t_total) + " ms");
    return effective;
  }

  bool isDone() const override {
    return totalFrames_ > 0 && frame_ == totalFrames_ - 1;
  }

  const char* helpText() const override {
    return "VIDEO  Up=next  Dn=prev  B0=play/pause  B1=restart  B2=mode  B3=balls";
  }

  int targetFrameMs() const override {
    return 33;
  }

 private:
  int totalFrames_;
  int frame_ = 0;
  int prefetchFrame_ = -1;
  int64_t playbackStartUs_ = 0;
  static uint8_t __attribute__((aligned(16))) prefetchBuf_[EInkDisplay::BUFFER_SIZE];
};

// Static member definition
inline uint8_t __attribute__((aligned(16))) VideoPlayer::prefetchBuf_[EInkDisplay::BUFFER_SIZE];
