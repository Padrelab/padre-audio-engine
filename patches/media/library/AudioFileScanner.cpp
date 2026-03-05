#include "AudioFileScanner.h"

namespace {

constexpr const char* kDefaultAudioExtensions[] = {
    ".wav",
    ".mp3",
    ".flac",
};

}  // namespace

namespace padre {

AudioFileScanner::AudioFileScanner(fs::FS& fs) : fs_(&fs) {}

bool AudioFileScanner::hasSupportedExtension(const String& path,
                                             const char* const* extensions,
                                             size_t extension_count) const {
  resolveExtensions(extensions, extension_count);
  if (extensions == nullptr || extension_count == 0) return false;

  String lower = path;
  lower.toLowerCase();
  for (size_t i = 0; i < extension_count; ++i) {
    const char* ext = extensions[i];
    if (ext == nullptr || ext[0] == '\0') continue;
    if (lower.endsWith(ext)) return true;
  }
  return false;
}

size_t AudioFileScanner::scan(const String& root_path,
                              std::vector<String>& out_paths,
                              const AudioFileScannerOptions& options) const {
  if (fs_ == nullptr) return out_paths.size();

  const char* const* extensions = options.extensions;
  size_t extension_count = options.extension_count;
  resolveExtensions(extensions, extension_count);

  const size_t before = out_paths.size();
  scanDirRecursive(root_path, options.max_depth, extensions, extension_count, out_paths);
  return out_paths.size() - before;
}

void AudioFileScanner::scanDirRecursive(const String& dir_path,
                                        uint8_t depth_left,
                                        const char* const* extensions,
                                        size_t extension_count,
                                        std::vector<String>& out_paths) const {
  if (fs_ == nullptr) return;

  File dir = fs_->open(dir_path.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  File entry = dir.openNextFile();
  while (entry) {
    String path = entry.path();
    if (path.length() == 0) {
      path = dir_path;
      if (!path.endsWith("/")) path += "/";
      path += entry.name();
    }

    if (entry.isDirectory()) {
      if (depth_left > 0) {
        scanDirRecursive(path, static_cast<uint8_t>(depth_left - 1), extensions,
                         extension_count, out_paths);
      }
    } else if (hasSupportedExtension(path, extensions, extension_count)) {
      out_paths.push_back(path);
    }

    entry.close();
    entry = dir.openNextFile();
  }

  dir.close();
}

void AudioFileScanner::resolveExtensions(const char* const*& extensions,
                                         size_t& extension_count) {
  if (extensions != nullptr && extension_count > 0) return;
  extensions = kDefaultAudioExtensions;
  extension_count = sizeof(kDefaultAudioExtensions) / sizeof(kDefaultAudioExtensions[0]);
}

}  // namespace padre
