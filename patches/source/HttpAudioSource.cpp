#include "HttpAudioSource.h"

namespace padre {

HttpAudioSource::HttpAudioSource(Callbacks callbacks) : cb_(callbacks) {}

bool HttpAudioSource::begin() { return cb_.begin ? cb_.begin() : true; }

bool HttpAudioSource::open(const String& uri) {
  if (!cb_.connect) return false;
  return cb_.connect(uri);
}

size_t HttpAudioSource::read(uint8_t* dst, size_t bytes) {
  if (!cb_.read || dst == nullptr || bytes == 0) return 0;
  return cb_.read(dst, bytes);
}

bool HttpAudioSource::seek(size_t offset) {
  if (!cb_.seek) return false;
  return cb_.seek(offset);
}

size_t HttpAudioSource::position() const { return cb_.position ? cb_.position() : 0; }

size_t HttpAudioSource::size() const { return cb_.size ? cb_.size() : 0; }

bool HttpAudioSource::eof() const { return cb_.eof ? cb_.eof() : true; }

bool HttpAudioSource::isOpen() const { return cb_.is_open ? cb_.is_open() : false; }

void HttpAudioSource::close() {
  if (cb_.close) cb_.close();
}

const char* HttpAudioSource::type() const { return "http"; }

}  // namespace padre
