#pragma once

#include <Arduino.h>

#include "SdAudioSource.h"

namespace padre {

class EmmcAudioSource : public IAudioSource {
 public:
  using Callbacks = SdAudioSource::Callbacks;

  explicit EmmcAudioSource(Callbacks callbacks);

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
