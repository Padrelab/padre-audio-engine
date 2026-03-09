#include "DualWavLoopI2sApp.h"

#include <Esp.h>

#include <algorithm>

#include "../../audio/decoder/WavDecoder.h"
#include "../../media/library/AudioFileScanner.h"
#include "../../media/source/FsAudioSource.h"

namespace padre {

namespace {

constexpr const char* kWavExtensions[] = {
    ".wav",
};

constexpr size_t kWavExtensionCount = sizeof(kWavExtensions) / sizeof(kWavExtensions[0]);

size_t alignAppStereoSamples(size_t sample_count) {
  return sample_count & ~static_cast<size_t>(1);
}

uint32_t stereoSamplesToMs(size_t sample_count, uint32_t sample_rate) {
  if (sample_rate == 0) return 0;
  const uint64_t numerator = static_cast<uint64_t>(sample_count) * 1000ull;
  const uint64_t denominator = static_cast<uint64_t>(sample_rate) * 2ull;
  return static_cast<uint32_t>(numerator / denominator);
}

DualWavLoopI2sAppConfig normalizeConfig(DualWavLoopI2sAppConfig config) {
  if (config.runtime_action_logs_enabled) {
    config.voice.runtime_action_logs_enabled = true;
  }
  return config;
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

bool detectOutputSampleRate(fs::FS& fs,
                            const char* source_type_name,
                            const std::vector<String>& music_candidates,
                            const std::vector<String>& foley_candidates,
                            uint32_t& out_sample_rate) {
  const std::vector<String>* lists[] = {
      &music_candidates,
      &foley_candidates,
  };

  for (const auto* list : lists) {
    for (const auto& path : *list) {
      uint32_t sample_rate = 0;
      if (!inspectWavTrack(fs, source_type_name, path, sample_rate)) continue;
      out_sample_rate = sample_rate;
      return true;
    }
  }

  return false;
}

void filterPlaylistBySampleRate(HardwareSerial& serial,
                                fs::FS& fs,
                                const char* source_type_name,
                                const char* label,
                                const std::vector<String>& candidates,
                                uint32_t target_sample_rate,
                                std::vector<String>& accepted) {
  accepted.clear();

  for (const auto& path : candidates) {
    uint32_t sample_rate = 0;
    if (!inspectWavTrack(fs, source_type_name, path, sample_rate)) {
      serial.printf("[%s] skipped invalid WAV: %s\n", label, path.c_str());
      continue;
    }

    if (sample_rate != target_sample_rate) {
      serial.printf("[%s] skipped sample rate mismatch: %s (%lu Hz)\n",
                    label,
                    path.c_str(),
                    static_cast<unsigned long>(sample_rate));
      continue;
    }

    accepted.push_back(path);
  }
}

void printTrackList(HardwareSerial& serial, const char* label, const std::vector<String>& tracks) {
  serial.printf("[%s] %u track(s)\n", label, static_cast<unsigned>(tracks.size()));
  for (const auto& track : tracks) {
    serial.printf("  - %s\n", track.c_str());
  }
}

}  // namespace

DualWavLoopI2sApp::DualWavLoopI2sApp(HardwareSerial& serial,
                                     TwoWire& wire,
                                     FsStorageBackend& storage,
                                     DualWavLoopI2sAppConfig config)
    : serial_(&serial),
      wire_(&wire),
      storage_(&storage),
      config_(normalizeConfig(config)),
      diag_(config_.diagnostics),
      touch_device_(
          mpr121_,
          wire,
          Mpr121AdafruitDriverPins{
              static_cast<int8_t>(config_.pins.mpr121_sda),
              static_cast<int8_t>(config_.pins.mpr121_scl),
              static_cast<int8_t>(config_.pins.mpr121_irq),
              config_.pins.mpr121_addr,
              config_.touch.i2c_clock_hz,
          },
          Mpr121AdafruitDriverConfig{
              static_cast<uint8_t>(config_.touch.touch_threshold),
              static_cast<uint8_t>(config_.touch.release_threshold),
              config_.touch.diagnostics_autoconfig,
              config_.touch.diagnostics_stream_enabled,
              config_.touch.diagnostics_report_interval_ms,
              config_.touch.diagnostics_mode,
              config_.touch.diagnostics_log_transitions,
          }),
      touch_controller_(
          touch_device_.asTouchControllerIo(),
          Mpr121TouchControllerConfig{
              config_.touch.active_electrodes,
              Mpr121InputConfig{},
              config_.touch.controller_debug,
              &serial,
          }),
      runtime_console_(nullptr, 0, serial, &runtime_command_, 1),
      i2s_io_(
          Esp32StdI2sPins{
              static_cast<int8_t>(config_.pins.i2s_bclk),
              static_cast<int8_t>(config_.pins.i2s_lrc),
              static_cast<int8_t>(config_.pins.i2s_dout),
              -1,
          },
          Esp32StdI2sOutputConfig{
              config_.i2s_dma_desc_num,
              config_.i2s_dma_frame_num,
              2,
              config_.i2s_write_timeout_ms,
              config_.i2s_work_samples,
          },
          Esp32StdI2sSampleTransform{
              this,
              &DualWavLoopI2sApp::applyVolumeSampleThunk,
          }),
      sink_(
          i2s_io_.asIo(),
          I2sOutputConfig{
              config_.sink_queue_samples,
              config_.sink_watermark_samples,
          }),
      mixer_(2),
      music_voice_("music", storage.fs(), storage.typeName(), config_.voice, &serial),
      foley_voice_("foley", storage.fs(), storage.typeName(), config_.voice, &serial),
      mix_buffer_(config_.mix_chunk_samples, 0),
      volume_(config_.volume.initial) {
  runtime_command_ = touch_device_.runtimeCommandEntry();
}

bool DualWavLoopI2sApp::begin() {
  serial_->println(config_.example_name);
  serial_->println("Serial runtime console: help/mpr121 ...");
  printPinout();
  diag_.resetWindow();

  if (!initStorage()) return false;
  applyVolume();
  initTouch();

  uint32_t sample_rate = 0;
  if (!preparePlaylists(sample_rate)) return false;
  if (!startAudio(sample_rate)) return false;
  if (!startAudioTask()) return false;

  audio_ready_ = true;
  notifyAudioTask();
  return true;
}

void DualWavLoopI2sApp::loop() {
  runtime_console_.poll(*serial_);

  if (!audio_ready_) {
    delay(100);
    return;
  }

  serviceTouch(millis());
  reportDiagnosticsIfDue(millis(), false);
  delay(0);
}

int16_t DualWavLoopI2sApp::applyVolumeSampleThunk(void* ctx, int16_t sample) {
  auto* self = static_cast<DualWavLoopI2sApp*>(ctx);
  return self == nullptr ? sample : self->applyVolumeToSample(sample);
}

void DualWavLoopI2sApp::audioTaskEntry(void* ctx) {
  auto* self = static_cast<DualWavLoopI2sApp*>(ctx);
  if (self == nullptr) {
    vTaskDelete(nullptr);
    return;
  }
  self->audioTaskMain();
}

void DualWavLoopI2sApp::onTouchEventThunk(void* ctx, const InputEvent& event) {
  auto* self = static_cast<DualWavLoopI2sApp*>(ctx);
  if (self == nullptr) return;
  self->onTouchEvent(event);
}

int16_t DualWavLoopI2sApp::applyVolumeToSample(int16_t sample) const {
  const int32_t scaled = (static_cast<int32_t>(sample) * volume_gain_q15_) >> 15;
  if (scaled > 32767) return 32767;
  if (scaled < -32768) return -32768;
  return static_cast<int16_t>(scaled);
}

void DualWavLoopI2sApp::updateVolumeGain() {
  const int32_t volume = static_cast<int32_t>(volume_);
  const int32_t max_volume = static_cast<int32_t>(config_.volume.max);
  const int32_t gain_num = volume * volume;
  const int32_t gain_den = max_volume * max_volume;
  volume_gain_q15_ = (gain_num * 32767 + (gain_den / 2)) / gain_den;
}

void DualWavLoopI2sApp::applyVolume() {
  updateVolumeGain();
  if (config_.runtime_action_logs_enabled) {
    serial_->printf("Volume: %d/%d\n", volume_, config_.volume.max);
  }
}

bool DualWavLoopI2sApp::stepVolume(int delta) {
  const int next_volume =
      max(config_.volume.min, min(config_.volume.max, volume_ + delta));
  if (next_volume == volume_) return false;

  volume_ = next_volume;
  applyVolume();
  return true;
}

void DualWavLoopI2sApp::printPinout() {
  serial_->printf("Build:  %s\n", config_.build_tag);
  storage_->printConfig(*serial_);
  serial_->printf("I2S:    bclk=%u lrck=%u dout=%u\n",
                  config_.pins.i2s_bclk,
                  config_.pins.i2s_lrc,
                  config_.pins.i2s_dout);
  serial_->printf("MPR121: sda=%u scl=%u irq=%u addr=0x%02X\n",
                  config_.pins.mpr121_sda,
                  config_.pins.mpr121_scl,
                  config_.pins.mpr121_irq,
                  config_.pins.mpr121_addr);
  serial_->printf("Audio:  queue=%u prebuffer=%u target=%u dma=%ux%u work=%u\n",
                  static_cast<unsigned>(config_.sink_queue_samples),
                  static_cast<unsigned>(config_.startup_prebuffer_samples),
                  static_cast<unsigned>(config_.queue_refill_target_samples),
                  static_cast<unsigned>(config_.i2s_dma_desc_num),
                  static_cast<unsigned>(config_.i2s_dma_frame_num),
                  static_cast<unsigned>(config_.i2s_work_samples));
  serial_->printf(
      "Switch: retain=%u min=%u coalesce=%lums max=%lums retain_ms@%luk=%lums\n",
      static_cast<unsigned>(config_.track_switch_retained_queue_samples),
      static_cast<unsigned>(config_.voice.track_switch_min_queue_samples),
      static_cast<unsigned long>(config_.voice.track_switch_coalesce_ms),
      static_cast<unsigned long>(config_.voice.track_switch_max_delay_ms),
      static_cast<unsigned long>(config_.startup_sample_rate_hint / 1000),
      static_cast<unsigned long>(
          stereoSamplesToMs(config_.track_switch_retained_queue_samples,
                            config_.startup_sample_rate_hint)));
}

bool DualWavLoopI2sApp::initStorage() {
  return storage_->begin(*serial_);
}

bool DualWavLoopI2sApp::initTouch() {
  touch_device_.setDiagnosticsOutput(serial_);
  touch_device_.scanI2c(*serial_);
  if (!touch_device_.begin()) {
    serial_->println("MPR121 init failed, touch disabled");
    return false;
  }

  touch_controller_.setEventHandler(this, onTouchEventThunk);
  if (!touch_controller_.begin()) {
    serial_->println("Touch controller init failed, touch disabled");
    return false;
  }

  touch_ready_ = true;
  serial_->println("MPR121 touch ready");
  return true;
}

bool DualWavLoopI2sApp::preparePlaylists(uint32_t& out_sample_rate) {
  std::vector<String> music_candidates;
  std::vector<String> foley_candidates;

  scanWavFiles(storage_->fs(), config_.max_dir_depth, config_.music_dir, music_candidates);
  scanWavFiles(storage_->fs(), config_.max_dir_depth, config_.foley_dir, foley_candidates);

  serial_->printf("Scanned %s: %u file(s)\n",
                  config_.music_dir,
                  static_cast<unsigned>(music_candidates.size()));
  serial_->printf("Scanned %s: %u file(s)\n",
                  config_.foley_dir,
                  static_cast<unsigned>(foley_candidates.size()));

  if (!detectOutputSampleRate(storage_->fs(),
                              storage_->typeName(),
                              music_candidates,
                              foley_candidates,
                              out_sample_rate)) {
    serial_->println("No valid WAV files found in /music or /foley");
    return false;
  }

  filterPlaylistBySampleRate(*serial_,
                             storage_->fs(),
                             storage_->typeName(),
                             "music",
                             music_candidates,
                             out_sample_rate,
                             music_tracks_);
  filterPlaylistBySampleRate(*serial_,
                             storage_->fs(),
                             storage_->typeName(),
                             "foley",
                             foley_candidates,
                             out_sample_rate,
                             foley_tracks_);

  printTrackList(*serial_, "music", music_tracks_);
  printTrackList(*serial_, "foley", foley_tracks_);

  if (music_tracks_.empty() && foley_tracks_.empty()) {
    serial_->println("No playable WAV files left after sample rate filtering");
    return false;
  }

  if (music_tracks_.empty()) {
    serial_->println("Warning: /music has no playable WAV files, only /foley will run");
  }

  if (foley_tracks_.empty()) {
    serial_->println("Warning: /foley has no playable WAV files, only /music will run");
  }

  return true;
}

bool DualWavLoopI2sApp::startAudio(uint32_t sample_rate) {
  if (!music_voice_.configure(sample_rate) || !foley_voice_.configure(sample_rate)) {
    serial_->println("Voice configuration failed");
    return false;
  }

  music_voice_.setTracks(music_tracks_);
  foley_voice_.setTracks(foley_tracks_);

  mixer_.attachSource(0, &music_voice_);
  mixer_.attachSource(1, &foley_voice_);
  mixer_.setGlobalGain(1.0f);
  mixer_.setVoiceGain(0, config_.music_gain);
  mixer_.setVoiceGain(1, config_.foley_gain);

  if (!sink_.begin(DecoderConfig{sample_rate, 16, true})) {
    serial_->println("I2S sink init failed");
    return false;
  }

  noteCurrentQueueLevel();
  i2s_io_.setPrebuffering(true);
  const uint32_t prefill_start_ms = millis();
  while (sink_.queuedSamples() < config_.startup_prebuffer_samples) {
    const size_t writable_samples = alignAppStereoSamples(sink_.writableSamples());
    if (writable_samples < 2) break;

    const size_t request = min(config_.mix_chunk_samples, writable_samples);
    const size_t mixed = mixVoices(request);
    if (mixed == 0) break;

    const size_t written = writeMixedSamples(mixed);
    if (written == 0) break;

    if (static_cast<uint32_t>(millis() - prefill_start_ms) > config_.startup_prebuffer_budget_ms) {
      break;
    }
  }
  i2s_io_.setPrebuffering(false);
  pumpSink();
  diagNoteStartupPrefill(static_cast<uint32_t>(millis() - prefill_start_ms), sink_.queuedSamples());

  serial_->printf("Audio started at %lu Hz, queued=%lu samples\n",
                  static_cast<unsigned long>(sample_rate),
                  static_cast<unsigned long>(sink_.queuedSamples()));
  return sink_.queuedSamples() > 0;
}

bool DualWavLoopI2sApp::startAudioTask() {
  if (audio_task_handle_ != nullptr) return true;

  BaseType_t result = xTaskCreatePinnedToCore(audioTaskEntry,
                                              "audio_service",
                                              config_.audio_task_stack_bytes,
                                              this,
                                              config_.audio_task_priority,
                                              &audio_task_handle_,
                                              config_.audio_task_core);
  if (result != pdPASS) {
    audio_task_handle_ = nullptr;
    serial_->println("Audio task create failed");
    return false;
  }

  return true;
}

void DualWavLoopI2sApp::audioTaskMain() {
  for (;;) {
    if (!audio_ready_) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
      continue;
    }

    const uint32_t loop_start_us = micros();
    applyPendingControls(millis());
    serviceAudio();
    diagNoteLoop(static_cast<uint32_t>(micros() - loop_start_us));

    const bool idle = !hasPendingControls() && !music_voice_.hasPendingNextRequest() &&
                      !foley_voice_.hasPendingNextRequest() &&
                      sink_.queuedSamples() >= config_.queue_refill_target_samples &&
                      !sink_.pumpRequested();
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(idle ? 2 : config_.audio_task_loop_delay_ms));
  }
}

