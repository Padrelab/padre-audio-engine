#pragma once

#include <Arduino.h>
#include <vector>

namespace padre {

enum class PlayOrder {
  Sequential,
  Shuffle,
};

class PlaylistManager {
 public:
  explicit PlaylistManager(uint32_t seed = 0);

  void setTracks(const std::vector<String>& tracks);
  void setOrder(PlayOrder order);

  const String* current() const;
  const String* next(bool reshuffle_on_end = true);
  const String* previous();

  bool empty() const;
  size_t size() const;
  size_t index() const;

 private:
  void rebuildShuffle(bool keep_anchor);

  std::vector<String> tracks_;
  std::vector<size_t> order_;
  size_t index_ = 0;
  PlayOrder mode_ = PlayOrder::Sequential;
  size_t last_tail_ = SIZE_MAX;
};

}  // namespace padre
