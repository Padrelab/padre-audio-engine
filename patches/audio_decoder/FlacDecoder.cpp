#include "FlacDecoder.h"

#if __has_include("third_party/dr_flac.h")
#define DR_FLAC_IMPLEMENTATION
#include "third_party/dr_flac.h"
#define PADRE_HAS_DR_FLAC 1
#else
#define PADRE_HAS_DR_FLAC 0
#endif

namespace padre {

DecodeResult FlacDecoder::decode_file(const std::string& path, DecodedAudio& out) const {
#if PADRE_HAS_DR_FLAC
  unsigned int channels = 0;
  unsigned int sample_rate = 0;
  drflac_uint64 frame_count = 0;
  float* pcm = drflac_open_file_and_read_pcm_frames_f32(path.c_str(), &channels, &sample_rate, &frame_count,
                                                        nullptr);
  if (pcm == nullptr) {
    return DecodeResult::fail("FLAC: failed to decode file: " + path);
  }

  if (channels < 1 || channels > 2) {
    drflac_free(pcm, nullptr);
    return DecodeResult::fail("FLAC: only mono/stereo supported");
  }
  if (!(sample_rate == 44100 || sample_rate == 48000)) {
    drflac_free(pcm, nullptr);
    return DecodeResult::fail("FLAC: supported sample rates are 44100/48000 Hz");
  }

  const std::size_t sample_count = static_cast<std::size_t>(frame_count) * channels;
  out.channels = static_cast<uint16_t>(channels);
  out.sample_rate = sample_rate;
  out.source_format = SampleFormat::Float32;
  out.samples.assign(pcm, pcm + sample_count);

  drflac_free(pcm, nullptr);
  return DecodeResult::success();
#else
  (void)path;
  (void)out;
  return DecodeResult::fail(
      "FLAC decoder backend is missing. Add dr_flac.h to patches/audio_decoder/third_party/");
#endif
}

}  // namespace padre
