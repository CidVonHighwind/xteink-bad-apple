#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#define SD_CS GPIO_NUM_12
#define SD_MOUNT "/sdcard"

// ---- Internal state ----
static sdmmc_card_t* sd_card_ = nullptr;
static sdspi_dev_handle_t sd_handle_ = {};
static FILE* sd_video_ = nullptr;
static int sd_frame_count_ = 0;
static uint32_t* sd_offsets_ = nullptr;  // [frame_count+1], malloc'd

// Scratch buffer for reading compressed data off the SD card.
// Worst-case PackBits size: 48000 + ceil(48000/128) = ~48376 bytes.
static constexpr size_t SD_FRAME_BYTES = 800 / 8 * 480;  // 48000
static constexpr size_t SD_SCRATCH_SIZE = SD_FRAME_BYTES + 512;
static uint8_t sd_scratch_[SD_SCRATCH_SIZE];

// ---- PackBits decompressor ----
static int sd_packbits_decode(const uint8_t* src, int src_len, uint8_t* dst, int dst_cap) {
  int si = 0, di = 0;
  while (si < src_len && di < dst_cap) {
    const int8_t n = (int8_t)src[si++];
    if (n >= 0) {
      // Literal run: copy n+1 bytes.
      const int count = n + 1;
      if (si + count > src_len || di + count > dst_cap)
        break;
      memcpy(dst + di, src + si, count);
      si += count;
      di += count;
    } else if (n != -128) {
      // Repeat run: repeat next byte (1-n) times.
      const int count = 1 - (int)n;
      if (si >= src_len || di + count > dst_cap)
        break;
      memset(dst + di, src[si++], count);
      di += count;
    }
  }
  return di;
}

// ---- SD init ----
// Call AFTER epd.begin() so SPI2 is already initialised.
inline bool sd_init() {
  sdspi_device_config_t dev_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
  dev_cfg.gpio_cs = SD_CS;
  dev_cfg.host_id = SPI2_HOST;

  esp_err_t err = sdspi_host_init_device(&dev_cfg, &sd_handle_);
  if (err != ESP_OK) {
    ESP_LOGE("sd", "sdspi_host_init_device failed: %s", esp_err_to_name(err));
    return false;
  }

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = sd_handle_;
  host.max_freq_khz = 20000;

  esp_vfs_fat_mount_config_t mnt = {};
  mnt.format_if_mount_failed = false;
  mnt.max_files = 4;
  mnt.allocation_unit_size = 16 * 1024;

  err = esp_vfs_fat_sdspi_mount(SD_MOUNT, &host, &dev_cfg, &mnt, &sd_card_);
  if (err != ESP_OK) {
    ESP_LOGE("sd", "mount failed: %s", esp_err_to_name(err));
    return false;
  }
  ESP_LOGI("sd", "mounted, %.1f MB", (float)sd_card_->csd.capacity * sd_card_->csd.sector_size / (1024.0f * 1024.0f));

  // ---- Try compressed format first (video.mrv) ----
  sd_video_ = fopen(SD_MOUNT "/video.mrv", "rb");
  if (sd_video_) {
    // Header: 4-byte magic + 4-byte frame_count.
    char magic[4];
    uint32_t frame_count = 0;
    if (fread(magic, 1, 4, sd_video_) == 4 && memcmp(magic, "MRVD", 4) == 0 &&
        fread(&frame_count, 4, 1, sd_video_) == 1 && frame_count > 0) {
      // Read offset table: (frame_count+1) uint32_t values.
      sd_offsets_ = (uint32_t*)malloc((frame_count + 1) * sizeof(uint32_t));
      if (sd_offsets_ && fread(sd_offsets_, sizeof(uint32_t), frame_count + 1, sd_video_) == frame_count + 1) {
        sd_frame_count_ = (int)frame_count;
        const uint32_t data_bytes = sd_offsets_[frame_count] - sd_offsets_[0];
        ESP_LOGI("sd", "video.mrv: %d frames, %.1f KB compressed (%.1f KB raw)", sd_frame_count_, data_bytes / 1024.0f,
                 (float)sd_frame_count_ * SD_FRAME_BYTES / 1024.0f);
        return true;
      }
    }
    fclose(sd_video_);
    sd_video_ = nullptr;
    free(sd_offsets_);
    sd_offsets_ = nullptr;
    ESP_LOGE("sd", "video.mrv invalid");
    return false;
  }

  ESP_LOGW("sd", "no video.mrv found");
  return false;
}

// ---- Frame reader ----
// Always outputs SD_FRAME_BYTES (48000) of decompressed 1bpp data into buf.
inline bool sd_read_frame(int frame_num, uint8_t* buf, size_t /*size*/) {
  if (!sd_video_)
    return false;

  const uint32_t offset = sd_offsets_[frame_num];
  const uint32_t comp_size = sd_offsets_[frame_num + 1] - offset;
  if (comp_size > SD_SCRATCH_SIZE) {
    ESP_LOGE("sd", "frame %d: compressed size %lu exceeds scratch buffer", frame_num, (unsigned long)comp_size);
    return false;
  }
  if (fseek(sd_video_, (long)offset, SEEK_SET) != 0)
    return false;
  if (fread(sd_scratch_, 1, comp_size, sd_video_) != comp_size)
    return false;
  const int decoded = sd_packbits_decode(sd_scratch_, (int)comp_size, buf, (int)SD_FRAME_BYTES);
  return decoded == (int)SD_FRAME_BYTES;
}

inline int sd_count_frames() {
  return sd_frame_count_;
}
