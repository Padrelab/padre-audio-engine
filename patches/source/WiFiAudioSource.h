#pragma once

#include <Arduino.h>
#include <functional>

#include "IAudioSource.h"

namespace padre {

class WiFiAudioSource : public IAudioSource {
 public:
  struct Callbacks {
    std::function<bool()> begin;
    std::function<bool(const String&)> connect;
    std::function<size_t(uint8_t*, size_t)> read;
    std::function<bool(size_t)> seek;
    std::function<size_t()> position;
    std::function<size_t()> size;
    std::function<bool()> eof;
    std::function<bool()> is_open;
    std::function<void()> close;
  };

  explicit WiFiAudioSource(Callbacks callbacks);

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
