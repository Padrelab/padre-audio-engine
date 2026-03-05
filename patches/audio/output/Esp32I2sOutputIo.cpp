#include "Esp32I2sOutputIo.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_intr_alloc.h>
#include <freertos/FreeRTOS.h>
#endif

namespace padre {

Esp32I2sOutputIo::Esp32I2sOutputIo(Esp32I2sPins pins, Esp32I2sDriverConfig config)
    : pins_(pins), config_(config) {}

I2sOutputIo Esp32I2sOutputIo::asIo() {
  I2sOutputIo io;
  io.ctx = this;
  io.begin = &Esp32I2sOutputIo::beginThunk;
  io.availableForWrite = &Esp32I2sOutputIo::availableForWriteThunk;
  io.writeSamples = &Esp32I2sOutputIo::writeSamplesThunk;
  io.end = &Esp32I2sOutputIo::endThunk;
  return io;
}

size_t Esp32I2sOutputIo::dmaWatermarkSamples() const {
  return config_.dma_watermark_samples;
}

bool Esp32I2sOutputIo::isRunning() const { return running_; }

uint8_t Esp32I2sOutputIo::dmaBufferCount() const { return config_.dma_buffer_count; }

uint16_t Esp32I2sOutputIo::dmaBufferSamples() const {
  return config_.dma_buffer_samples;
}

uint32_t Esp32I2sOutputIo::writeTimeoutMs() const { return config_.write_timeout_ms; }

bool Esp32I2sOutputIo::applyDmaProfile(const Esp32I2sDmaProfile& profile) {
  if (profile.dma_buffer_count == 0 || profile.dma_buffer_samples == 0) return false;

  const bool was_running = running_;
  const uint32_t resume_rate = sample_rate_;
  const bool resume_stereo = input_stereo_;

  config_.dma_buffer_count = profile.dma_buffer_count;
  config_.dma_buffer_samples = profile.dma_buffer_samples;
  config_.write_timeout_ms = profile.write_timeout_ms;
  config_.dma_watermark_samples = profile.dma_watermark_samples;

#if !defined(ARDUINO_ARCH_ESP32)
  return true;
#else
  if (!was_running) return true;
  return begin(resume_rate, 16, resume_stereo);
#endif
}

bool Esp32I2sOutputIo::beginThunk(void* ctx, uint32_t sample_rate, uint8_t bits, bool stereo) {
  auto* self = static_cast<Esp32I2sOutputIo*>(ctx);
  return self ? self->begin(sample_rate, bits, stereo) : false;
}

size_t Esp32I2sOutputIo::availableForWriteThunk(void* ctx) {
  auto* self = static_cast<Esp32I2sOutputIo*>(ctx);
  return self ? self->availableForWrite() : 0;
}

size_t Esp32I2sOutputIo::writeSamplesThunk(void* ctx,
                                           const int16_t* samples,
                                           size_t sample_count) {
  auto* self = static_cast<Esp32I2sOutputIo*>(ctx);
  return self ? self->writeSamples(samples, sample_count) : 0;
}

void Esp32I2sOutputIo::endThunk(void* ctx) {
  auto* self = static_cast<Esp32I2sOutputIo*>(ctx);
  if (self) self->end();
}

bool Esp32I2sOutputIo::begin(uint32_t sample_rate, uint8_t bits, bool stereo) {
  end();

#if !defined(ARDUINO_ARCH_ESP32)
  (void)sample_rate;
  (void)bits;
  (void)stereo;
  return false;
#else
  if (bits != 16) return false;
  if (pins_.bclk < 0 || pins_.ws < 0 || pins_.dout < 0) return false;
  if (config_.dma_buffer_count == 0 || config_.dma_buffer_samples == 0) return false;

  input_stereo_ = stereo;
  output_stereo_ = config_.force_stereo_slot || stereo;

  i2s_config_t i2s_cfg = {};
  i2s_cfg.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
  i2s_cfg.sample_rate = sample_rate;
  i2s_cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  i2s_cfg.channel_format =
      output_stereo_ ? I2S_CHANNEL_FMT_RIGHT_LEFT : I2S_CHANNEL_FMT_ONLY_LEFT;
  i2s_cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  i2s_cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  i2s_cfg.dma_buf_count = config_.dma_buffer_count;
  i2s_cfg.dma_buf_len = config_.dma_buffer_samples;
  i2s_cfg.use_apll = config_.use_apll;
  i2s_cfg.tx_desc_auto_clear = config_.tx_desc_auto_clear;
  i2s_cfg.fixed_mclk = 0;

  if (i2s_driver_install(static_cast<i2s_port_t>(config_.port), &i2s_cfg, 0, nullptr) != ESP_OK) {
    return false;
  }

  i2s_pin_config_t pin_cfg = {};
  pin_cfg.mck_io_num = pins_.mclk >= 0 ? pins_.mclk : I2S_PIN_NO_CHANGE;
  pin_cfg.bck_io_num = pins_.bclk;
  pin_cfg.ws_io_num = pins_.ws;
  pin_cfg.data_out_num = pins_.dout;
  pin_cfg.data_in_num = I2S_PIN_NO_CHANGE;

  if (i2s_set_pin(static_cast<i2s_port_t>(config_.port), &pin_cfg) != ESP_OK) {
    i2s_driver_uninstall(static_cast<i2s_port_t>(config_.port));
    return false;
  }

  if (i2s_set_clk(static_cast<i2s_port_t>(config_.port),
                  sample_rate,
                  I2S_BITS_PER_SAMPLE_16BIT,
                  output_stereo_ ? I2S_CHANNEL_STEREO : I2S_CHANNEL_MONO) != ESP_OK) {
    i2s_driver_uninstall(static_cast<i2s_port_t>(config_.port));
    return false;
  }

  if (!input_stereo_ && output_stereo_) {
    mono_to_stereo_capacity_ = max(static_cast<size_t>(config_.dma_buffer_samples),
                                   static_cast<size_t>(64));
    mono_to_stereo_buf_ = new int16_t[mono_to_stereo_capacity_ * 2];
    if (mono_to_stereo_buf_ == nullptr) {
      i2s_driver_uninstall(static_cast<i2s_port_t>(config_.port));
      mono_to_stereo_capacity_ = 0;
      return false;
    }
  }

  i2s_zero_dma_buffer(static_cast<i2s_port_t>(config_.port));

  sample_rate_ = sample_rate;
  dma_inflight_samples_ = 0;
  dma_account_us_ = micros();
  running_ = true;
  return true;
#endif
}

size_t Esp32I2sOutputIo::availableForWrite() {
#if !defined(ARDUINO_ARCH_ESP32)
  return 0;
#else
  if (!running_) return 0;

  updateEstimatedDmaFill();

  const size_t capacity = outputChannels() *
                          static_cast<size_t>(config_.dma_buffer_count) *
                          static_cast<size_t>(config_.dma_buffer_samples);
  if (capacity <= dma_inflight_samples_) return 0;

  const size_t free_samples = capacity - dma_inflight_samples_;
  if (config_.dma_watermark_samples > 0 &&
      free_samples < config_.dma_watermark_samples) {
    return 0;
  }
  return free_samples;
#endif
}

size_t Esp32I2sOutputIo::writeSamples(const int16_t* samples, size_t sample_count) {
#if !defined(ARDUINO_ARCH_ESP32)
  (void)samples;
  (void)sample_count;
  return 0;
#else
  if (!running_ || samples == nullptr || sample_count == 0) return 0;

  if (input_stereo_ || !output_stereo_) {
    return writeRawSamples(samples, sample_count);
  }

  if (mono_to_stereo_buf_ == nullptr || mono_to_stereo_capacity_ == 0) return 0;

  size_t consumed_input_samples = 0;
  while (consumed_input_samples < sample_count) {
    const size_t chunk_input_samples =
        min(mono_to_stereo_capacity_, sample_count - consumed_input_samples);

    for (size_t i = 0; i < chunk_input_samples; ++i) {
      const int16_t s = samples[consumed_input_samples + i];
      mono_to_stereo_buf_[i * 2] = s;
      mono_to_stereo_buf_[i * 2 + 1] = s;
    }

    const size_t written_output_samples =
        writeRawSamples(mono_to_stereo_buf_, chunk_input_samples * 2);
    const size_t written_input_samples = written_output_samples / 2;
    consumed_input_samples += written_input_samples;
    if (written_input_samples < chunk_input_samples) break;
  }

  return consumed_input_samples;
#endif
}

void Esp32I2sOutputIo::end() {
#if !defined(ARDUINO_ARCH_ESP32)
  running_ = false;
#else
  if (!running_) return;

  i2s_zero_dma_buffer(static_cast<i2s_port_t>(config_.port));
  i2s_stop(static_cast<i2s_port_t>(config_.port));
  i2s_driver_uninstall(static_cast<i2s_port_t>(config_.port));

  delete[] mono_to_stereo_buf_;
  mono_to_stereo_buf_ = nullptr;
  mono_to_stereo_capacity_ = 0;
  dma_inflight_samples_ = 0;
  dma_account_us_ = 0;
  sample_rate_ = 0;
  input_stereo_ = true;
  output_stereo_ = true;
  running_ = false;
#endif
}

#if defined(ARDUINO_ARCH_ESP32)
size_t Esp32I2sOutputIo::writeRawSamples(const int16_t* samples, size_t sample_count) {
  if (!running_ || samples == nullptr || sample_count == 0) return 0;

  const TickType_t timeout_ticks =
      config_.write_timeout_ms == 0 ? 0 : pdMS_TO_TICKS(config_.write_timeout_ms);
  const size_t capacity = outputChannels() *
                          static_cast<size_t>(config_.dma_buffer_count) *
                          static_cast<size_t>(config_.dma_buffer_samples);

  size_t total_written = 0;
  while (total_written < sample_count) {
    size_t written_bytes = 0;
    const size_t to_write_samples = sample_count - total_written;
    const size_t to_write_bytes = to_write_samples * sizeof(int16_t);

    const esp_err_t err = i2s_write(static_cast<i2s_port_t>(config_.port),
                                    samples + total_written,
                                    to_write_bytes,
                                    &written_bytes,
                                    timeout_ticks);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) break;

    const size_t written_samples = written_bytes / sizeof(int16_t);
    if (written_samples == 0) break;

    total_written += written_samples;
    updateEstimatedDmaFill();
    dma_inflight_samples_ += written_samples;
    if (dma_inflight_samples_ > capacity) dma_inflight_samples_ = capacity;

    if (written_samples < to_write_samples) break;
    if (timeout_ticks == 0) break;
  }

  return total_written;
}

void Esp32I2sOutputIo::updateEstimatedDmaFill() {
  if (!running_ || sample_rate_ == 0) return;

  const uint32_t now_us = micros();
  const uint32_t elapsed_us = now_us - dma_account_us_;
  dma_account_us_ = now_us;
  if (elapsed_us == 0) return;

  const uint64_t consumed_samples =
      (static_cast<uint64_t>(elapsed_us) * static_cast<uint64_t>(sample_rate_) *
       static_cast<uint64_t>(outputChannels())) /
      1000000ULL;
  if (consumed_samples >= dma_inflight_samples_) {
    dma_inflight_samples_ = 0;
    return;
  }
  dma_inflight_samples_ -= static_cast<size_t>(consumed_samples);
}

size_t Esp32I2sOutputIo::outputChannels() const { return output_stereo_ ? 2u : 1u; }
#endif

}  // namespace padre
