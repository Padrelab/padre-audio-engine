#include "Esp32StdI2sOutputIo.h"

namespace padre {

namespace {

int16_t clampPcm16Sample(int32_t sample) {
  if (sample > 32767) return 32767;
  if (sample < -32768) return -32768;
  return static_cast<int16_t>(sample);
}

}  // namespace

Esp32StdI2sOutputIo::Esp32StdI2sOutputIo(Esp32StdI2sPins pins,
                                         Esp32StdI2sOutputConfig config,
                                         Esp32StdI2sSampleTransform transform)
    : pins_(pins), config_(config), transform_(transform) {}

Esp32StdI2sOutputIo::~Esp32StdI2sOutputIo() { end(); }

I2sOutputIo Esp32StdI2sOutputIo::asIo() {
  I2sOutputIo io;
  io.ctx = this;
  io.begin = &Esp32StdI2sOutputIo::beginThunk;
  io.availableForWrite = &Esp32StdI2sOutputIo::availableForWriteThunk;
  io.writeSamples = &Esp32StdI2sOutputIo::writeSamplesThunk;
  io.end = &Esp32StdI2sOutputIo::endThunk;
  return io;
}

void Esp32StdI2sOutputIo::setPrebuffering(bool enabled) { prebuffering_ = enabled; }

bool Esp32StdI2sOutputIo::prebuffering() const { return prebuffering_; }

bool Esp32StdI2sOutputIo::isRunning() const { return running_; }

bool Esp32StdI2sOutputIo::beginThunk(void* ctx,
                                     uint32_t sample_rate,
                                     uint8_t bits,
                                     bool stereo) {
  auto* self = static_cast<Esp32StdI2sOutputIo*>(ctx);
  return self ? self->begin(sample_rate, bits, stereo) : false;
}

size_t Esp32StdI2sOutputIo::availableForWriteThunk(void* ctx) {
  auto* self = static_cast<Esp32StdI2sOutputIo*>(ctx);
  return self ? self->availableForWrite() : 0;
}

size_t Esp32StdI2sOutputIo::writeSamplesThunk(void* ctx,
                                              const int32_t* samples,
                                              size_t sample_count) {
  auto* self = static_cast<Esp32StdI2sOutputIo*>(ctx);
  return self ? self->writeSamples(samples, sample_count) : 0;
}

void Esp32StdI2sOutputIo::endThunk(void* ctx) {
  auto* self = static_cast<Esp32StdI2sOutputIo*>(ctx);
  if (self) self->end();
}

bool Esp32StdI2sOutputIo::begin(uint32_t sample_rate, uint8_t bits, bool stereo) {
  end();

#if !defined(ARDUINO_ARCH_ESP32)
  (void)sample_rate;
  (void)bits;
  (void)stereo;
  return false;
#else
  if (bits != 16) return false;
  if (pins_.bclk < 0 || pins_.ws < 0 || pins_.dout < 0) return false;
  if (config_.dma_desc_num == 0 || config_.dma_frame_num == 0 || config_.work_samples == 0) {
    return false;
  }

  i2s_chan_config_t chan_cfg = {};
  chan_cfg.id = I2S_NUM_0;
  chan_cfg.role = I2S_ROLE_MASTER;
  chan_cfg.dma_desc_num = config_.dma_desc_num;
  chan_cfg.dma_frame_num = config_.dma_frame_num;
  chan_cfg.auto_clear = true;
  chan_cfg.intr_priority = config_.intr_priority;

  if (i2s_new_channel(&chan_cfg, &tx_, nullptr) != ESP_OK) {
    tx_ = nullptr;
    return false;
  }

  i2s_std_config_t std_cfg = {};
  std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
  std_cfg.slot_cfg =
      I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  std_cfg.gpio_cfg.mclk = pins_.mclk >= 0 ? static_cast<gpio_num_t>(pins_.mclk) : I2S_GPIO_UNUSED;
  std_cfg.gpio_cfg.bclk = static_cast<gpio_num_t>(pins_.bclk);
  std_cfg.gpio_cfg.ws = static_cast<gpio_num_t>(pins_.ws);
  std_cfg.gpio_cfg.dout = static_cast<gpio_num_t>(pins_.dout);
  std_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
  std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
  std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
  std_cfg.gpio_cfg.invert_flags.ws_inv = false;

  if (i2s_channel_init_std_mode(tx_, &std_cfg) != ESP_OK) {
    i2s_del_channel(tx_);
    tx_ = nullptr;
    return false;
  }

  if (!ensureWorkBuffers()) {
    i2s_channel_disable(tx_);
    i2s_del_channel(tx_);
    tx_ = nullptr;
    return false;
  }

  if (i2s_channel_enable(tx_) != ESP_OK) {
    i2s_channel_disable(tx_);
    i2s_del_channel(tx_);
    tx_ = nullptr;
    releaseWorkBuffers();
    return false;
  }

  stereo_input_ = stereo;
  prebuffering_ = false;
  running_ = true;
  return true;
#endif
}

size_t Esp32StdI2sOutputIo::availableForWrite() {
  if (!running_ || prebuffering_) return 0;
  return config_.work_samples;
}

size_t Esp32StdI2sOutputIo::writeSamples(const int32_t* samples, size_t sample_count) {
  if (!running_ || samples == nullptr || sample_count == 0) return 0;
#if !defined(ARDUINO_ARCH_ESP32)
  return 0;
#else
  if (tx_ == nullptr || !ensureWorkBuffers()) return 0;

  size_t consumed_input_samples = 0;

  if (stereo_input_) {
    while (consumed_input_samples < sample_count) {
      size_t chunk_samples = min(config_.work_samples, sample_count - consumed_input_samples);
      if ((chunk_samples & 1u) != 0u && chunk_samples > 1) {
        --chunk_samples;
      }
      if (chunk_samples == 0) break;

      transformSamples(samples + consumed_input_samples, work_stereo_, chunk_samples);

      size_t written_bytes = 0;
      const size_t total_bytes = chunk_samples * sizeof(int16_t);
      const esp_err_t err =
          i2s_channel_write(tx_, work_stereo_, total_bytes, &written_bytes, config_.write_timeout_ms);
      const size_t written_samples = written_bytes / sizeof(int16_t);
      commitTransformedSamples(written_samples);
      if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        return consumed_input_samples + written_samples;
      }

      consumed_input_samples += written_samples;
      if (written_samples < chunk_samples) break;
    }
    return consumed_input_samples;
  }

