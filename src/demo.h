#pragma once

#include "epd.h"
#include "log_types.h"

class IDemo {
 public:
  virtual ~IDemo() = default;
  virtual void activate(EInkDisplay& /*epd*/) {}
  virtual void reset() = 0;
  virtual void forward() = 0;
  virtual void backward() = 0;
  virtual RefreshMode show(EInkDisplay& epd, microreader::ILogger& logger, RefreshMode mode) = 0;
  virtual bool isDone() const = 0;
  virtual const char* helpText() const = 0;
  virtual int targetFrameMs() const {
    return 20;
  }
};
