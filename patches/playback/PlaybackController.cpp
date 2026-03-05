#include "PlaybackController.h"

namespace padre {

PlaybackController::PlaybackController(DecoderFacade& decoder,
                                       IAudioSource& source,
                                       BufferedI2sOutput& sink,
                                       PlaylistManager& playlist,
                                       PlaybackControllerConfig config,
                                       PlaybackControllerHooks hooks,
                                       PlaybackControllerTelemetryCallbacks telemetry)
    : decoder_(&decoder),
      source_(&source),
      sink_(&sink),
      playlist_(&playlist),
      config_(config),
      hooks_(hooks),
      telemetry_(telemetry) {}

void PlaybackController::setConfig(const PlaybackControllerConfig& config) {
  config_ = config;
}

const PlaybackControllerConfig& PlaybackController::config() const { return config_; }

void PlaybackController::setHooks(const PlaybackControllerHooks& hooks) { hooks_ = hooks; }

void PlaybackController::setTelemetryCallbacks(
    const PlaybackControllerTelemetryCallbacks& telemetry) {
  telemetry_ = telemetry;
}

bool PlaybackController::isPaused() const { return paused_; }

bool PlaybackController::isRunning() const { return decoder_ != nullptr && decoder_->isRunning(); }

void PlaybackController::setPaused(bool paused) { paused_ = paused; }

bool PlaybackController::togglePaused() {
  paused_ = !paused_;
  return paused_;
}

void PlaybackController::stop() {
  if (decoder_) decoder_->stop();
  if (source_) source_->close();
}

bool PlaybackController::startTrack(const String& path) {
  if (decoder_ == nullptr || source_ == nullptr || sink_ == nullptr) return false;

  paused_ = false;
  stop();

  if (!source_->open(path)) return false;
  if (!decoder_->begin(*source_, *sink_, path)) {
    source_->close();
    return false;
  }

  setPrebuffering(true);
  const uint32_t prebuffer_start_us = micros();
  size_t stagnant_iters = 0;
  while (decoder_->isRunning()) {
    if (sink_->queuedSamples() >= config_.prebuffer_min_samples) break;
    if (static_cast<uint32_t>(micros() - prebuffer_start_us) >=
        config_.start_prebuffer_budget_us) {
      break;
    }

    const size_t before = sink_->queuedSamples();
    decoder_->process(config_.start_reads_per_step);
    if (sink_->queuedSamples() == before) {
      ++stagnant_iters;
      if (stagnant_iters >= 2) break;
      delay(0);
      continue;
    }
    stagnant_iters = 0;
  }
  setPrebuffering(false);
  sink_->pump();

  if (hooks_.onTrackStarted) hooks_.onTrackStarted(hooks_.ctx, path);
  return true;
}

bool PlaybackController::playCurrentTrack() {
  if (playlist_ == nullptr) return false;
  const String* track = playlist_->current();
  if (track == nullptr) return false;
  return startTrack(*track);
}

bool PlaybackController::playNextTrack() {
  if (playlist_ == nullptr || playlist_->empty()) return false;

  for (size_t attempt = 0; attempt < playlist_->size(); ++attempt) {
    const String* track = playlist_->next(true);
    if (track == nullptr) continue;
    if (startTrack(*track)) return true;
  }
  return false;
}

bool PlaybackController::service() {
  if (decoder_ == nullptr || source_ == nullptr || sink_ == nullptr) return false;
  if (!decoder_->isRunning()) return false;
  if (paused_) return true;

  const uint32_t service_start_us = micros();
  if (telemetry_.onServiceBegin) {
    telemetry_.onServiceBegin(telemetry_.ctx, service_start_us);
  }

  uint32_t decode_iters = 0;
  bool hit_budget = false;
  while (decoder_->isRunning()) {
    if (static_cast<uint32_t>(micros() - service_start_us) >=
        config_.service_decode_budget_us) {
      hit_budget = true;
      break;
    }

    sink_->pump();

    const uint32_t decode_start_us = micros();
    const size_t produced = decoder_->process(config_.service_reads_per_step);
    const uint32_t decode_elapsed_us =
        static_cast<uint32_t>(micros() - decode_start_us);
    ++decode_iters;

    sink_->pump();
    const size_t queued_samples = sink_->queuedSamples();
    const size_t writable_samples = sink_->writableSamples();

    if (telemetry_.onDecodeIteration) {
      telemetry_.onDecodeIteration(telemetry_.ctx, decode_elapsed_us, produced,
                                   queued_samples, writable_samples);
    }

    if (writable_samples == 0) break;
  }

  const uint32_t service_elapsed_us =
      static_cast<uint32_t>(micros() - service_start_us);
  if (telemetry_.onServiceEnd) {
    telemetry_.onServiceEnd(telemetry_.ctx, service_elapsed_us, decode_iters, hit_budget);
  }

  if (!decoder_->isRunning()) {
    source_->close();
    return false;
  }

  return true;
}

void PlaybackController::setPrebuffering(bool enabled) {
  if (hooks_.setPrebuffering) hooks_.setPrebuffering(hooks_.ctx, enabled);
}

}  // namespace padre
