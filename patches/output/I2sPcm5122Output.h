#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#include "../decoder/DecoderFacade.h"

namespace padre {

struct I2sOutputIo {
  void* ctx = nullptr;
  bool (*begin)(void* ctx,
                uint32_t sample_rate,
                uint8_t bits,
                bool stereo) = nullptr;
  size_t (*availableForWrite)(void* ctx) = nullptr;
  size_t (*writeSamples)(void* ctx, const int16_t* samples, size_t sample_count) = nullptr;
  void (*end)(void* ctx) = nullptr;
};

struct I2sOutputConfig {
  size_t queue_samples = 4096;
};

class I2sPcm5122Output : public IAudioSink {
 public:
  I2sPcm5122Output(I2sOutputIo io, I2sOutputConfig config = {});

  bool begin(const DecoderConfig& config) override;
  size_t write(const int16_t* samples, size_t sample_count) override;
  void end() override;

  size_t writableSamples() const override;
  size_t queuedSamples() const;
  size_t queueCapacity() const;

  // Вызывает вывод из очереди в I2S; полезно вызывать в loop().
  size_t pump();

 private:
  bool pushToQueue(const int16_t* samples, size_t count);
  size_t queueFreeSamples() const;

  I2sOutputIo io_;
  I2sOutputConfig config_;

  DecoderConfig decoder_cfg_;

  int16_t* queue_ = nullptr;
  size_t queue_head_ = 0;
  size_t queue_tail_ = 0;
  size_t queued_samples_ = 0;
  bool running_ = false;
};

}  // namespace padre
