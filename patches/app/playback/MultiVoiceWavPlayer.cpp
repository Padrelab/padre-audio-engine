#include "MultiVoiceWavPlayer.h"

#include <algorithm>
#include <cstring>

#include "../../audio/decoder/WavDecoder.h"
#include "../../media/library/AudioFileScanner.h"
#include "../../media/source/FsAudioSource.h"

namespace padre {

namespace {

constexpr const char* kWavExtensions[] = {
    ".wav",
};

constexpr size_t kWavExtensionCount = sizeof(kWavExtensions) / sizeof(kWavExtensions[0]);

size_t alignStereoSamples(size_t sample_count) {
  return sample_count & ~static_cast<size_t>(1);
}

int32_t multiVoiceClampPcm32Sample(int64_t sample) {
  if (sample > static_cast<int64_t>(INT32_MAX)) return INT32_MAX;
  if (sample < static_cast<int64_t>(INT32_MIN)) return INT32_MIN;
  return static_cast<int32_t>(sample);
}

uint32_t stereoSamplesToMs(size_t sample_count, uint32_t sample_rate) {
  if (sample_rate == 0) return 0;

  const uint64_t numerator = static_cast<uint64_t>(sample_count) * 1000ull;
  const uint64_t denominator = static_cast<uint64_t>(sample_rate) * 2ull;
  return static_cast<uint32_t>(numerator / denominator);
}

bool inspectWavTrack(fs::FS& fs,
                     const char* source_type_name,
                     const String& path,
                     uint32_t& sample_rate_out) {
  FsAudioSource source(fs, FsAudioSourceConfig{source_type_name});
  WavDecoder decoder;

  if (!source.begin()) return false;
  if (!source.open(path)) return false;

  const bool ok = decoder.begin(source);
  if (ok) sample_rate_out = decoder.streamInfo().sample_rate;

  decoder.stop();
  source.close();
  return ok;
}

size_t scanWavFiles(fs::FS& fs,
                    uint8_t max_dir_depth,
                    const char* dir_path,
                    std::vector<String>& out_tracks) {
  out_tracks.clear();

  const AudioFileScannerOptions options{
      max_dir_depth,
      kWavExtensions,
      kWavExtensionCount,
  };

  AudioFileScanner scanner(fs);
  scanner.scan(dir_path, out_tracks, options);
  std::sort(out_tracks.begin(), out_tracks.end(), [](const String& lhs, const String& rhs) {
    return lhs.compareTo(rhs) < 0;
  });
  return out_tracks.size();
}

}  // namespace

MultiVoiceWavPlayerRuntimeProfile deriveAdaptiveRuntimeProfile(
    const MultiVoiceWavPlayerRuntimeProfile& base,
    const MultiVoiceWavPlayerAdaptiveProfileRules& rules,
    size_t voice_count,
    size_t loop_voice_count,
    size_t oneshot_voice_count,
    uint32_t sample_rate) {
  MultiVoiceWavPlayerRuntimeProfile profile = base;
  profile.startup_sample_rate_hint = sample_rate;

  if (!rules.enabled) return profile;

  const size_t weighted_voice_count =
      loop_voice_count + (oneshot_voice_count * static_cast<size_t>(rules.oneshot_voice_weight));
  const size_t safe_weighted_count = std::max<size_t>(1, weighted_voice_count == 0 ? voice_count : weighted_voice_count);

  profile.mix_chunk_samples = std::max(
      alignStereoSamples(profile.mix_chunk_samples),
      alignStereoSamples(rules.mix_chunk_per_weighted_voice_samples * safe_weighted_count));
  profile.sink_queue_samples = std::max(
      alignStereoSamples(profile.sink_queue_samples),
      alignStereoSamples(rules.sink_queue_per_weighted_voice_samples * safe_weighted_count));
  profile.startup_prebuffer_samples = std::max(
      alignStereoSamples(profile.startup_prebuffer_samples),
      alignStereoSamples(rules.startup_prebuffer_per_weighted_voice_samples * safe_weighted_count));
  profile.queue_refill_target_samples = std::max(
      alignStereoSamples(profile.queue_refill_target_samples),
      alignStereoSamples(rules.queue_refill_per_weighted_voice_samples * safe_weighted_count));
  profile.loop_track_switch_retained_queue_samples = std::max(
      alignStereoSamples(profile.loop_track_switch_retained_queue_samples),
      alignStereoSamples(rules.loop_switch_retained_per_loop_voice_samples *
                         std::max<size_t>(1, loop_voice_count)));
  profile.service_budget_us = std::max(
      profile.service_budget_us,
      static_cast<uint32_t>(rules.service_budget_per_weighted_voice_us * safe_weighted_count));

  if (sample_rate > rules.high_sample_rate_threshold) {
    profile.mix_chunk_samples += alignStereoSamples(rules.mix_chunk_high_sample_rate_bonus_samples);
    profile.sink_queue_samples += alignStereoSamples(rules.sink_queue_high_sample_rate_bonus_samples);
    profile.startup_prebuffer_samples +=
        alignStereoSamples(rules.startup_prebuffer_high_sample_rate_bonus_samples);
    profile.queue_refill_target_samples +=
        alignStereoSamples(rules.queue_refill_high_sample_rate_bonus_samples);
    profile.loop_track_switch_retained_queue_samples +=
        alignStereoSamples(rules.loop_switch_retained_high_sample_rate_bonus_samples);
    profile.service_budget_us += rules.service_budget_high_sample_rate_bonus_us;
  }

  profile.startup_prebuffer_samples =
      std::min(profile.startup_prebuffer_samples, profile.sink_queue_samples);
  profile.queue_refill_target_samples =
      std::min(profile.queue_refill_target_samples, profile.sink_queue_samples);
  profile.loop_track_switch_retained_queue_samples =
      std::min(profile.loop_track_switch_retained_queue_samples, profile.sink_queue_samples);
  if (profile.oneshot_trigger_retained_queue_samples > 0) {
    profile.oneshot_trigger_retained_queue_samples =
        std::min(alignStereoSamples(profile.oneshot_trigger_retained_queue_samples),
                 profile.sink_queue_samples);
  }
  if (profile.oneshot_queue_refill_target_samples > 0) {
    profile.oneshot_queue_refill_target_samples =
        std::min(alignStereoSamples(profile.oneshot_queue_refill_target_samples),
                 profile.sink_queue_samples);
    if (profile.oneshot_trigger_retained_queue_samples > 0 &&
        profile.oneshot_queue_refill_target_samples <=
            profile.oneshot_trigger_retained_queue_samples) {
      profile.oneshot_queue_refill_target_samples =
          std::min(profile.sink_queue_samples,
                   profile.oneshot_trigger_retained_queue_samples + static_cast<size_t>(2));
    }
  }
  return profile;
}

MultiVoiceWavPlayer::MultiVoiceWavPlayer(Print& serial,
                                         FsStorageBackend& storage,
                                         std::vector<MultiVoiceWavPlayerVoiceSpec> voice_specs,
                                         MultiVoiceWavPlayerConfig config)
    : serial_(&serial),
      storage_(&storage),
      config_(config),
      active_runtime_(config.runtime),
      volume_(config.volume.initial) {
  voice_slots_.reserve(voice_specs.size());
  for (auto& spec : voice_specs) {
    VoiceSlot slot;
    slot.spec = spec;
    voice_slots_.push_back(std::move(slot));
  }

  pending_next_requests_.assign(voice_slots_.size(), 0);
  pending_trigger_requests_.assign(voice_slots_.size(), 0);
  pending_selected_track_indices_.assign(voice_slots_.size(), -1);
  pending_selected_track_trigger_requests_.assign(voice_slots_.size(), 0);
}

bool MultiVoiceWavPlayer::begin() {
  serial_->println(config_.player_name);
  printConfigSummary();

  if (voice_slots_.empty()) {
    serial_->println("No voice specs configured");
    return false;
  }

  if (!initStorage()) return false;
  applyVolume();

  uint32_t sample_rate = 0;
  if (!preparePlaylists(sample_rate)) return false;

  active_runtime_ = deriveAdaptiveRuntimeProfile(config_.runtime,
                                                 config_.adaptive,
                                                 voice_slots_.size(),
                                                 loopVoiceCount(),
                                                 oneShotVoiceCount(),
                                                 sample_rate);
  serial_->printf("Runtime: profile=%s voices=%u loops=%u oneshots=%u sample_rate=%lu\n",
                  active_runtime_.name,
                  static_cast<unsigned>(voice_slots_.size()),
                  static_cast<unsigned>(loopVoiceCount()),
                  static_cast<unsigned>(oneShotVoiceCount()),
                  static_cast<unsigned long>(sample_rate));

  if (!buildAudioGraph(sample_rate)) return false;
  if (!startAudio(sample_rate)) return false;
  if (!startAudioTask()) return false;

  audio_ready_ = true;
  notifyAudioTask();
  return true;
}

void MultiVoiceWavPlayer::loop() {
  if (!audio_ready_) {
    delay(100);
    return;
  }

  delay(0);
}

size_t MultiVoiceWavPlayer::voiceCount() const { return voice_slots_.size(); }

WavVoiceMode MultiVoiceWavPlayer::voiceMode(size_t voice_index) const {
  if (voice_index >= voice_slots_.size()) return WavVoiceMode::Loop;
  return voice_slots_[voice_index].spec.voice.mode;
}

bool MultiVoiceWavPlayer::activateVoice(size_t voice_index) {
  if (voiceMode(voice_index) == WavVoiceMode::OneShot) {
    return triggerVoice(voice_index);
  }
  return requestNextTrack(voice_index);
}

bool MultiVoiceWavPlayer::requestNextTrack(size_t voice_index, size_t steps) {
  if (voice_index >= voice_slots_.size() || steps == 0) return false;
  queueNextTrackRequest(voice_index, steps);
  return true;
}

bool MultiVoiceWavPlayer::triggerVoice(size_t voice_index) {
  if (voice_index >= voice_slots_.size()) return false;
  queueTriggerRequest(voice_index);
  return true;
}

bool MultiVoiceWavPlayer::triggerVoiceTrack(size_t voice_index, size_t track_index) {
  if (voice_index >= voice_slots_.size()) return false;
  if (track_index >= voice_slots_[voice_index].tracks.size()) return false;
  queueTriggerTrackRequest(voice_index, track_index);
  return true;
}

bool MultiVoiceWavPlayer::stepVolume(int delta) {
  if (delta == 0) return false;
  queueVolumeDelta(delta);
  return true;
}

int MultiVoiceWavPlayer::volume() const { return volume_; }

const MultiVoiceWavPlayerRuntimeProfile& MultiVoiceWavPlayer::activeRuntimeProfile() const {
  return active_runtime_;
}

size_t MultiVoiceWavPlayer::queuedOutputSamples() const {
  return sink_ == nullptr ? 0 : sink_->queuedSamples();
}

const String& MultiVoiceWavPlayer::activeTrack(size_t voice_index) const {
  if (voice_index >= voice_slots_.size() || voice_slots_[voice_index].voice == nullptr) {
    return empty_track_;
  }
  return voice_slots_[voice_index].voice->activeTrack();
}

int MultiVoiceWavPlayer::findTrackIndex(size_t voice_index, const char* path) const {
  if (voice_index >= voice_slots_.size() || path == nullptr || path[0] == '\0') return -1;

  const String needle(path);
  const bool exact_path = needle.startsWith("/");
  const String suffix = exact_path ? needle : String("/") + needle;
  const auto& tracks = voice_slots_[voice_index].tracks;
  for (size_t i = 0; i < tracks.size(); ++i) {
    if (tracks[i] == needle) return static_cast<int>(i);
    if (!exact_path && tracks[i].endsWith(suffix)) return static_cast<int>(i);
  }
  return -1;
}

int32_t MultiVoiceWavPlayer::applyVolumeSampleThunk(void* ctx, int32_t sample) {
  auto* self = static_cast<MultiVoiceWavPlayer*>(ctx);
  return self == nullptr ? sample : self->applyVolumeToSample(sample);
}

void MultiVoiceWavPlayer::prepareOutputSamplesThunk(void* ctx,
                                                    const int32_t* input,
                                                    int32_t* output,
                                                    size_t sample_count) {
  auto* self = static_cast<MultiVoiceWavPlayer*>(ctx);
  if (self == nullptr || output == nullptr) return;
  self->prepareOutputSamples(input, output, sample_count);
}

void MultiVoiceWavPlayer::commitOutputSamplesThunk(void* ctx, size_t written_samples) {
  auto* self = static_cast<MultiVoiceWavPlayer*>(ctx);
  if (self == nullptr) return;
  self->commitOutputSamples(written_samples);
}

void MultiVoiceWavPlayer::audioTaskEntry(void* ctx) {
  auto* self = static_cast<MultiVoiceWavPlayer*>(ctx);
  if (self == nullptr) {
    vTaskDelete(nullptr);
    return;
  }

  self->audioTaskMain();
}

int32_t MultiVoiceWavPlayer::applyVolumeToSample(int64_t sample) const {
  const int64_t scaled = (static_cast<int64_t>(sample) * volume_gain_q15_) >> 15;
  return multiVoiceClampPcm32Sample(scaled);
}

void MultiVoiceWavPlayer::prepareOutputSamples(const int32_t* input,
                                               int32_t* output,
                                               size_t sample_count) const {
  if (output == nullptr || sample_count == 0) return;

  for (size_t i = 0; i < sample_count; ++i) {
    const int32_t base_sample = input == nullptr ? 0 : input[i];
    const int64_t with_oneshot =
        static_cast<int64_t>(base_sample) + static_cast<int64_t>(peekOneShotOverlaySample(i));
    output[i] = applyVolumeToSample(with_oneshot);
  }
}

void MultiVoiceWavPlayer::commitOutputSamples(size_t written_samples) {
  consumeOneShotOverlaySamples(written_samples);
}

void MultiVoiceWavPlayer::updateVolumeGain() {
  const int32_t volume = static_cast<int32_t>(volume_);
  const int32_t max_volume = static_cast<int32_t>(config_.volume.max);
  if (max_volume <= 0) {
    volume_gain_q15_ = 0;
    return;
  }

  const int32_t gain_num = volume * volume;
  const int32_t gain_den = max_volume * max_volume;
  volume_gain_q15_ = (gain_num * 32767 + (gain_den / 2)) / gain_den;
}

void MultiVoiceWavPlayer::applyVolume() {
  updateVolumeGain();
  serial_->printf("Volume: %d/%d\n", volume_, config_.volume.max);
}

bool MultiVoiceWavPlayer::stepVolumeInternal(int delta) {
  const int next_volume =
      std::max(config_.volume.min, std::min(config_.volume.max, volume_ + delta));
  if (next_volume == volume_) return false;

  volume_ = next_volume;
  applyVolume();
  return true;
}

void MultiVoiceWavPlayer::printConfigSummary() const {
  serial_->printf("Build:  %s\n", config_.build_tag);
  storage_->printConfig(*serial_);
  serial_->printf("I2S:    bclk=%u lrck=%u dout=%u\n",
                  config_.pins.i2s_bclk,
                  config_.pins.i2s_lrc,
                  config_.pins.i2s_dout);
  serial_->printf("Audio:  base_queue=%u base_prebuffer=%u base_target=%u dma=%ux%u work=%u\n",
                  static_cast<unsigned>(config_.runtime.sink_queue_samples),
                  static_cast<unsigned>(config_.runtime.startup_prebuffer_samples),
                  static_cast<unsigned>(config_.runtime.queue_refill_target_samples),
                  static_cast<unsigned>(config_.runtime.i2s_dma_desc_num),
                  static_cast<unsigned>(config_.runtime.i2s_dma_frame_num),
                  static_cast<unsigned>(config_.runtime.i2s_work_samples));
  if (config_.runtime.oneshot_trigger_retained_queue_samples > 0 ||
      config_.runtime.oneshot_queue_refill_target_samples > 0) {
    serial_->printf("Audio:  oneshot_keep=%u oneshot_target=%u\n",
                    static_cast<unsigned>(config_.runtime.oneshot_trigger_retained_queue_samples),
                    static_cast<unsigned>(config_.runtime.oneshot_queue_refill_target_samples));
  }
}

bool MultiVoiceWavPlayer::initStorage() { return storage_->begin(*serial_); }

bool MultiVoiceWavPlayer::preparePlaylists(uint32_t& out_sample_rate) {
  std::vector<std::vector<String>> candidates(voice_slots_.size());

  for (size_t i = 0; i < voice_slots_.size(); ++i) {
    auto& slot = voice_slots_[i];
    scanWavFiles(storage_->fs(), config_.max_dir_depth, slot.spec.tracks_dir, candidates[i]);
    serial_->printf("[%s] scanned %s: %u file(s)\n",
                    slot.spec.label,
                    slot.spec.tracks_dir,
                    static_cast<unsigned>(candidates[i].size()));
  }

  for (const auto& track_list : candidates) {
    for (const auto& path : track_list) {
      uint32_t sample_rate = 0;
      if (!inspectWavTrack(storage_->fs(), storage_->typeName(), path, sample_rate)) continue;
      out_sample_rate = sample_rate;
      break;
    }
    if (out_sample_rate != 0) break;
  }

  if (out_sample_rate == 0) {
    serial_->println("No valid WAV files found for any voice");
    return false;
  }

  size_t playable_voice_count = 0;
  for (size_t i = 0; i < voice_slots_.size(); ++i) {
    auto& slot = voice_slots_[i];
    slot.tracks.clear();

    for (const auto& path : candidates[i]) {
      uint32_t sample_rate = 0;
      if (!inspectWavTrack(storage_->fs(), storage_->typeName(), path, sample_rate)) {
        serial_->printf("[%s] skipped invalid WAV: %s\n", slot.spec.label, path.c_str());
        continue;
      }

      if (sample_rate != out_sample_rate) {
        serial_->printf("[%s] skipped sample rate mismatch: %s (%lu Hz)\n",
                        slot.spec.label,
                        path.c_str(),
                        static_cast<unsigned long>(sample_rate));
        continue;
      }

      slot.tracks.push_back(path);
    }

    serial_->printf("[%s] %u playable track(s)\n",
                    slot.spec.label,
                    static_cast<unsigned>(slot.tracks.size()));
    for (const auto& track : slot.tracks) {
      serial_->printf("  - %s\n", track.c_str());
    }

    if (!slot.tracks.empty()) {
      ++playable_voice_count;
    } else {
      serial_->printf("[%s] warning: no playable WAV files in %s\n",
                      slot.spec.label,
                      slot.spec.tracks_dir);
    }
  }

  return playable_voice_count > 0;
}

bool MultiVoiceWavPlayer::buildAudioGraph(uint32_t sample_rate) {
  mix_buffer_.assign(active_runtime_.mix_chunk_samples, 0);

  i2s_io_.reset(new Esp32StdI2sOutputIo(
      Esp32StdI2sPins{
          static_cast<int8_t>(config_.pins.i2s_bclk),
          static_cast<int8_t>(config_.pins.i2s_lrc),
          static_cast<int8_t>(config_.pins.i2s_dout),
          -1,
      },
      Esp32StdI2sOutputConfig{
          active_runtime_.i2s_dma_desc_num,
          active_runtime_.i2s_dma_frame_num,
          2,
          active_runtime_.i2s_write_timeout_ms,
          active_runtime_.i2s_work_samples,
      },
      Esp32StdI2sSampleTransform{
          this,
          &MultiVoiceWavPlayer::applyVolumeSampleThunk,
          &MultiVoiceWavPlayer::prepareOutputSamplesThunk,
          &MultiVoiceWavPlayer::commitOutputSamplesThunk,
      }));
  if (i2s_io_ == nullptr) {
    serial_->println("I2S output allocation failed");
    return false;
  }

  sink_.reset(new BufferedI2sOutput(
      i2s_io_->asIo(),
      I2sOutputConfig{
          active_runtime_.sink_queue_samples,
          active_runtime_.sink_watermark_samples,
      }));
  if (sink_ == nullptr) {
    serial_->println("I2S sink allocation failed");
    return false;
  }

  mixer_.reset(new VoiceMixer(voice_slots_.size()));
  if (mixer_ == nullptr) {
    serial_->println("Voice mixer allocation failed");
    return false;
  }
  mixer_->setGlobalGain(1.0f);

  if (lowLatencyOneShotEnabled() && oneShotVoiceCount() > 0) {
    oneshot_mixer_.reset(new VoiceMixer(voice_slots_.size()));
    if (oneshot_mixer_ == nullptr) {
      serial_->println("One-shot mixer allocation failed");
      return false;
    }
    oneshot_mixer_->setGlobalGain(1.0f);

    const size_t overlay_capacity = std::max(currentOneShotOverlayTargetSamples(),
                                             active_runtime_.mix_chunk_samples);
    if (!ensureOneShotOverlayQueueCapacity(overlay_capacity)) {
      serial_->println("One-shot overlay queue allocation failed");
      return false;
    }
  } else {
    oneshot_mixer_.reset();
    oneshot_overlay_queue_.clear();
    oneshot_overlay_queue_head_ = 0;
    oneshot_overlay_queue_tail_ = 0;
    oneshot_overlay_queued_samples_ = 0;
  }

  for (size_t i = 0; i < voice_slots_.size(); ++i) {
    auto& slot = voice_slots_[i];
    slot.voice.reset(new WavVoice(slot.spec.label, storage_->fs(), storage_->typeName(), slot.spec.voice));
    if (slot.voice == nullptr || !slot.voice->configure(sample_rate)) {
      serial_->printf("[%s] voice configuration failed\n", slot.spec.label);
      return false;
    }

    slot.voice->setTracks(slot.tracks);
    if (slot.spec.voice.mode == WavVoiceMode::OneShot && lowLatencyOneShotEnabled()) {
      if (oneshot_mixer_ != nullptr) {
        oneshot_mixer_->setVoiceGain(i, slot.spec.gain);
      }
    } else {
      mixer_->attachSource(i, slot.voice.get());
      mixer_->setVoiceGain(i, slot.spec.gain);
    }
  }

  return true;
}

bool MultiVoiceWavPlayer::startAudio(uint32_t sample_rate) {
  if (sink_ == nullptr || i2s_io_ == nullptr) return false;

  if (!sink_->begin(DecoderConfig{sample_rate, 16, true})) {
    serial_->println("I2S sink init failed");
    return false;
  }

  i2s_io_->setPrebuffering(true);
  const uint32_t prefill_start_ms = millis();
  while (sink_->queuedSamples() < active_runtime_.startup_prebuffer_samples) {
    const size_t writable_samples = alignStereoSamples(sink_->writableSamples());
    if (writable_samples < 2) break;

    const size_t request = std::min(active_runtime_.mix_chunk_samples, writable_samples);
    const size_t mixed = mixVoices(request);
    if (mixed == 0) break;

    const size_t written = writeMixedSamples(mixed);
    if (written == 0) break;

    if (static_cast<uint32_t>(millis() - prefill_start_ms) >
        active_runtime_.startup_prebuffer_budget_ms) {
      break;
    }
  }
  i2s_io_->setPrebuffering(false);
  pumpSink();

  serial_->printf("Audio started at %lu Hz, queued=%lu samples\n",
                  static_cast<unsigned long>(sample_rate),
                  static_cast<unsigned long>(sink_->queuedSamples()));
  serial_->printf(
      "Runtime: queue=%u prebuffer=%u target=%u loop_retain=%u service_us=%lu\n",
      static_cast<unsigned>(active_runtime_.sink_queue_samples),
      static_cast<unsigned>(active_runtime_.startup_prebuffer_samples),
      static_cast<unsigned>(active_runtime_.queue_refill_target_samples),
      static_cast<unsigned>(active_runtime_.loop_track_switch_retained_queue_samples),
      static_cast<unsigned long>(active_runtime_.service_budget_us));
  if (active_runtime_.oneshot_trigger_retained_queue_samples > 0 ||
      active_runtime_.oneshot_queue_refill_target_samples > 0) {
    serial_->printf("Runtime: oneshot_keep=%u oneshot_target=%u\n",
                    static_cast<unsigned>(active_runtime_.oneshot_trigger_retained_queue_samples),
                    static_cast<unsigned>(active_runtime_.oneshot_queue_refill_target_samples));
  }
  serial_->printf("Runtime: retain_ms@%luk=%lums\n",
                  static_cast<unsigned long>(sample_rate / 1000),
                  static_cast<unsigned long>(stereoSamplesToMs(
                      active_runtime_.loop_track_switch_retained_queue_samples,
                      sample_rate)));
  return sink_->queuedSamples() > 0;
}

bool MultiVoiceWavPlayer::startAudioTask() {
  if (audio_task_handle_ != nullptr) return true;

  BaseType_t result = xTaskCreatePinnedToCore(audioTaskEntry,
                                              "audio_service",
                                              active_runtime_.audio_task_stack_bytes,
                                              this,
                                              active_runtime_.audio_task_priority,
                                              &audio_task_handle_,
                                              active_runtime_.audio_task_core);
  if (result != pdPASS) {
    audio_task_handle_ = nullptr;
    serial_->println("Audio task create failed");
    return false;
  }

  return true;
}

void MultiVoiceWavPlayer::audioTaskMain() {
  for (;;) {
    if (!audio_ready_) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
      continue;
    }

    applyPendingControls(millis());
    serviceAudio();

    const size_t refill_target_samples = currentQueueRefillTargetSamples();
    const bool idle = !hasPendingControls() && sink_ != nullptr &&
                      sink_->queuedSamples() >= refill_target_samples && !sink_->pumpRequested();
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(idle ? 2 : active_runtime_.audio_task_loop_delay_ms));
  }
}

