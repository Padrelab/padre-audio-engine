#pragma once

#include <Arduino.h>

#include "InputEvent.h"

namespace padre {

struct PlaybackInputBindings {
  InputEventType trigger_event = InputEventType::PressDown;
  uint8_t pause_toggle_source_id = 0;
  uint8_t next_track_source_id = 1;
  uint8_t volume_down_source_id = 2;
  uint8_t volume_up_source_id = 3;
};

struct PlaybackInputActionsCallbacks {
  void* ctx = nullptr;
  bool (*isPlaybackRunning)(void* ctx) = nullptr;
  bool (*togglePaused)(void* ctx) = nullptr;
  void (*onPauseChanged)(void* ctx, bool paused) = nullptr;
  void (*requestNextTrack)(void* ctx) = nullptr;
  bool (*stepVolume)(void* ctx, int delta, int* out_new_value) = nullptr;
  void (*onVolumeChanged)(void* ctx, int new_value) = nullptr;
};

class PlaybackInputActions {
 public:
  PlaybackInputActions(const PlaybackInputBindings& bindings = {},
                       const PlaybackInputActionsCallbacks& callbacks = {});

  void setBindings(const PlaybackInputBindings& bindings);
  const PlaybackInputBindings& bindings() const;

  void setCallbacks(const PlaybackInputActionsCallbacks& callbacks);
  const PlaybackInputActionsCallbacks& callbacks() const;

  bool handle(const InputEvent& event) const;

 private:
  PlaybackInputBindings bindings_;
  PlaybackInputActionsCallbacks callbacks_;
};

}  // namespace padre
