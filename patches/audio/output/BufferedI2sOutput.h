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
  size_t (*writeSamples)(void* ctx, const int32_t* samples, size_t sample_count) = nullptr;
  void (*end)(void* ctx) = nullptr;
};

struct I2sOutputConfig {
  size_t queue_samples = 4096;
  // Минимальный размер свободного окна (в samples), чтобы pump() начал отправку в I2S.
  size_t dma_watermark_samples = 0;
};

class BufferedI2sOutput : public IAudioSink {
 public:
  BufferedI2sOutput(I2sOutputIo io, I2sOutputConfig config = {});

  bool begin(const DecoderConfig& config) override;
  size_t write(const int16_t* samples, size_t sample_count) override;
  size_t writePcm32(const int32_t* samples, size_t sample_count);
  void end() override;

  size_t writableSamples() const override;
  size_t queuedSamples() const;
  size_t queueCapacity() const;
  size_t dmaWatermarkSamples() const;
  void setDmaWatermarkSamples(size_t watermark_samples);
  // Неразрушающе подмешивает samples в уже queued PCM, начиная от ближайших
  // к выводу сэмплов. Возвращает число реально смешанных сэмплов.
  size_t mixQueuedSamples(const int32_t* samples,
                          size_t sample_count,
                          size_t offset_samples = 0);
  // Оставляет в очереди не более keep_samples ближайших к выводу сэмплов.
  size_t trimQueuedSamples(size_t keep_samples);

  // Вызывает вывод из очереди в I2S; полезно вызывать в loop().
  size_t pump();
  // Безопасный для ISR запрос обслуживания очереди; реальный pump() вызвать в loop().
  void IRAM_ATTR requestPumpFromIsr();
  bool pumpRequested() const;

 private:
  size_t pumpInternal(bool ignore_watermark);
  bool pushToQueue(const int16_t* samples, size_t count);
  bool pushToQueue(const int32_t* samples, size_t count);
  size_t queueFreeSamples() const;

  I2sOutputIo io_;
  I2sOutputConfig config_;

  DecoderConfig decoder_cfg_;

  int32_t* queue_ = nullptr;
  size_t queue_head_ = 0;
  size_t queue_tail_ = 0;
  size_t queued_samples_ = 0;
  bool running_ = false;
  volatile bool pump_requested_ = false;
};

}  // namespace padre
