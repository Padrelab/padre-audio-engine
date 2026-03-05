#include "PlaybackAutoAdvance.h"

namespace padre {

PlaybackAutoAdvance::PlaybackAutoAdvance(const PlaybackAutoAdvanceConfig& config)
    : config_(config) {}

void PlaybackAutoAdvance::setConfig(const PlaybackAutoAdvanceConfig& config) {
  config_ = config;
}

const PlaybackAutoAdvanceConfig& PlaybackAutoAdvance::config() const { return config_; }

void PlaybackAutoAdvance::reset() {
  was_running_ = false;
  next_requested_ = false;
  retry_at_ms_ = 0;
}

void PlaybackAutoAdvance::markTrackStarted() {
  retry_at_ms_ = 0;
  was_running_ = true;
}

void PlaybackAutoAdvance::requestNextTrack() { next_requested_ = true; }

bool PlaybackAutoAdvance::startPlayback(PlaybackController& playback,
                                        bool has_tracks,
                                        bool try_current_first,
                                        uint32_t now_ms) {
  if (!has_tracks) return false;

  bool started = false;
  if (try_current_first) {
    started = playback.playCurrentTrack();
  }
  if (!started) {
    started = playback.playNextTrack();
  }

  if (!started) {
    retry_at_ms_ = now_ms + config_.retry_delay_ms;
    return false;
  }

  markTrackStarted();
  return true;
}

PlaybackAutoAdvanceStep PlaybackAutoAdvance::step(PlaybackController& playback, uint32_t now_ms) {
  PlaybackAutoAdvanceStep result;

  if (next_requested_) {
    next_requested_ = false;
    result.handled_next_request = true;
    result.next_started = playback.playNextTrack();
    if (!result.next_started) {
      result.no_next_available = true;
    } else {
      retry_at_ms_ = 0;
      was_running_ = true;
    }
  }

  bool running = playback.service();

  if (was_running_ && !running && !playback.isPaused()) {
    if (playback.playNextTrack()) {
      running = true;
      result.next_started = true;
      retry_at_ms_ = 0;
    } else {
      retry_at_ms_ = now_ms + config_.retry_delay_ms;
      result.no_next_available = true;
    }
  }

  if (!running && !playback.isPaused() && retry_at_ms_ != 0 && now_ms >= retry_at_ms_) {
    retry_at_ms_ = 0;
    if (playback.playNextTrack()) {
      running = true;
      result.next_started = true;
    } else {
      retry_at_ms_ = now_ms + config_.retry_delay_ms;
      result.no_next_available = true;
    }
  }

  was_running_ = running;
  result.running = running;
  return result;
}

bool PlaybackAutoAdvance::hasPendingNextRequest() const { return next_requested_; }

uint32_t PlaybackAutoAdvance::retryAtMs() const { return retry_at_ms_; }

}  // namespace padre