void DualWavLoopI2sApp::notifyAudioTask() {
  if (audio_task_handle_ != nullptr) {
    xTaskNotifyGive(audio_task_handle_);
  }
}

void DualWavLoopI2sApp::queueMusicNextRequest() {
  portENTER_CRITICAL(&control_mux_);
  if (pending_controls_.music_next_requests < UINT32_MAX) {
    ++pending_controls_.music_next_requests;
  }
  portEXIT_CRITICAL(&control_mux_);
  notifyAudioTask();
}

void DualWavLoopI2sApp::queueFoleyNextRequest() {
  portENTER_CRITICAL(&control_mux_);
  if (pending_controls_.foley_next_requests < UINT32_MAX) {
    ++pending_controls_.foley_next_requests;
  }
  portEXIT_CRITICAL(&control_mux_);
  notifyAudioTask();
}

void DualWavLoopI2sApp::queueVolumeDelta(int delta) {
  portENTER_CRITICAL(&control_mux_);
  const int32_t updated_delta = pending_controls_.volume_delta + static_cast<int32_t>(delta);
  pending_controls_.volume_delta =
      max<int32_t>(-config_.volume.max, min<int32_t>(config_.volume.max, updated_delta));
  portEXIT_CRITICAL(&control_mux_);
  notifyAudioTask();
}

