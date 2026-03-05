#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "../input/InputEvent.h"
#include "Mpr121Input.h"

namespace padre {

using InputEventHandlerFn = void (*)(void* ctx, const InputEvent& event);

struct Mpr121TouchControllerIo {
  void* user_data = nullptr;
  uint16_t (*read_touch_mask)(void* user_data) = nullptr;
};

struct Mpr121TouchControllerConfig {
  uint8_t electrode_count = 4;
  Mpr121InputConfig input_config = {};
  bool debug_touch_mask = false;
  Print* debug_out = nullptr;
};

class Mpr121TouchController {
 public:
  Mpr121TouchController(const Mpr121TouchControllerIo& io,
                        const Mpr121TouchControllerConfig& config = {});
  ~Mpr121TouchController();

  bool begin();
  void end();

  void setEventHandler(void* ctx, InputEventHandlerFn handler);
  void poll(uint32_t now_ms);

  uint16_t touchMask() const;
  uint8_t electrodeCount() const;

 private:
  struct TouchMaskCache {
    uint16_t mask = 0;
  };

  static uint16_t readTouchMaskFromCache(void* user_data);
  static uint8_t clampElectrodeCount(uint8_t requested);
  void dispatchEvent(const InputEvent& event);

  Mpr121TouchControllerIo io_;
  Mpr121TouchControllerConfig config_;
  TouchMaskCache cache_;
  Mpr121Input* inputs_[12] = {nullptr};
  uint8_t electrode_count_ = 0;
  void* event_ctx_ = nullptr;
  InputEventHandlerFn event_handler_ = nullptr;
};

}  // namespace padre