void MultiVoiceWavPlayer::notifyAudioTask() {
  if (audio_task_handle_ != nullptr) {
    xTaskNotifyGive(audio_task_handle_);
  }
}

void MultiVoiceWavPlayer::queueNextTrackRequest(size_t voice_index, size_t steps) {
  if (voice_index >= pending_next_requests_.size()) return;

  portENTER_CRITICAL(&control_mux_);
  const uint32_t current = pending_next_requests_[voice_index];
  const uint32_t limited_steps = static_cast<uint32_t>(std::min<size_t>(steps, UINT32_MAX));
  pending_next_requests_[voice_index] =
      (UINT32_MAX - current < limited_steps) ? UINT32_MAX : current + limited_steps;
  portEXIT_CRITICAL(&control_mux_);
  notifyAudioTask();
}

void MultiVoiceWavPlayer::queueTriggerRequest(size_t voice_index) {
  if (voice_index >= pending_trigger_requests_.size()) return;

  portENTER_CRITICAL(&control_mux_);
  if (pending_trigger_requests_[voice_index] < UINT32_MAX) {
    ++pending_trigger_requests_[voice_index];
  }
  portEXIT_CRITICAL(&control_mux_);
  notifyAudioTask();
}

void MultiVoiceWavPlayer::queueTriggerTrackRequest(size_t voice_index, size_t track_index) {
  if (voice_index >= pending_selected_track_indices_.size()) return;

  portENTER_CRITICAL(&control_mux_);
  pending_selected_track_indices_[voice_index] =
      static_cast<int32_t>(std::min<size_t>(track_index, INT32_MAX));
  pending_selected_track_trigger_requests_[voice_index] = 1;
  portEXIT_CRITICAL(&control_mux_);
  notifyAudioTask();
}

