#include "DecoderFacade.h"

namespace padre {
namespace {

bool readExactly(IAudioSource& source, uint8_t* dst, size_t size) {
  size_t total = 0;
  while (total < size) {
    const size_t got = source.read(dst + total, size - total);
    if (got == 0) return false;
    total += got;
  }
  return true;
}

uint16_t le16(const uint8_t* b) {
  return static_cast<uint16_t>(b[0]) |
         static_cast<uint16_t>(b[1] << 8);
}

uint32_t le32(const uint8_t* b) {
  return static_cast<uint32_t>(b[0]) |
         (static_cast<uint32_t>(b[1]) << 8) |
         (static_cast<uint32_t>(b[2]) << 16) |
         (static_cast<uint32_t>(b[3]) << 24);
}

}  // namespace

DecoderFacade::DecoderFacade(DecoderConfig config) : config_(config) {}

void DecoderFacade::setConfig(const DecoderConfig& config) { config_ = config; }

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
  ExternalDecoder* started_decoder = nullptr;

  const auto fail = [&]() {
    if (started_decoder && started_decoder->end) {
      started_decoder->end(started_decoder->ctx);
    }
    source_ = nullptr;
    sink_ = nullptr;
    format_ = AudioFormat::Unknown;
    pending_samples_ = 0;
    running_ = false;
    return false;
  };

  if (format_ == AudioFormat::Unknown) return fail();

  if (format_ == AudioFormat::WAV && !initWav()) return fail();

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

  if (!sink_->begin(config_)) return fail();

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

  for (size_t i = 0; i < max_source_reads && !source_->eof(); ++i) {
    if (sinkWritableSamples() == 0) break;

    if (format_ == AudioFormat::WAV) {
      if (!decodeWavChunk()) break;
      written += 1;
      continue;
    }

    if (format_ == AudioFormat::MP3) {
      const size_t chunk_written = decodeExternalChunk(mp3_decoder_);
      if (chunk_written == 0) break;
      written += chunk_written;
      continue;
    }

    if (format_ == AudioFormat::FLAC) {
      const size_t chunk_written = decodeExternalChunk(flac_decoder_);
      if (chunk_written == 0) break;
      written += chunk_written;
      continue;
    }
  }

  if (source_->eof()) {
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

  source_ = nullptr;
  sink_ = nullptr;
  format_ = AudioFormat::Unknown;
  pending_samples_ = 0;
  running_ = false;
}

bool DecoderFacade::isRunning() const { return running_; }

AudioFormat DecoderFacade::currentFormat() const { return format_; }

bool DecoderFacade::initWav() {
  if (source_ == nullptr) return false;

  uint8_t riff[12] = {0};
  if (!readExactly(*source_, riff, sizeof(riff))) return false;

  if (memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) return false;

  bool fmt_found = false;
  bool data_found = false;

  while (!source_->eof()) {
    uint8_t chunk_header[8] = {0};
    if (!readExactly(*source_, chunk_header, sizeof(chunk_header))) return false;

    const uint32_t chunk_size = le32(chunk_header + 4);

    if (memcmp(chunk_header, "fmt ", 4) == 0) {
      uint8_t fmt[16] = {0};
      if (chunk_size < sizeof(fmt)) return false;
      if (!readExactly(*source_, fmt, sizeof(fmt))) return false;

      const uint16_t audio_format = le16(fmt);
      wav_channels_ = static_cast<uint8_t>(le16(fmt + 2));
      wav_sample_rate_ = le32(fmt + 4);
      wav_bits_per_sample_ = le16(fmt + 14);

      if (audio_format != 1) return false;

      const size_t extra_fmt_bytes =
          chunk_size > sizeof(fmt) ? static_cast<size_t>(chunk_size - sizeof(fmt)) : 0;
      const size_t fmt_padding = (chunk_size & 1u) ? 1u : 0u;
      if ((extra_fmt_bytes > 0 || fmt_padding > 0) &&
          !source_->seek(source_->position() + extra_fmt_bytes + fmt_padding)) {
        return false;
      }

      fmt_found = true;
      continue;
    }

    if (memcmp(chunk_header, "data", 4) == 0) {
      data_found = true;
      break;
    }

    const size_t skip = static_cast<size_t>(chunk_size) + ((chunk_size & 1u) ? 1u : 0u);
    if (!source_->seek(source_->position() + skip)) return false;
  }

  if (!fmt_found || !data_found) return false;

  config_.stereo = wav_channels_ >= 2;
  config_.output_sample_rate = wav_sample_rate_;
  config_.output_bits = wav_bits_per_sample_;

  return true;
}

bool DecoderFacade::decodeWavChunk() {
  if (source_ == nullptr || sink_ == nullptr) return false;
  if (wav_bits_per_sample_ != 16) return false;
  if (pending_samples_ > 0) return false;

  const size_t bytes_to_read = kOutputSamples * sizeof(int16_t);
  const size_t bytes_read = source_->read(input_buffer_, bytes_to_read);
  if (bytes_read == 0) return false;

  const size_t samples = bytes_read / sizeof(int16_t);
  memcpy(output_buffer_, input_buffer_, samples * sizeof(int16_t));

  return writeToSink(output_buffer_, samples) > 0;
}

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
