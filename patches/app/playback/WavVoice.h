#pragma once

#include <Arduino.h>
#include <FS.h>

#include <vector>

#include "../../audio/decoder/WavDecoder.h"
#include "../../audio/mixer/VoiceMixer.h"
#include "../../media/source/FsAudioSource.h"

namespace padre {

enum class WavVoiceMode : uint8_t {
  Loop,
  OneShot,
};

struct WavVoiceConfig {
  size_t pcm_buffer_samples = 8192;
  size_t pcm_low_water_samples = 2048;
  size_t pcm_refill_chunk_samples = 1024;
  uint8_t pcm_max_refill_attempts = 2;
  WavVoiceMode mode = WavVoiceMode::Loop;
  size_t track_switch_min_queue_samples = 4096;
  uint32_t track_switch_coalesce_ms = 60;
  uint32_t track_switch_max_delay_ms = 250;
};

class WavVoice : public IMixerVoiceSource {
 public:
  WavVoice(const char* label,
           fs::FS& fs,
           const char* source_type_name,
           WavVoiceConfig config = {});

  void setTracks(const std::vector<String>& tracks);
  bool configure(uint32_t output_sample_rate);
  WavVoiceMode mode() const;
  bool hasTracks() const;
  const char* label() const;
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
  bool trigger();
  bool selectTrackIndex(size_t track_index);
  void requestNextTrack();
  void queueNextTracks(size_t steps, uint32_t request_ms);
  void selectNextTracks(size_t steps = 1);
  bool servicePendingNextRequest(uint32_t now_ms,
                                 size_t queue_samples_at_switch,
                                 size_t trimmed_queue_samples);

  size_t readSamples(int16_t* dst, size_t sample_count) override;
  bool eof() const override;

 private:
  bool ensureTrackOpen();
  bool openTrack(const String& path);
  void advanceTrack(size_t steps = 1);
  bool ensurePcmBufferCapacity(size_t capacity_samples);
  void clearPcmBuffer();
  void consumePcmBuffer(size_t consumed_samples);
  void refillPcmBuffer(size_t target_samples);
  size_t decodePcmSamples(int16_t* dst, size_t sample_count);
  void stopCurrentStream();

  const char* label_ = nullptr;
  FsAudioSource source_;
  WavDecoder decoder_;
  WavVoiceConfig config_;
  std::vector<String> tracks_;
  std::vector<int16_t> pcm_buffer_;
  std::vector<int16_t> mono_decode_buffer_;
  size_t pcm_buffered_samples_ = 0;
  size_t track_index_ = 0;
  uint32_t output_sample_rate_ = 0;
  String active_track_;
  size_t pending_next_requests_ = 0;
  uint32_t pending_next_request_ms_ = 0;
  bool trigger_armed_ = false;
};

}  // namespace padre