void MultiVoiceWavPlayer::queueVolumeDelta(int delta) {
  portENTER_CRITICAL(&control_mux_);
  const int32_t updated_delta = pending_volume_delta_ + static_cast<int32_t>(delta);
  const int32_t max_volume = static_cast<int32_t>(config_.volume.max);
  pending_volume_delta_ =
      std::max<int32_t>(-max_volume, std::min<int32_t>(max_volume, updated_delta));
  portEXIT_CRITICAL(&control_mux_);
  notifyAudioTask();
}

MultiVoiceWavPlayer::PendingControlState MultiVoiceWavPlayer::takePendingControls() {
  PendingControlState pending;
  portENTER_CRITICAL(&control_mux_);
  pending.next_requests = pending_next_requests_;
  pending.trigger_requests = pending_trigger_requests_;
  pending.selected_track_indices = pending_selected_track_indices_;
  pending.selected_track_trigger_requests = pending_selected_track_trigger_requests_;
  pending.volume_delta = pending_volume_delta_;
  std::fill(pending_next_requests_.begin(), pending_next_requests_.end(), 0);
  std::fill(pending_trigger_requests_.begin(), pending_trigger_requests_.end(), 0);
  std::fill(pending_selected_track_indices_.begin(), pending_selected_track_indices_.end(), -1);
  std::fill(pending_selected_track_trigger_requests_.begin(),
            pending_selected_track_trigger_requests_.end(),
            0);
  pending_volume_delta_ = 0;
  portEXIT_CRITICAL(&control_mux_);
  return pending;
}

