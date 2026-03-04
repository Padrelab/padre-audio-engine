#include "PreferencesStore.h"

namespace padre {

bool PreferencesStore::begin(const char* name_space, bool read_only) {
  return prefs_.begin(name_space, read_only);
}

void PreferencesStore::end() { prefs_.end(); }

bool PreferencesStore::containsKey(const char* key) const {
  return prefs_.isKey(key);
}

float PreferencesStore::getFloat(const char* key, float default_value) const {
  return prefs_.getFloat(key, default_value);
}

bool PreferencesStore::putFloat(const char* key, float value) {
  return prefs_.putFloat(key, value) > 0;
}

}  // namespace padre
