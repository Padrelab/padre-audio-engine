#include "PlaylistManager.h"

namespace padre {

PlaylistManager::PlaylistManager(uint32_t seed) {
  randomSeed(seed == 0 ? micros() : seed);
}

void PlaylistManager::setTracks(const std::vector<String>& tracks) {
  tracks_ = tracks;
  index_ = 0;
  rebuildShuffle(false);
}

void PlaylistManager::setOrder(PlayOrder order) {
  mode_ = order;
  index_ = 0;
  rebuildShuffle(true);
}

const String* PlaylistManager::current() const {
  if (empty()) return nullptr;
  return &tracks_[order_[index_]];
}

const String* PlaylistManager::next(bool reshuffle_on_end) {
  if (empty()) return nullptr;
  if (index_ + 1 < order_.size()) {
    ++index_;
    return current();
  }

  if (mode_ == PlayOrder::Shuffle && reshuffle_on_end) {
    last_tail_ = order_.back();
    rebuildShuffle(false);
    index_ = 0;
    return current();
  }

  return nullptr;
}

const String* PlaylistManager::previous() {
  if (empty() || index_ == 0) return nullptr;
  --index_;
  return current();
}

bool PlaylistManager::empty() const { return tracks_.empty(); }
size_t PlaylistManager::size() const { return tracks_.size(); }
size_t PlaylistManager::index() const { return index_; }

void PlaylistManager::rebuildShuffle(bool keep_anchor) {
  order_.clear();
  order_.reserve(tracks_.size());

  for (size_t i = 0; i < tracks_.size(); ++i) order_.push_back(i);

  if (mode_ == PlayOrder::Shuffle && order_.size() > 1) {
    for (int i = static_cast<int>(order_.size()) - 1; i > 0; --i) {
      const int j = random(0, i + 1);
      const auto tmp = order_[i];
      order_[i] = order_[j];
      order_[j] = tmp;
    }

    if (last_tail_ != SIZE_MAX && order_.front() == last_tail_) {
      const size_t swap_index = order_.size() > 2 ? 2 : 1;
      const auto tmp = order_[0];
      order_[0] = order_[swap_index];
      order_[swap_index] = tmp;
    }
  }

  if (keep_anchor && index_ < order_.size()) {
    return;
  }
  index_ = 0;
}

}  // namespace padre
