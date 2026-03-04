#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace padre {

enum class SampleFormat {
  Int16,
  Int24,
  Int32,
  Float32,
};

struct DecodedAudio {
  uint32_t sample_rate = 0;
  uint16_t channels = 0;
  SampleFormat source_format = SampleFormat::Float32;
  std::vector<float> samples;  // Interleaved [-1.0f..1.0f]
};

struct DecodeResult {
  bool ok = false;
  std::string error;

  static DecodeResult success() { return {true, {}}; }
  static DecodeResult fail(std::string msg) { return {false, std::move(msg)}; }
};

}  // namespace padre
