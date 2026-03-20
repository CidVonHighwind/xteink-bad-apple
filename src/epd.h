#pragma once

// EInkDisplay driver for ESP-IDF (SSD1677).

#include <cstdint>
#include <cstring>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ---- Pin assignments ----
#define EPD_SCLK GPIO_NUM_8
#define EPD_MOSI GPIO_NUM_10
#define EPD_CS GPIO_NUM_21
#define EPD_DC GPIO_NUM_4
#define EPD_RST GPIO_NUM_5
#define EPD_BUSY GPIO_NUM_6

// ---- SSD1677 command definitions ----
#define CMD_SOFT_RESET 0x12
#define CMD_BOOSTER_SOFT_START 0x0C
#define CMD_DRIVER_OUTPUT_CONTROL 0x01
#define CMD_BORDER_WAVEFORM 0x3C
#define CMD_TEMP_SENSOR_CONTROL 0x18
#define CMD_DATA_ENTRY_MODE 0x11
#define CMD_SET_RAM_X_RANGE 0x44
#define CMD_SET_RAM_Y_RANGE 0x45
#define CMD_SET_RAM_X_COUNTER 0x4E
#define CMD_SET_RAM_Y_COUNTER 0x4F
#define CMD_WRITE_RAM_BW 0x24
#define CMD_WRITE_RAM_RED 0x26
#define CMD_DISPLAY_OPTIONS 0x37
#define CMD_AUTO_WRITE_BW_RAM 0x46
#define CMD_AUTO_WRITE_RED_RAM 0x47
#define CMD_DISPLAY_UPDATE_CTRL1 0x21
#define CMD_DISPLAY_UPDATE_CTRL2 0x22
#define CMD_MASTER_ACTIVATION 0x20
#define CTRL1_NORMAL 0x00
#define CTRL1_BYPASS_RED 0x40
#define CMD_WRITE_LUT 0x32
#define CMD_GATE_VOLTAGE 0x03
#define CMD_SOURCE_VOLTAGE 0x04
#define CMD_WRITE_VCOM 0x2C
#define CMD_WRITE_TEMP 0x1A
#define CMD_DEEP_SLEEP 0x10

// ---- Refresh modes ----
enum RefreshMode { FULL_REFRESH, HALF_REFRESH, FAST_REFRESH };

// ---- LUT ----
static const uint8_t lut_fast[] = {
    // VS L0–L3 (voltage patterns per transition)
    // Black → Black: [VSH1 → VSL → VSH1 → VSH1]
    0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // Black → White: [VSL → VSH1 → VSL → VSL]
    0x9A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // White → Black: [VSL → VSH2 → VSL → VSH1]
    0xB9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // White → White: [VSH2 → VSL → VSH2 → VSL]
    0xEE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // L4 (VCOM)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    // TP/RP groups
    0x01, 0x01, 0x01, 0x01, 0x00,  // G0: A=1 B=1 C=1 D=1 RP=0 (4 frames)
    0x00, 0x00, 0x00, 0x00, 0x00,  // G1: A=0 B=0 C=0 D=0 RP=0
    0x00, 0x00, 0x00, 0x00, 0x00,  // G2: A=0 B=0 C=0 D=0 RP=0
    0x00, 0x00, 0x00, 0x00, 0x00,  // G3: A=0 B=0 C=0 D=0 RP=0
    0x00, 0x00, 0x00, 0x00, 0x00,  // G4: A=0 B=0 C=0 D=0 RP=0
    0x00, 0x00, 0x00, 0x00, 0x00,  // G5: A=0 B=0 C=0 D=0 RP=0
    0x00, 0x00, 0x00, 0x00, 0x00,  // G6: A=0 B=0 C=0 D=0 RP=0
    0x00, 0x00, 0x00, 0x00, 0x00,  // G7: A=0 B=0 C=0 D=0 RP=0
    0x00, 0x00, 0x00, 0x00, 0x00,  // G8: A=0 B=0 C=0 D=0 RP=0
    0x00, 0x00, 0x00, 0x00, 0x00,  // G9: A=0 B=0 C=0 D=0 RP=0

    // Frame rate
    0x86, 0x86, 0x86, 0x86, 0x86,

    // Voltages (VGH, VSH1, VSH2, VSL, VCOM)
    0x17, 0x41, 0xA8, 0x32, 0x30,

    // Reserved
    0x00, 0x00};

