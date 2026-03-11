#pragma once

#include <Arduino.h>

#include <vector>

#include "../playback/PlaybackAutoAdvance.h"
#include "../playback/PlaybackController.h"
#include "../../media/playlist/PlaylistManager.h"

namespace padre {

struct PlaybackEngineConfig {
  PlaybackControllerConfig controller = {};
  PlaybackAutoAdvanceConfig auto_advance = {};
  PlayOrder order = PlayOrder::Sequential;
  uint32_t playlist_seed = 0;
};

class PlaybackEngine {
 public:
  PlaybackEngine(DecoderFacade& decoder,
                 IAudioSource& source,
                 BufferedI2sOutput& sink,
                 const PlaybackEngineConfig& config = {},
                 PlaybackControllerHooks hooks = {});

  void setTracks(const std::vector<String>& tracks, PlayOrder order);
  bool start(uint32_t now_ms, bool try_current_first = true);
  PlaybackAutoAdvanceStep step(uint32_t now_ms);

  bool isRunning() const;
  bool isPaused() const;
  void setPaused(bool paused);
  bool togglePaused();
  void stop();
  void requestNextTrack();

  PlaybackController& controller();
  const PlaybackController& controller() const;

  PlaybackAutoAdvance& autoAdvance();
  const PlaybackAutoAdvance& autoAdvance() const;

  PlaylistManager& playlist();
  const PlaylistManager& playlist() const;

 private:
  static void forwardSetPrebuffering(void* ctx, bool enabled);
  static void forwardTrackStarted(void* ctx, const String& path);

  PlaybackControllerHooks user_hooks_;
  PlaylistManager playlist_;
  PlaybackAutoAdvance auto_advance_;
  PlaybackController controller_;
};

}  // namespace padre
