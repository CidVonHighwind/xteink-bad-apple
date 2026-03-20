#pragma once

#include <cstdint>

namespace microreader {

enum class Button : uint8_t {
  Button0 = 0,  // Back
  Button1 = 1,  // Confirm
  Button2 = 2,  // Left
  Button3 = 3,  // Right
  Up = 4,       // Vol+
  Down = 5,     // Vol-
  Power = 6,
};

struct ButtonState {
  uint8_t current = 0;
  uint8_t previous = 0;

  void update(uint8_t new_state) {
    previous = current;
    current = new_state;
  }

  bool is_pressed(Button b) const {
    const uint8_t mask = static_cast<uint8_t>(1u << static_cast<uint8_t>(b));
    return (current & mask) != 0 && (previous & mask) == 0;
  }

  bool is_down(Button b) const {
    return (current & (1u << static_cast<uint8_t>(b))) != 0;
  }
};

class IInputSource {
 public:
  virtual ~IInputSource() = default;
  virtual ButtonState poll_buttons() = 0;
};

}  // namespace microreader
