#include "Esp32SdSpiStorage.h"

#include <cstdio>

namespace padre {

Esp32SdSpiStorage::Esp32SdSpiStorage(SPIClass& spi, Esp32SdSpiStorageConfig config)
    : spi_(&spi), config_(config) {}

bool Esp32SdSpiStorage::begin(Print& log) {
  if (spi_ == nullptr) {
    log.println("SD SPI storage: SPI bus is not configured");
    return false;
  }

  spi_->begin(config_.sck_pin, config_.miso_pin, config_.mosi_pin, config_.cs_pin);
  if (!SD.begin(config_.cs_pin, *spi_, config_.clock_hz)) {
    log.println("SD.begin failed");
    return false;
  }

  return true;
}

void Esp32SdSpiStorage::printConfig(Print& out) const {
  char buffer[128];
  snprintf(buffer,
           sizeof(buffer),
           "SD SPI: cs=%u sck=%u miso=%u mosi=%u\n",
           static_cast<unsigned>(config_.cs_pin),
           static_cast<unsigned>(config_.sck_pin),
           static_cast<unsigned>(config_.miso_pin),
           static_cast<unsigned>(config_.mosi_pin));
  out.print(buffer);
}

fs::FS& Esp32SdSpiStorage::fs() { return SD; }

const char* Esp32SdSpiStorage::typeName() const { return "sd"; }

}  // namespace padre