DualWavLoopI2sApp::PendingControlState DualWavLoopI2sApp::takePendingControls() {
  PendingControlState pending;
  portENTER_CRITICAL(&control_mux_);
  pending = pending_controls_;
  pending_controls_ = {};
  portEXIT_CRITICAL(&control_mux_);
  return pending;
}

bool DualWavLoopI2sApp::hasPendingControls() {
  portENTER_CRITICAL(&control_mux_);
  const bool has_pending = pending_controls_.music_next_requests != 0 ||
                           pending_controls_.foley_next_requests != 0 ||
                           pending_controls_.volume_delta != 0;
  portEXIT_CRITICAL(&control_mux_);
  return has_pending;
}

void DualWavLoopI2sApp::applyPendingControls(uint32_t now_ms) {
  const PendingControlState pending = takePendingControls();
  if (pending.music_next_requests != 0) {
    music_voice_.queueNextTracks(pending.music_next_requests, now_ms);
  }
  if (pending.foley_next_requests != 0) {
    foley_voice_.queueNextTracks(pending.foley_next_requests, now_ms);
  }
  if (pending.volume_delta != 0) {
    stepVolume(static_cast<int>(pending.volume_delta));
  }
}

void DualWavLoopI2sApp::onTouchEvent(const InputEvent& event) {
  if (event.type != InputEventType::PressDown) return;

  diagNoteTouchEvent(event.source_id);

  switch (event.source_id) {
    case 0:
      if (config_.runtime_action_logs_enabled) serial_->println("Touch 0: next music track");
      queueMusicNextRequest();
      break;
    case 1:
      if (config_.runtime_action_logs_enabled) serial_->println("Touch 1: next foley track");
      queueFoleyNextRequest();
      break;
    case 2:
      if (config_.runtime_action_logs_enabled) serial_->println("Touch 2: volume down");
      queueVolumeDelta(-1);
      break;
    case 3:
      if (config_.runtime_action_logs_enabled) serial_->println("Touch 3: volume up");
      queueVolumeDelta(1);
      break;
    default:
      break;
  }
}

