#include <Arduino.h>
#include <Adafruit_MPR121.h>
#include <Esp.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>

#include <algorithm>
#include <vector>

#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

#include "../../patches/audio/decoder/WavDecoder.h"
#include "../../patches/audio/mixer/VoiceMixer.h"
#include "../../patches/audio/output/BufferedI2sOutput.h"
#include "../../patches/audio/output/Esp32StdI2sOutputIo.h"
#include "../../patches/app/serial/SerialRuntimeConsole.h"
#include "../../patches/input/core/InputEvent.h"
#include "../../patches/input/mpr121/Mpr121AdafruitDriver.h"
#include "../../patches/input/mpr121/Mpr121TouchController.h"
#include "../../patches/media/library/AudioFileScanner.h"
#include "../../patches/media/source/FsAudioSource.h"

namespace {

constexpr uint8_t SD_CS = 10;
constexpr uint8_t SD_SCK = 12;
constexpr uint8_t SD_MISO = 13;
constexpr uint8_t SD_MOSI = 11;

constexpr uint8_t I2S_BCLK = 17;
constexpr uint8_t I2S_LRC = 18;
constexpr uint8_t I2S_DOUT = 21;

constexpr uint8_t MPR121_SDA = 4;
constexpr uint8_t MPR121_SCL = 5;
constexpr uint8_t MPR121_IRQ = 6;
constexpr uint8_t MPR121_ADDR = 0x5A;
constexpr char kBuildTag[] = "dual-sd-wav-i2s-rtos-q49152-r3";

constexpr char kMusicDir[] = "/music";
constexpr char kFoleyDir[] = "/foley";
constexpr uint8_t kMaxDirDepth = 5;
constexpr uint32_t kSdClockHz = 20000000;

constexpr const char* kWavExtensions[] = {
    ".wav",
};
constexpr size_t kWavExtensionCount = sizeof(kWavExtensions) / sizeof(kWavExtensions[0]);

constexpr size_t kMixChunkSamples = 512;
constexpr size_t kSinkQueueSamples = 49152;
constexpr size_t kSinkWatermarkSamples = 1024;
constexpr size_t kStartupPrebufferSamples = 24576;
constexpr uint32_t kStartupPrebufferBudgetMs = 800;
constexpr size_t kQueueRefillTargetSamples = 36864;
constexpr size_t kTrackSwitchSafeQueueSamples = kQueueRefillTargetSamples;
constexpr uint32_t kTrackSwitchCoalesceMs = 60;
constexpr uint32_t kTrackSwitchMaxDelayMs = 250;
constexpr uint32_t kServiceBudgetUs = 5000;
constexpr uint32_t kI2sWriteTimeoutMs = 0;
constexpr uint8_t kI2sDmaDescNum = 32;
constexpr uint16_t kI2sDmaFrameNum = 512;
constexpr size_t kI2sWorkSamples = 2048;
constexpr size_t kVoicePcmBufferSamples = 8192;
constexpr size_t kVoicePcmLowWaterSamples = 2048;
constexpr size_t kVoicePcmRefillChunkSamples = 1024;
constexpr uint8_t kVoicePcmMaxRefillAttempts = 2;

constexpr float kMusicGain = 0.60f;
constexpr float kFoleyGain = 0.60f;

constexpr int kVolumeMin = 0;
constexpr int kVolumeMax = 20;
constexpr int kVolumeDefault = 18;

constexpr uint16_t kTouchThreshold = 120;
constexpr uint16_t kReleaseThreshold = 100;
constexpr uint32_t kTouchPollMs = 10;
constexpr bool kRuntimeActionLogsEnabled = false;
constexpr uint32_t kDiagSlowLoopUs = 10000;
constexpr uint32_t kDiagSlowServiceUs = 10000;
constexpr uint32_t kDiagSlowPumpUs = 2000;
constexpr uint32_t kDiagSlowVoiceReadUs = 12000;
constexpr uint32_t kDiagSlowReportUs = 50000;
constexpr size_t kDiagQueueLowSamples = 8192;
constexpr size_t kDiagQueueCriticalSamples = 512;
constexpr size_t kDiagQueueRearmSamples = 24576;
constexpr uint32_t kDiagHeapWarnBytes = 65536;
constexpr uint32_t kDiagHeapCriticalBytes = 32768;

constexpr uint32_t kAudioTaskStackBytes = 8192;
constexpr UBaseType_t kAudioTaskPriority = 3;
constexpr uint32_t kAudioTaskLoopDelayMs = 1;

#if CONFIG_FREERTOS_UNICORE
constexpr BaseType_t kAudioTaskCore = 0;
#else
constexpr BaseType_t kAudioTaskCore = 0;
#endif

int g_volume = kVolumeDefault;
int32_t g_volume_gain_q15 = 32767;

int16_t applyVolumeToSample(int16_t sample) {
  const int32_t scaled = (static_cast<int32_t>(sample) * g_volume_gain_q15) >> 15;
  if (scaled > 32767) return 32767;
  if (scaled < -32768) return -32768;
  return static_cast<int16_t>(scaled);
}

void updateVolumeGain() {
  const int32_t volume = static_cast<int32_t>(g_volume);
  const int32_t max_volume = static_cast<int32_t>(kVolumeMax);
  const int32_t gain_num = volume * volume;
  const int32_t gain_den = max_volume * max_volume;
  g_volume_gain_q15 = (gain_num * 32767 + (gain_den / 2)) / gain_den;
}

void applyVolume() {
  updateVolumeGain();
  if (kRuntimeActionLogsEnabled) {
    Serial.printf("Volume: %d/%d\n", g_volume, kVolumeMax);
  }
}

bool stepVolume(int delta) {
  const int next_volume = max(kVolumeMin, min(kVolumeMax, g_volume + delta));
  if (next_volume == g_volume) return false;

  g_volume = next_volume;
  applyVolume();
  return true;
}

int16_t i2sApplyVolumeSample(void*, int16_t sample) {
  return applyVolumeToSample(sample);
}

size_t alignStereoSamples(size_t sample_count) {
  return sample_count & ~static_cast<size_t>(1);
}

struct DurationStats {
  uint32_t count = 0;
  uint64_t total_us = 0;
  uint32_t max_us = 0;

  void add(uint32_t elapsed_us) {
    ++count;
    total_us += elapsed_us;
    if (elapsed_us > max_us) max_us = elapsed_us;
  }

  uint32_t avgUs() const {
    return count == 0 ? 0 : static_cast<uint32_t>(total_us / count);
  }

  void reset() {
    count = 0;
    total_us = 0;
    max_us = 0;
  }
};

struct SampleStats {
  uint32_t count = 0;
  uint64_t total = 0;
  size_t min_value = SIZE_MAX;
  size_t max_value = 0;
  size_t last_value = 0;

  void add(size_t value) {
    last_value = value;
    ++count;
    total += value;
    if (count == 1 || value < min_value) min_value = value;
    if (value > max_value) max_value = value;
  }

  size_t avg() const { return count == 0 ? 0 : static_cast<size_t>(total / count); }

  size_t minValue() const { return count == 0 ? 0 : min_value; }

