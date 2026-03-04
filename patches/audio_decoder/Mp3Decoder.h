#pragma once

#include <string>

#include "AudioTypes.h"

namespace padre {

class Mp3Decoder {
 public:
  DecodeResult decode_file(const std::string& path, DecodedAudio& out) const;
};

}  // namespace padre
