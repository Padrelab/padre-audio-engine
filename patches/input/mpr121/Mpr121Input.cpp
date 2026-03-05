#include "Mpr121Input.h"

namespace padre {

Mpr121Input::Mpr121Input(uint8_t electrode, const Mpr121InputIo& io,
                         const Mpr121InputConfig& config)
    : electrode_(electrode), io_(io), detector_(config.long_press_ms) {}

InputEvent Mpr121Input::update(uint32_t now_ms) {
  if (!io_.read_touch_mask || electrode_ > 11) return InputEvent::none();

  const uint16_t mask = io_.read_touch_mask(io_.user_data);
  const bool now_touched = (mask & (static_cast<uint16_t>(1u) << electrode_)) != 0;

  const bool state_changed = (now_touched != touched_);
  touched_ = now_touched;

  const PressEvent press_event = detector_.update(touched_, now_ms);
  if (press_event == PressEvent::ShortPress) {
    return {InputEventType::ShortPress, InputSourceType::Touch, electrode_, 1.0f,
            now_ms};
  }
  if (press_event == PressEvent::LongPress) {
    return {InputEventType::LongPress, InputSourceType::Touch, electrode_, 1.0f,
            now_ms};
  }

  if (state_changed) {
    return {touched_ ? InputEventType::PressDown : InputEventType::PressUp,
            InputSourceType::Touch,
            electrode_,
            touched_ ? 1.0f : 0.0f,
            now_ms};
  }

  return InputEvent::none();
}

bool Mpr121Input::touched() const { return touched_; }

}  // namespace padre
