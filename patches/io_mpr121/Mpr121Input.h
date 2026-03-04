#pragma once

#include <Arduino.h>

#include "../input/InputEvent.h"
#include "../input/PressDetector.h"

namespace padre {

struct Mpr121InputConfig {
  uint32_t long_press_ms = 550;
};

struct Mpr121InputIo {
  void* user_data = nullptr;
  uint16_t (*read_touch_mask)(void* user_data) = nullptr;
};

class Mpr121Input {
 public:
  Mpr121Input(uint8_t electrode, const Mpr121InputIo& io,
              const Mpr121InputConfig& config = {});

  InputEvent update(uint32_t now_ms);
  bool touched() const;

 private:
  uint8_t electrode_;
  Mpr121InputIo io_;
  PressDetector detector_;
  bool touched_ = false;
};

}  // namespace padre
