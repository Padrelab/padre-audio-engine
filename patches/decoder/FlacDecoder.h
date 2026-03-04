#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#include "../source/IAudioSource.h"

namespace padre {

struct FlacStreamInfo {
  bool valid = false;
  uint16_t channels = 0;
  uint16_t output_channels = 0;
  uint32_t sample_rate = 0;
  uint8_t bits_per_sample = 0;
  uint64_t total_pcm_frames = 0;
  bool total_frames_known = false;
};

class FlacDecoder {
 public:
  FlacDecoder() = default;

  bool begin(IAudioSource& source);
  size_t decode(int16_t* out_samples, size_t out_capacity_samples);
  void stop();

  bool isRunning() const;
  const FlacStreamInfo& streamInfo() const;

 private:
  static constexpr size_t kDecodeChunkFrames = 256;
  static constexpr uint16_t kMaxInputChannels = 8;

  IAudioSource* source_ = nullptr;
  FlacStreamInfo info_;
  bool running_ = false;
  void* decoder_ = nullptr;

  int16_t decode_buffer_[kDecodeChunkFrames * kMaxInputChannels] = {0};
};

}  // namespace padre
