#include "SdAudioSource.h"

namespace padre {

SdAudioSource::SdAudioSource(Callbacks callbacks) : cb_(callbacks) {}

bool SdAudioSource::begin() { return cb_.begin ? cb_.begin() : true; }

bool SdAudioSource::open(const String& uri) {
  if (!cb_.open) return false;
  return cb_.open(uri);
}

size_t SdAudioSource::read(uint8_t* dst, size_t bytes) {
  if (!cb_.read || dst == nullptr || bytes == 0) return 0;
  return cb_.read(dst, bytes);
}

bool SdAudioSource::seek(size_t offset) {
  if (!cb_.seek) return false;
  return cb_.seek(offset);
}

size_t SdAudioSource::position() const { return cb_.position ? cb_.position() : 0; }

size_t SdAudioSource::size() const { return cb_.size ? cb_.size() : 0; }

bool SdAudioSource::eof() const { return cb_.eof ? cb_.eof() : true; }

bool SdAudioSource::isOpen() const { return cb_.is_open ? cb_.is_open() : false; }

void SdAudioSource::close() {
  if (cb_.close) cb_.close();
}

const char* SdAudioSource::type() const { return "sd"; }

}  // namespace padre