bool MultiVoiceWavPlayer::hasPendingControls() {
  portENTER_CRITICAL(&control_mux_);
  const bool has_volume = pending_volume_delta_ != 0;
  bool has_voice = false;
  for (size_t i = 0; i < pending_next_requests_.size(); ++i) {
    if (pending_next_requests_[i] != 0 || pending_trigger_requests_[i] != 0 ||
        pending_selected_track_indices_[i] >= 0 ||
        pending_selected_track_trigger_requests_[i] != 0) {
      has_voice = true;
      break;
    }
  }
  portEXIT_CRITICAL(&control_mux_);
  return has_volume || has_voice;
}

void MultiVoiceWavPlayer::applyPendingControls(uint32_t now_ms) {
  PendingControlState pending = takePendingControls();

  for (size_t i = 0; i < voice_slots_.size(); ++i) {
    auto& slot = voice_slots_[i];
    if (slot.voice == nullptr) continue;
    const bool has_selected_track =
        pending.selected_track_indices.size() > i && pending.selected_track_indices[i] >= 0;
    const bool has_selected_track_trigger =
        pending.selected_track_trigger_requests.size() > i &&
        pending.selected_track_trigger_requests[i] != 0;

    if (has_selected_track) {
      slot.voice->selectTrackIndex(static_cast<size_t>(pending.selected_track_indices[i]));
    }

    if (!has_selected_track_trigger && pending.next_requests.size() > i &&
        pending.next_requests[i] != 0) {
      if (slot.voice->mode() == WavVoiceMode::Loop) {
        slot.voice->queueNextTracks(pending.next_requests[i], now_ms);
      } else {
        slot.voice->selectNextTracks(pending.next_requests[i]);
      }
    }

    if (has_selected_track_trigger) {
      if (slot.voice->trigger() && mixer_ != nullptr) {
        if (slot.voice->mode() == WavVoiceMode::OneShot) {
          if (lowLatencyOneShotEnabled() && oneshot_mixer_ != nullptr) {
            oneshot_mixer_->attachSource(i, slot.voice.get());
            primeOneShotOverlay(currentOneShotOverlayPrimeSamples());
          } else {
            mixer_->attachSource(i, slot.voice.get());
          }
        } else {
          mixer_->attachSource(i, slot.voice.get());
        }
      }
      continue;
    }

    if (pending.trigger_requests.size() > i && pending.trigger_requests[i] != 0) {
      if (slot.voice->trigger() && mixer_ != nullptr) {
        if (slot.voice->mode() == WavVoiceMode::OneShot) {
          if (lowLatencyOneShotEnabled() && oneshot_mixer_ != nullptr) {
            oneshot_mixer_->attachSource(i, slot.voice.get());
            primeOneShotOverlay(currentOneShotOverlayPrimeSamples());
          } else {
            mixer_->attachSource(i, slot.voice.get());
          }
        } else {
          mixer_->attachSource(i, slot.voice.get());
        }
      }
    }
  }

  if (pending.volume_delta != 0) {
    stepVolumeInternal(static_cast<int>(pending.volume_delta));
  }
}

