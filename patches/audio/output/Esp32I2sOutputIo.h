#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#include "BufferedI2sOutput.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <driver/i2s.h>
#endif

namespace padre {

struct Esp32I2sPins {
  int8_t bclk = -1;
  int8_t ws = -1;
  int8_t dout = -1;
  int8_t mclk = -1;
};

struct Esp32I2sDriverConfig {
#if defined(ARDUINO_ARCH_ESP32)
  i2s_port_t port = I2S_NUM_0;
#else
  int port = 0;
#endif
  uint8_t dma_buffer_count = 8;
  uint16_t dma_buffer_samples = 256;
  uint32_t write_timeout_ms = 0;
  bool use_apll = false;
  bool tx_desc_auto_clear = true;
  bool force_stereo_slot = true;
  size_t dma_watermark_samples = 1024;
};

struct Esp32I2sDmaProfile {
  uint8_t dma_buffer_count = 8;
  uint16_t dma_buffer_samples = 256;
  uint32_t write_timeout_ms = 0;
  size_t dma_watermark_samples = 1024;
};

class Esp32I2sOutputIo {
 public:
  Esp32I2sOutputIo(Esp32I2sPins pins, Esp32I2sDriverConfig config = {});

  I2sOutputIo asIo();
  size_t dmaWatermarkSamples() const;
  bool isRunning() const;
  uint8_t dmaBufferCount() const;
  uint16_t dmaBufferSamples() const;
  uint32_t writeTimeoutMs() const;

  bool applyDmaProfile(const Esp32I2sDmaProfile& profile);

 private:
  static bool beginThunk(void* ctx, uint32_t sample_rate, uint8_t bits, bool stereo);
  static size_t availableForWriteThunk(void* ctx);
  static size_t writeSamplesThunk(void* ctx, const int16_t* samples, size_t sample_count);
  static void endThunk(void* ctx);

  bool begin(uint32_t sample_rate, uint8_t bits, bool stereo);
  size_t availableForWrite();
  size_t writeSamples(const int16_t* samples, size_t sample_count);
  void end();

#if defined(ARDUINO_ARCH_ESP32)
  size_t writeRawSamples(const int16_t* samples, size_t sample_count);
  void updateEstimatedDmaFill();
  size_t outputChannels() const;

  int16_t* mono_to_stereo_buf_ = nullptr;
  size_t mono_to_stereo_capacity_ = 0;
  size_t dma_inflight_samples_ = 0;
  uint32_t dma_account_us_ = 0;
#endif

  Esp32I2sPins pins_;
  Esp32I2sDriverConfig config_;
  uint32_t sample_rate_ = 0;
  bool input_stereo_ = true;
  bool output_stereo_ = true;
  bool running_ = false;
};

}  // namespace padre
