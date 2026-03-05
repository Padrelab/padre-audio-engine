#pragma once

#include <Arduino.h>
#include <FS.h>
#include <stdint.h>

#include <vector>

namespace padre {

struct AudioFileScannerOptions {
  uint8_t max_depth = 5;
  const char* const* extensions = nullptr;
  size_t extension_count = 0;
};

class AudioFileScanner {
 public:
  explicit AudioFileScanner(fs::FS& fs);

  bool hasSupportedExtension(const String& path,
                             const char* const* extensions = nullptr,
                             size_t extension_count = 0) const;

  size_t scan(const String& root_path,
              std::vector<String>& out_paths,
              const AudioFileScannerOptions& options = {}) const;

 private:
  void scanDirRecursive(const String& dir_path,
                        uint8_t depth_left,
                        const char* const* extensions,
                        size_t extension_count,
                        std::vector<String>& out_paths) const;

  static void resolveExtensions(const char* const*& extensions, size_t& extension_count);

  fs::FS* fs_ = nullptr;
};

}  // namespace padre
