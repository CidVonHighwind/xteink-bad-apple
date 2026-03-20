#pragma once

// Receives LUT frames over USB Serial/JTAG and applies them to the EPD.
//
// Frame format (little-endian):
//   [0xDE 0xAD 0xBE 0xEF]  magic (4 bytes)
//   [length]               payload length (4 bytes LE)
//   [payload]              LUT bytes (112 bytes)
//   [crc32]                CRC-32/JAMCRC of payload (4 bytes LE)
//
// Call serial_lut_start() once from app_main.
// Poll serial_lut_take(buf) each loop — returns true and fills buf when a
// valid new LUT has arrived.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static constexpr const char* kLutRxTag = "lut_rx";
static constexpr uint8_t kFrameMagic[4] = {0xDE, 0xAD, 0xBE, 0xEF};
static constexpr uint32_t kMaxPayload = 256;
static constexpr uint32_t kLutSize = 112;

// Shared between the receiver task and the main loop.
static uint8_t g_lut_buf[kLutSize];
static volatile bool g_lut_pending = false;

// Call from the main loop. Returns true (and copies into `out`) when a fresh
// LUT has been received since the last call.
inline bool serial_lut_take(uint8_t* out) {
  if (!g_lut_pending)
    return false;
  memcpy(out, g_lut_buf, kLutSize);
  g_lut_pending = false;
  return true;
}

// Read exactly `n` bytes with a timeout. Returns true on success.
static bool lut_read(uint8_t* buf, size_t n, uint32_t timeout_ms) {
  size_t received = 0;
  const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
  while (received < n) {
    const TickType_t now = xTaskGetTickCount();
    if ((int32_t)(deadline - now) <= 0)
      return false;
    const int r = usb_serial_jtag_read_bytes(buf + received, n - received, deadline - now);
    if (r > 0)
      received += r;
  }
  return true;
}

static void lut_receiver_task(void* /*arg*/) {
  uint8_t magic_pos = 0;

  ESP_LOGI(kLutRxTag, "receiver ready");

  while (true) {
    uint8_t byte;
    if (usb_serial_jtag_read_bytes(&byte, 1, pdMS_TO_TICKS(50)) != 1) {
      continue;
    }

    if (byte == kFrameMagic[magic_pos]) {
      if (++magic_pos < 4)
        continue;
      magic_pos = 0;

      // --- Length ---
      uint8_t len_buf[4];
      if (!lut_read(len_buf, 4, 500)) {
        ESP_LOGW(kLutRxTag, "timeout reading length");
        continue;
      }
      const uint32_t length = (uint32_t)len_buf[0] | ((uint32_t)len_buf[1] << 8) | ((uint32_t)len_buf[2] << 16) |
                              ((uint32_t)len_buf[3] << 24);

      if (length == 0 || length > kMaxPayload) {
        ESP_LOGW(kLutRxTag, "invalid length: %lu", length);
        continue;
      }

      // --- Payload ---
      uint8_t payload[kMaxPayload];
      if (!lut_read(payload, length, 2000)) {
        ESP_LOGW(kLutRxTag, "timeout reading payload");
        continue;
      }

      // --- CRC32 ---
      uint8_t crc_buf[4];
      if (!lut_read(crc_buf, 4, 500)) {
        ESP_LOGW(kLutRxTag, "timeout reading crc");
        continue;
      }
      const uint32_t recv_crc = (uint32_t)crc_buf[0] | ((uint32_t)crc_buf[1] << 8) | ((uint32_t)crc_buf[2] << 16) |
                                ((uint32_t)crc_buf[3] << 24);
      const uint32_t calc_crc = esp_rom_crc32_le(0, payload, length);

      if (recv_crc != calc_crc) {
        ESP_LOGE(kLutRxTag, "CRC mismatch: received=0x%08lX calculated=0x%08lX", recv_crc, calc_crc);
        continue;
      }

      // --- Store and signal ---
      ESP_LOGI(kLutRxTag, "OK: received %lu-byte LUT (CRC 0x%08lX)", length, calc_crc);
      memcpy(g_lut_buf, payload, kLutSize);
      g_lut_pending = true;

      // --- Log received LUT bytes ---
      char line[64];
      for (uint32_t i = 0; i < length; i += 10) {
        char* p = line;
        for (uint32_t j = i; j < length && j < i + 10; j++) {
          p += snprintf(p, (line + sizeof(line)) - p, "0x%02X ", payload[j]);
        }
        ESP_LOGI(kLutRxTag, "[%3lu] %s", i, line);
      }

    } else {
      // Re-check in case this byte starts the magic sequence.
      magic_pos = (byte == kFrameMagic[0]) ? 1 : 0;
    }
  }
}

// Call once from app_main before the main loop.
inline void serial_lut_start() {
  usb_serial_jtag_driver_config_t cfg = {
      .tx_buffer_size = 1024,
      .rx_buffer_size = 1024,
  };
  usb_serial_jtag_driver_install(&cfg);
  // Route ESP_LOGI / printf through the driver's TX buffer.
  usb_serial_jtag_vfs_register();

  xTaskCreate(lut_receiver_task, "lut_rx", 4096, nullptr, 3, nullptr);
}
