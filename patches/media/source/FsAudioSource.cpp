#include "FsAudioSource.h"

#include <cctype>
#include <cstring>

namespace padre {

FsAudioSource::FsAudioSource(fs::FS& fs, const FsAudioSourceConfig& config)
    : fs_(&fs), config_(config) {}

bool FsAudioSource::begin() { return fs_ != nullptr; }

bool FsAudioSource::open(const String& uri) {
  close();
  if (fs_ == nullptr) return false;

  String path;
  if (!parsePath(uri, path)) return false;
  if (!path.startsWith("/")) path = "/" + path;

  file_ = fs_->open(path.c_str(), FILE_READ);
  return static_cast<bool>(file_);
}

size_t FsAudioSource::read(uint8_t* dst, size_t bytes) {
  if (!file_ || dst == nullptr || bytes == 0) return 0;
  const int got = file_.read(dst, bytes);
  return got > 0 ? static_cast<size_t>(got) : 0;
}

bool FsAudioSource::seek(size_t offset) { return file_ ? file_.seek(offset) : false; }

size_t FsAudioSource::position() const {
  return file_ ? static_cast<size_t>(file_.position()) : 0;
}

size_t FsAudioSource::size() const {
  return file_ ? static_cast<size_t>(file_.size()) : 0;
}

bool FsAudioSource::eof() const {
  if (!file_) return true;
  return file_.position() >= file_.size();
}

bool FsAudioSource::isOpen() const { return static_cast<bool>(file_); }

void FsAudioSource::close() {
  if (file_) file_.close();
}

const char* FsAudioSource::type() const {
  return config_.type_name ? config_.type_name : "fs";
}

bool FsAudioSource::parsePath(const String& uri, String& out_path) const {
  if (uri.length() == 0) return false;

  if (config_.required_prefix != nullptr && config_.required_prefix[0] != '\0') {
    const size_t prefix_len = strlen(config_.required_prefix);
    if (!startsWithIgnoreCase(uri, config_.required_prefix)) return false;
    out_path = uri.substring(prefix_len);
    return out_path.length() > 0;
  }

  out_path = uri;
  return true;
}

bool FsAudioSource::startsWithIgnoreCase(const String& str, const char* prefix) {
  if (prefix == nullptr) return false;

  const size_t prefix_len = strlen(prefix);
  if (str.length() < prefix_len) return false;

  for (size_t i = 0; i < prefix_len; ++i) {
    if (tolower(static_cast<unsigned char>(str[i])) !=
        tolower(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}

}  // namespace padre