void DualWavLoopI2sApp::serviceTouch(uint32_t now_ms) {
  if (!touch_ready_) return;

  const bool touch_irq = touch_device_.consumeIrq();

  if (touch_irq || (now_ms - last_touch_poll_ms_) >= config_.touch.poll_ms) {
    diagNoteTouchPoll(now_ms);
    touch_controller_.poll(now_ms);
    last_touch_poll_ms_ = now_ms;
    touch_device_.serviceRuntime(now_ms);
  }
}

void DualWavLoopI2sApp::noteCurrentQueueLevel() {
  diagNoteQueue(sink_.queuedSamples());
}

size_t DualWavLoopI2sApp::pumpSink() {
  const size_t queued_before = sink_.queuedSamples();
  const uint32_t start_us = micros();
  const size_t pumped_samples = sink_.pump();
  const uint32_t elapsed_us = static_cast<uint32_t>(micros() - start_us);
  diagNotePump(queued_before, pumped_samples, elapsed_us);
  noteCurrentQueueLevel();
  return pumped_samples;
}

size_t DualWavLoopI2sApp::mixVoices(size_t request_samples) {
  const size_t mixed_samples =
      alignAppStereoSamples(mixer_.mix(mix_buffer_.data(), request_samples));
  diagNoteMix(mixed_samples);
  return mixed_samples;
}

