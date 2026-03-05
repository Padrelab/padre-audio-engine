#pragma once

#include <Arduino.h>

#include "../core/InputEvent.h"
#include "../core/PressDetector.h"

namespace padre {

struct ButtonInputConfig {
  bool active_low = true;
  uint32_t debounce_ms = 35;
  uint32_t long_press_ms = 550;
};

struct ButtonInputIo {
  void* user_data = nullptr;
  bool (*read_pin)(void* user_data, uint8_t pin) = nullptr;
};

class ButtonInput {
 public:
  ButtonInput(uint8_t pin, const ButtonInputIo& io,
              const ButtonInputConfig& config = {});

  InputEvent update(uint32_t now_ms);
  bool stablePressed() const;

 private:
  bool normalize(bool raw_level) const;

  uint8_t pin_;
  ButtonInputIo io_;
  ButtonInputConfig cfg_;
  PressDetector detector_;

  bool stable_pressed_ = false;
  bool last_raw_pressed_ = false;
  uint32_t last_raw_change_ms_ = 0;
};

}  // namespace padre
