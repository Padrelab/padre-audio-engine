#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

#include "../source/IAudioSource.h"

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
  using DecodeChunkFn = size_t (*)(void* ctx,
                                   const uint8_t* input,
                                   size_t input_size,
                                   int16_t* output,
                                   size_t output_capacity,
                                   bool* frame_done);
  using BeginFn = bool (*)(void* ctx);
  using EndFn = void (*)(void* ctx);

  struct ExternalDecoder {
    void* ctx = nullptr;
    BeginFn begin = nullptr;
    DecodeChunkFn decode = nullptr;
    EndFn end = nullptr;
  };

  explicit DecoderFacade(DecoderConfig config = {});

  void setConfig(const DecoderConfig& config);
  const DecoderConfig& config() const;

  void attachMp3Decoder(ExternalDecoder decoder);
  void attachFlacDecoder(ExternalDecoder decoder);

  bool begin(IAudioSource& source, IAudioSink& sink, const String& uri);
  size_t process(size_t max_source_reads = 4);
  void stop();

  bool isRunning() const;
  AudioFormat currentFormat() const;

 private:
  bool initWav();
  bool decodeWavChunk();
  size_t decodeExternalChunk(ExternalDecoder decoder);
  size_t flushPendingOutput();
  size_t writeToSink(const int16_t* samples, size_t sample_count);
  size_t sinkWritableSamples() const;

  static constexpr size_t kInputBufferSize = 2048;
  static constexpr size_t kOutputSamples = 1024;

  DecoderConfig config_;
  IAudioSource* source_ = nullptr;
  IAudioSink* sink_ = nullptr;
  AudioFormat format_ = AudioFormat::Unknown;
  bool running_ = false;

  ExternalDecoder mp3_decoder_;
  ExternalDecoder flac_decoder_;

  uint8_t input_buffer_[kInputBufferSize] = {0};
  int16_t output_buffer_[kOutputSamples] = {0};
  size_t pending_samples_ = 0;

  uint8_t wav_channels_ = 2;
  uint32_t wav_sample_rate_ = 48000;
  uint16_t wav_bits_per_sample_ = 16;
};

}  // namespace padre