  void reset() {
    count = 0;
    total = 0;
    min_value = SIZE_MAX;
    max_value = 0;
    last_value = 0;
  }
};

class LoopingWavVoice : public padre::IMixerVoiceSource {
 public:
  struct Stats {
    uint32_t source_begin_failures = 0;
    uint32_t open_failures = 0;
    uint32_t invalid_wav = 0;
    uint32_t sample_rate_mismatches = 0;
    uint32_t manual_next_requests = 0;
    uint32_t track_open_attempts = 0;
    uint32_t track_opened = 0;
    uint32_t track_ended = 0;
    uint32_t playlist_wraps = 0;
    uint32_t mono_tracks = 0;
    uint32_t stereo_tracks = 0;
    uint32_t read_calls = 0;
    uint64_t read_total_us = 0;
    uint32_t read_max_us = 0;
    uint32_t slow_reads = 0;
    uint64_t produced_samples = 0;
    uint32_t zero_reads = 0;
    uint32_t consecutive_zero_reads = 0;
    uint32_t last_read_us = 0;
    size_t last_read_samples = 0;
    uint32_t short_reads = 0;
    size_t buffer_last_samples = 0;
    size_t buffer_peak_samples = 0;
    uint32_t last_track_start_ms = 0;
    uint32_t last_track_sample_rate = 0;
    uint8_t last_track_channels = 0;
    DurationStats request_next_us;
    uint32_t request_next_last_us = 0;
    DurationStats close_us;
    uint32_t close_last_us = 0;
    DurationStats open_us;
    uint32_t open_last_us = 0;
    DurationStats decoder_begin_us;
    uint32_t decoder_begin_last_us = 0;
  };

  struct DebugSnapshot {
    Stats stats;
    size_t current_track_index = 0;
    size_t playlist_size = 0;
    size_t current_file_position = 0;
    size_t current_file_size = 0;
    bool is_track_running = false;
    char active_track[160] = {0};
  };

  LoopingWavVoice(const char* label, fs::FS& fs)
      : label_(label), source_(fs, padre::FsAudioSourceConfig{"sd"}) {
    syncDebugSnapshot();
  }

  void setTracks(const std::vector<String>& tracks) {
    stop();
    tracks_ = tracks;
    track_index_ = 0;
    syncDebugSnapshot();
  }

  bool configure(uint32_t output_sample_rate) {
    output_sample_rate_ = output_sample_rate;
    return output_sample_rate_ > 0;
  }

  bool hasTracks() const { return !tracks_.empty(); }

  const char* label() const { return label_; }

  const Stats& stats() const { return stats_; }

  const String& activeTrack() const { return active_track_; }

  bool isTrackRunning() const { return decoder_.isRunning(); }

  bool hasPendingNextRequest() const { return pending_next_requests_ != 0; }

  size_t currentTrackIndex() const { return track_index_; }

  size_t playlistSize() const { return tracks_.size(); }

  size_t currentFilePosition() const { return source_.position(); }

  size_t currentFileSize() const { return source_.size(); }

  uint32_t outputSampleRate() const { return output_sample_rate_; }

  void stop() {
    decoder_.stop();
    closeSourceTimed();
    track_index_ = 0;
    active_track_ = String();
    pending_next_requests_ = 0;
    pending_next_request_ms_ = 0;
    clearPcmBuffer();
    syncDebugSnapshot();
  }

  void requestNextTrack() {
    queueNextTracks(1, millis());
  }

  void queueNextTracks(size_t steps, uint32_t request_ms) {
    if (tracks_.empty() || steps == 0) return;

    stats_.manual_next_requests += static_cast<uint32_t>(steps);
    pending_next_requests_ += steps;
    pending_next_request_ms_ = request_ms;
    syncDebugSnapshot();
  }

  bool servicePendingNextRequest(bool queue_safe, uint32_t now_ms) {
    if (pending_next_requests_ == 0) return false;

    const uint32_t pending_age_ms =
        static_cast<uint32_t>(now_ms - pending_next_request_ms_);
    if (pending_age_ms < kTrackSwitchCoalesceMs) {
      return false;
    }
    if (!queue_safe && pending_age_ms < kTrackSwitchMaxDelayMs) {
      return false;
    }

    const uint32_t request_start_us = micros();
    const size_t requested_steps = pending_next_requests_;
    const size_t pending_steps = requested_steps % tracks_.size();
    pending_next_requests_ = 0;
    decoder_.stop();
    closeSourceTimed();
    clearPcmBuffer();
    if (pending_steps != 0) {
      advanceTrack(pending_steps);
    }
    const uint32_t elapsed_us = static_cast<uint32_t>(micros() - request_start_us);
    stats_.request_next_us.add(elapsed_us);
    stats_.request_next_last_us = elapsed_us;
    syncDebugSnapshot();
    return true;
  }

  void copyDebugSnapshot(DebugSnapshot& out) const {
    portENTER_CRITICAL(&debug_snapshot_mux_);
    out = debug_snapshot_;
    portEXIT_CRITICAL(&debug_snapshot_mux_);
  }

  size_t readSamples(int16_t* dst, size_t sample_count) override {
    const uint32_t read_start_us = micros();

    if (dst == nullptr || sample_count == 0 || output_sample_rate_ == 0 || tracks_.empty()) {
      const uint32_t elapsed_us = static_cast<uint32_t>(micros() - read_start_us);
      noteReadStats(0, elapsed_us);
      return 0;
    }

    sample_count = alignStereoSamples(sample_count);
    ensurePcmBufferCapacity(max(kVoicePcmBufferSamples, sample_count));

    if (pcm_buffered_samples_ < sample_count) {
      refillPcmBuffer(max(sample_count, kVoicePcmLowWaterSamples));
    } else if (pcm_buffered_samples_ < kVoicePcmLowWaterSamples) {
      refillPcmBuffer(kVoicePcmLowWaterSamples);
    }

    const size_t produced_total = min(sample_count, pcm_buffered_samples_);
    if (produced_total > 0) {
      memcpy(dst, pcm_buffer_.data(), produced_total * sizeof(int16_t));
      consumePcmBuffer(produced_total);
    }

    if (produced_total < sample_count && (decoder_.isRunning() || hasTracks())) {
      ++stats_.short_reads;
    }

    const uint32_t elapsed_us = static_cast<uint32_t>(micros() - read_start_us);
    noteReadStats(produced_total, elapsed_us);
    syncDebugSnapshot();
    return produced_total;
  }

  bool eof() const override { return tracks_.empty(); }

 private:
  bool ensureTrackOpen() {
    if (decoder_.isRunning()) return true;
    if (tracks_.empty() || output_sample_rate_ == 0) return false;

    const size_t track_count = tracks_.size();
    for (size_t attempt = 0; attempt < track_count; ++attempt) {
      if (openTrack(tracks_[track_index_])) return true;
      advanceTrack();
    }

    return false;
  }

