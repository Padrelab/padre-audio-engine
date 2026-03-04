#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_MPR121.h>

namespace test_sd_mpr121 {

class Mpr121TapInput {
 public:
  struct Config {
    uint8_t touch_threshold = 12;
    uint8_t release_threshold = 6;
  };

  bool begin(TwoWire& wire, uint8_t sda, uint8_t scl, uint8_t irq_pin,
             const Config& config = {});

  uint16_t update();

 private:
  Adafruit_MPR121 mpr_;
  uint8_t irq_pin_ = 0;
  uint16_t previous_touched_ = 0;
};

}  // namespace test_sd_mpr121