size_t MultiVoiceWavPlayer::primeOneShotOverlay(size_t target_samples) {
  if (!lowLatencyOneShotEnabled() || oneshot_mixer_ == nullptr || mix_buffer_.empty()) {
    return 0;
  }

  const size_t aligned_target = alignStereoSamples(target_samples);
  size_t queued_samples = oneShotOverlayQueuedSamples();
  size_t produced_total = 0;

  while (queued_samples < aligned_target) {
    const size_t free_samples = oneshot_overlay_queue_.size() - queued_samples;
    const size_t request_samples =
        alignStereoSamples(std::min(mix_buffer_.size(), free_samples));
    if (request_samples < 2) break;

    const size_t produced_samples = alignStereoSamples(oneshot_mixer_->mix(
        mix_buffer_.data(), request_samples));
    if (produced_samples == 0) break;
    if (!pushOneShotOverlaySamples(mix_buffer_.data(), produced_samples)) break;

    produced_total += produced_samples;
    queued_samples += produced_samples;
    if (produced_samples < request_samples) break;
  }

  return produced_total;
}

size_t MultiVoiceWavPlayer::currentOneShotOverlayTargetSamples() const {
  const size_t configured_target = active_runtime_.oneshot_queue_refill_target_samples > 0
                                       ? active_runtime_.oneshot_queue_refill_target_samples
                                       : active_runtime_.oneshot_trigger_retained_queue_samples;
  return alignStereoSamples(
      std::min(configured_target, active_runtime_.sink_queue_samples));
}