size_t DualWavLoopI2sApp::writeMixedSamples(size_t sample_count) {
  const size_t written_samples = sink_.write(mix_buffer_.data(), sample_count);
  diagNoteWrite(sample_count, written_samples);
  noteCurrentQueueLevel();
  return written_samples;
}

bool DualWavLoopI2sApp::servicePendingTrackSwitches() {
  const uint32_t now_ms = millis();

  LoopingWavVoice* voices[] = {
      &music_voice_,
      &foley_voice_,
  };

  for (LoopingWavVoice* voice : voices) {
    if (voice == nullptr || !voice->hasPendingNextRequest()) continue;

    const size_t switch_queue_samples =
        min(sink_.queuedSamples(), config_.track_switch_retained_queue_samples);
    if (!voice->canApplyPendingNextRequest(switch_queue_samples, now_ms)) continue;

    size_t queue_after_trim = sink_.queuedSamples();
    size_t trimmed_samples = 0;
    if (queue_after_trim > config_.track_switch_retained_queue_samples) {
      trimmed_samples = sink_.trimQueuedSamples(config_.track_switch_retained_queue_samples);
      if (trimmed_samples > 0) {
        noteCurrentQueueLevel();
        queue_after_trim = sink_.queuedSamples();
      }
    }

    if (voice->servicePendingNextRequest(now_ms, queue_after_trim, trimmed_samples)) {
      return true;
    }
  }

  return false;
}

