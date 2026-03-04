#pragma once

#include <Arduino.h>
#include <vector>

namespace test_sd_mpr121 {

class RandomPlaylist {
 public:
  bool loadFromDirectory(fs::FS& fs, const char* directory_path);
  const String& current() const;
  const String& next();
  bool empty() const;
  size_t size() const;

 private:
  bool isSupportedFile(const String& path) const;
  void reshuffle();

  std::vector<String> tracks_;
  std::vector<size_t> order_;
  size_t order_index_ = 0;
  String empty_track_;
};

}  // namespace test_sd_mpr121
