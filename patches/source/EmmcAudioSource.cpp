#include "EmmcAudioSource.h"

namespace padre {

EmmcAudioSource::EmmcAudioSource(Callbacks callbacks) : cb_(callbacks) {}

bool EmmcAudioSource::begin() { return cb_.begin ? cb_.begin() : true; }

bool EmmcAudioSource::open(const String& uri) {
  if (!cb_.open) return false;
  return cb_.open(uri);
}

size_t EmmcAudioSource::read(uint8_t* dst, size_t bytes) {
  if (!cb_.read || dst == nullptr || bytes == 0) return 0;
  return cb_.read(dst, bytes);
}

bool EmmcAudioSource::seek(size_t offset) {
  if (!cb_.seek) return false;
  return cb_.seek(offset);
}

size_t EmmcAudioSource::position() const { return cb_.position ? cb_.position() : 0; }

size_t EmmcAudioSource::size() const { return cb_.size ? cb_.size() : 0; }

bool EmmcAudioSource::eof() const { return cb_.eof ? cb_.eof() : true; }

bool EmmcAudioSource::isOpen() const { return cb_.is_open ? cb_.is_open() : false; }

void EmmcAudioSource::close() {
  if (cb_.close) cb_.close();
}

const char* EmmcAudioSource::type() const { return "emmc"; }

}  // namespace padre