size_t MultiVoiceWavPlayer::currentOneShotOverlayPrimeSamples() const {
  const size_t configured_prime = active_runtime_.oneshot_trigger_retained_queue_samples > 0
                                      ? active_runtime_.oneshot_trigger_retained_queue_samples
                                      : currentOneShotOverlayTargetSamples();
  return alignStereoSamples(
      std::min(configured_prime, currentOneShotOverlayTargetSamples()));
}

bool MultiVoiceWavPlayer::ensureOneShotOverlayQueueCapacity(size_t capacity_samples) {
  const size_t aligned_capacity = alignStereoSamples(std::max<size_t>(capacity_samples, 2));
  if (oneshot_overlay_queue_.size() >= aligned_capacity) return true;

  oneshot_overlay_queue_.assign(aligned_capacity, 0);
  oneshot_overlay_queue_head_ = 0;
  oneshot_overlay_queue_tail_ = 0;
  oneshot_overlay_queued_samples_ = 0;
  return oneshot_overlay_queue_.size() >= aligned_capacity;
}

size_t MultiVoiceWavPlayer::oneShotOverlayQueuedSamples() const {
  return oneshot_overlay_queued_samples_;
}

int32_t MultiVoiceWavPlayer::peekOneShotOverlaySample(size_t offset_samples) const {
  if (offset_samples >= oneshot_overlay_queued_samples_ || oneshot_overlay_queue_.empty()) {
    return 0;
  }

  const size_t index =
      (oneshot_overlay_queue_tail_ + offset_samples) % oneshot_overlay_queue_.size();
  return oneshot_overlay_queue_[index];
}

