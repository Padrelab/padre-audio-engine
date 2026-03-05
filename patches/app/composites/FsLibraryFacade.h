#pragma once

#include <Arduino.h>
#include <FS.h>

#include <vector>

#include "../../media/library/AudioFileScanner.h"
#include "../../media/source/FsAudioSource.h"

namespace padre {

struct FsLibraryFacadeConfig {
  FsAudioSourceConfig source = {};
  AudioFileScannerOptions scanner = {};
};

class FsLibraryFacade {
 public:
  explicit FsLibraryFacade(fs::FS& fs, const FsLibraryFacadeConfig& config = {});

  FsAudioSource& source();
  const FsAudioSource& source() const;

  AudioFileScanner& scanner();
  const AudioFileScanner& scanner() const;

  size_t scan(const String& root_path, std::vector<String>& out_paths) const;

 private:
  FsAudioSource source_;
  AudioFileScanner scanner_;
  AudioFileScannerOptions scanner_options_;
};

}  // namespace padre
