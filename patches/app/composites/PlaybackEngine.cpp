#include "PlaybackEngine.h"

namespace padre {

PlaybackEngine::PlaybackEngine(DecoderFacade& decoder,
                               IAudioSource& source,
                               BufferedI2sOutput& sink,
                               const PlaybackEngineConfig& config,
                               PlaybackControllerHooks hooks)
    : user_hooks_(hooks),
      playlist_(config.playlist_seed),
      auto_advance_(config.auto_advance),
      controller_(decoder,
                  source,
                  sink,
                  playlist_,
                  config.controller,
                  PlaybackControllerHooks{this,
                                          &PlaybackEngine::forwardSetPrebuffering,
                                          &PlaybackEngine::forwardTrackStarted}) {
  playlist_.setOrder(config.order);
}

void PlaybackEngine::setTracks(const std::vector<String>& tracks, PlayOrder order) {
  playlist_.setOrder(order);
  playlist_.setTracks(tracks);
}

bool PlaybackEngine::start(uint32_t now_ms, bool try_current_first) {
  return auto_advance_.startPlayback(controller_, !playlist_.empty(), try_current_first, now_ms);
}

PlaybackAutoAdvanceStep PlaybackEngine::step(uint32_t now_ms) {
  return auto_advance_.step(controller_, now_ms);
}

bool PlaybackEngine::isRunning() const { return controller_.isRunning(); }

bool PlaybackEngine::isPaused() const { return controller_.isPaused(); }

void PlaybackEngine::setPaused(bool paused) { controller_.setPaused(paused); }

bool PlaybackEngine::togglePaused() { return controller_.togglePaused(); }

void PlaybackEngine::stop() { controller_.stop(); }

void PlaybackEngine::requestNextTrack() { auto_advance_.requestNextTrack(); }

PlaybackController& PlaybackEngine::controller() { return controller_; }

const PlaybackController& PlaybackEngine::controller() const { return controller_; }

PlaybackAutoAdvance& PlaybackEngine::autoAdvance() { return auto_advance_; }

const PlaybackAutoAdvance& PlaybackEngine::autoAdvance() const { return auto_advance_; }

PlaylistManager& PlaybackEngine::playlist() { return playlist_; }

const PlaylistManager& PlaybackEngine::playlist() const { return playlist_; }

void PlaybackEngine::forwardSetPrebuffering(void* ctx, bool enabled) {
  auto* self = static_cast<PlaybackEngine*>(ctx);
  if (self == nullptr || self->user_hooks_.setPrebuffering == nullptr) return;
  self->user_hooks_.setPrebuffering(self->user_hooks_.ctx, enabled);
}

void PlaybackEngine::forwardTrackStarted(void* ctx, const String& path) {
  auto* self = static_cast<PlaybackEngine*>(ctx);
  if (self == nullptr) return;

  self->auto_advance_.markTrackStarted();
  if (self->user_hooks_.onTrackStarted == nullptr) return;
  self->user_hooks_.onTrackStarted(self->user_hooks_.ctx, path);
}

}  // namespace padre
