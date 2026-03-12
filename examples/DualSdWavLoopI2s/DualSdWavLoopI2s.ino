#include <Arduino.h>
#include <SPI.h>

#include "../DualWavLoopSketchApp.h"
#include "../../patches/media/source/Esp32SdSpiStorage.h"

namespace {

SPIClass g_storage_spi(FSPI);

padre::Esp32SdSpiStorageConfig makeStorageConfig() {
  padre::Esp32SdSpiStorageConfig config;
  config.cs_pin = 10;
  config.sck_pin = 12;
  config.miso_pin = 13;
  config.mosi_pin = 11;
  config.clock_hz = 20000000ul;
  return config;
}

padre::DualWavLoopI2sAppConfig makeAppConfig() {
  padre::DualWavLoopI2sAppConfig config;
  config.example_name = "DualSdWavLoopI2s";
  config.build_tag = "dual-sd-wav-i2s-fastnext-d16f256-r3";
  return config;
}

padre::Esp32SdSpiStorage g_storage(g_storage_spi, makeStorageConfig());
padre::DualWavLoopI2sApp g_app(Serial, Wire, g_storage, makeAppConfig());

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  g_app.begin();
}

void loop() {
  g_app.loop();
}
