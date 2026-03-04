#pragma once

#include <Arduino.h>

namespace padre {

enum class AudioFormat {
  WAV,
  MP3,
  FLAC,
  Unknown,
};

inline AudioFormat detectAudioFormat(String path) {
  path.toLowerCase();
  if (path.endsWith(".wav")) return AudioFormat::WAV;
  if (path.endsWith(".mp3")) return AudioFormat::MP3;
  if (path.endsWith(".flac")) return AudioFormat::FLAC;
  return AudioFormat::Unknown;
}

struct DecoderConfig {
  uint32_t output_sample_rate = 48000;
  uint8_t output_bits = 24;
  bool stereo = true;
};

}  // namespace padre
