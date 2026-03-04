#include "I2sPcm5122Output.h"

namespace padre {

I2sPcm5122Output::I2sPcm5122Output(I2sOutputIo io, I2sOutputConfig config)
    : io_(io), config_(config) {}

bool I2sPcm5122Output::begin(const DecoderConfig& config) {
  end();

  if (io_.begin == nullptr || io_.writeSamples == nullptr) return false;

  if (config_.queue_samples == 0) return false;

  decoder_cfg_ = config;
  if (!io_.begin(io_.ctx,
                 decoder_cfg_.output_sample_rate,
                 decoder_cfg_.output_bits,
                 decoder_cfg_.stereo)) {
    return false;
  }

  queue_ = new int16_t[config_.queue_samples];
  if (queue_ == nullptr) {
    if (io_.end) io_.end(io_.ctx);
    return false;
  }

  queue_head_ = 0;
  queue_tail_ = 0;
  queued_samples_ = 0;
  running_ = true;
  return true;
}

size_t I2sPcm5122Output::write(const int16_t* samples, size_t sample_count) {
  if (!running_ || samples == nullptr || sample_count == 0) return 0;

  pump();

  const size_t accepted = min(sample_count, queueFreeSamples());
  if (accepted == 0) return 0;

  if (!pushToQueue(samples, accepted)) return 0;

  pump();
  return accepted;
}

void I2sPcm5122Output::end() {
  if (!running_) return;

  while (queued_samples_ > 0) {
    if (pump() == 0) break;
  }

  if (io_.end) io_.end(io_.ctx);

  delete[] queue_;
  queue_ = nullptr;
  queue_head_ = 0;
  queue_tail_ = 0;
  queued_samples_ = 0;
  running_ = false;
}

size_t I2sPcm5122Output::writableSamples() const {
  if (!running_) return 0;
  return queueFreeSamples();
}

size_t I2sPcm5122Output::queuedSamples() const { return queued_samples_; }

size_t I2sPcm5122Output::queueCapacity() const { return config_.queue_samples; }

size_t I2sPcm5122Output::pump() {
  if (!running_ || queued_samples_ == 0 || io_.writeSamples == nullptr) return 0;

  size_t available = queued_samples_;
  if (io_.availableForWrite) {
    available = min(available, io_.availableForWrite(io_.ctx));
  }
  if (available == 0) return 0;

  const size_t first_chunk = min(available, config_.queue_samples - queue_tail_);
  size_t written = io_.writeSamples(io_.ctx, queue_ + queue_tail_, first_chunk);

  queue_tail_ = (queue_tail_ + written) % config_.queue_samples;
  queued_samples_ -= written;

  if (written < first_chunk) return written;

  const size_t remain = available - written;
  if (remain == 0) return written;

  const size_t second_written = io_.writeSamples(io_.ctx, queue_ + queue_tail_, remain);
  queue_tail_ = (queue_tail_ + second_written) % config_.queue_samples;
  queued_samples_ -= second_written;

  return written + second_written;
}

bool I2sPcm5122Output::pushToQueue(const int16_t* samples, size_t count) {
  if (count == 0 || count > queueFreeSamples()) return false;

  const size_t first_chunk = min(count, config_.queue_samples - queue_head_);
  memcpy(queue_ + queue_head_, samples, first_chunk * sizeof(int16_t));
  queue_head_ = (queue_head_ + first_chunk) % config_.queue_samples;
  queued_samples_ += first_chunk;

  const size_t remain = count - first_chunk;
  if (remain == 0) return true;

  memcpy(queue_ + queue_head_, samples + first_chunk, remain * sizeof(int16_t));
  queue_head_ = (queue_head_ + remain) % config_.queue_samples;
  queued_samples_ += remain;
  return true;
}

size_t I2sPcm5122Output::queueFreeSamples() const {
  return config_.queue_samples - queued_samples_;
}

}  // namespace padre
