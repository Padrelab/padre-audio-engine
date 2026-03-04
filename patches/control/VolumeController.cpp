#include "VolumeController.h"

namespace padre {

VolumeController::VolumeController(const VolumeConfig& config) : cfg_(config) {
  current_ = cfg_.boot_fallback;
  target_ = cfg_.boot_fallback;
}

void VolumeController::restore(float stored_value) {
  const float start = (stored_value <= cfg_.safe_boot_max) ? stored_value : cfg_.boot_fallback;
  current_ = clamp(start);
  target_ = clamp(start);
}

void VolumeController::set(float value) { target_ = clamp(value); }

void VolumeController::step(float delta) { set(target_ + delta); }

float VolumeController::tick(uint32_t dt_ms) {
  const float max_step = cfg_.smoothing_per_sec * static_cast<float>(dt_ms) / 1000.0f;
  const float diff = target_ - current_;

  if (fabsf(diff) <= max_step) {
    current_ = target_;
  } else {
    current_ += (diff > 0 ? max_step : -max_step);
  }
  return current_;
}

float VolumeController::current() const { return current_; }
float VolumeController::target() const { return target_; }

float VolumeController::clamp(float v) const {
  if (v < cfg_.min) return cfg_.min;
  if (v > cfg_.max) return cfg_.max;
  return v;
}

}  // namespace padre