  bool openTrack(const String& path) {
    ++stats_.track_open_attempts;
    decoder_.stop();
    closeSourceTimed();

    if (!source_.begin()) {
      ++stats_.source_begin_failures;
      Serial.printf("[%s] source init failed\n", label_);
      return false;
    }

    const uint32_t open_start_us = micros();
    const bool open_ok = source_.open(path);
    const uint32_t open_elapsed_us = static_cast<uint32_t>(micros() - open_start_us);
    stats_.open_us.add(open_elapsed_us);
    stats_.open_last_us = open_elapsed_us;

    if (!open_ok) {
      ++stats_.open_failures;
      Serial.printf("[%s] open failed: %s\n", label_, path.c_str());
      return false;
    }

    const uint32_t decoder_begin_start_us = micros();
    const bool decoder_begin_ok = decoder_.begin(source_);
    const uint32_t decoder_begin_elapsed_us =
        static_cast<uint32_t>(micros() - decoder_begin_start_us);
    stats_.decoder_begin_us.add(decoder_begin_elapsed_us);
    stats_.decoder_begin_last_us = decoder_begin_elapsed_us;

    if (!decoder_begin_ok) {
      ++stats_.invalid_wav;
      Serial.printf("[%s] invalid WAV: %s\n", label_, path.c_str());
      closeSourceTimed();
      return false;
    }

    const padre::WavStreamInfo& info = decoder_.streamInfo();
    if (info.sample_rate != output_sample_rate_) {
      ++stats_.sample_rate_mismatches;
      Serial.printf("[%s] sample rate mismatch: %s (%lu Hz)\n",
                    label_,
                    path.c_str(),
                    static_cast<unsigned long>(info.sample_rate));
      decoder_.stop();
      closeSourceTimed();
      return false;
    }

    ++stats_.track_opened;
    stats_.last_track_start_ms = millis();
    stats_.last_track_sample_rate = info.sample_rate;
    stats_.last_track_channels = info.output_channels;
    if (info.output_channels >= 2) {
      ++stats_.stereo_tracks;
    } else {
      ++stats_.mono_tracks;
    }

    active_track_ = path;
    if (kRuntimeActionLogsEnabled) {
      Serial.printf("[%s] now playing: %s\n", label_, active_track_.c_str());
    }
    syncDebugSnapshot();
    return true;
  }

  void advanceTrack(size_t steps = 1) {
    if (tracks_.empty()) return;
    if (steps == 0) return;

    const size_t track_count = tracks_.size();
    const size_t wrapped_index = track_index_ + steps;
    const size_t wraps = wrapped_index / track_count;
    if ((wrapped_index % track_count) == 0 && wrapped_index != 0) {
      ++stats_.playlist_wraps;
      if (wraps > 1) stats_.playlist_wraps += static_cast<uint32_t>(wraps - 1);
    } else if (wraps > 0) {
      stats_.playlist_wraps += static_cast<uint32_t>(wraps);
    }
    track_index_ = wrapped_index % track_count;
  }

  void noteReadStats(size_t produced_samples, uint32_t elapsed_us) {
    ++stats_.read_calls;
    stats_.read_total_us += elapsed_us;
    stats_.last_read_us = elapsed_us;
    stats_.last_read_samples = produced_samples;
    stats_.buffer_last_samples = pcm_buffered_samples_;
    if (pcm_buffered_samples_ > stats_.buffer_peak_samples) {
      stats_.buffer_peak_samples = pcm_buffered_samples_;
    }
    stats_.produced_samples += produced_samples;
    if (elapsed_us > stats_.read_max_us) stats_.read_max_us = elapsed_us;
    if (elapsed_us >= kDiagSlowVoiceReadUs) ++stats_.slow_reads;

    if (produced_samples == 0) {
      ++stats_.zero_reads;
      ++stats_.consecutive_zero_reads;
    } else {
      stats_.consecutive_zero_reads = 0;
    }
  }

  void closeSourceTimed() {
    if (!source_.isOpen()) return;

    const uint32_t close_start_us = micros();
    source_.close();
    const uint32_t close_elapsed_us = static_cast<uint32_t>(micros() - close_start_us);
    stats_.close_us.add(close_elapsed_us);
    stats_.close_last_us = close_elapsed_us;
  }

  bool ensurePcmBufferCapacity(size_t capacity_samples) {
    if (pcm_buffer_.size() >= capacity_samples) return true;
    pcm_buffer_.resize(capacity_samples);
    return pcm_buffer_.size() >= capacity_samples;
  }

  void clearPcmBuffer() {
    pcm_buffered_samples_ = 0;
    stats_.buffer_last_samples = 0;
  }

  void consumePcmBuffer(size_t consumed_samples) {
    if (consumed_samples >= pcm_buffered_samples_) {
      pcm_buffered_samples_ = 0;
      return;
    }

    const size_t remain = pcm_buffered_samples_ - consumed_samples;
    memmove(pcm_buffer_.data(),
            pcm_buffer_.data() + consumed_samples,
            remain * sizeof(int16_t));
    pcm_buffered_samples_ = remain;
  }

  void refillPcmBuffer(size_t target_samples) {
    if (pcm_buffer_.empty()) return;

    const size_t capped_target = min(target_samples, pcm_buffer_.size());
    uint8_t attempts = 0;

    while (pcm_buffered_samples_ < capped_target && attempts < kVoicePcmMaxRefillAttempts) {
      const size_t free_capacity = pcm_buffer_.size() - pcm_buffered_samples_;
      if (free_capacity < 2) break;

      const size_t request = alignStereoSamples(min(free_capacity, kVoicePcmRefillChunkSamples));
      if (request < 2) break;

      const size_t produced_now =
          decodePcmSamples(pcm_buffer_.data() + pcm_buffered_samples_, request);
      if (produced_now == 0) break;

      pcm_buffered_samples_ += produced_now;
      if (pcm_buffered_samples_ > stats_.buffer_peak_samples) {
        stats_.buffer_peak_samples = pcm_buffered_samples_;
      }

      ++attempts;
      if (produced_now < request) break;
    }
  }

  size_t decodePcmSamples(int16_t* dst, size_t sample_count) {
    if (dst == nullptr || sample_count == 0) return 0;

    sample_count = alignStereoSamples(sample_count);
    size_t produced_total = 0;

    while (produced_total < sample_count) {
      if (!ensureTrackOpen()) break;

      const padre::WavStreamInfo& info = decoder_.streamInfo();
      size_t produced_now = 0;

      if (info.output_channels >= 2) {
        const size_t request = alignStereoSamples(sample_count - produced_total);
        produced_now = alignStereoSamples(decoder_.decode(dst + produced_total, request));
      } else {
        const size_t stereo_room = sample_count - produced_total;
        const size_t mono_capacity = stereo_room / 2;
        if (mono_capacity == 0) break;

        if (mono_decode_buffer_.size() < mono_capacity) {
          mono_decode_buffer_.resize(mono_capacity);
        }

        const size_t mono_samples = decoder_.decode(mono_decode_buffer_.data(), mono_capacity);
        for (size_t i = 0; i < mono_samples; ++i) {
          const int16_t sample = mono_decode_buffer_[i];
          dst[produced_total + (i * 2)] = sample;
          dst[produced_total + (i * 2) + 1] = sample;
        }
        produced_now = mono_samples * 2;
      }

      produced_total += produced_now;

      if (decoder_.isRunning()) {
        if (produced_now == 0) break;
        continue;
      }

      closeSourceTimed();
      ++stats_.track_ended;
      advanceTrack();

      if (produced_now == 0) continue;
    }

    return produced_total;
  }

