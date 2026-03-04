#pragma once

#include <Arduino.h>

namespace padre {

struct FadeConfig {
  float min_gain = 0.0f;
  float max_gain = 1.0f;
  float default_speed_per_sec = 1.0f;
  float epsilon = 0.0001f;
};

class FadeValue {
 public:
  explicit FadeValue(const FadeConfig& config = {});

  void setInstant(float gain);
  void fadeTo(float target_gain, float speed_per_sec = 0.0f);
  void fadeIn(float speed_per_sec = 0.0f);
  void fadeOut(float speed_per_sec = 0.0f);

  float tick(uint32_t dt_ms);

  float current() const;
  float target() const;
  float speed() const;
  bool isTransitioning() const;

 private:
  float clamp(float value) const;

  FadeConfig cfg_;
  float current_ = 0.0f;
  float target_ = 0.0f;
  float speed_per_sec_ = 0.0f;
};

struct CrossfadeState {
  float from_gain = 1.0f;
  float to_gain = 0.0f;
  bool active = false;
};

class CrossfadeController {
 public:
  explicit CrossfadeController(const FadeConfig& config = {});

  void configure(float from_gain, float to_gain);
  void start(float speed_per_sec = 0.0f);
  void stop();

  CrossfadeState tick(uint32_t dt_ms);
  CrossfadeState state() const;

 private:
  FadeValue from_;
  FadeValue to_;
};

}  // namespace padre
