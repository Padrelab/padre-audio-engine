#pragma once

#include <Adafruit_MPR121.h>
#include <Arduino.h>
#include <Wire.h>

#include "Mpr121TouchController.h"

namespace padre {

struct Mpr121AdafruitDriverPins {
  int8_t sda = -1;
  int8_t scl = -1;
  int8_t irq = -1;
  uint8_t address = 0x5A;
  uint32_t i2c_clock_hz = 400000;
};

struct Mpr121AdafruitDriverConfig {
  uint8_t touch_threshold = 12;
  uint8_t release_threshold = 6;
  bool autoconfig = true;
};

using Mpr121AdafruitDriverIrqHookFn = void (*)(void* ctx);

class Mpr121AdafruitDriver {
 public:
  Mpr121AdafruitDriver(Adafruit_MPR121& device,
                       TwoWire& wire,
                       const Mpr121AdafruitDriverPins& pins,
                       const Mpr121AdafruitDriverConfig& config = {});

  bool begin();
  void end();
  bool reconfigure(uint8_t touch_threshold, uint8_t release_threshold, bool autoconfig);

  void setIrqHook(void* ctx, Mpr121AdafruitDriverIrqHookFn hook);

  bool ready() const;
  bool consumeIrq();
  bool irqFlag() const;
  uint32_t irqCount() const;
  bool irqLineActive() const;
  uint16_t touchMask() const;

  uint16_t readTouchMask();
  const Mpr121AdafruitDriverConfig& config() const;

  Mpr121TouchControllerIo asTouchControllerIo();

 private:
  static uint16_t readTouchMaskFromDevice(void* user_data);
  static void irqThunk();

  bool configureWire();
  bool attachInterruptHandler();
  void detachInterruptHandler();
  void handleIrq();

  Adafruit_MPR121* device_ = nullptr;
  TwoWire* wire_ = nullptr;
  Mpr121AdafruitDriverPins pins_;
  Mpr121AdafruitDriverConfig config_;
  void* irq_hook_ctx_ = nullptr;
  Mpr121AdafruitDriverIrqHookFn irq_hook_ = nullptr;
  bool wire_ready_ = false;
  bool ready_ = false;
  bool interrupt_attached_ = false;
  volatile bool irq_flag_ = false;
  volatile uint32_t irq_count_ = 0;
  uint16_t touch_mask_ = 0;

  static Mpr121AdafruitDriver* active_instance_;
};

}  // namespace padre
