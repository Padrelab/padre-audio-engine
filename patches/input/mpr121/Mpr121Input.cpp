#include "Mpr121Input.h"

namespace padre {

Mpr121Input::Mpr121Input(uint8_t electrode, const Mpr121InputIo& io,
                         const Mpr121InputConfig& config)
    : electrode_(electrode), io_(io), cfg_(config), detector_(config.long_press_ms) {}

InputEvent Mpr121Input::update(uint32_t now_ms) {
  if (!io_.read_touch_mask || electrode_ > 11) return InputEvent::none();

  const uint16_t mask = io_.read_touch_mask(io_.user_data);
  const bool raw_touched = (mask & (static_cast<uint16_t>(1u) << electrode_)) != 0;

  if (raw_touched != last_raw_touched_) {
    last_raw_touched_ = raw_touched;
    last_raw_change_ms_ = now_ms;
  }

  bool state_changed = false;
  if (raw_touched != stable_touched_ &&
      (now_ms - last_raw_change_ms_) >= cfg_.debounce_ms) {
    stable_touched_ = raw_touched;
    state_changed = true;
  }

  const PressEvent press_event = detector_.update(stable_touched_, now_ms);
  if (press_event == PressEvent::ShortPress) {
    return {InputEventType::ShortPress, InputSourceType::Touch, electrode_, 1.0f,
            now_ms};
  }
  if (press_event == PressEvent::LongPress) {
    return {InputEventType::LongPress, InputSourceType::Touch, electrode_, 1.0f,
            now_ms};
  }

  if (state_changed) {
    return {stable_touched_ ? InputEventType::PressDown : InputEventType::PressUp,
            InputSourceType::Touch,
            electrode_,
            stable_touched_ ? 1.0f : 0.0f,
            now_ms};
  }

  return InputEvent::none();
}

bool Mpr121Input::touched() const { return stable_touched_; }

}  // namespace padre
