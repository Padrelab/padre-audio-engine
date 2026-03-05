#include "FsLibraryFacade.h"

namespace padre {

FsLibraryFacade::FsLibraryFacade(fs::FS& fs, const FsLibraryFacadeConfig& config)
    : source_(fs, config.source), scanner_(fs), scanner_options_(config.scanner) {}

FsAudioSource& FsLibraryFacade::source() { return source_; }

const FsAudioSource& FsLibraryFacade::source() const { return source_; }

AudioFileScanner& FsLibraryFacade::scanner() { return scanner_; }

const AudioFileScanner& FsLibraryFacade::scanner() const { return scanner_; }

size_t FsLibraryFacade::scan(const String& root_path, std::vector<String>& out_paths) const {
  return scanner_.scan(root_path, out_paths, scanner_options_);
}

}  // namespace padre
