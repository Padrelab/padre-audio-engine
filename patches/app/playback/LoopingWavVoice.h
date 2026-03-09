#pragma once

#include <Arduino.h>
#include <FS.h>

#include <vector>

#include "../../audio/decoder/WavDecoder.h"
#include "../../audio/mixer/VoiceMixer.h"
#include "../../media/source/FsAudioSource.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#endif

namespace padre {

struct LoopingWavVoiceConfig {
  size_t pcm_buffer_samples = 8192;
  size_t pcm_low_water_samples = 2048;
  size_t pcm_refill_chunk_samples = 1024;
  uint8_t pcm_max_refill_attempts = 2;
  size_t track_switch_min_queue_samples = 4096;
  uint32_t track_switch_coalesce_ms = 60;
  uint32_t track_switch_max_delay_ms = 250;
  uint32_t slow_read_threshold_us = 12000;
  bool runtime_action_logs_enabled = false;
};

struct LoopingWavVoiceDurationStats {
  uint32_t count = 0;
  uint64_t total_us = 0;
  uint32_t max_us = 0;

  void add(uint32_t elapsed_us);
  uint32_t avgUs() const;
  void reset();
};

class LoopingWavVoice : public IMixerVoiceSource {
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
    LoopingWavVoiceDurationStats request_next_us;
    uint32_t request_next_last_us = 0;
    uint32_t request_next_age_last_ms = 0;
    uint32_t request_next_age_max_ms = 0;
    size_t request_next_queue_last_samples = 0;
    size_t request_next_queue_max_samples = 0;
    size_t request_next_trimmed_last_samples = 0;
    size_t request_next_trimmed_max_samples = 0;
    LoopingWavVoiceDurationStats close_us;
    uint32_t close_last_us = 0;
    LoopingWavVoiceDurationStats open_us;
    uint32_t open_last_us = 0;
    LoopingWavVoiceDurationStats decoder_begin_us;
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

  LoopingWavVoice(const char* label,
                  fs::FS& fs,
                  const char* source_type_name,
                  LoopingWavVoiceConfig config = {},
                  Print* log = nullptr);

  void setLog(Print* log);
  void setTracks(const std::vector<String>& tracks);
  bool configure(uint32_t output_sample_rate);
  bool hasTracks() const;
  const char* label() const;
  const Stats& stats() const;
  const String& activeTrack() const;
  bool isTrackRunning() const;
  bool hasPendingNextRequest() const;
  bool canApplyPendingNextRequest(size_t queue_samples, uint32_t now_ms) const;
  size_t currentTrackIndex() const;
  size_t playlistSize() const;
  size_t currentFilePosition() const;
  size_t currentFileSize() const;
  uint32_t outputSampleRate() const;
  void stop();
  void requestNextTrack();
  void queueNextTracks(size_t steps, uint32_t request_ms);
  bool servicePendingNextRequest(uint32_t now_ms,
                                 size_t queue_samples_at_switch,
                                 size_t trimmed_queue_samples);
  void copyDebugSnapshot(DebugSnapshot& out) const;

  size_t readSamples(int16_t* dst, size_t sample_count) override;
  bool eof() const override;

 private:
  bool ensureTrackOpen();
  bool openTrack(const String& path);
  void advanceTrack(size_t steps = 1);
  void noteReadStats(size_t produced_samples, uint32_t elapsed_us);
  void closeSourceTimed();
  bool ensurePcmBufferCapacity(size_t capacity_samples);
  void clearPcmBuffer();
  void consumePcmBuffer(size_t consumed_samples);
  void refillPcmBuffer(size_t target_samples);
  size_t decodePcmSamples(int16_t* dst, size_t sample_count);
  void syncDebugSnapshot();
  void logf(const char* format, ...) const;

  const char* label_ = nullptr;
  FsAudioSource source_;
  WavDecoder decoder_;
  LoopingWavVoiceConfig config_;
  Print* log_ = nullptr;
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

}  // namespace padre
