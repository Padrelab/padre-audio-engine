#pragma once

#include <Arduino.h>
#include <Adafruit_MPR121.h>
#include <Wire.h>

namespace test_sd_mpr121 {

class Mpr121TapInput {
 public:
  struct Config {
    Config(uint8_t touch = 12, uint8_t release = 6)
        : touch_threshold(touch), release_threshold(release) {}

    uint8_t touch_threshold;
    uint8_t release_threshold;
  };

  Mpr121TapInput();

  bool begin(TwoWire& wire, uint8_t sda, uint8_t scl, uint8_t irq_pin);
  bool begin(TwoWire& wire,
             uint8_t sda,
             uint8_t scl,
             uint8_t irq_pin,
             const Config& config);

  uint16_t update();

 private:
  Adafruit_MPR121 mpr_;
  uint8_t irq_pin_;
  uint16_t previous_touched_;
};

}  // namespace test_sd_mpr121