bool MultiVoiceWavPlayer::pushOneShotOverlaySamples(const int32_t* samples, size_t sample_count) {
  if (samples == nullptr || sample_count == 0) return true;
  if (oneshot_overlay_queue_.empty()) return false;
  if ((sample_count & 1u) != 0u) return false;

  const size_t free_samples = oneshot_overlay_queue_.size() - oneshot_overlay_queued_samples_;
  if (sample_count > free_samples) return false;

  const size_t first_chunk =
      std::min(sample_count, oneshot_overlay_queue_.size() - oneshot_overlay_queue_head_);
  memcpy(oneshot_overlay_queue_.data() + oneshot_overlay_queue_head_,
         samples,
         first_chunk * sizeof(int32_t));
  oneshot_overlay_queue_head_ =
      (oneshot_overlay_queue_head_ + first_chunk) % oneshot_overlay_queue_.size();
  oneshot_overlay_queued_samples_ += first_chunk;

  const size_t remain = sample_count - first_chunk;
  if (remain == 0) return true;

  memcpy(oneshot_overlay_queue_.data() + oneshot_overlay_queue_head_,
         samples + first_chunk,
         remain * sizeof(int32_t));
  oneshot_overlay_queue_head_ =
      (oneshot_overlay_queue_head_ + remain) % oneshot_overlay_queue_.size();
  oneshot_overlay_queued_samples_ += remain;
  return true;
}

int32_t MultiVoiceWavPlayer::popOneShotOverlaySample() {
  if (oneshot_overlay_queued_samples_ == 0 || oneshot_overlay_queue_.empty()) return 0;

  const int32_t sample = oneshot_overlay_queue_[oneshot_overlay_queue_tail_];
  oneshot_overlay_queue_tail_ =
      (oneshot_overlay_queue_tail_ + 1) % oneshot_overlay_queue_.size();
  --oneshot_overlay_queued_samples_;
  return sample;
}

