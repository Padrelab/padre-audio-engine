#include "WavDecoder.h"

#include <math.h>
#include <string.h>

namespace padre {

uint16_t WavDecoder::readLe16(const uint8_t* b) {
  return static_cast<uint16_t>(b[0]) |
         (static_cast<uint16_t>(b[1]) << 8);
}

uint32_t WavDecoder::readLe32(const uint8_t* b) {
  return static_cast<uint32_t>(b[0]) |
         (static_cast<uint32_t>(b[1]) << 8) |
         (static_cast<uint32_t>(b[2]) << 16) |
         (static_cast<uint32_t>(b[3]) << 24);
}

WavCodecClass WavDecoder::codecClass(const WavStreamInfo& info) {
  if (info.codec_tag == 1) return WavCodecClass::PcmInt;
  if (info.codec_tag == 3) return WavCodecClass::Float;
  if (info.codec_tag == 6) return WavCodecClass::ALaw;
  if (info.codec_tag == 7) return WavCodecClass::MuLaw;
  return WavCodecClass::Unsupported;
}

bool WavDecoder::isSupported(const WavStreamInfo& info, uint8_t bytes_per_sample) {
  if (!info.valid) return false;
  if (info.channels == 0) return false;
  if (info.sample_rate == 0) return false;
  if (info.block_align == 0) return false;
  if (bytes_per_sample == 0) return false;

  const auto cls = codecClass(info);
  if (cls == WavCodecClass::PcmInt) {
    if (!(info.bits_per_sample == 8 || info.bits_per_sample == 16 ||
          info.bits_per_sample == 24 || info.bits_per_sample == 32)) {
      return false;
    }
    if (info.bits_per_sample == 16 && bytes_per_sample < 2) return false;
    if (info.bits_per_sample == 24 && bytes_per_sample < 3) return false;
    if (info.bits_per_sample == 32 && bytes_per_sample < 4) return false;
    return true;
  }

  if (cls == WavCodecClass::Float) {
    if (info.bits_per_sample == 32) return bytes_per_sample >= 4;
    if (info.bits_per_sample == 64) return bytes_per_sample >= 8;
    return false;
  }

  if (cls == WavCodecClass::ALaw || cls == WavCodecClass::MuLaw) {
    return info.bits_per_sample == 8 && bytes_per_sample >= 1;
  }

  return false;
}

int16_t WavDecoder::decodeALaw(uint8_t value) {
  value ^= 0x55;

  int16_t t = static_cast<int16_t>((value & 0x0F) << 4);
  const uint8_t seg = static_cast<uint8_t>((value & 0x70) >> 4);
  switch (seg) {
    case 0:
      t += 8;
      break;
    case 1:
      t += 0x108;
      break;
    default:
      t += 0x108;
      t <<= (seg - 1);
      break;
  }
  return (value & 0x80) ? t : static_cast<int16_t>(-t);
}

int16_t WavDecoder::decodeMuLaw(uint8_t value) {
  value = static_cast<uint8_t>(~value);
  const int sign = (value & 0x80) ? -1 : 1;
  const int exponent = (value >> 4) & 0x07;
  const int mantissa = value & 0x0F;
  int sample = ((mantissa << 3) + 0x84) << exponent;
  sample -= 0x84;
  sample *= sign;
  if (sample > 32767) sample = 32767;
  if (sample < -32768) sample = -32768;
  return static_cast<int16_t>(sample);
}

int32_t WavDecoder::readSignedInt(const uint8_t* src, uint8_t bytes_per_sample) {
  if (bytes_per_sample == 1) {
    return static_cast<int32_t>(static_cast<int8_t>(src[0]));
  }

  if (bytes_per_sample == 2) {
    return static_cast<int32_t>(static_cast<int16_t>(readLe16(src)));
  }

  if (bytes_per_sample == 3) {
    int32_t v = static_cast<int32_t>(src[0]) |
                (static_cast<int32_t>(src[1]) << 8) |
                (static_cast<int32_t>(src[2]) << 16);
    if (v & 0x800000) v |= ~0xFFFFFF;
    return v;
  }

  int32_t v = static_cast<int32_t>(src[0]) |
              (static_cast<int32_t>(src[1]) << 8) |
              (static_cast<int32_t>(src[2]) << 16) |
              (static_cast<int32_t>(src[3]) << 24);
  return v;
}

int16_t WavDecoder::clampToPcm16(int32_t value) {
  if (value > 32767) return 32767;
  if (value < -32768) return -32768;
  return static_cast<int16_t>(value);
}

bool WavDecoder::begin(IAudioSource& source) {
  stop();

  source_ = &source;
  if (!parseHeader()) {
    stop();
    return false;
  }

  bytes_per_sample_ =
      static_cast<uint8_t>(max<uint16_t>(1, info_.block_align / info_.channels));

  if (!isSupported(info_, bytes_per_sample_)) {
    stop();
    return false;
  }

  if (info_.block_align > kCarryBufferSize) {
    stop();
    return false;
  }

  running_ = true;
  return true;
}

size_t WavDecoder::decode(int16_t* out_samples, size_t out_capacity_samples) {
  if (!running_ || source_ == nullptr || out_samples == nullptr || out_capacity_samples == 0) {
    return 0;
  }

  const size_t out_channels = info_.output_channels;
  if (out_channels == 0) {
    stop();
    return 0;
  }

  const size_t max_frames = out_capacity_samples / out_channels;
  if (max_frames == 0) return 0;

  const size_t frame_bytes = info_.block_align;
  size_t produced_samples = 0;
  size_t consumed_from_buffer = 0;
  const uint8_t right_channel = info_.channels >= 2 ? 1 : 0;

  while ((produced_samples / out_channels) < max_frames) {
    const size_t available = carry_bytes_ - consumed_from_buffer;
    if (available < frame_bytes) {
      if (consumed_from_buffer > 0 && available > 0) {
        memmove(input_buffer_, input_buffer_ + consumed_from_buffer, available);
      }
      carry_bytes_ = available;
      consumed_from_buffer = 0;

      if (data_remaining_ == 0) {
        break;
      }

      const size_t total_buf_size = sizeof(input_buffer_);
      const size_t room = total_buf_size - carry_bytes_;
      if (room == 0) break;

      const size_t frames_remaining = max_frames - (produced_samples / out_channels);
      const size_t needed_bytes = frames_remaining * frame_bytes;
      const size_t read_budget =
          max(frame_bytes, min(room, min(kMaxReadChunkSize, needed_bytes)));
      const size_t want = min(data_remaining_, read_budget);
      const size_t got = source_->read(input_buffer_ + carry_bytes_, want);
      if (got == 0) {
        break;
      }

      carry_bytes_ += got;
      data_remaining_ -= got;
      continue;
    }

    const uint8_t* frame = input_buffer_ + consumed_from_buffer;
    const int16_t left = decodeSample(frame);
    out_samples[produced_samples++] = left;

    if (out_channels == 2) {
      const int16_t right = decodeSample(frame + (right_channel * bytes_per_sample_));
      out_samples[produced_samples++] = right;
    }

    consumed_from_buffer += frame_bytes;
  }

  if (consumed_from_buffer > 0) {
    const size_t remain = carry_bytes_ - consumed_from_buffer;
    if (remain > 0) {
      memmove(input_buffer_, input_buffer_ + consumed_from_buffer, remain);
    }
    carry_bytes_ = remain;
  }

  if (data_remaining_ == 0 && carry_bytes_ < info_.block_align) {
    running_ = false;
  }

  if (produced_samples == 0 && !running_) {
    return 0;
  }

  return produced_samples;
}

void WavDecoder::stop() {
  source_ = nullptr;
  info_ = {};
  running_ = false;
  data_remaining_ = 0;
  carry_bytes_ = 0;
  bytes_per_sample_ = 0;
}

bool WavDecoder::isRunning() const { return running_; }

const WavStreamInfo& WavDecoder::streamInfo() const { return info_; }

bool WavDecoder::parseHeader() {
  if (source_ == nullptr) return false;

  uint8_t riff[12] = {0};
  if (!readExactly(riff, sizeof(riff))) return false;
  if (memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) return false;

  bool fmt_found = false;
  bool data_found = false;

  while (!source_->eof()) {
    uint8_t chunk[8] = {0};
    if (!readExactly(chunk, sizeof(chunk))) return false;

    const uint32_t chunk_size = readLe32(chunk + 4);

    if (memcmp(chunk, "fmt ", 4) == 0) {
      if (chunk_size < 16) return false;

      uint8_t fmt[64] = {0};
      const size_t fmt_to_read = min<size_t>(chunk_size, sizeof(fmt));
      if (!readExactly(fmt, fmt_to_read)) return false;

      info_.format_code = readLe16(fmt + 0);
      info_.codec_tag = info_.format_code;
      info_.channels = readLe16(fmt + 2);
      info_.sample_rate = readLe32(fmt + 4);
      info_.block_align = readLe16(fmt + 12);
      info_.bits_per_sample = readLe16(fmt + 14);
      info_.valid_bits_per_sample = info_.bits_per_sample;

      if (info_.format_code == 0xFFFE && fmt_to_read >= 40) {
        info_.valid_bits_per_sample = readLe16(fmt + 18);
        info_.codec_tag = readLe16(fmt + 24);
      }

      if (chunk_size > fmt_to_read && !skipBytes(chunk_size - fmt_to_read)) return false;
      if ((chunk_size & 1u) != 0u && !skipBytes(1)) return false;

      fmt_found = true;
      continue;
    }

    if (memcmp(chunk, "data", 4) == 0) {
      info_.data_size = chunk_size;
      data_remaining_ = chunk_size;
      data_found = true;
      break;
    }

    const size_t skip = static_cast<size_t>(chunk_size) + ((chunk_size & 1u) ? 1u : 0u);
    if (!skipBytes(skip)) return false;
  }

  if (!fmt_found || !data_found) return false;
  if (info_.channels == 0) return false;
  if (info_.block_align % info_.channels != 0) return false;

  info_.output_channels = info_.channels >= 2 ? 2 : 1;
  info_.valid = true;
  return true;
}

bool WavDecoder::readExactly(uint8_t* dst, size_t bytes) {
  if (source_ == nullptr || dst == nullptr) return false;
  size_t total = 0;
  while (total < bytes) {
    const size_t got = source_->read(dst + total, bytes - total);
    if (got == 0) return false;
    total += got;
  }
  return true;
}

bool WavDecoder::skipBytes(size_t bytes) {
  if (source_ == nullptr) return false;
  if (bytes == 0) return true;

  const size_t pos = source_->position();
  if (source_->seek(pos + bytes)) return true;

  size_t left = bytes;
  while (left > 0) {
    const size_t want = min(left, sizeof(skip_scratch_));
    const size_t got = source_->read(skip_scratch_, want);
    if (got == 0) return false;
    left -= got;
  }
  return true;
}

int16_t WavDecoder::decodeSample(const uint8_t* src) const {
  const auto cls = codecClass(info_);

  if (cls == WavCodecClass::PcmInt) {
    if (info_.bits_per_sample == 8 && bytes_per_sample_ >= 1) {
      const int32_t v = static_cast<int32_t>(src[0]) - 128;
      return clampToPcm16(v << 8);
    }

    if ((info_.bits_per_sample == 16 || info_.bits_per_sample == 24 ||
         info_.bits_per_sample == 32) &&
        bytes_per_sample_ >= 2) {
      int32_t raw = readSignedInt(src, bytes_per_sample_);

      const uint8_t container_bits = static_cast<uint8_t>(min<uint16_t>(32, bytes_per_sample_ * 8));
      uint8_t valid_bits = info_.valid_bits_per_sample > 0
                               ? static_cast<uint8_t>(info_.valid_bits_per_sample)
                               : static_cast<uint8_t>(info_.bits_per_sample);
      if (valid_bits > container_bits) valid_bits = container_bits;
      if (valid_bits == 0) valid_bits = container_bits;

      if (valid_bits < container_bits) {
        raw >>= (container_bits - valid_bits);
      }

      if (valid_bits > 16) {
        raw >>= (valid_bits - 16);
      } else if (valid_bits < 16) {
        raw <<= (16 - valid_bits);
      }

      return clampToPcm16(raw);
    }

    return 0;
  }

  if (cls == WavCodecClass::Float) {
    float sample = 0.0f;
    if (info_.bits_per_sample == 32 && bytes_per_sample_ >= 4) {
      memcpy(&sample, src, sizeof(float));
    } else if (info_.bits_per_sample == 64 && bytes_per_sample_ >= 8) {
      double d = 0.0;
      memcpy(&d, src, sizeof(double));
      sample = static_cast<float>(d);
    } else {
      return 0;
    }

    if (!isfinite(sample)) sample = 0.0f;
    if (sample > 1.0f) sample = 1.0f;
    if (sample < -1.0f) sample = -1.0f;
    return static_cast<int16_t>(sample * 32767.0f);
  }

  if (cls == WavCodecClass::ALaw && bytes_per_sample_ >= 1) {
    return decodeALaw(src[0]);
  }

  if (cls == WavCodecClass::MuLaw && bytes_per_sample_ >= 1) {
    return decodeMuLaw(src[0]);
  }

  return 0;
}

}  // namespace padre
