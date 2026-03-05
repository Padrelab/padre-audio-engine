#pragma once

#include <Arduino.h>

#include "../../audio/control/VolumeController.h"
#include "SettingsStore.h"

namespace padre {

struct PersistedFloatParam {
  const char* key = nullptr;
  float* value = nullptr;
  float min = 0.0f;
  float max = 1.0f;
};

class RuntimeSettingsPersistence {
 public:
  explicit RuntimeSettingsPersistence(ISettingsStore& store);

  bool begin(const char* name_space = "audio", bool read_only = false);
  void end();

  bool loadVolume(VolumeController& volume_controller, const char* key = "volume");
  bool saveVolume(const VolumeController& volume_controller, const char* key = "volume");

  size_t loadParams(PersistedFloatParam* params, size_t count);
  size_t saveParams(const PersistedFloatParam* params, size_t count);

 private:
  static float clamp(float value, float min_value, float max_value);

  ISettingsStore& store_;
};

}  // namespace padre
