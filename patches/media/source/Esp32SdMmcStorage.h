#pragma once

#include <Arduino.h>
#include <SD_MMC.h>

#include "FsStorageBackend.h"

namespace padre {

struct Esp32SdMmcStorageConfig {
  int clk_pin = 12;
  int cmd_pin = 11;
  int d0_pin = 13;
  int d1_pin = 14;
  int d2_pin = 9;
  int d3_pin = 10;
  uint32_t frequency_hz = 20000000ul;
  bool mode_1bit = false;
  const char* mount_point = "/sdcard";
  bool format_if_mount_failed = false;
};

class Esp32SdMmcStorage : public FsStorageBackend {
 public:
  explicit Esp32SdMmcStorage(Esp32SdMmcStorageConfig config = {});

  bool begin(Print& log) override;
  void printConfig(Print& out) const override;
  fs::FS& fs() override;
  const char* typeName() const override;

 private:
  static const char* cardTypeName(uint8_t card_type);

  Esp32SdMmcStorageConfig config_;
};

}  // namespace padre
