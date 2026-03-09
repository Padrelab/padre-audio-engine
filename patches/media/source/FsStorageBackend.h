#pragma once

#include <Arduino.h>
#include <FS.h>

namespace padre {

class FsStorageBackend {
 public:
  virtual ~FsStorageBackend() = default;

  virtual bool begin(Print& log) = 0;
  virtual void printConfig(Print& out) const = 0;
  virtual fs::FS& fs() = 0;
  virtual const char* typeName() const = 0;
};

}  // namespace padre
