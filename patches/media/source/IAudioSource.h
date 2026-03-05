#pragma once

#include <Arduino.h>
#include <stddef.h>

namespace padre {

class IAudioSource {
 public:
  virtual ~IAudioSource() = default;

  virtual bool begin() = 0;
  virtual bool open(const String& uri) = 0;
  virtual size_t read(uint8_t* dst, size_t bytes) = 0;
  virtual bool seek(size_t offset) = 0;
  virtual size_t position() const = 0;
  virtual size_t size() const = 0;
  virtual bool eof() const = 0;
  virtual bool isOpen() const = 0;
  virtual void close() = 0;

  virtual const char* type() const = 0;
};

}  // namespace padre
