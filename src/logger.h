#pragma once

#include <string>

#include "esp_log.h"
#include "log_types.h"

class Esp32Logger final : public microreader::ILogger {
 public:
  void log(microreader::LogLevel level, const std::string& message) override {
    switch (level) {
      case microreader::LogLevel::Info:
        ESP_LOGI("microreader2", "%s", message.c_str());
        break;
      case microreader::LogLevel::Warning:
        ESP_LOGW("microreader2", "%s", message.c_str());
        break;
      case microreader::LogLevel::Error:
        ESP_LOGE("microreader2", "%s", message.c_str());
        break;
    }
  }
};