class EInkDisplay {
 public:
  static const uint16_t DISPLAY_WIDTH = 800;
  static const uint16_t DISPLAY_HEIGHT = 480;
  static const uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;
  static const uint32_t BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT;

  __attribute__((aligned(16))) uint8_t frameBuffer0[BUFFER_SIZE];
  __attribute__((aligned(16))) uint8_t frameBuffer1[BUFFER_SIZE];

  uint8_t* frameBuffer;
  uint8_t* frameBufferActive;

  bool isScreenOn = false;
  bool customLutActive = false;

  spi_device_handle_t spi_;

  EInkDisplay() : frameBuffer(frameBuffer0), frameBufferActive(frameBuffer1) {}

  void begin() {
    memset(frameBuffer0, 0xFF, BUFFER_SIZE);
    memset(frameBuffer1, 0xFF, BUFFER_SIZE);

    // GPIO outputs: CS, DC, RST
    gpio_config_t out_cfg{};
    out_cfg.pin_bit_mask = (1ULL << EPD_CS) | (1ULL << EPD_DC) | (1ULL << EPD_RST);
    out_cfg.mode = GPIO_MODE_OUTPUT;
    out_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    out_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    out_cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&out_cfg);

    // GPIO input: BUSY
    gpio_config_t in_cfg{};
    in_cfg.pin_bit_mask = (1ULL << EPD_BUSY);
    in_cfg.mode = GPIO_MODE_INPUT;
    in_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    in_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    in_cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&in_cfg);

    gpio_set_level(EPD_CS, 1);
    gpio_set_level(EPD_DC, 1);

    spi_bus_config_t bus{};
    bus.mosi_io_num = EPD_MOSI;
    bus.miso_io_num = GPIO_NUM_7;  // shared with SD card
    bus.sclk_io_num = EPD_SCLK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4096;
    spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);

    spi_device_interface_config_t dev{};
    dev.clock_speed_hz = 40 * 1000 * 1000;
    dev.mode = 0;
    dev.spics_io_num = -1;
    dev.queue_size = 1;
    spi_bus_add_device(SPI2_HOST, &dev, &spi_);

    resetDisplay();
    initDisplayController();
  }

  void clearScreen(uint8_t color = 0xFF) {
    memset(frameBuffer, color, BUFFER_SIZE);
  }

  void swapBuffers() {
    uint8_t* temp = frameBuffer;
    frameBuffer = frameBufferActive;
    frameBufferActive = temp;
  }

  void displayBuffer(RefreshMode mode = FAST_REFRESH) {
    if (!isScreenOn)
      mode = HALF_REFRESH;
    writeBuffers(mode);
    refreshDisplay(mode, false);
  }

  // Upload data to RED RAM without triggering a refresh.
  void writeRedRam(const uint8_t* data, uint32_t size) {
    setRamArea(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    writeRamBuffer(CMD_WRITE_RAM_RED, data, size);
  }

  // Write framebuffer data to display RAM (SPI transfer only, no waveform).
  // Call refreshDisplay() afterwards to trigger the actual panel update.
  RefreshMode writeBuffers(RefreshMode mode = FAST_REFRESH) {
    if (!isScreenOn)
      mode = HALF_REFRESH;

    setRamArea(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);

    if (mode != FAST_REFRESH) {
      writeRamBuffer(CMD_WRITE_RAM_BW, frameBuffer, BUFFER_SIZE);
      writeRamBuffer(CMD_WRITE_RAM_RED, frameBuffer, BUFFER_SIZE);
    } else {
      if (customLutActive) {
        writeRamBuffer(CMD_WRITE_RAM_BW, frameBuffer, BUFFER_SIZE);
      } else {
        writeRamBuffer(CMD_WRITE_RAM_BW, frameBuffer, BUFFER_SIZE);
        writeRamBuffer(CMD_WRITE_RAM_RED, frameBufferActive, BUFFER_SIZE);
      }
    }

    swapBuffers();

    return mode;  // return effective mode so refreshDisplay() gets the right one
  }

  void refreshDisplay(RefreshMode mode = FAST_REFRESH, bool turnOffScreen = false) {
    sendCommand(CMD_DISPLAY_UPDATE_CTRL1);
    // if (customLutActive) {
    //   sendData(CTRL1_BYPASS_RED);
    // } else {
    sendData((mode == FAST_REFRESH) ? CTRL1_NORMAL : CTRL1_BYPASS_RED);
    // }

    uint8_t displayMode = 0x00;

    if (!isScreenOn) {
      isScreenOn = true;
      displayMode |= 0xC0;
    }

    if (turnOffScreen) {
      isScreenOn = false;
      displayMode |= 0x03;
    }

    if (mode == FULL_REFRESH) {
      displayMode |= 0x34;
    } else if (mode == HALF_REFRESH) {
      sendCommand(CMD_WRITE_TEMP);
      sendData(0x5A);
      displayMode |= 0xD4;
    } else if (customLutActive) {
      displayMode |= 0x0C;
    } else {  // FAST_REFRESH
      displayMode |= 0x1C;
    }

    ESP_LOGD("epd", "refreshDisplay: mode=%s displayMode=0x%02X",
             mode == FULL_REFRESH   ? "FULL"
             : mode == HALF_REFRESH ? "HALF"
                                    : "FAST",
             displayMode);

    sendCommand(CMD_DISPLAY_UPDATE_CTRL2);
    sendData(displayMode);
    sendCommand(CMD_MASTER_ACTIVATION);
    waitWhileBusy();
  }

  // Send the refresh command and return immediately (EPD will be busy).
  // Call waitForRefresh() once you're done with other work.
  void triggerRefresh(RefreshMode mode = FAST_REFRESH) {
    // disable ram ping poing; I think its okay to writer over everything here
    sendCommand(CMD_DISPLAY_OPTIONS);
    for (size_t i = 0; i < 5; i++) {
      sendData(0xFF);
    }
    sendData(0x0F);

    sendCommand(CMD_DISPLAY_UPDATE_CTRL1);
    // if (customLutActive)
    //   sendData(CTRL1_BYPASS_RED);
    // else
    sendData((mode == FAST_REFRESH) ? CTRL1_NORMAL : CTRL1_BYPASS_RED);

    uint8_t displayMode = 0x00;
    if (!isScreenOn) {
      isScreenOn = true;
      displayMode |= 0xC0;
    }
    if (mode == FULL_REFRESH)
      displayMode |= 0x34;
    else if (mode == HALF_REFRESH) {
      sendCommand(CMD_WRITE_TEMP);
      sendData(0x5A);
      displayMode |= 0xD4;
    } else if (customLutActive)
      displayMode |= 0x0C;
    else
      displayMode |= 0x1C;

    sendCommand(CMD_DISPLAY_UPDATE_CTRL2);
    sendData(displayMode);
    sendCommand(CMD_MASTER_ACTIVATION);
    // returns immediately; EPD BUSY pin will be high until done
  }

  void waitForRefresh() {
    waitWhileBusy();
  }

  void setCustomLUT(bool enabled, const uint8_t* lutData = nullptr) {
    if (enabled) {
      if (!lutData)
        lutData = lut_fast;
      sendCommand(CMD_WRITE_LUT);
      for (uint16_t i = 0; i < 105; i++)
        sendData(lutData[i]);
      sendCommand(CMD_GATE_VOLTAGE);
      sendData(lutData[105]);
      sendCommand(CMD_SOURCE_VOLTAGE);
      sendData(lutData[106]);
      sendData(lutData[107]);
      sendData(lutData[108]);
      sendCommand(CMD_WRITE_VCOM);
      sendData(lutData[109]);
      customLutActive = true;
    } else {
      customLutActive = false;
    }
  }

  void deepSleep() {
    sendCommand(CMD_DEEP_SLEEP);
    sendData(0x01);
  }

 private:
  static uint32_t millis() {
    return (uint32_t)(esp_timer_get_time() / 1000);
  }
  static void delay(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
  }

  void sendCommand(uint8_t command) {
    gpio_set_level(EPD_DC, 0);
    gpio_set_level(EPD_CS, 0);
    spi_transaction_t t{};
    t.length = 8;
    t.flags = SPI_TRANS_USE_TXDATA;
    t.tx_data[0] = command;
    spi_device_polling_transmit(spi_, &t);
    gpio_set_level(EPD_CS, 1);
  }

  void sendData(uint8_t data) {
    gpio_set_level(EPD_DC, 1);
    gpio_set_level(EPD_CS, 0);
    spi_transaction_t t{};
    t.length = 8;
    t.flags = SPI_TRANS_USE_TXDATA;
    t.tx_data[0] = data;
    spi_device_polling_transmit(spi_, &t);
    gpio_set_level(EPD_CS, 1);
  }

  void sendData(const uint8_t* data, uint32_t length) {
    static constexpr size_t kChunk = 4092;
    gpio_set_level(EPD_DC, 1);
    size_t offset = 0;
    while (offset < length) {
      const size_t chunk = (length - offset < kChunk) ? (length - offset) : kChunk;
      gpio_set_level(EPD_CS, 0);
      spi_transaction_t t{};
      t.length = chunk * 8;
      t.tx_buffer = data + offset;
      spi_device_polling_transmit(spi_, &t);
      gpio_set_level(EPD_CS, 1);
      offset += chunk;
    }
  }

  void waitWhileBusy(const char* comment = nullptr) {
    uint32_t start = millis();
    while (gpio_get_level(EPD_BUSY) == 1) {
      vTaskDelay(1);  // 1 tick (~10 ms at 100 Hz); pdMS_TO_TICKS(1) would truncate to 0
      if (millis() - start > 10000) {
        ESP_LOGW("epd", "waitWhileBusy timeout%s", comment ? comment : "");
        break;
      }
    }
    if (comment) {
      ESP_LOGI("epd", "waitWhileBusy done: %s (%lu ms)", comment, millis() - start);
    }
  }

  void resetDisplay() {
    gpio_set_level(EPD_RST, 1);
    delay(20);
    gpio_set_level(EPD_RST, 0);
    delay(2);
    gpio_set_level(EPD_RST, 1);
    delay(20);
  }

  void initDisplayController() {
    sendCommand(CMD_SOFT_RESET);
    waitWhileBusy("CMD_SOFT_RESET");

    sendCommand(CMD_TEMP_SENSOR_CONTROL);
    sendData(0x80);

    sendCommand(CMD_BOOSTER_SOFT_START);
    sendData(0xAE);
    sendData(0xC7);
    sendData(0xC3);
    sendData(0xC0);
    sendData(0x40);

    sendCommand(CMD_DRIVER_OUTPUT_CONTROL);
    sendData((DISPLAY_HEIGHT - 1) % 256);
    sendData((DISPLAY_HEIGHT - 1) / 256);
    sendData(0x02);

    sendCommand(CMD_BORDER_WAVEFORM);
    sendData(0x01);

    setRamArea(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);

    sendCommand(CMD_AUTO_WRITE_BW_RAM);
    sendData(0xF7);
    waitWhileBusy("CMD_AUTO_WRITE_BW_RAM");
    sendCommand(CMD_AUTO_WRITE_RED_RAM);
    sendData(0xF7);
    waitWhileBusy("CMD_AUTO_WRITE_RED_RAM");
  }

  void setRamArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    y = DISPLAY_HEIGHT - y - h;

    sendCommand(CMD_DATA_ENTRY_MODE);
    sendData(0x01);  // X inc, Y dec

    sendCommand(CMD_SET_RAM_X_RANGE);
    sendData(x % 256);
    sendData(x / 256);
    sendData((x + w - 1) % 256);
    sendData((x + w - 1) / 256);

    sendCommand(CMD_SET_RAM_Y_RANGE);
    sendData((y + h - 1) % 256);
    sendData((y + h - 1) / 256);
    sendData(y % 256);
    sendData(y / 256);

    sendCommand(CMD_SET_RAM_X_COUNTER);
    sendData(x % 256);
    sendData(x / 256);

    sendCommand(CMD_SET_RAM_Y_COUNTER);
    sendData((y + h - 1) % 256);
    sendData((y + h - 1) / 256);
  }

  void writeRamBuffer(uint8_t ramBuffer, const uint8_t* data, uint32_t size) {
    sendCommand(ramBuffer);
    sendData(data, size);
  }
};
