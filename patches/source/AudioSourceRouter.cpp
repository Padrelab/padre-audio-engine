#include "AudioSourceRouter.h"

namespace padre {

AudioSourceRouter::AudioSourceRouter(const Entry* entries, size_t count)
    : entries_(entries), count_(count) {}

IAudioSource* AudioSourceRouter::resolve(const String& uri) const {
  const int split = uri.indexOf("://");
  if (split <= 0) return nullptr;

  const String scheme = uri.substring(0, split);
  for (size_t i = 0; i < count_; ++i) {
    if (entries_[i].source == nullptr || entries_[i].scheme == nullptr) continue;
    if (scheme.equalsIgnoreCase(entries_[i].scheme)) {
      return entries_[i].source;
    }
  }
  return nullptr;
}

}  // namespace padre
