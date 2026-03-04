#include "DecoderFacade.h"

namespace padre {

DecoderFacade::DecoderFacade(DecoderConfig config)
    : config_(config), active_config_(config) {}

void DecoderFacade::setConfig(const DecoderConfig& config) {
  config_ = config;
  if (!running_) active_config_ = config;
}

const DecoderConfig& DecoderFacade::config() const { return config_; }

void DecoderFacade::attachMp3Decoder(ExternalDecoder decoder) {
  mp3_decoder_ = decoder;
}

void DecoderFacade::attachFlacDecoder(ExternalDecoder decoder) {
  flac_decoder_ = decoder;
}

bool DecoderFacade::begin(IAudioSource& source, IAudioSink& sink, const String& uri) {
  stop();

  source_ = &source;
  sink_ = &sink;
  format_ = detectAudioFormat(uri);
  active_config_ = config_;
  ExternalDecoder* started_decoder = nullptr;

  const auto fail = [&]() {
    if (started_decoder && started_decoder->end) {
      started_decoder->end(started_decoder->ctx);
    }
    wav_decoder_.stop();
    source_ = nullptr;
    sink_ = nullptr;
    format_ = AudioFormat::Unknown;
    active_config_ = config_;
    pending_samples_ = 0;
    running_ = false;
    return false;
  };

  if (format_ == AudioFormat::Unknown) return fail();

  if (format_ == AudioFormat::WAV) {
    if (!wav_decoder_.begin(*source_)) return fail();

    const WavStreamInfo& wav_info = wav_decoder_.streamInfo();
    active_config_.output_sample_rate = wav_info.sample_rate;
    active_config_.stereo = wav_info.output_channels >= 2;
    active_config_.output_bits = 16;  // DecoderFacade sink contract is int16 PCM.
  }

  if (format_ == AudioFormat::MP3) {
    if (!mp3_decoder_.decode) return fail();
    if (mp3_decoder_.begin && !mp3_decoder_.begin(mp3_decoder_.ctx)) return fail();
    started_decoder = &mp3_decoder_;
  }

  if (format_ == AudioFormat::FLAC) {
    if (!flac_decoder_.decode) return fail();
    if (flac_decoder_.begin && !flac_decoder_.begin(flac_decoder_.ctx)) return fail();
    started_decoder = &flac_decoder_;
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

    if (format_ == AudioFormat::WAV) {
      const size_t produced = wav_decoder_.decode(output_buffer_, kOutputSamples);
      if (produced == 0) {
        if (!wav_decoder_.isRunning()) {
          stop();
        }
        break;
      }

      written += writeToSink(output_buffer_, produced);
      if (pending_samples_ > 0) break;
      continue;
    }

    if (format_ == AudioFormat::MP3) {
      if (source_->eof()) break;
      const size_t chunk_written = decodeExternalChunk(mp3_decoder_);
      if (chunk_written == 0) break;
      written += chunk_written;
      continue;
    }

    if (format_ == AudioFormat::FLAC) {
      if (source_->eof()) break;
      const size_t chunk_written = decodeExternalChunk(flac_decoder_);
      if (chunk_written == 0) break;
      written += chunk_written;
      continue;
    }
  }

  if (running_ && format_ != AudioFormat::WAV && source_->eof()) {
    stop();
  }

  return written;
}

void DecoderFacade::stop() {
  if (running_) {
    if (sink_) sink_->end();
    if (format_ == AudioFormat::MP3 && mp3_decoder_.end) mp3_decoder_.end(mp3_decoder_.ctx);
    if (format_ == AudioFormat::FLAC && flac_decoder_.end) flac_decoder_.end(flac_decoder_.ctx);
  }

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

size_t DecoderFacade::decodeExternalChunk(ExternalDecoder decoder) {
  if (source_ == nullptr || sink_ == nullptr || !decoder.decode) return 0;
  if (pending_samples_ > 0) return 0;

  const size_t bytes_read = source_->read(input_buffer_, sizeof(input_buffer_));
  if (bytes_read == 0) return 0;

  bool frame_done = false;
  const size_t produced = decoder.decode(decoder.ctx,
                                         input_buffer_,
                                         bytes_read,
                                         output_buffer_,
                                         kOutputSamples,
                                         &frame_done);
  if (produced == 0) return 0;

  (void)frame_done;
  return writeToSink(output_buffer_, produced);
}

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
