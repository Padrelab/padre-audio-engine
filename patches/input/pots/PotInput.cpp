#include "PotInput.h"

namespace padre {

PotInput::PotInput(uint8_t channel, const PotInputIo& io,
                   const PotInputConfig& config)
    : channel_(channel), io_(io), cfg_(config) {
  if (cfg_.raw_max <= cfg_.raw_min) {
    cfg_.raw_max = cfg_.raw_min + 1;
  }
}

InputEvent PotInput::update(uint32_t now_ms) {
  if (!io_.read_raw) return InputEvent::none();
  if (primed_ && (now_ms - last_sample_ms_) < cfg_.sample_period_ms) {
    return InputEvent::none();
  }

  last_sample_ms_ = now_ms;
  const int raw = clampRaw(io_.read_raw(io_.user_data, channel_));

  if (!primed_) {
    primed_ = true;
    last_raw_ = raw;
    normalized_ = normalize(raw);
    return {InputEventType::ValueChanged, InputSourceType::Pot, channel_,
            normalized_, now_ms};
  }

  if (abs(raw - last_raw_) <= cfg_.deadband) return InputEvent::none();

  last_raw_ = raw;
  normalized_ = normalize(raw);
  return {InputEventType::ValueChanged, InputSourceType::Pot, channel_,
          normalized_, now_ms};
}

float PotInput::normalizedValue() const { return normalized_; }

float PotInput::normalize(int raw) const {
  const float span = static_cast<float>(cfg_.raw_max - cfg_.raw_min);
  return static_cast<float>(raw - cfg_.raw_min) / span;
}

int PotInput::clampRaw(int raw) const {
  if (raw < cfg_.raw_min) return cfg_.raw_min;
  if (raw > cfg_.raw_max) return cfg_.raw_max;
  return raw;
}

}  // namespace padre