void DualWavLoopI2sApp::serviceAudio() {
  const uint32_t service_start_us = micros();
  pumpSink();

  const bool switched_this_pass = servicePendingTrackSwitches();
  (void)switched_this_pass;

  uint32_t refill_iterations = 0;
  bool made_progress = false;
  while (sink_.queuedSamples() < config_.queue_refill_target_samples) {
    if (static_cast<uint32_t>(micros() - service_start_us) >= config_.service_budget_us) {
      diagNoteServiceBudgetHit();
      break;
    }

    ++refill_iterations;
    const size_t writable_samples = alignAppStereoSamples(sink_.writableSamples());
    if (writable_samples < 2) break;

    const size_t request = min(config_.mix_chunk_samples, writable_samples);
    const size_t mixed = mixVoices(request);
    if (mixed == 0) break;

    const size_t written = writeMixedSamples(mixed);
    if (written == 0) break;
    made_progress = true;
  }

  diagNoteRefill(refill_iterations, made_progress);
  pumpSink();
  diagNoteService(static_cast<uint32_t>(micros() - service_start_us));
}

void DualWavLoopI2sApp::printVoiceDiagnostics(const char* label,
                                              const LoopingWavVoice::DebugSnapshot& snapshot) {
  const auto& stats = snapshot.stats;
  const uint32_t read_avg_us =
      stats.read_calls == 0 ? 0 : static_cast<uint32_t>(stats.read_total_us / stats.read_calls);

  serial_->printf(
      "DIAG voice=%s running=%s idx=%u/%u file=%lu/%lu read avg/max/last=%lu/%lu/%luus "
      "next us=%lu/%lu age=%lu/%lums q=%lu/%lu cut=%lu/%lu close last/max=%lu/%luus open last/max=%lu/%luus "
      "dec last/max=%lu/%luus manualNext=%lu zero=%lu streak=%lu short=%lu buf=%lu/%lu track=%s\n",
      label,
      snapshot.is_track_running ? "yes" : "no",
      static_cast<unsigned>(snapshot.playlist_size == 0 ? 0 : (snapshot.current_track_index + 1)),
      static_cast<unsigned>(snapshot.playlist_size),
      static_cast<unsigned long>(snapshot.current_file_position),
      static_cast<unsigned long>(snapshot.current_file_size),
      static_cast<unsigned long>(read_avg_us),
      static_cast<unsigned long>(stats.read_max_us),
      static_cast<unsigned long>(stats.last_read_us),
      static_cast<unsigned long>(stats.request_next_last_us),
      static_cast<unsigned long>(stats.request_next_us.max_us),
      static_cast<unsigned long>(stats.request_next_age_last_ms),
      static_cast<unsigned long>(stats.request_next_age_max_ms),
      static_cast<unsigned long>(stats.request_next_queue_last_samples),
      static_cast<unsigned long>(stats.request_next_queue_max_samples),
      static_cast<unsigned long>(stats.request_next_trimmed_last_samples),
      static_cast<unsigned long>(stats.request_next_trimmed_max_samples),
      static_cast<unsigned long>(stats.close_last_us),
      static_cast<unsigned long>(stats.close_us.max_us),
      static_cast<unsigned long>(stats.open_last_us),
      static_cast<unsigned long>(stats.open_us.max_us),
      static_cast<unsigned long>(stats.decoder_begin_last_us),
      static_cast<unsigned long>(stats.decoder_begin_us.max_us),
      static_cast<unsigned long>(stats.manual_next_requests),
      static_cast<unsigned long>(stats.zero_reads),
      static_cast<unsigned long>(stats.consecutive_zero_reads),
      static_cast<unsigned long>(stats.short_reads),
      static_cast<unsigned long>(stats.buffer_last_samples),
      static_cast<unsigned long>(stats.buffer_peak_samples),
      snapshot.active_track[0] != '\0' ? snapshot.active_track : "-");
}

