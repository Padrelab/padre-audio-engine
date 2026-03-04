#pragma once

#include <Arduino.h>

#include "WiFiAudioSource.h"

namespace padre {

class HttpAudioSource : public IAudioSource {
 public:
  using Callbacks = WiFiAudioSource::Callbacks;

  explicit HttpAudioSource(Callbacks callbacks);

  bool begin() override;
  bool open(const String& uri) override;
  size_t read(uint8_t* dst, size_t bytes) override;
  bool seek(size_t offset) override;
  size_t position() const override;
  size_t size() const override;
  bool eof() const override;
  bool isOpen() const override;
  void close() override;
  const char* type() const override;

 private:
  Callbacks cb_;
};

}  // namespace padre
