#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#include "../../audio/decoder/DecoderFacade.h"
#include "../../audio/output/BufferedI2sOutput.h"
#include "../../media/playlist/PlaylistManager.h"
#include "../../media/source/IAudioSource.h"

namespace padre {

struct PlaybackControllerConfig {
  size_t prebuffer_min_samples = 8192;
  uint32_t start_prebuffer_budget_us = 30000;
  size_t start_reads_per_step = 8;

  uint32_t service_decode_budget_us = 2500;
  size_t service_reads_per_step = 8;
};

struct PlaybackControllerHooks {
  void* ctx = nullptr;
  void (*setPrebuffering)(void* ctx, bool enabled) = nullptr;
  void (*onTrackStarted)(void* ctx, const String& path) = nullptr;
};

class PlaybackController {
 public:
  PlaybackController(DecoderFacade& decoder,
                     IAudioSource& source,
                     BufferedI2sOutput& sink,
                     PlaylistManager& playlist,
                     PlaybackControllerConfig config = {},
                     PlaybackControllerHooks hooks = {});

  void setConfig(const PlaybackControllerConfig& config);
  const PlaybackControllerConfig& config() const;

  void setHooks(const PlaybackControllerHooks& hooks);

  bool isPaused() const;
  bool isRunning() const;
  void setPaused(bool paused);
  bool togglePaused();

  void stop();
  bool startTrack(const String& path);
  bool playCurrentTrack();
  bool playNextTrack();
  bool service();

 private:
  void setPrebuffering(bool enabled);

  DecoderFacade* decoder_ = nullptr;
  IAudioSource* source_ = nullptr;
  BufferedI2sOutput* sink_ = nullptr;
  PlaylistManager* playlist_ = nullptr;
  PlaybackControllerConfig config_;
  PlaybackControllerHooks hooks_;
  bool paused_ = false;
};

}  // namespace padre
