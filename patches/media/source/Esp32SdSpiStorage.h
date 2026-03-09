#pragma once

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

#include "FsStorageBackend.h"

namespace padre {

struct Esp32SdSpiStorageConfig {
  uint8_t cs_pin = 10;
  uint8_t sck_pin = 12;
  uint8_t miso_pin = 13;
  uint8_t mosi_pin = 11;
  uint32_t clock_hz = 20000000ul;
};

class Esp32SdSpiStorage : public FsStorageBackend {
 public:
  Esp32SdSpiStorage(SPIClass& spi, Esp32SdSpiStorageConfig config = {});

  bool begin(Print& log) override;
  void printConfig(Print& out) const override;
  fs::FS& fs() override;
  const char* typeName() const override;

 private:
  SPIClass* spi_ = nullptr;
  Esp32SdSpiStorageConfig config_;
};

}  // namespace padre
