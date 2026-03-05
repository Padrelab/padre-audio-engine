#include "PlaybackInputActions.h"

namespace padre {

PlaybackInputActions::PlaybackInputActions(
    const PlaybackInputBindings& bindings,
    const PlaybackInputActionsCallbacks& callbacks)
    : bindings_(bindings), callbacks_(callbacks) {}

void PlaybackInputActions::setBindings(const PlaybackInputBindings& bindings) {
  bindings_ = bindings;
}

const PlaybackInputBindings& PlaybackInputActions::bindings() const { return bindings_; }

void PlaybackInputActions::setCallbacks(const PlaybackInputActionsCallbacks& callbacks) {
  callbacks_ = callbacks;
}

const PlaybackInputActionsCallbacks& PlaybackInputActions::callbacks() const {
  return callbacks_;
}

bool PlaybackInputActions::handle(const InputEvent& event) const {
  if (event.type != bindings_.trigger_event) return false;

  if (event.source_id == bindings_.pause_toggle_source_id) {
    if (callbacks_.isPlaybackRunning && !callbacks_.isPlaybackRunning(callbacks_.ctx)) {
      return true;
    }
    if (callbacks_.togglePaused) {
      const bool paused = callbacks_.togglePaused(callbacks_.ctx);
      if (callbacks_.onPauseChanged) {
        callbacks_.onPauseChanged(callbacks_.ctx, paused);
      }
    }
    return true;
  }

  if (event.source_id == bindings_.next_track_source_id) {
    if (callbacks_.requestNextTrack) callbacks_.requestNextTrack(callbacks_.ctx);
    return true;
  }

  int delta = 0;
  if (event.source_id == bindings_.volume_down_source_id) {
    delta = -1;
  } else if (event.source_id == bindings_.volume_up_source_id) {
    delta = 1;
  } else {
    return false;
  }

  if (callbacks_.stepVolume) {
    int new_value = 0;
    const bool changed = callbacks_.stepVolume(callbacks_.ctx, delta, &new_value);
    if (changed && callbacks_.onVolumeChanged) {
      callbacks_.onVolumeChanged(callbacks_.ctx, new_value);
    }
  }
  return true;
}

}  // namespace padre