void MultiVoiceWavPlayer::consumeOneShotOverlaySamples(size_t sample_count) {
  if (sample_count == 0 || oneshot_overlay_queued_samples_ == 0 || oneshot_overlay_queue_.empty()) {
    return;
  }

  const size_t consumed_samples = std::min(sample_count, oneshot_overlay_queued_samples_);
  oneshot_overlay_queue_tail_ =
      (oneshot_overlay_queue_tail_ + consumed_samples) % oneshot_overlay_queue_.size();
  oneshot_overlay_queued_samples_ -= consumed_samples;
}

size_t MultiVoiceWavPlayer::pumpSink() {
  return sink_ == nullptr ? 0 : sink_->pump();
}

size_t MultiVoiceWavPlayer::mixVoices(size_t request_samples) {
  if (mixer_ == nullptr || mix_buffer_.empty()) return 0;
  return alignStereoSamples(mixer_->mix(mix_buffer_.data(), request_samples));
}

size_t MultiVoiceWavPlayer::writeMixedSamples(size_t sample_count) {
  if (sink_ == nullptr) return 0;
  return sink_->writePcm32(mix_buffer_.data(), sample_count);
}

bool MultiVoiceWavPlayer::servicePendingTrackSwitches() {
  if (sink_ == nullptr) return false;

  const uint32_t now_ms = millis();

  for (auto& slot : voice_slots_) {
    if (slot.voice == nullptr || slot.voice->mode() != WavVoiceMode::Loop ||
        !slot.voice->hasPendingNextRequest()) {
      continue;
    }

    const size_t switch_queue_samples = sink_->queuedSamples();
    if (!slot.voice->canApplyPendingNextRequest(switch_queue_samples, now_ms)) continue;

    if (slot.voice->servicePendingNextRequest(now_ms, switch_queue_samples, 0)) {
      return true;
    }
  }

  return false;
}

void MultiVoiceWavPlayer::serviceAudio() {
  if (sink_ == nullptr) return;

  const uint32_t service_start_us = micros();
  const size_t refill_target_samples = currentQueueRefillTargetSamples();
  if (lowLatencyOneShotEnabled() && hasActiveOneShotVoice()) {
    primeOneShotOverlay(currentOneShotOverlayTargetSamples());
  }
  pumpSink();
  servicePendingTrackSwitches();

  while (sink_->queuedSamples() < refill_target_samples) {
    if (static_cast<uint32_t>(micros() - service_start_us) >= active_runtime_.service_budget_us) {
      break;
    }

    const size_t writable_samples = alignStereoSamples(sink_->writableSamples());
    if (writable_samples < 2) break;

    const size_t request = std::min(active_runtime_.mix_chunk_samples, writable_samples);
    const size_t mixed = mixVoices(request);
    if (mixed == 0) {
      if (lowLatencyOneShotEnabled() && oneShotOverlayQueuedSamples() > 0) {
        std::fill(mix_buffer_.begin(), mix_buffer_.begin() + request, 0);
        const size_t written = writeMixedSamples(request);
        if (written == 0) break;
      } else {
        break;
      }
    } else {
      const size_t written = writeMixedSamples(mixed);
      if (written == 0) break;
    }

    if (lowLatencyOneShotEnabled() && hasActiveOneShotVoice()) {
      primeOneShotOverlay(currentOneShotOverlayTargetSamples());
    }
  }

  pumpSink();
}

bool MultiVoiceWavPlayer::hasPendingLoopTrackSwitch(size_t* out_target_samples) const {
  size_t min_target_samples = 0;
  bool has_pending_switch = false;

  for (const auto& slot : voice_slots_) {
    if (slot.voice == nullptr || slot.voice->mode() != WavVoiceMode::Loop ||
        !slot.voice->hasPendingNextRequest()) {
      continue;
    }

    const size_t voice_target_samples =
        alignStereoSamples(slot.voice->trackSwitchMinQueueSamples());
    if (!has_pending_switch || voice_target_samples < min_target_samples) {
      min_target_samples = voice_target_samples;
    }
    has_pending_switch = true;
  }

  if (out_target_samples != nullptr) {
    *out_target_samples = has_pending_switch ? min_target_samples : 0;
  }
  return has_pending_switch;
}

size_t MultiVoiceWavPlayer::currentQueueRefillTargetSamples() const {
  size_t target_samples = active_runtime_.queue_refill_target_samples;

  size_t pending_loop_switch_target = 0;
  if (hasPendingLoopTrackSwitch(&pending_loop_switch_target) && pending_loop_switch_target > 0) {
    target_samples = std::min(target_samples, pending_loop_switch_target);
  }

  return std::min(target_samples, active_runtime_.sink_queue_samples);
}

bool MultiVoiceWavPlayer::lowLatencyOneShotEnabled() const {
  return currentOneShotOverlayTargetSamples() > 0;
}

bool MultiVoiceWavPlayer::hasActiveOneShotVoice() const {
  for (const auto& slot : voice_slots_) {
    if (slot.spec.voice.mode != WavVoiceMode::OneShot || slot.voice == nullptr) continue;
    if (slot.voice->isTrackRunning()) return true;
  }
  return false;
}

size_t MultiVoiceWavPlayer::loopVoiceCount() const {
  size_t count = 0;
  for (const auto& slot : voice_slots_) {
    if (slot.spec.voice.mode == WavVoiceMode::Loop) ++count;
  }
  return count;
}

size_t MultiVoiceWavPlayer::oneShotVoiceCount() const {
  size_t count = 0;
  for (const auto& slot : voice_slots_) {
    if (slot.spec.voice.mode == WavVoiceMode::OneShot) ++count;
  }
  return count;
}

}  // namespace padre