void DualWavLoopI2sApp::reportDiagnosticsIfDue(uint32_t now_ms, bool force) {
  const uint32_t report_start_us = micros();
  DualWavLoopRuntimeDiagnostics diag_snapshot{config_.diagnostics};
  size_t queue_now = 0;
  uint32_t touch_irq_count_snapshot = 0;

  portENTER_CRITICAL(&diag_mux_);
  if (!diag_.reportDue(now_ms, force)) {
    portEXIT_CRITICAL(&diag_mux_);
    return;
  }

  diag_snapshot = diag_;
  diag_.last_report_ms = now_ms;
  touch_irq_count_snapshot = touch_device_.irqCount();
  queue_now = last_queue_samples_;
  diag_.clearSnapshotPending();
  diag_.resetWindow();
  diag_.noteQueue(last_queue_samples_);
  portEXIT_CRITICAL(&diag_mux_);

  LoopingWavVoice::DebugSnapshot music_snapshot;
  LoopingWavVoice::DebugSnapshot foley_snapshot;
  music_voice_.copyDebugSnapshot(music_snapshot);
  foley_voice_.copyDebugSnapshot(foley_snapshot);

  const char* event_name = force ? "manual" : diag_snapshot.snapshotEventName();
  const size_t queue_trigger = diag_snapshot.snapshotTriggerQueueMin();
  const size_t queue_capacity = sink_.queueCapacity();
  const size_t queue_avg = diag_snapshot.queue_samples.avg();
  const size_t queue_min = diag_snapshot.queue_samples.minValue();
  const size_t queue_max = diag_snapshot.queue_samples.max_value;
  const uint32_t free_heap = ESP.getFreeHeap();
  const uint32_t min_free_heap = ESP.getMinFreeHeap();
  const uint32_t max_alloc_heap = ESP.getMaxAllocHeap();
  const uint32_t psram_size = ESP.getPsramSize();
  const uint32_t free_psram = ESP.getFreePsram();

  serial_->printf(
      "DIAG event=%s queue trig/now/cap=%lu/%lu/%lu avg/min/max=%lu/%lu/%lu low=%lu/%lu "
      "empty=%lu/%lu loop avg/max=%lu/%luus svc avg/max=%lu/%luus refill peak/no=%lu/%lu budget=%lu/%lu\n",
      event_name,
      static_cast<unsigned long>(queue_trigger),
      static_cast<unsigned long>(queue_now),
      static_cast<unsigned long>(queue_capacity),
      static_cast<unsigned long>(queue_avg),
      static_cast<unsigned long>(queue_min),
      static_cast<unsigned long>(queue_max),
      static_cast<unsigned long>(diag_snapshot.queue_low_events),
      static_cast<unsigned long>(diag_snapshot.queue_low_events_total),
      static_cast<unsigned long>(diag_snapshot.queue_empty_events),
      static_cast<unsigned long>(diag_snapshot.queue_empty_events_total),
      static_cast<unsigned long>(diag_snapshot.loop_us.avgUs()),
      static_cast<unsigned long>(diag_snapshot.loop_us.max_us),
      static_cast<unsigned long>(diag_snapshot.service_us.avgUs()),
      static_cast<unsigned long>(diag_snapshot.service_us.max_us),
      static_cast<unsigned long>(diag_snapshot.refill_peak_iterations),
      static_cast<unsigned long>(diag_snapshot.refill_no_progress),
      static_cast<unsigned long>(diag_snapshot.service_budget_hits),
      static_cast<unsigned long>(diag_snapshot.service_budget_hits_total));

  serial_->printf(
      "DIAG io pump calls=%lu/%lu stall=%lu/%lu avg/max=%lu/%luus write short=%lu/%lu "
      "zero=%lu/%lu mix zero=%lu/%lu report last/max=%lu/%luus slow=%lu heap free/min/max=%lu/%lu/%lu",
      static_cast<unsigned long>(diag_snapshot.pump_calls),
      static_cast<unsigned long>(diag_snapshot.pump_calls_total),
      static_cast<unsigned long>(diag_snapshot.pump_zero_with_data),
      static_cast<unsigned long>(diag_snapshot.pump_zero_with_data_total),
      static_cast<unsigned long>(diag_snapshot.pump_us.avgUs()),
      static_cast<unsigned long>(diag_snapshot.pump_us.max_us),
      static_cast<unsigned long>(diag_snapshot.short_writes),
      static_cast<unsigned long>(diag_snapshot.short_writes_total),
      static_cast<unsigned long>(diag_snapshot.zero_writes),
      static_cast<unsigned long>(diag_snapshot.zero_writes_total),
      static_cast<unsigned long>(diag_snapshot.mix_zero),
      static_cast<unsigned long>(diag_snapshot.mix_zero_total),
      static_cast<unsigned long>(diag_snapshot.report_last_us),
      static_cast<unsigned long>(diag_snapshot.report_us.max_us),
      static_cast<unsigned long>(diag_snapshot.report_slow),
      static_cast<unsigned long>(free_heap),
      static_cast<unsigned long>(min_free_heap),
      static_cast<unsigned long>(max_alloc_heap));

  if (psram_size > 0) {
    serial_->printf(" psram=%lu/%lu",
                    static_cast<unsigned long>(free_psram),
                    static_cast<unsigned long>(psram_size));
  }

  serial_->printf(
      " touch irq=%lu polls=%lu events=%lu gap=%lums prefill=%lums/%lu\n",
      static_cast<unsigned long>(touch_irq_count_snapshot),
      static_cast<unsigned long>(diag_snapshot.touch_polls),
      static_cast<unsigned long>(diag_snapshot.touch_events),
      static_cast<unsigned long>(diag_snapshot.max_touch_poll_gap_ms),
      static_cast<unsigned long>(diag_snapshot.startup_prefill_ms),
      static_cast<unsigned long>(diag_snapshot.startup_prefill_samples));

  printVoiceDiagnostics(music_voice_.label(), music_snapshot);
  printVoiceDiagnostics(foley_voice_.label(), foley_snapshot);

  const uint32_t report_elapsed_us = static_cast<uint32_t>(micros() - report_start_us);
  diagNoteReport(report_elapsed_us);
}

