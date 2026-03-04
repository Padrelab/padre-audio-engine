#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#include "../source/IAudioSource.h"

namespace padre {

struct Mp3StreamInfo {
  bool valid = false;
  uint16_t channels = 0;
  uint16_t output_channels = 0;
  uint32_t sample_rate = 0;
  uint8_t bits_per_sample = 16;
  uint64_t total_pcm_frames = 0;
  bool total_frames_known = false;
};

class Mp3Decoder {
 public:
  Mp3Decoder() = default;

  bool begin(IAudioSource& source);
  size_t decode(int16_t* out_samples, size_t out_capacity_samples);
  void stop();

  bool isRunning() const;
  const Mp3StreamInfo& streamInfo() const;

 private:
  static constexpr uint16_t kMaxInputChannels = 2;

  IAudioSource* source_ = nullptr;
  Mp3StreamInfo info_;
  bool running_ = false;
  void* decoder_ = nullptr;
};

}  // namespace padre
