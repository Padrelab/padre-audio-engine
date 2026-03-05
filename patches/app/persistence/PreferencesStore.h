#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include "SettingsStore.h"

namespace padre {

class PreferencesStore : public ISettingsStore {
 public:
  bool begin(const char* name_space, bool read_only = false) override;
  void end() override;

  bool containsKey(const char* key) const override;
  float getFloat(const char* key, float default_value) const override;
  bool putFloat(const char* key, float value) override;

 private:
  mutable Preferences prefs_;
};

}  // namespace padre
