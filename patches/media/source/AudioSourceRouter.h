#pragma once

#include <Arduino.h>
#include <stddef.h>

#include "IAudioSource.h"

namespace padre {

class AudioSourceRouter {
 public:
  struct Entry {
    const char* scheme;
    IAudioSource* source;
  };

  AudioSourceRouter(const Entry* entries, size_t count);

  IAudioSource* resolve(const String& uri) const;

 private:
  const Entry* entries_ = nullptr;
  size_t count_ = 0;
};

}  // namespace padre
