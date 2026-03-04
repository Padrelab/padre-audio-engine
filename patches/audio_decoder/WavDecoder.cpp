#include "WavDecoder.h"

#include <cstring>
#include <fstream>
#include <limits>

namespace padre {
namespace {

struct WavFmt {
  uint16_t audio_format = 0;      // 1 PCM, 3 IEEE float
  uint16_t channels = 0;
  uint32_t sample_rate = 0;
  uint16_t bits_per_sample = 0;
};

bool read_exact(std::ifstream& in, void* dst, std::size_t n) {
  in.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(n));
  return static_cast<std::size_t>(in.gcount()) == n;
}

template <typename T>
bool read_le(std::ifstream& in, T& value) {
  return read_exact(in, &value, sizeof(T));
}

float int24_to_float(const uint8_t* p) {
  int32_t v = (static_cast<int32_t>(p[0]) | (static_cast<int32_t>(p[1]) << 8) |
               (static_cast<int32_t>(p[2]) << 16));
  if (v & 0x00800000) {
    v |= ~0x00FFFFFF;
  }
  return static_cast<float>(v) / 8388608.0f;
}

}  // namespace

DecodeResult WavDecoder::decode_file(const std::string& path, DecodedAudio& out) const {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return DecodeResult::fail("WAV: cannot open file: " + path);
  }

  char riff[4] = {};
  uint32_t file_size = 0;
  char wave[4] = {};
  if (!read_exact(in, riff, 4) || !read_le(in, file_size) || !read_exact(in, wave, 4) ||
      std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(wave, "WAVE", 4) != 0) {
    return DecodeResult::fail("WAV: invalid RIFF/WAVE header");
  }

  WavFmt fmt;
  std::vector<uint8_t> data;
  bool has_fmt = false;
  bool has_data = false;

  while (in && (!has_fmt || !has_data)) {
    char chunk_id[4] = {};
    uint32_t chunk_size = 0;
    if (!read_exact(in, chunk_id, 4) || !read_le(in, chunk_size)) {
      break;
    }

    if (std::memcmp(chunk_id, "fmt ", 4) == 0) {
      if (chunk_size < 16) {
        return DecodeResult::fail("WAV: invalid fmt chunk");
      }
      if (!read_le(in, fmt.audio_format) || !read_le(in, fmt.channels) || !read_le(in, fmt.sample_rate)) {
        return DecodeResult::fail("WAV: failed reading fmt chunk");
      }
      uint32_t byte_rate = 0;
      uint16_t block_align = 0;
      if (!read_le(in, byte_rate) || !read_le(in, block_align) || !read_le(in, fmt.bits_per_sample)) {
        return DecodeResult::fail("WAV: failed reading fmt chunk fields");
      }
      if (chunk_size > 16) {
        in.seekg(chunk_size - 16, std::ios::cur);
      }
      has_fmt = true;
    } else if (std::memcmp(chunk_id, "data", 4) == 0) {
      data.resize(chunk_size);
      if (!read_exact(in, data.data(), chunk_size)) {
        return DecodeResult::fail("WAV: failed reading data chunk");
      }
      has_data = true;
    } else {
      in.seekg(chunk_size, std::ios::cur);
    }

    if (chunk_size & 1u) {
      in.seekg(1, std::ios::cur);
    }
  }

  if (!has_fmt || !has_data) {
    return DecodeResult::fail("WAV: missing fmt/data chunk");
  }
  if (fmt.channels < 1 || fmt.channels > 2) {
    return DecodeResult::fail("WAV: only mono/stereo supported");
  }
  if (!(fmt.sample_rate == 44100 || fmt.sample_rate == 48000)) {
    return DecodeResult::fail("WAV: supported sample rates are 44100/48000 Hz");
  }

  const uint32_t bits = fmt.bits_per_sample;
  const uint32_t bytes_per_sample = bits / 8;
  if (!(bits == 16 || bits == 24 || bits == 32)) {
    return DecodeResult::fail("WAV: supported bit depth is 16/24/32/32float");
  }
  if (fmt.audio_format != 1 && fmt.audio_format != 3) {
    return DecodeResult::fail("WAV: unsupported format (only PCM or float)");
  }
  if (fmt.audio_format == 3 && bits != 32) {
    return DecodeResult::fail("WAV: float WAV must be 32-bit");
  }

  const std::size_t frame_size = static_cast<std::size_t>(bytes_per_sample) * fmt.channels;
  if (frame_size == 0 || (data.size() % frame_size) != 0) {
    return DecodeResult::fail("WAV: invalid data block alignment");
  }

  const std::size_t sample_count = data.size() / bytes_per_sample;
  out.samples.resize(sample_count);
  out.channels = fmt.channels;
  out.sample_rate = fmt.sample_rate;
  out.source_format = (fmt.audio_format == 3) ? SampleFormat::Float32
                                               : (bits == 16 ? SampleFormat::Int16
                                                             : (bits == 24 ? SampleFormat::Int24
                                                                           : SampleFormat::Int32));

  if (fmt.audio_format == 3) {
    for (std::size_t i = 0; i < sample_count; ++i) {
      float v = 0.0f;
      std::memcpy(&v, data.data() + i * 4, 4);
      out.samples[i] = v;
    }
  } else if (bits == 16) {
    for (std::size_t i = 0; i < sample_count; ++i) {
      int16_t v = 0;
      std::memcpy(&v, data.data() + i * 2, 2);
      out.samples[i] = static_cast<float>(v) / 32768.0f;
    }
  } else if (bits == 24) {
    for (std::size_t i = 0; i < sample_count; ++i) {
      out.samples[i] = int24_to_float(data.data() + i * 3);
    }
  } else {
    for (std::size_t i = 0; i < sample_count; ++i) {
      int32_t v = 0;
      std::memcpy(&v, data.data() + i * 4, 4);
      out.samples[i] = static_cast<float>(v / 2147483648.0);
    }
  }

  return DecodeResult::success();
}

}  // namespace padre
