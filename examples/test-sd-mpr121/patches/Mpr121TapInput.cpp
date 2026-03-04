#include "Mpr121TapInput.h"

namespace test_sd_mpr121 {

Mpr121TapInput::Mpr121TapInput() : irq_pin_(0), previous_touched_(0) {}

bool Mpr121TapInput::begin(TwoWire& wire,
                           uint8_t sda,
                           uint8_t scl,
                           uint8_t irq_pin) {
  return begin(wire, sda, scl, irq_pin, Config());
}

bool Mpr121TapInput::begin(TwoWire& wire,
                           uint8_t sda,
                           uint8_t scl,
                           uint8_t irq_pin,
                           const Config& config) {
  irq_pin_ = irq_pin;

  pinMode(irq_pin_, INPUT_PULLUP);
  wire.begin(sda, scl);

  if (!mpr_.begin(0x5A, &wire)) {
    return false;
  }

  mpr_.setThreshholds(config.touch_threshold, config.release_threshold);
  previous_touched_ = mpr_.touched();
  return true;
}

uint16_t Mpr121TapInput::update() {
  if (digitalRead(irq_pin_) != LOW) {
    return 0;
  }

  const uint16_t touched = mpr_.touched();
  const uint16_t rising_edges = touched & ~previous_touched_;
  previous_touched_ = touched;
  return rising_edges;
}

}  // namespace test_sd_mpr121
