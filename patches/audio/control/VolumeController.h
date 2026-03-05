#pragma once

#include <Arduino.h>

namespace padre {

struct VolumeConfig {
  float min = 0.0f;
  float max = 20.0f;
  float safe_boot_max = 15.0f;
  float boot_fallback = 10.0f;
  float smoothing_per_sec = 18.0f;
};

class VolumeController {
 public:
  explicit VolumeController(const VolumeConfig& config = {});

  void restore(float stored_value);
  void set(float value);
  void step(float delta);

  float tick(uint32_t dt_ms);
  float current() const;
  float target() const;

 private:
  float clamp(float v) const;

  VolumeConfig cfg_;
  float current_ = 10.0f;
  float target_ = 10.0f;
};

}  // namespace padre