void DualWavLoopI2sApp::diagNoteQueue(size_t queued_samples) {
  portENTER_CRITICAL(&diag_mux_);
  last_queue_samples_ = queued_samples;
  diag_.noteQueue(queued_samples);
  portEXIT_CRITICAL(&diag_mux_);
}

void DualWavLoopI2sApp::diagNoteLoop(uint32_t elapsed_us) {
  portENTER_CRITICAL(&diag_mux_);
  diag_.noteLoop(elapsed_us);
  portEXIT_CRITICAL(&diag_mux_);
}

void DualWavLoopI2sApp::diagNoteService(uint32_t elapsed_us) {
  portENTER_CRITICAL(&diag_mux_);
  diag_.noteService(elapsed_us);
  portEXIT_CRITICAL(&diag_mux_);
}

void DualWavLoopI2sApp::diagNotePump(size_t queued_before,
                                     size_t pumped_samples,
                                     uint32_t elapsed_us) {
  portENTER_CRITICAL(&diag_mux_);
  diag_.notePump(queued_before, pumped_samples, elapsed_us);
  portEXIT_CRITICAL(&diag_mux_);
}

void DualWavLoopI2sApp::diagNoteWrite(size_t requested_samples, size_t written_samples_now) {
  portENTER_CRITICAL(&diag_mux_);
  diag_.noteWrite(requested_samples, written_samples_now);
  portEXIT_CRITICAL(&diag_mux_);
}

void DualWavLoopI2sApp::diagNoteMix(size_t mixed_samples_now) {
  portENTER_CRITICAL(&diag_mux_);
  diag_.noteMix(mixed_samples_now);
  portEXIT_CRITICAL(&diag_mux_);
}

void DualWavLoopI2sApp::diagNoteRefill(uint32_t iterations, bool made_progress) {
  portENTER_CRITICAL(&diag_mux_);
  diag_.noteRefill(iterations, made_progress);
  portEXIT_CRITICAL(&diag_mux_);
}

void DualWavLoopI2sApp::diagNoteServiceBudgetHit() {
  portENTER_CRITICAL(&diag_mux_);
  diag_.noteServiceBudgetHit();
  portEXIT_CRITICAL(&diag_mux_);
}

void DualWavLoopI2sApp::diagNoteTouchPoll(uint32_t now_ms) {
  portENTER_CRITICAL(&diag_mux_);
  diag_.noteTouchPoll(now_ms);
  portEXIT_CRITICAL(&diag_mux_);
}

void DualWavLoopI2sApp::diagNoteTouchEvent(uint8_t electrode) {
  portENTER_CRITICAL(&diag_mux_);
  diag_.noteTouchEvent(electrode);
  portEXIT_CRITICAL(&diag_mux_);
}

void DualWavLoopI2sApp::diagNoteStartupPrefill(uint32_t elapsed_ms, size_t queued_samples) {
  portENTER_CRITICAL(&diag_mux_);
  diag_.noteStartupPrefill(elapsed_ms, queued_samples);
  portEXIT_CRITICAL(&diag_mux_);
}

void DualWavLoopI2sApp::diagNoteReport(uint32_t elapsed_us) {
  portENTER_CRITICAL(&diag_mux_);
  diag_.noteReport(elapsed_us);
  portEXIT_CRITICAL(&diag_mux_);
}

}  // namespace padre
