#pragma once

#include <Arduino.h>

namespace padre {

class ISettingsStore {
 public:
  virtual ~ISettingsStore() = default;

  virtual bool begin(const char* name_space, bool read_only = false) = 0;
  virtual void end() = 0;

  virtual bool containsKey(const char* key) const = 0;
  virtual float getFloat(const char* key, float default_value) const = 0;
  virtual bool putFloat(const char* key, float value) = 0;
};

}  // namespace padre
