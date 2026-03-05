#pragma once

#include <Arduino.h>
#include <FS.h>

#include "IAudioSource.h"

namespace padre {

struct FsAudioSourceConfig {
  const char* type_name = "fs";
  const char* required_prefix = nullptr;
};

class FsAudioSource : public IAudioSource {
 public:
  FsAudioSource(fs::FS& fs, const FsAudioSourceConfig& config = {});

  bool begin() override;
  bool open(const String& uri) override;
  size_t read(uint8_t* dst, size_t bytes) override;
  bool seek(size_t offset) override;
  size_t position() const override;
  size_t size() const override;
  bool eof() const override;
  bool isOpen() const override;
  void close() override;
  const char* type() const override;

 private:
  bool parsePath(const String& uri, String& out_path) const;
  static bool startsWithIgnoreCase(const String& str, const char* prefix);

  fs::FS* fs_ = nullptr;
  FsAudioSourceConfig config_;
  mutable File file_;
};

}  // namespace padre
