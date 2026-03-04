#include "Mp3Decoder.h"

#if __has_include("third_party/minimp3_ex.h")
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "third_party/minimp3_ex.h"
#define PADRE_HAS_MINIMP3 1
#else
#define PADRE_HAS_MINIMP3 0
#endif

namespace padre {

DecodeResult Mp3Decoder::decode_file(const std::string& path, DecodedAudio& out) const {
#if PADRE_HAS_MINIMP3
  mp3dec_ex_t decoder{};
  if (mp3dec_ex_open(&decoder, path.c_str(), MP3D_SEEK_TO_SAMPLE)) {
    return DecodeResult::fail("MP3: failed to open file: " + path);
  }

  if (decoder.info.channels < 1 || decoder.info.channels > 2) {
    mp3dec_ex_close(&decoder);
    return DecodeResult::fail("MP3: only mono/stereo supported");
  }
  if (!(decoder.info.hz == 44100 || decoder.info.hz == 48000)) {
    mp3dec_ex_close(&decoder);
    return DecodeResult::fail("MP3: supported sample rates are 44100/48000 Hz");
  }

  std::vector<mp3d_sample_t> tmp(decoder.samples);
  const size_t read = mp3dec_ex_read(&decoder, tmp.data(), tmp.size());
  mp3dec_ex_close(&decoder);
  if (read == 0) {
    return DecodeResult::fail("MP3: no audio samples decoded");
  }

  out.channels = static_cast<uint16_t>(decoder.info.channels);
  out.sample_rate = static_cast<uint32_t>(decoder.info.hz);
  out.source_format = SampleFormat::Int16;
  out.samples.resize(read);

  for (size_t i = 0; i < read; ++i) {
    out.samples[i] = static_cast<float>(tmp[i]) / 32768.0f;
  }
  return DecodeResult::success();
#else
  (void)path;
  (void)out;
  return DecodeResult::fail(
      "MP3 decoder backend is missing. Add minimp3_ex.h and minimp3.h to patches/audio_decoder/third_party/");
#endif
}

}  // namespace padre