  while (consumed_input_samples < sample_count) {
    const size_t chunk_input_samples =
        min(config_.work_samples, sample_count - consumed_input_samples);
    transformSamples(samples + consumed_input_samples, work_stereo_, chunk_input_samples);
    for (size_t i = 0; i < chunk_input_samples; ++i) {
      const int16_t s = work_stereo_[i];
      work_mono_to_stereo_[i * 2] = s;
      work_mono_to_stereo_[i * 2 + 1] = s;
    }

    size_t written_bytes = 0;
    const size_t total_bytes = chunk_input_samples * 2 * sizeof(int16_t);
    const esp_err_t err = i2s_channel_write(
        tx_, work_mono_to_stereo_, total_bytes, &written_bytes, config_.write_timeout_ms);
    const size_t written_output_samples = written_bytes / sizeof(int16_t);
    const size_t written_input_samples = written_output_samples / 2;
    commitTransformedSamples(written_input_samples);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
      return consumed_input_samples + written_input_samples;
    }

    consumed_input_samples += written_input_samples;
    if (written_input_samples < chunk_input_samples) break;
  }

  return consumed_input_samples;
#endif
}

void Esp32StdI2sOutputIo::end() {
#if !defined(ARDUINO_ARCH_ESP32)
  running_ = false;
  prebuffering_ = false;
#else
  if (tx_ != nullptr) {
    i2s_channel_disable(tx_);
    i2s_del_channel(tx_);
    tx_ = nullptr;
  }
  releaseWorkBuffers();
  stereo_input_ = true;
  prebuffering_ = false;
  running_ = false;
#endif
}

void Esp32StdI2sOutputIo::transformSamples(const int32_t* input,
                                           int16_t* output,
                                           size_t sample_count) const {
  if (input == nullptr || output == nullptr || sample_count == 0) return;
  if (transform_.prepare != nullptr) {
    transform_.prepare(transform_.ctx, input, output, sample_count);
    return;
  }

  for (size_t i = 0; i < sample_count; ++i) {
    output[i] = transformSample(input[i]);
  }
}

void Esp32StdI2sOutputIo::commitTransformedSamples(size_t written_samples) const {
  if (transform_.commit == nullptr || written_samples == 0) return;
  transform_.commit(transform_.ctx, written_samples);
}

int16_t Esp32StdI2sOutputIo::transformSample(int32_t sample) const {
  if (transform_.apply == nullptr) return clampPcm16Sample(sample);
  return transform_.apply(transform_.ctx, sample);
}

bool Esp32StdI2sOutputIo::ensureWorkBuffers() {
  if (config_.work_samples == 0) return false;
  if (work_stereo_ == nullptr) {
    work_stereo_ = new int16_t[config_.work_samples];
    if (work_stereo_ == nullptr) return false;
  }
  if (work_mono_to_stereo_ == nullptr) {
    work_mono_to_stereo_ = new int16_t[config_.work_samples * 2];
    if (work_mono_to_stereo_ == nullptr) {
      delete[] work_stereo_;
      work_stereo_ = nullptr;
      return false;
    }
  }
  return true;
}

void Esp32StdI2sOutputIo::releaseWorkBuffers() {
  delete[] work_stereo_;
  work_stereo_ = nullptr;
  delete[] work_mono_to_stereo_;
  work_mono_to_stereo_ = nullptr;
}

}  // namespace padre
