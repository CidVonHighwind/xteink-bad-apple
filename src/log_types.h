#pragma once

#include <string>

namespace microreader {

enum class LogLevel { Info, Warning, Error };

class ILogger {
 public:
  virtual ~ILogger() = default;
  virtual void log(LogLevel level, const std::string& message) = 0;
};

}  // namespace microreader
