#include "ButtonInput.h"

namespace padre {

ButtonInput::ButtonInput(uint8_t pin, const ButtonInputIo& io,
                         const ButtonInputConfig& config)
    : pin_(pin), io_(io), cfg_(config), detector_(config.long_press_ms) {}

InputEvent ButtonInput::update(uint32_t now_ms) {
  if (!io_.read_pin) return InputEvent::none();

  const bool raw_pressed = normalize(io_.read_pin(io_.user_data, pin_));
  if (raw_pressed != last_raw_pressed_) {
    last_raw_pressed_ = raw_pressed;
    last_raw_change_ms_ = now_ms;
  }

  bool state_changed = false;
  if (raw_pressed != stable_pressed_ &&
      (now_ms - last_raw_change_ms_) >= cfg_.debounce_ms) {
    stable_pressed_ = raw_pressed;
    state_changed = true;
  }

  const PressEvent press_event = detector_.update(stable_pressed_, now_ms);
  if (press_event == PressEvent::ShortPress) {
    return {InputEventType::ShortPress, InputSourceType::Button, pin_, 1.0f,
            now_ms};
  }
  if (press_event == PressEvent::LongPress) {
    return {InputEventType::LongPress, InputSourceType::Button, pin_, 1.0f,
            now_ms};
  }

  if (state_changed) {
    return {stable_pressed_ ? InputEventType::PressDown : InputEventType::PressUp,
            InputSourceType::Button,
            pin_,
            stable_pressed_ ? 1.0f : 0.0f,
            now_ms};
  }

  return InputEvent::none();
}

bool ButtonInput::stablePressed() const { return stable_pressed_; }

bool ButtonInput::normalize(bool raw_level) const {
  return cfg_.active_low ? !raw_level : raw_level;
}

}  // namespace padre
