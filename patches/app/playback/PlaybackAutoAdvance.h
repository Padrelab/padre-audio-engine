#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "PlaybackController.h"

namespace padre {

struct PlaybackAutoAdvanceConfig {
  uint32_t retry_delay_ms = 500;
};

struct PlaybackAutoAdvanceStep {
  bool running = false;
  bool handled_next_request = false;
  bool next_started = false;
  bool no_next_available = false;
};

class PlaybackAutoAdvance {
 public:
  explicit PlaybackAutoAdvance(const PlaybackAutoAdvanceConfig& config = {});

  void setConfig(const PlaybackAutoAdvanceConfig& config);
  const PlaybackAutoAdvanceConfig& config() const;

  void reset();
  void markTrackStarted();
  void requestNextTrack();

  bool startPlayback(PlaybackController& playback,
                     bool has_tracks,
                     bool try_current_first,
                     uint32_t now_ms);
  PlaybackAutoAdvanceStep step(PlaybackController& playback, uint32_t now_ms);

  bool hasPendingNextRequest() const;
  uint32_t retryAtMs() const;

 private:
  PlaybackAutoAdvanceConfig config_;
  bool was_running_ = false;
  bool next_requested_ = false;
  uint32_t retry_at_ms_ = 0;
};

}  // namespace padre
