#pragma once

#include <string>

#include "AudioTypes.h"

namespace padre {

class WavDecoder {
 public:
  DecodeResult decode_file(const std::string& path, DecodedAudio& out) const;
};

}  // namespace padre