  void syncDebugSnapshot() {
    DebugSnapshot snapshot;
    snapshot.stats = stats_;
    snapshot.current_track_index = track_index_;
    snapshot.playlist_size = tracks_.size();
    snapshot.current_file_position = source_.position();
    snapshot.current_file_size = source_.size();
    snapshot.is_track_running = decoder_.isRunning();
    if (active_track_.length() > 0) {
      snprintf(snapshot.active_track, sizeof(snapshot.active_track), "%s", active_track_.c_str());
    } else {
      snapshot.active_track[0] = '\0';
    }

    portENTER_CRITICAL(&debug_snapshot_mux_);
    debug_snapshot_ = snapshot;
    portEXIT_CRITICAL(&debug_snapshot_mux_);
  }

  const char* label_ = nullptr;
  padre::FsAudioSource source_;
  padre::WavDecoder decoder_;
  std::vector<String> tracks_;
  std::vector<int16_t> pcm_buffer_;
  std::vector<int16_t> mono_decode_buffer_;
  size_t pcm_buffered_samples_ = 0;
  size_t track_index_ = 0;
  uint32_t output_sample_rate_ = 0;
  String active_track_;
  Stats stats_;
  size_t pending_next_requests_ = 0;
  uint32_t pending_next_request_ms_ = 0;
  mutable portMUX_TYPE debug_snapshot_mux_ = portMUX_INITIALIZER_UNLOCKED;
  DebugSnapshot debug_snapshot_;
};

struct RuntimeDiagnostics {
  DurationStats loop_us;
  DurationStats service_us;
  DurationStats pump_us;
  SampleStats queue_samples;

  uint32_t loop_slow = 0;
  uint32_t service_slow = 0;
  uint32_t pump_slow = 0;

  uint32_t queue_low_events = 0;
  uint32_t queue_low_events_total = 0;
  uint32_t queue_empty_events = 0;
  uint32_t queue_empty_events_total = 0;
  bool snapshot_low_pending = false;
  bool snapshot_empty_pending = false;
  size_t snapshot_trigger_queue_min = SIZE_MAX;
  bool snapshot_rearm_wait = false;

  uint32_t pump_calls = 0;
  uint32_t pump_calls_total = 0;
  uint64_t pump_samples = 0;
  uint64_t pump_samples_total = 0;
  uint32_t pump_zero_with_data = 0;
  uint32_t pump_zero_with_data_total = 0;

  uint32_t write_calls = 0;
  uint32_t write_calls_total = 0;
  uint64_t written_samples = 0;
  uint64_t written_samples_total = 0;
  uint32_t short_writes = 0;
  uint32_t short_writes_total = 0;
  uint32_t zero_writes = 0;
  uint32_t zero_writes_total = 0;

  uint32_t mix_calls = 0;
  uint32_t mix_calls_total = 0;
  uint64_t mixed_samples = 0;
  uint64_t mixed_samples_total = 0;
  uint32_t mix_zero = 0;
  uint32_t mix_zero_total = 0;

  uint32_t refill_loops = 0;
  uint32_t refill_loops_total = 0;
  uint32_t refill_iterations = 0;
  uint32_t refill_iterations_total = 0;
  uint32_t refill_peak_iterations = 0;
  uint32_t refill_no_progress = 0;
  uint32_t refill_no_progress_total = 0;
  uint32_t service_budget_hits = 0;
  uint32_t service_budget_hits_total = 0;

  uint32_t touch_polls = 0;
  uint32_t touch_polls_total = 0;
  uint32_t touch_events = 0;
  uint32_t touch_events_total = 0;
  uint32_t touch_event_counts[4] = {0, 0, 0, 0};
  uint32_t touch_event_counts_total[4] = {0, 0, 0, 0};
  uint32_t touch_irq_count_snapshot = 0;
  uint32_t max_touch_poll_gap_ms = 0;
  uint32_t last_touch_poll_ms = 0;

  size_t startup_prefill_samples = 0;
  uint32_t startup_prefill_ms = 0;
  bool startup_prefill_done = false;

  uint32_t last_report_ms = 0;
  bool queue_low_active = false;
  bool queue_empty_active = false;
  DurationStats report_us;
  uint32_t report_last_us = 0;
  uint32_t report_slow = 0;
  uint32_t reports = 0;
  uint32_t reports_total = 0;

  void noteQueue(size_t queued_samples) {
    queue_samples.add(queued_samples);

    if (queued_samples >= kDiagQueueRearmSamples) {
      snapshot_rearm_wait = false;
    }

    const bool is_low = queued_samples <= kDiagQueueLowSamples;
    if (!snapshot_rearm_wait && is_low && !queue_low_active) {
      ++queue_low_events;
      ++queue_low_events_total;
      snapshot_low_pending = true;
    }
    queue_low_active = is_low;

    const bool is_empty = queued_samples == 0;
    if (!snapshot_rearm_wait && is_empty && !queue_empty_active) {
      ++queue_empty_events;
      ++queue_empty_events_total;
      snapshot_empty_pending = true;
    }
    queue_empty_active = is_empty;

    if ((snapshot_low_pending || snapshot_empty_pending) &&
        (snapshot_trigger_queue_min == SIZE_MAX || queued_samples < snapshot_trigger_queue_min)) {
      snapshot_trigger_queue_min = queued_samples;
    }
  }

  void noteLoop(uint32_t elapsed_us) {
    loop_us.add(elapsed_us);
    if (elapsed_us >= kDiagSlowLoopUs) ++loop_slow;
  }

  void noteService(uint32_t elapsed_us) {
    service_us.add(elapsed_us);
    if (elapsed_us >= kDiagSlowServiceUs) ++service_slow;
  }

  void notePump(size_t queued_before, size_t pumped_samples, uint32_t elapsed_us) {
    ++pump_calls;
    ++pump_calls_total;
    pump_samples += pumped_samples;
    pump_samples_total += pumped_samples;
    pump_us.add(elapsed_us);
    if (elapsed_us >= kDiagSlowPumpUs) ++pump_slow;
    if (queued_before > 0 && pumped_samples == 0) {
      ++pump_zero_with_data;
      ++pump_zero_with_data_total;
    }
  }

  void noteWrite(size_t requested_samples, size_t written_samples_now) {
    ++write_calls;
    ++write_calls_total;
    written_samples += written_samples_now;
    written_samples_total += written_samples_now;
    if (written_samples_now == 0) {
      ++zero_writes;
      ++zero_writes_total;
    }
    if (written_samples_now < requested_samples) {
      ++short_writes;
      ++short_writes_total;
    }
  }

  void noteMix(size_t mixed_samples_now) {
    ++mix_calls;
    ++mix_calls_total;
    mixed_samples += mixed_samples_now;
    mixed_samples_total += mixed_samples_now;
    if (mixed_samples_now == 0) {
      ++mix_zero;
      ++mix_zero_total;
    }
  }

