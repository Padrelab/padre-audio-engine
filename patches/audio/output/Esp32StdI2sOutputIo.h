#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#include "BufferedI2sOutput.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <driver/i2s_std.h>
#endif

namespace padre {

struct Esp32StdI2sPins {
  int8_t bclk = -1;
  int8_t ws = -1;
  int8_t dout = -1;
  int8_t mclk = -1;
};

struct Esp32StdI2sOutputConfig {
  uint8_t dma_desc_num = 8;
  uint16_t dma_frame_num = 256;
  int intr_priority = 2;
  uint32_t write_timeout_ms = 0;
  size_t work_samples = 2048;
};

using Pcm32SampleTransformFn = int16_t (*)(void* ctx, int32_t sample);
using Pcm32PrepareTransformFn =
    void (*)(void* ctx, const int32_t* input, int16_t* output, size_t sample_count);
using Pcm16CommitTransformFn = void (*)(void* ctx, size_t written_samples);

struct Esp32StdI2sSampleTransform {
  void* ctx = nullptr;
  Pcm32SampleTransformFn apply = nullptr;
  Pcm32PrepareTransformFn prepare = nullptr;
  Pcm16CommitTransformFn commit = nullptr;
};

class Esp32StdI2sOutputIo {
 public:
  Esp32StdI2sOutputIo(Esp32StdI2sPins pins,
                      Esp32StdI2sOutputConfig config = {},
                      Esp32StdI2sSampleTransform transform = {});
  ~Esp32StdI2sOutputIo();

  I2sOutputIo asIo();
  void setPrebuffering(bool enabled);
  bool prebuffering() const;
  bool isRunning() const;

 private:
  static bool beginThunk(void* ctx, uint32_t sample_rate, uint8_t bits, bool stereo);
  static size_t availableForWriteThunk(void* ctx);
  static size_t writeSamplesThunk(void* ctx, const int32_t* samples, size_t sample_count);
  static void endThunk(void* ctx);

  bool begin(uint32_t sample_rate, uint8_t bits, bool stereo);
  size_t availableForWrite();
  size_t writeSamples(const int32_t* samples, size_t sample_count);
  void end();

  void transformSamples(const int32_t* input, int16_t* output, size_t sample_count) const;
  void commitTransformedSamples(size_t written_samples) const;
  int16_t transformSample(int32_t sample) const;
  bool ensureWorkBuffers();
  void releaseWorkBuffers();

  Esp32StdI2sPins pins_;
  Esp32StdI2sOutputConfig config_;
  Esp32StdI2sSampleTransform transform_;

#if defined(ARDUINO_ARCH_ESP32)
  i2s_chan_handle_t tx_ = nullptr;
#endif

  bool stereo_input_ = true;
  bool prebuffering_ = false;
  bool running_ = false;
  int16_t* work_stereo_ = nullptr;
  int16_t* work_mono_to_stereo_ = nullptr;
};

}  // namespace padre
