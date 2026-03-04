#include "RuntimeSettingsPersistence.h"

namespace padre {

RuntimeSettingsPersistence::RuntimeSettingsPersistence(ISettingsStore& store)
    : store_(store) {}

bool RuntimeSettingsPersistence::begin(const char* name_space, bool read_only) {
  return store_.begin(name_space, read_only);
}

void RuntimeSettingsPersistence::end() { store_.end(); }

bool RuntimeSettingsPersistence::loadVolume(VolumeController& volume_controller,
                                            const char* key) {
  const float stored = store_.getFloat(key, volume_controller.current());
  volume_controller.restore(stored);
  return store_.containsKey(key);
}

bool RuntimeSettingsPersistence::saveVolume(const VolumeController& volume_controller,
                                            const char* key) {
  return store_.putFloat(key, volume_controller.target());
}

size_t RuntimeSettingsPersistence::loadParams(PersistedFloatParam* params,
                                              size_t count) {
  size_t loaded = 0;
  for (size_t i = 0; i < count; ++i) {
    PersistedFloatParam& param = params[i];
    if (param.key == nullptr || param.value == nullptr) continue;

    const float current = *param.value;
    const float stored = store_.getFloat(param.key, current);
    *param.value = clamp(stored, param.min, param.max);

    if (store_.containsKey(param.key)) ++loaded;
  }
  return loaded;
}

size_t RuntimeSettingsPersistence::saveParams(const PersistedFloatParam* params,
                                              size_t count) {
  size_t saved = 0;
  for (size_t i = 0; i < count; ++i) {
    const PersistedFloatParam& param = params[i];
    if (param.key == nullptr || param.value == nullptr) continue;

    const float value = clamp(*param.value, param.min, param.max);
    if (store_.putFloat(param.key, value)) ++saved;
  }
  return saved;
}

float RuntimeSettingsPersistence::clamp(float value, float min_value,
                                        float max_value) {
  if (value < min_value) return min_value;
  if (value > max_value) return max_value;
  return value;
}

}  // namespace padre
