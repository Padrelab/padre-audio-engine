#pragma once

#include <Arduino.h>

#include "../core/InputEvent.h"

namespace padre {

struct PotInputConfig {
  int raw_min = 0;
  int raw_max = 4095;
  uint16_t deadband = 8;
  uint32_t sample_period_ms = 15;
};

struct PotInputIo {
  void* user_data = nullptr;
  int (*read_raw)(void* user_data, uint8_t channel) = nullptr;
};

class PotInput {
 public:
  PotInput(uint8_t channel, const PotInputIo& io,
           const PotInputConfig& config = {});

  InputEvent update(uint32_t now_ms);
  float normalizedValue() const;

 private:
  float normalize(int raw) const;
  int clampRaw(int raw) const;

  uint8_t channel_;
  PotInputIo io_;
  PotInputConfig cfg_;
  int last_raw_ = 0;
  float normalized_ = 0.0f;
  uint32_t last_sample_ms_ = 0;
  bool primed_ = false;
};

}  // namespace padre
