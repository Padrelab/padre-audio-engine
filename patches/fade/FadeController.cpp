#include "FadeController.h"

#include <math.h>

namespace padre {

FadeValue::FadeValue(const FadeConfig& config) : cfg_(config) {
  current_ = clamp(cfg_.min_gain);
  target_ = current_;
}

void FadeValue::setInstant(float gain) {
  current_ = clamp(gain);
  target_ = current_;
  speed_per_sec_ = 0.0f;
}

void FadeValue::fadeTo(float target_gain, float speed_per_sec) {
  target_ = clamp(target_gain);
  speed_per_sec_ = (speed_per_sec > 0.0f) ? speed_per_sec : cfg_.default_speed_per_sec;
}

void FadeValue::fadeIn(float speed_per_sec) { fadeTo(cfg_.max_gain, speed_per_sec); }

void FadeValue::fadeOut(float speed_per_sec) { fadeTo(cfg_.min_gain, speed_per_sec); }

float FadeValue::tick(uint32_t dt_ms) {
  const float diff = target_ - current_;
  const float abs_diff = fabsf(diff);

  if (abs_diff <= cfg_.epsilon || speed_per_sec_ <= 0.0f || dt_ms == 0) {
    if (abs_diff <= cfg_.epsilon) {
      current_ = target_;
      speed_per_sec_ = 0.0f;
    }
    return current_;
  }

  const float max_step = speed_per_sec_ * static_cast<float>(dt_ms) / 1000.0f;
  if (abs_diff <= max_step) {
    current_ = target_;
    speed_per_sec_ = 0.0f;
  } else {
    current_ += (diff > 0.0f) ? max_step : -max_step;
  }

  return current_;
}

float FadeValue::current() const { return current_; }

float FadeValue::target() const { return target_; }

float FadeValue::speed() const { return speed_per_sec_; }

bool FadeValue::isTransitioning() const {
  return fabsf(target_ - current_) > cfg_.epsilon;
}

float FadeValue::clamp(float value) const {
  if (value < cfg_.min_gain) return cfg_.min_gain;
  if (value > cfg_.max_gain) return cfg_.max_gain;
  return value;
}

CrossfadeController::CrossfadeController(const FadeConfig& config)
    : from_(config), to_(config) {
  from_.setInstant(config.max_gain);
  to_.setInstant(config.min_gain);
}

void CrossfadeController::configure(float from_gain, float to_gain) {
  from_.setInstant(from_gain);
  to_.setInstant(to_gain);
}

void CrossfadeController::start(float speed_per_sec) {
  from_.fadeOut(speed_per_sec);
  to_.fadeIn(speed_per_sec);
}

void CrossfadeController::stop() {
  from_.setInstant(from_.current());
  to_.setInstant(to_.current());
}

CrossfadeState CrossfadeController::tick(uint32_t dt_ms) {
  from_.tick(dt_ms);
  to_.tick(dt_ms);
  return state();
}

CrossfadeState CrossfadeController::state() const {
  return {
      from_.current(),
      to_.current(),
      from_.isTransitioning() || to_.isTransitioning(),
  };
}

}  // namespace padre
