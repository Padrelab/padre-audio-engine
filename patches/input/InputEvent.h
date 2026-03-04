#pragma once

#include <Arduino.h>

namespace padre {

enum class InputSourceType {
  Unknown = 0,
  Button,
  Touch,
  Pot,
};

enum class InputEventType {
  None = 0,
  PressDown,
  PressUp,
  ShortPress,
  LongPress,
  ValueChanged,
};

struct InputEvent {
  InputEventType type = InputEventType::None;
  InputSourceType source = InputSourceType::Unknown;
  uint8_t source_id = 0;
  float value = 0.0f;
  uint32_t timestamp_ms = 0;

  bool isNone() const { return type == InputEventType::None; }

  static InputEvent none() { return {}; }
};

}  // namespace padre
