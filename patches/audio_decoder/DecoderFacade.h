#pragma once

#include <algorithm>
#include <cctype>
#include <string>

#include "AudioTypes.h"
#include "FlacDecoder.h"
#include "Mp3Decoder.h"
#include "WavDecoder.h"

namespace padre {

class DecoderFacade {
 public:
  DecodeResult decode_file(const std::string& path, DecodedAudio& out) const {
    const std::string ext = extension(path);
    if (ext == ".wav") {
      return wav_.decode_file(path, out);
    }
    if (ext == ".mp3") {
      return mp3_.decode_file(path, out);
    }
    if (ext == ".flac") {
      return flac_.decode_file(path, out);
    }
    return DecodeResult::fail("Unsupported extension: " + ext);
  }

 private:
  static std::string extension(const std::string& path) {
    const auto pos = path.find_last_of('.');
    if (pos == std::string::npos) {
      return {};
    }
    std::string ext = path.substr(pos);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
  }

  WavDecoder wav_;
  Mp3Decoder mp3_;
  FlacDecoder flac_;
};

}  // namespace padre
