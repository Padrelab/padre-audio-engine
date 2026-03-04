#include "RandomPlaylist.h"

#include <FS.h>

namespace test_sd_mpr121 {

bool RandomPlaylist::loadFromDirectory(fs::FS& fs, const char* directory_path) {
  tracks_.clear();
  order_.clear();
  order_index_ = 0;

  File dir = fs.open(directory_path);
  if (!dir || !dir.isDirectory()) {
    return false;
  }

  File entry = dir.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      const String path = String(entry.path());
      if (isSupportedFile(path)) {
        tracks_.push_back(path);
      }
    }
    entry = dir.openNextFile();
  }

  if (tracks_.empty()) {
    return false;
  }

  order_.reserve(tracks_.size());
  for (size_t i = 0; i < tracks_.size(); ++i) {
    order_.push_back(i);
  }

  reshuffle();
  return true;
}

const String& RandomPlaylist::current() const {
  if (tracks_.empty() || order_.empty()) {
    return empty_track_;
  }
  return tracks_[order_[order_index_]];
}

const String& RandomPlaylist::next() {
  if (tracks_.empty() || order_.empty()) {
    return empty_track_;
  }

  ++order_index_;
  if (order_index_ >= order_.size()) {
    reshuffle();
    order_index_ = 0;
  }

  return tracks_[order_[order_index_]];
}

bool RandomPlaylist::empty() const { return tracks_.empty(); }

size_t RandomPlaylist::size() const { return tracks_.size(); }

bool RandomPlaylist::isSupportedFile(const String& path) const {
  String lower = path;
  lower.toLowerCase();
  return lower.endsWith(".wav") || lower.endsWith(".mp3") || lower.endsWith(".flac");
}

void RandomPlaylist::reshuffle() {
  if (order_.size() < 2) return;

  for (size_t i = order_.size() - 1; i > 0; --i) {
    const size_t j = static_cast<size_t>(random(static_cast<long>(i + 1)));
    const size_t tmp = order_[i];
    order_[i] = order_[j];
    order_[j] = tmp;
  }
}

}  // namespace test_sd_mpr121
