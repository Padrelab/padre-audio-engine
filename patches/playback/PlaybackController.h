#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#include "../decoder/DecoderFacade.h"
#include "../output/BufferedI2sOutput.h"
#include "../playlist/PlaylistManager.h"
#include "../source/IAudioSource.h"

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

struct PlaybackControllerTelemetryCallbacks {
  void* ctx = nullptr;
  void (*onServiceBegin)(void* ctx, uint32_t now_us) = nullptr;
  void (*onDecodeIteration)(void* ctx,
                            uint32_t decode_elapsed_us,
                            size_t produced_samples,
                            size_t queued_samples,
                            size_t writable_samples) = nullptr;
  void (*onServiceEnd)(void* ctx,
                       uint32_t service_elapsed_us,
                       uint32_t decode_iterations,
                       bool hit_budget) = nullptr;
};

class PlaybackController {
 public:
  PlaybackController(DecoderFacade& decoder,
                     IAudioSource& source,
                     BufferedI2sOutput& sink,
                     PlaylistManager& playlist,
                     PlaybackControllerConfig config = {},
                     PlaybackControllerHooks hooks = {},
                     PlaybackControllerTelemetryCallbacks telemetry = {});

  void setConfig(const PlaybackControllerConfig& config);
  const PlaybackControllerConfig& config() const;

  void setHooks(const PlaybackControllerHooks& hooks);
  void setTelemetryCallbacks(const PlaybackControllerTelemetryCallbacks& telemetry);

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
  PlaybackControllerTelemetryCallbacks telemetry_;
  bool paused_ = false;
};

}  // namespace padre