  void noteRefill(uint32_t iterations, bool made_progress) {
    ++refill_loops;
    ++refill_loops_total;
    refill_iterations += iterations;
    refill_iterations_total += iterations;
    if (iterations > refill_peak_iterations) refill_peak_iterations = iterations;
    if (!made_progress) {
      ++refill_no_progress;
      ++refill_no_progress_total;
    }
  }

  void noteServiceBudgetHit() {
    ++service_budget_hits;
    ++service_budget_hits_total;
  }

  void noteTouchPoll(uint32_t now_ms) {
    ++touch_polls;
    ++touch_polls_total;
    if (last_touch_poll_ms != 0) {
      const uint32_t gap_ms = now_ms - last_touch_poll_ms;
      if (gap_ms > max_touch_poll_gap_ms) max_touch_poll_gap_ms = gap_ms;
    }
    last_touch_poll_ms = now_ms;
  }

  void noteTouchEvent(uint8_t electrode) {
    ++touch_events;
    ++touch_events_total;
    if (electrode < 4) {
      ++touch_event_counts[electrode];
      ++touch_event_counts_total[electrode];
    }
  }

  void noteStartupPrefill(uint32_t elapsed_ms, size_t queued_samples) {
    startup_prefill_done = true;
    startup_prefill_ms = elapsed_ms;
    startup_prefill_samples = queued_samples;
  }

  void noteReport(uint32_t elapsed_us) {
    ++reports;
    ++reports_total;
    report_us.add(elapsed_us);
    report_last_us = elapsed_us;
    if (elapsed_us >= kDiagSlowReportUs) ++report_slow;
  }

  bool snapshotPending() const { return snapshot_low_pending || snapshot_empty_pending; }

  const char* snapshotEventName() const {
    if (snapshot_empty_pending) return "queue-empty";
    if (snapshot_low_pending) return "queue-low";
    return "manual";
  }

  size_t snapshotTriggerQueueMin() const {
    return snapshot_trigger_queue_min == SIZE_MAX ? 0 : snapshot_trigger_queue_min;
  }

  void clearSnapshotPending() {
    snapshot_low_pending = false;
    snapshot_empty_pending = false;
    snapshot_trigger_queue_min = SIZE_MAX;
    snapshot_rearm_wait = true;
  }

