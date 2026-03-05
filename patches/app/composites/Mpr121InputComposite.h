#pragma once

#include <Arduino.h>

#include "../../input/core/PlaybackInputActions.h"
#include "../../input/mpr121/Mpr121TouchController.h"

namespace padre {

class Mpr121InputComposite {
 public:
  Mpr121InputComposite(const Mpr121TouchControllerIo& io,
                       const Mpr121TouchControllerConfig& config = {});

  bool begin();
  void end();
  void poll(uint32_t now_ms);

  void bindActions(PlaybackInputActions* actions);
  void setEventHandler(void* ctx, InputEventHandlerFn handler);

  Mpr121TouchController& controller();
  const Mpr121TouchController& controller() const;

 private:
  static void handleTouchEvent(void* ctx, const InputEvent& event);

  Mpr121TouchController controller_;
  PlaybackInputActions* actions_ = nullptr;
  void* event_ctx_ = nullptr;
  InputEventHandlerFn event_handler_ = nullptr;
};

}  // namespace padre
