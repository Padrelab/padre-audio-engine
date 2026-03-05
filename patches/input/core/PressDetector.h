#pragma once

#include <Arduino.h>

namespace padre {

enum class PressEvent {
  None,
  ShortPress,
  LongPress,
};

class PressDetector {
 public:
  explicit PressDetector(uint32_t long_press_ms = 500) : long_press_ms_(long_press_ms) {}

  void setLongPressMs(uint32_t value) { long_press_ms_ = value; }

  PressEvent update(bool pressed, uint32_t now_ms) {
    if (pressed && !last_pressed_) {
      down_at_ = now_ms;
      long_emitted_ = false;
    }

    if (pressed && !long_emitted_ && (now_ms - down_at_) >= long_press_ms_) {
      long_emitted_ = true;
      last_pressed_ = true;
      return PressEvent::LongPress;
    }

    if (!pressed && last_pressed_) {
      const bool was_long = long_emitted_;
      long_emitted_ = false;
      last_pressed_ = false;
      return was_long ? PressEvent::None : PressEvent::ShortPress;
    }

    last_pressed_ = pressed;
    return PressEvent::None;
  }

 private:
  uint32_t long_press_ms_;
  uint32_t down_at_ = 0;
  bool last_pressed_ = false;
  bool long_emitted_ = false;
};

}  // namespace padre
