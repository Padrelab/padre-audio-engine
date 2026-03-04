#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

#include "FlacDecoder.h"
#include "Mp3Decoder.h"
#include "../source/IAudioSource.h"
#include "WavDecoder.h"

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
  uint8_t output_bits = 16;
  bool stereo = true;
};

class IAudioSink {
 public:
  virtual ~IAudioSink() = default;
  virtual bool begin(const DecoderConfig& config) = 0;
  virtual size_t write(const int16_t* samples, size_t sample_count) = 0;
  virtual void end() = 0;
  virtual size_t writableSamples() const { return static_cast<size_t>(-1); }
};

class DecoderFacade {
 public:
  explicit DecoderFacade(DecoderConfig config = {});

  void setConfig(const DecoderConfig& config);
  const DecoderConfig& config() const;

  bool begin(IAudioSource& source, IAudioSink& sink, const String& uri);
  size_t process(size_t max_source_reads = 4);
  void stop();

  bool isRunning() const;
  AudioFormat currentFormat() const;

 private:
  size_t flushPendingOutput();
  size_t writeToSink(const int16_t* samples, size_t sample_count);
  size_t sinkWritableSamples() const;

  static constexpr size_t kOutputSamples = 1024;

  DecoderConfig config_;
  IAudioSource* source_ = nullptr;
  IAudioSink* sink_ = nullptr;
  AudioFormat format_ = AudioFormat::Unknown;
  bool running_ = false;

  Mp3Decoder mp3_decoder_;
  FlacDecoder flac_decoder_;
  WavDecoder wav_decoder_;

  DecoderConfig active_config_;
  int16_t output_buffer_[kOutputSamples] = {0};
  size_t pending_samples_ = 0;
};

}  // namespace padre
