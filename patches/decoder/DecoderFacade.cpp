#include "DecoderFacade.h"

#include <string.h>

namespace padre {

DecoderFacade::DecoderFacade(DecoderConfig config)
    : config_(config), active_config_(config) {}

void DecoderFacade::setConfig(const DecoderConfig& config) {
  config_ = config;
  if (!running_) active_config_ = config;
}

const DecoderConfig& DecoderFacade::config() const { return config_; }

bool DecoderFacade::begin(IAudioSource& source, IAudioSink& sink, const String& uri) {
  stop();

  source_ = &source;
  sink_ = &sink;
  format_ = detectAudioFormat(uri);
  active_config_ = config_;

  const auto fail = [&]() {
    mp3_decoder_.stop();
    flac_decoder_.stop();
    wav_decoder_.stop();
    source_ = nullptr;
    sink_ = nullptr;
    format_ = AudioFormat::Unknown;
    active_config_ = config_;
    pending_samples_ = 0;
    running_ = false;
    return false;
  };

  switch (format_) {
    case AudioFormat::WAV: {
      if (!wav_decoder_.begin(*source_)) return fail();

      const WavStreamInfo& wav_info = wav_decoder_.streamInfo();
      active_config_.output_sample_rate = wav_info.sample_rate;
      active_config_.stereo = wav_info.output_channels >= 2;
      active_config_.output_bits = 16;  // DecoderFacade sink contract is int16 PCM.
      break;
    }

    case AudioFormat::MP3: {
      if (!mp3_decoder_.begin(*source_)) return fail();

      const Mp3StreamInfo& mp3_info = mp3_decoder_.streamInfo();
      active_config_.output_sample_rate = mp3_info.sample_rate;
      active_config_.stereo = mp3_info.output_channels >= 2;
      active_config_.output_bits = 16;
      break;
    }

    case AudioFormat::FLAC: {
      if (!flac_decoder_.begin(*source_)) return fail();

      const FlacStreamInfo& flac_info = flac_decoder_.streamInfo();
      active_config_.output_sample_rate = flac_info.sample_rate;
      active_config_.stereo = flac_info.output_channels >= 2;
      active_config_.output_bits = 16;
      break;
    }

    case AudioFormat::Unknown:
    default:
      return fail();
  }

  if (!sink_->begin(active_config_)) return fail();

  pending_samples_ = 0;
  running_ = true;
  return true;
}

size_t DecoderFacade::process(size_t max_source_reads) {
  if (!running_ || source_ == nullptr || sink_ == nullptr) return 0;

  size_t written = 0;
  const size_t pending_flushed = flushPendingOutput();
  written += pending_flushed;

  if (pending_samples_ > 0) return written;

  for (size_t i = 0; i < max_source_reads; ++i) {
    if (sinkWritableSamples() == 0) break;

    size_t produced = 0;
    switch (format_) {
      case AudioFormat::WAV:
        produced = wav_decoder_.decode(output_buffer_, kOutputSamples);
        break;
      case AudioFormat::MP3:
        produced = mp3_decoder_.decode(output_buffer_, kOutputSamples);
        break;
      case AudioFormat::FLAC:
        produced = flac_decoder_.decode(output_buffer_, kOutputSamples);
        break;
      case AudioFormat::Unknown:
      default:
        stop();
        return written;
    }

    if (produced == 0) {
      bool decoder_running = false;
      switch (format_) {
        case AudioFormat::WAV:
          decoder_running = wav_decoder_.isRunning();
          break;
        case AudioFormat::MP3:
          decoder_running = mp3_decoder_.isRunning();
          break;
        case AudioFormat::FLAC:
          decoder_running = flac_decoder_.isRunning();
          break;
        case AudioFormat::Unknown:
        default:
          decoder_running = false;
          break;
      }
      if (!decoder_running) stop();
      break;
    }

    written += writeToSink(output_buffer_, produced);
    if (pending_samples_ > 0) break;
  }

  return written;
}

void DecoderFacade::stop() {
  if (running_) {
    if (sink_) sink_->end();
  }

  mp3_decoder_.stop();
  flac_decoder_.stop();
  wav_decoder_.stop();
  source_ = nullptr;
  sink_ = nullptr;
  format_ = AudioFormat::Unknown;
  active_config_ = config_;
  pending_samples_ = 0;
  running_ = false;
}

bool DecoderFacade::isRunning() const { return running_; }

AudioFormat DecoderFacade::currentFormat() const { return format_; }

size_t DecoderFacade::flushPendingOutput() {
  if (pending_samples_ == 0) return 0;

  const size_t flushed = writeToSink(output_buffer_, pending_samples_);
  return flushed;
}

size_t DecoderFacade::writeToSink(const int16_t* samples, size_t sample_count) {
  if (sink_ == nullptr || samples == nullptr || sample_count == 0) return 0;

  const size_t written = sink_->write(samples, sample_count);
  if (written >= sample_count) {
    pending_samples_ = 0;
    return written;
  }

  const size_t remain = sample_count - written;
  memmove(output_buffer_, samples + written, remain * sizeof(int16_t));
  pending_samples_ = remain;
  return written;
}

size_t DecoderFacade::sinkWritableSamples() const {
  if (sink_ == nullptr) return 0;

  return sink_->writableSamples();
}

}  // namespace padre
