#include "Esp32SdMmcStorage.h"

#include <cstdio>

namespace padre {

Esp32SdMmcStorage::Esp32SdMmcStorage(Esp32SdMmcStorageConfig config) : config_(config) {}

bool Esp32SdMmcStorage::begin(Print& log) {
#if defined(SOC_SDMMC_USE_GPIO_MATRIX) && !defined(BOARD_HAS_SDMMC)
  const bool pins_ok = config_.mode_1bit
                           ? SD_MMC.setPins(config_.clk_pin, config_.cmd_pin, config_.d0_pin)
                           : SD_MMC.setPins(config_.clk_pin,
                                            config_.cmd_pin,
                                            config_.d0_pin,
                                            config_.d1_pin,
                                            config_.d2_pin,
                                            config_.d3_pin);
  if (!pins_ok) {
    log.println(config_.mode_1bit ? "SD_MMC.setPins(1-bit) failed"
                                  : "SD_MMC.setPins(4-bit) failed");
    return false;
  }
#endif

  if (!SD_MMC.begin(config_.mount_point,
                    config_.mode_1bit,
                    config_.format_if_mount_failed,
                    static_cast<int>(config_.frequency_hz))) {
    log.println("SD_MMC.begin failed");
    log.println("Check SDMMC wiring and 10k pull-ups on CMD/D0/D1/D2/D3");
    return false;
  }

  const uint8_t card_type = SD_MMC.cardType();
  if (card_type == CARD_NONE) {
    log.println("No SD_MMC card attached");
    return false;
  }

  char buffer[160];
  snprintf(buffer,
           sizeof(buffer),
           "SD_MMC card: %s, size=%lluMB used=%lluMB total=%lluMB\n",
           cardTypeName(card_type),
           SD_MMC.cardSize() / (1024ull * 1024ull),
           SD_MMC.usedBytes() / (1024ull * 1024ull),
           SD_MMC.totalBytes() / (1024ull * 1024ull));
  log.print(buffer);
  return true;
}

void Esp32SdMmcStorage::printConfig(Print& out) const {
  char buffer[160];
  snprintf(buffer,
           sizeof(buffer),
           "SDMMC: clk=%d cmd=%d d0=%d d1=%d d2=%d d3=%d mode=%s freq=%lu\n",
           config_.clk_pin,
           config_.cmd_pin,
           config_.d0_pin,
           config_.d1_pin,
           config_.d2_pin,
           config_.d3_pin,
           config_.mode_1bit ? "1-bit" : "4-bit",
           static_cast<unsigned long>(config_.frequency_hz));
  out.print(buffer);
}

fs::FS& Esp32SdMmcStorage::fs() { return SD_MMC; }

const char* Esp32SdMmcStorage::typeName() const { return "sdmmc"; }

const char* Esp32SdMmcStorage::cardTypeName(uint8_t card_type) {
  switch (card_type) {
    case CARD_MMC:
      return "MMC";
    case CARD_SD:
      return "SDSC";
    case CARD_SDHC:
      return "SDHC";
    default:
      return "UNKNOWN";
  }
}

}  // namespace padre