  void resetWindow() {
    loop_us.reset();
    service_us.reset();
    pump_us.reset();
    queue_samples.reset();
    loop_slow = 0;
    service_slow = 0;
    pump_slow = 0;
    queue_low_events = 0;
    queue_empty_events = 0;
    pump_calls = 0;
    pump_samples = 0;
    pump_zero_with_data = 0;
    write_calls = 0;
    written_samples = 0;
    short_writes = 0;
    zero_writes = 0;
    mix_calls = 0;
    mixed_samples = 0;
    mix_zero = 0;
    refill_loops = 0;
    refill_iterations = 0;
    refill_peak_iterations = 0;
    refill_no_progress = 0;
    service_budget_hits = 0;
    touch_polls = 0;
    touch_events = 0;
    max_touch_poll_gap_ms = 0;
    reports = 0;
    for (uint8_t i = 0; i < 4; ++i) {
      touch_event_counts[i] = 0;
    }
  }
};

bool inspectWavTrack(const String& path, uint32_t& sample_rate_out) {
  padre::FsAudioSource source(SD, padre::FsAudioSourceConfig{"sd"});
  padre::WavDecoder decoder;

  if (!source.begin()) return false;
  if (!source.open(path)) return false;

  const bool ok = decoder.begin(source);
  if (ok) sample_rate_out = decoder.streamInfo().sample_rate;

  decoder.stop();
  source.close();
  return ok;
}

size_t scanWavFiles(const char* dir_path, std::vector<String>& out_tracks) {
  out_tracks.clear();

  const padre::AudioFileScannerOptions options{
      kMaxDirDepth,
      kWavExtensions,
      kWavExtensionCount,
  };

  padre::AudioFileScanner scanner(SD);
  scanner.scan(dir_path, out_tracks, options);
  std::sort(out_tracks.begin(), out_tracks.end(), [](const String& lhs, const String& rhs) {
    return lhs.compareTo(rhs) < 0;
  });
  return out_tracks.size();
}

bool detectOutputSampleRate(const std::vector<String>& music_candidates,
                            const std::vector<String>& foley_candidates,
                            uint32_t& out_sample_rate) {
  const std::vector<String>* lists[] = {
      &music_candidates,
      &foley_candidates,
  };

  for (const auto* list : lists) {
    for (const auto& path : *list) {
      uint32_t sample_rate = 0;
      if (!inspectWavTrack(path, sample_rate)) continue;
      out_sample_rate = sample_rate;
      return true;
    }
  }

  return false;
}

void filterPlaylistBySampleRate(const char* label,
                                const std::vector<String>& candidates,
                                uint32_t target_sample_rate,
                                std::vector<String>& accepted) {
  accepted.clear();

  for (const auto& path : candidates) {
    uint32_t sample_rate = 0;
    if (!inspectWavTrack(path, sample_rate)) {
      Serial.printf("[%s] skipped invalid WAV: %s\n", label, path.c_str());
      continue;
    }

    if (sample_rate != target_sample_rate) {
      Serial.printf("[%s] skipped sample rate mismatch: %s (%lu Hz)\n",
                    label,
                    path.c_str(),
                    static_cast<unsigned long>(sample_rate));
      continue;
    }

    accepted.push_back(path);
  }
}

void printTrackList(const char* label, const std::vector<String>& tracks) {
  Serial.printf("[%s] %u track(s)\n", label, static_cast<unsigned>(tracks.size()));
  for (const auto& track : tracks) {
    Serial.printf("  - %s\n", track.c_str());
  }
}

SPIClass g_sd_spi(FSPI);
Adafruit_MPR121 g_mpr121;
uint32_t g_last_touch_poll_ms = 0;
bool g_touch_ready = false;
RuntimeDiagnostics g_diag;
volatile size_t g_last_queue_samples = 0;
volatile bool g_audio_ready = false;
TaskHandle_t g_audio_task_handle = nullptr;
portMUX_TYPE g_diag_mux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE g_control_mux = portMUX_INITIALIZER_UNLOCKED;

struct PendingControlState {
  uint32_t music_next_requests = 0;
  uint32_t foley_next_requests = 0;
  int32_t volume_delta = 0;
};

PendingControlState g_pending_controls;

extern LoopingWavVoice g_music_voice;
extern LoopingWavVoice g_foley_voice;

padre::Mpr121AdafruitDriver g_touch_device(
    g_mpr121,
    Wire,
    padre::Mpr121AdafruitDriverPins{
        static_cast<int8_t>(MPR121_SDA),
        static_cast<int8_t>(MPR121_SCL),
        static_cast<int8_t>(MPR121_IRQ),
        MPR121_ADDR,
        400000,
    },
    padre::Mpr121AdafruitDriverConfig{
        static_cast<uint8_t>(kTouchThreshold),
        static_cast<uint8_t>(kReleaseThreshold),
        true,
        false,
        250,
        padre::Mpr121DiagnosticsOutputMode::Summary,
        true,
    });

void notifyAudioTask() {
  if (g_audio_task_handle != nullptr) {
    xTaskNotifyGive(g_audio_task_handle);
  }
}

void queueMusicNextRequest() {
  portENTER_CRITICAL(&g_control_mux);
  if (g_pending_controls.music_next_requests < UINT32_MAX) {
    ++g_pending_controls.music_next_requests;
  }
  portEXIT_CRITICAL(&g_control_mux);
  notifyAudioTask();
}

void queueFoleyNextRequest() {
  portENTER_CRITICAL(&g_control_mux);
  if (g_pending_controls.foley_next_requests < UINT32_MAX) {
    ++g_pending_controls.foley_next_requests;
  }
  portEXIT_CRITICAL(&g_control_mux);
  notifyAudioTask();
}

void queueVolumeDelta(int delta) {
  portENTER_CRITICAL(&g_control_mux);
  const int32_t updated_delta =
      g_pending_controls.volume_delta + static_cast<int32_t>(delta);
  g_pending_controls.volume_delta = max<int32_t>(-kVolumeMax, min<int32_t>(kVolumeMax, updated_delta));
  portEXIT_CRITICAL(&g_control_mux);
  notifyAudioTask();
}

PendingControlState takePendingControls() {
  PendingControlState pending;
  portENTER_CRITICAL(&g_control_mux);
  pending = g_pending_controls;
  g_pending_controls = {};
  portEXIT_CRITICAL(&g_control_mux);
  return pending;
}

bool hasPendingControls() {
  portENTER_CRITICAL(&g_control_mux);
  const bool has_pending = g_pending_controls.music_next_requests != 0 ||
                           g_pending_controls.foley_next_requests != 0 ||
                           g_pending_controls.volume_delta != 0;
  portEXIT_CRITICAL(&g_control_mux);
  return has_pending;
}

void diagNoteQueue(size_t queued_samples) {
  portENTER_CRITICAL(&g_diag_mux);
  g_last_queue_samples = queued_samples;
  g_diag.noteQueue(queued_samples);
  portEXIT_CRITICAL(&g_diag_mux);
}

void diagNoteLoop(uint32_t elapsed_us) {
  portENTER_CRITICAL(&g_diag_mux);
  g_diag.noteLoop(elapsed_us);
  portEXIT_CRITICAL(&g_diag_mux);
}

void diagNoteService(uint32_t elapsed_us) {
  portENTER_CRITICAL(&g_diag_mux);
  g_diag.noteService(elapsed_us);
  portEXIT_CRITICAL(&g_diag_mux);
}

void diagNotePump(size_t queued_before, size_t pumped_samples, uint32_t elapsed_us) {
  portENTER_CRITICAL(&g_diag_mux);
  g_diag.notePump(queued_before, pumped_samples, elapsed_us);
  portEXIT_CRITICAL(&g_diag_mux);
}

void diagNoteWrite(size_t requested_samples, size_t written_samples_now) {
  portENTER_CRITICAL(&g_diag_mux);
  g_diag.noteWrite(requested_samples, written_samples_now);
  portEXIT_CRITICAL(&g_diag_mux);
}

void diagNoteMix(size_t mixed_samples_now) {
  portENTER_CRITICAL(&g_diag_mux);
  g_diag.noteMix(mixed_samples_now);
  portEXIT_CRITICAL(&g_diag_mux);
}

void diagNoteRefill(uint32_t iterations, bool made_progress) {
  portENTER_CRITICAL(&g_diag_mux);
  g_diag.noteRefill(iterations, made_progress);
  portEXIT_CRITICAL(&g_diag_mux);
}

void diagNoteServiceBudgetHit() {
  portENTER_CRITICAL(&g_diag_mux);
  g_diag.noteServiceBudgetHit();
  portEXIT_CRITICAL(&g_diag_mux);
}

void diagNoteTouchPoll(uint32_t now_ms) {
  portENTER_CRITICAL(&g_diag_mux);
  g_diag.noteTouchPoll(now_ms);
  portEXIT_CRITICAL(&g_diag_mux);
}

void diagNoteTouchEvent(uint8_t electrode) {
  portENTER_CRITICAL(&g_diag_mux);
  g_diag.noteTouchEvent(electrode);
  portEXIT_CRITICAL(&g_diag_mux);
}

void diagNoteStartupPrefill(uint32_t elapsed_ms, size_t queued_samples) {
  portENTER_CRITICAL(&g_diag_mux);
  g_diag.noteStartupPrefill(elapsed_ms, queued_samples);
  portEXIT_CRITICAL(&g_diag_mux);
}

void diagNoteReport(uint32_t elapsed_us) {
  portENTER_CRITICAL(&g_diag_mux);
  g_diag.noteReport(elapsed_us);
  portEXIT_CRITICAL(&g_diag_mux);
}

void onTouchEvent(void*, const padre::InputEvent& event) {
  if (event.type != padre::InputEventType::PressDown) return;

  diagNoteTouchEvent(event.source_id);

  switch (event.source_id) {
    case 0:
      if (kRuntimeActionLogsEnabled) Serial.println("Touch 0: next music track");
      queueMusicNextRequest();
      break;
    case 1:
      if (kRuntimeActionLogsEnabled) Serial.println("Touch 1: next foley track");
      queueFoleyNextRequest();
      break;
    case 2:
      if (kRuntimeActionLogsEnabled) Serial.println("Touch 2: volume down");
      queueVolumeDelta(-1);
      break;
    case 3:
      if (kRuntimeActionLogsEnabled) Serial.println("Touch 3: volume up");
      queueVolumeDelta(1);
      break;
    default:
      break;
  }
}

padre::Mpr121TouchController g_touch_controller(
    g_touch_device.asTouchControllerIo(),
    padre::Mpr121TouchControllerConfig{
        4,
        padre::Mpr121InputConfig{},
        false,
        &Serial,
    });

padre::RuntimeCommandEntry g_runtime_commands[] = {
    {"mpr121",
     padre::Mpr121AdafruitDriver::handleRuntimeCommandEntry,
     &g_touch_device,
     "mpr121 [status|dump|scan|stream <on|off>|mode <summary|table|plot>|rate <ms>|thresholds <touch> <release>|auto <on|off>|help]"},
};

padre::SerialRuntimeConsole g_runtime_console(
    nullptr,
    0,
    Serial,
    g_runtime_commands,
    sizeof(g_runtime_commands) / sizeof(g_runtime_commands[0]));

padre::Esp32StdI2sOutputIo g_i2s_io(
    padre::Esp32StdI2sPins{I2S_BCLK, I2S_LRC, I2S_DOUT, -1},
    padre::Esp32StdI2sOutputConfig{
        kI2sDmaDescNum,
        kI2sDmaFrameNum,
        2,
        kI2sWriteTimeoutMs,
        kI2sWorkSamples,
    },
    padre::Esp32StdI2sSampleTransform{
        nullptr,
        i2sApplyVolumeSample,
    });

padre::BufferedI2sOutput g_sink(
    g_i2s_io.asIo(),
    padre::I2sOutputConfig{
        kSinkQueueSamples,
        kSinkWatermarkSamples,
    });

padre::VoiceMixer g_mixer(2);
LoopingWavVoice g_music_voice("music", SD);
LoopingWavVoice g_foley_voice("foley", SD);

std::vector<String> g_music_tracks;
std::vector<String> g_foley_tracks;

int16_t g_mix_buffer[kMixChunkSamples] = {0};

void serviceTouch(uint32_t now_ms) {
  if (!g_touch_ready) return;

  const bool touch_irq = g_touch_device.consumeIrq();

  if (touch_irq || (now_ms - g_last_touch_poll_ms) >= kTouchPollMs) {
    diagNoteTouchPoll(now_ms);
    g_touch_controller.poll(now_ms);
    g_last_touch_poll_ms = now_ms;
    g_touch_device.serviceRuntime(now_ms);
  }
}

void noteCurrentQueueLevel() {
  diagNoteQueue(g_sink.queuedSamples());
}

size_t pumpSink() {
  const size_t queued_before = g_sink.queuedSamples();
  const uint32_t start_us = micros();
  const size_t pumped_samples = g_sink.pump();
  const uint32_t elapsed_us = static_cast<uint32_t>(micros() - start_us);
  diagNotePump(queued_before, pumped_samples, elapsed_us);
  noteCurrentQueueLevel();
  return pumped_samples;
}

size_t mixVoices(size_t request_samples) {
  const uint32_t start_us = micros();
  const size_t mixed_samples = alignStereoSamples(g_mixer.mix(g_mix_buffer, request_samples));
  diagNoteMix(mixed_samples);
  (void)start_us;
  return mixed_samples;
}

size_t writeMixedSamples(size_t sample_count) {
  const size_t written_samples = g_sink.write(g_mix_buffer, sample_count);
  diagNoteWrite(sample_count, written_samples);
  noteCurrentQueueLevel();
  return written_samples;
}

bool servicePendingTrackSwitches() {
  const bool queue_safe = g_sink.queuedSamples() >= kTrackSwitchSafeQueueSamples;
  const uint32_t now_ms = millis();
  if (g_music_voice.servicePendingNextRequest(queue_safe, now_ms)) return true;
  if (g_foley_voice.servicePendingNextRequest(queue_safe, now_ms)) return true;
  return false;
}

void applyPendingControls(uint32_t now_ms) {
  const PendingControlState pending = takePendingControls();
  if (pending.music_next_requests != 0) {
    g_music_voice.queueNextTracks(pending.music_next_requests, now_ms);
  }
  if (pending.foley_next_requests != 0) {
    g_foley_voice.queueNextTracks(pending.foley_next_requests, now_ms);
  }
  if (pending.volume_delta != 0) {
    stepVolume(static_cast<int>(pending.volume_delta));
  }
}

void printVoiceDiagnostics(const char* label, const LoopingWavVoice::DebugSnapshot& snapshot) {
  const auto& stats = snapshot.stats;
  const uint32_t read_avg_us =
      stats.read_calls == 0 ? 0 : static_cast<uint32_t>(stats.read_total_us / stats.read_calls);

  Serial.printf(
      "DIAG voice=%s running=%s idx=%u/%u file=%lu/%lu read avg/max/last=%lu/%lu/%luus "
      "next last/max=%lu/%luus close last/max=%lu/%luus open last/max=%lu/%luus "
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

void reportDiagnosticsIfDue(uint32_t now_ms, bool force = false) {
  const uint32_t report_start_us = micros();
  RuntimeDiagnostics diag_snapshot;
  uint32_t touch_irq_count_snapshot = 0;
  size_t queue_now = 0;

  portENTER_CRITICAL(&g_diag_mux);
  if (!force && !g_diag.snapshotPending()) {
    portEXIT_CRITICAL(&g_diag_mux);
    return;
  }

  diag_snapshot = g_diag;
  g_diag.last_report_ms = now_ms;
  touch_irq_count_snapshot = g_touch_device.irqCount();
  queue_now = g_last_queue_samples;
  g_diag.clearSnapshotPending();
  g_diag.resetWindow();
  g_diag.noteQueue(g_last_queue_samples);
  portEXIT_CRITICAL(&g_diag_mux);

  LoopingWavVoice::DebugSnapshot music_snapshot;
  LoopingWavVoice::DebugSnapshot foley_snapshot;
  g_music_voice.copyDebugSnapshot(music_snapshot);
  g_foley_voice.copyDebugSnapshot(foley_snapshot);

  const char* event_name = force ? "manual" : diag_snapshot.snapshotEventName();
  const size_t queue_trigger = diag_snapshot.snapshotTriggerQueueMin();
  const size_t queue_capacity = g_sink.queueCapacity();
  const size_t queue_avg = diag_snapshot.queue_samples.avg();
  const size_t queue_min = diag_snapshot.queue_samples.minValue();
  const size_t queue_max = diag_snapshot.queue_samples.max_value;
  const uint32_t free_heap = ESP.getFreeHeap();
  const uint32_t min_free_heap = ESP.getMinFreeHeap();
  const uint32_t max_alloc_heap = ESP.getMaxAllocHeap();
  const uint32_t psram_size = ESP.getPsramSize();
  const uint32_t free_psram = ESP.getFreePsram();

  Serial.printf(
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

  Serial.printf(
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
    Serial.printf(
        " psram=%lu/%lu",
        static_cast<unsigned long>(free_psram),
        static_cast<unsigned long>(psram_size));
  }

  Serial.printf(
      " touch irq=%lu polls=%lu events=%lu gap=%lums prefill=%lums/%lu\n",
      static_cast<unsigned long>(touch_irq_count_snapshot),
      static_cast<unsigned long>(diag_snapshot.touch_polls),
      static_cast<unsigned long>(diag_snapshot.touch_events),
      static_cast<unsigned long>(diag_snapshot.max_touch_poll_gap_ms),
      static_cast<unsigned long>(diag_snapshot.startup_prefill_ms),
      static_cast<unsigned long>(diag_snapshot.startup_prefill_samples));

  printVoiceDiagnostics(g_music_voice.label(), music_snapshot);
  printVoiceDiagnostics(g_foley_voice.label(), foley_snapshot);

  const uint32_t report_elapsed_us = static_cast<uint32_t>(micros() - report_start_us);
  diagNoteReport(report_elapsed_us);
}

void printPinout() {
  Serial.printf("Build:  %s\n", kBuildTag);
  Serial.printf("SD SPI: cs=%u sck=%u miso=%u mosi=%u\n", SD_CS, SD_SCK, SD_MISO, SD_MOSI);
  Serial.printf("I2S:    bclk=%u lrck=%u dout=%u\n", I2S_BCLK, I2S_LRC, I2S_DOUT);
  Serial.printf("MPR121: sda=%u scl=%u irq=%u addr=0x%02X\n",
                MPR121_SDA,
                MPR121_SCL,
                MPR121_IRQ,
                MPR121_ADDR);
  Serial.printf("Audio:  queue=%u prebuffer=%u target=%u dma=%ux%u work=%u\n",
                static_cast<unsigned>(kSinkQueueSamples),
                static_cast<unsigned>(kStartupPrebufferSamples),
                static_cast<unsigned>(kQueueRefillTargetSamples),
                static_cast<unsigned>(kI2sDmaDescNum),
                static_cast<unsigned>(kI2sDmaFrameNum),
                static_cast<unsigned>(kI2sWorkSamples));
}

bool initSd() {
  g_sd_spi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, g_sd_spi, kSdClockHz)) {
    Serial.println("SD.begin failed");
    return false;
  }
  return true;
}

bool initTouch() {
  g_touch_device.setDiagnosticsOutput(&Serial);
  g_touch_device.scanI2c(Serial);
  if (!g_touch_device.begin()) {
    Serial.println("MPR121 init failed, touch disabled");
    return false;
  }

  g_touch_controller.setEventHandler(nullptr, onTouchEvent);
  if (!g_touch_controller.begin()) {
    Serial.println("Touch controller init failed, touch disabled");
    return false;
  }

  g_touch_ready = true;
  Serial.println("MPR121 touch ready");
  return true;
}

bool preparePlaylists(uint32_t& out_sample_rate) {
  std::vector<String> music_candidates;
  std::vector<String> foley_candidates;

  scanWavFiles(kMusicDir, music_candidates);
  scanWavFiles(kFoleyDir, foley_candidates);

  Serial.printf("Scanned %s: %u file(s)\n",
                kMusicDir,
                static_cast<unsigned>(music_candidates.size()));
  Serial.printf("Scanned %s: %u file(s)\n",
                kFoleyDir,
                static_cast<unsigned>(foley_candidates.size()));

  if (!detectOutputSampleRate(music_candidates, foley_candidates, out_sample_rate)) {
    Serial.println("No valid WAV files found in /music or /foley");
    return false;
  }

  filterPlaylistBySampleRate("music", music_candidates, out_sample_rate, g_music_tracks);
  filterPlaylistBySampleRate("foley", foley_candidates, out_sample_rate, g_foley_tracks);

  printTrackList("music", g_music_tracks);
  printTrackList("foley", g_foley_tracks);

  if (g_music_tracks.empty() && g_foley_tracks.empty()) {
    Serial.println("No playable WAV files left after sample rate filtering");
    return false;
  }

  if (g_music_tracks.empty()) {
    Serial.println("Warning: /music has no playable WAV files, only /foley will run");
  }

  if (g_foley_tracks.empty()) {
    Serial.println("Warning: /foley has no playable WAV files, only /music will run");
  }

  return true;
}

bool startAudio(uint32_t sample_rate) {
  if (!g_music_voice.configure(sample_rate) || !g_foley_voice.configure(sample_rate)) {
    Serial.println("Voice configuration failed");
    return false;
  }

  g_music_voice.setTracks(g_music_tracks);
  g_foley_voice.setTracks(g_foley_tracks);

  g_mixer.attachSource(0, &g_music_voice);
  g_mixer.attachSource(1, &g_foley_voice);
  g_mixer.setGlobalGain(1.0f);
  g_mixer.setVoiceGain(0, kMusicGain);
  g_mixer.setVoiceGain(1, kFoleyGain);

  if (!g_sink.begin(padre::DecoderConfig{sample_rate, 16, true})) {
    Serial.println("I2S sink init failed");
    return false;
  }

  noteCurrentQueueLevel();
  g_i2s_io.setPrebuffering(true);
  const uint32_t prefill_start_ms = millis();
  while (g_sink.queuedSamples() < kStartupPrebufferSamples) {
    const size_t writable_samples = alignStereoSamples(g_sink.writableSamples());
    if (writable_samples < 2) break;

    const size_t request = min(kMixChunkSamples, writable_samples);
    const size_t mixed = mixVoices(request);
    if (mixed == 0) break;

    const size_t written = writeMixedSamples(mixed);
    if (written == 0) break;

    if (static_cast<uint32_t>(millis() - prefill_start_ms) > kStartupPrebufferBudgetMs) break;
  }
  g_i2s_io.setPrebuffering(false);
  pumpSink();
  diagNoteStartupPrefill(static_cast<uint32_t>(millis() - prefill_start_ms),
                         g_sink.queuedSamples());

  Serial.printf("Audio started at %lu Hz, queued=%lu samples\n",
                static_cast<unsigned long>(sample_rate),
                static_cast<unsigned long>(g_sink.queuedSamples()));
  return g_sink.queuedSamples() > 0;
}

void serviceAudio() {
  const uint32_t service_start_us = micros();
  pumpSink();

  uint32_t refill_iterations = 0;
  bool made_progress = false;
  while (g_sink.queuedSamples() < kQueueRefillTargetSamples) {
    if (static_cast<uint32_t>(micros() - service_start_us) >= kServiceBudgetUs) {
      diagNoteServiceBudgetHit();
      break;
    }

    ++refill_iterations;
    const size_t writable_samples = alignStereoSamples(g_sink.writableSamples());
    if (writable_samples < 2) break;

    const size_t request = min(kMixChunkSamples, writable_samples);
    const size_t mixed = mixVoices(request);
    if (mixed == 0) break;

    const size_t written = writeMixedSamples(mixed);
    if (written == 0) break;
    made_progress = true;
  }

  if (servicePendingTrackSwitches() &&
      static_cast<uint32_t>(micros() - service_start_us) >= kServiceBudgetUs) {
    diagNoteServiceBudgetHit();
  }

  diagNoteRefill(refill_iterations, made_progress);
  pumpSink();
  diagNoteService(static_cast<uint32_t>(micros() - service_start_us));
}

void audioTaskMain(void*) {
  for (;;) {
    if (!g_audio_ready) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
      continue;
    }

    const uint32_t loop_start_us = micros();
    applyPendingControls(millis());
    serviceAudio();
    diagNoteLoop(static_cast<uint32_t>(micros() - loop_start_us));

    const bool idle = !hasPendingControls() && !g_music_voice.hasPendingNextRequest() &&
                      !g_foley_voice.hasPendingNextRequest() &&
                      g_sink.queuedSamples() >= kQueueRefillTargetSamples &&
                      !g_sink.pumpRequested();
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(idle ? 2 : kAudioTaskLoopDelayMs));
  }
}

bool startAudioTask() {
  if (g_audio_task_handle != nullptr) return true;

  BaseType_t result = xTaskCreatePinnedToCore(audioTaskMain,
                                              "audio_service",
                                              kAudioTaskStackBytes,
                                              nullptr,
                                              kAudioTaskPriority,
                                              &g_audio_task_handle,
                                              kAudioTaskCore);
  if (result != pdPASS) {
    g_audio_task_handle = nullptr;
    Serial.println("Audio task create failed");
    return false;
  }

  return true;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("DualSdWavLoopI2s");
  Serial.println("Serial runtime console: help/mpr121 ...");
  printPinout();
  g_diag.resetWindow();

  if (!initSd()) return;
  applyVolume();
  initTouch();

  uint32_t sample_rate = 0;
  if (!preparePlaylists(sample_rate)) return;
  if (!startAudio(sample_rate)) return;
  if (!startAudioTask()) return;

  g_audio_ready = true;
  notifyAudioTask();
}

void loop() {
  g_runtime_console.poll(Serial);

  if (!g_audio_ready) {
    delay(100);
    return;
  }

  serviceTouch(millis());
  reportDiagnosticsIfDue(millis(), false);
  delay(0);
}
