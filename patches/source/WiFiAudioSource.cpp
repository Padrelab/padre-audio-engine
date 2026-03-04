#include "WiFiAudioSource.h"

namespace padre {

WiFiAudioSource::WiFiAudioSource(Callbacks callbacks) : cb_(callbacks) {}

bool WiFiAudioSource::begin() { return cb_.begin ? cb_.begin() : true; }

bool WiFiAudioSource::open(const String& uri) {
  if (!cb_.connect) return false;
  return cb_.connect(uri);
}

size_t WiFiAudioSource::read(uint8_t* dst, size_t bytes) {
  if (!cb_.read || dst == nullptr || bytes == 0) return 0;
  return cb_.read(dst, bytes);
}

bool WiFiAudioSource::seek(size_t offset) {
  if (!cb_.seek) return false;
  return cb_.seek(offset);
}

size_t WiFiAudioSource::position() const { return cb_.position ? cb_.position() : 0; }

size_t WiFiAudioSource::size() const { return cb_.size ? cb_.size() : 0; }

bool WiFiAudioSource::eof() const { return cb_.eof ? cb_.eof() : true; }

bool WiFiAudioSource::isOpen() const { return cb_.is_open ? cb_.is_open() : false; }

void WiFiAudioSource::close() {
  if (cb_.close) cb_.close();
}

const char* WiFiAudioSource::type() const { return "wifi"; }

}  // namespace padre
