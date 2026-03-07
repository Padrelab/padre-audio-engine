#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#include "../../media/source/IAudioSource.h"

namespace padre {

enum class WavCodecClass {
  Unsupported = 0,
  PcmInt,
  Float,
  ALaw,
  MuLaw,
};

struct WavStreamInfo {
  bool valid = false;
  uint16_t format_code = 0;           // Raw WAV format tag.
  uint16_t codec_tag = 0;             // Effective codec (incl. EXTENSIBLE subformat).
  uint16_t channels = 0;
  uint16_t output_channels = 0;       // Decoder output channels: mono/stereo.
  uint32_t sample_rate = 0;
  uint16_t bits_per_sample = 0;
  uint16_t valid_bits_per_sample = 0; // For WAVE_EXTENSIBLE.
  uint16_t block_align = 0;
  uint32_t data_size = 0;
};

class WavDecoder {
 public:
  WavDecoder() = default;

  bool begin(IAudioSource& source);
  size_t decode(int16_t* out_samples, size_t out_capacity_samples);
  void stop();

  bool isRunning() const;
  const WavStreamInfo& streamInfo() const;

 private:
  static constexpr size_t kInputBufferSize = 4096;
  static constexpr size_t kCarryBufferSize = 512;
  static constexpr size_t kScratchSkipSize = 128;
  static constexpr size_t kMaxReadChunkSize = 1024;

  static uint16_t readLe16(const uint8_t* b);
  static uint32_t readLe32(const uint8_t* b);
  static WavCodecClass codecClass(const WavStreamInfo& info);
  static bool isSupported(const WavStreamInfo& info, uint8_t bytes_per_sample);
  static int16_t decodeALaw(uint8_t value);
  static int16_t decodeMuLaw(uint8_t value);
  static int32_t readSignedInt(const uint8_t* src, uint8_t bytes_per_sample);
  static int16_t clampToPcm16(int32_t value);

  bool parseHeader();
  bool readExactly(uint8_t* dst, size_t bytes);
  bool skipBytes(size_t bytes);
  int16_t decodeSample(const uint8_t* src) const;

  IAudioSource* source_ = nullptr;
  WavStreamInfo info_;
  bool running_ = false;
  size_t data_remaining_ = 0;
  size_t carry_bytes_ = 0;
  uint8_t bytes_per_sample_ = 0;

  uint8_t input_buffer_[kInputBufferSize + kCarryBufferSize] = {0};
  uint8_t skip_scratch_[kScratchSkipSize] = {0};
};

}  // namespace padre
