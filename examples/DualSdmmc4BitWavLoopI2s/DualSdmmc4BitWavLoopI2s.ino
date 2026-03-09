#include <Arduino.h>

#include "../../patches/app/composites/DualWavLoopI2sApp.h"
#include "../../patches/media/source/Esp32SdMmcStorage.h"

namespace {

padre::Esp32SdMmcStorageConfig makeStorageConfig() {
  padre::Esp32SdMmcStorageConfig config;
  config.clk_pin = 12;
  config.cmd_pin = 11;
  config.d0_pin = 13;
  config.d1_pin = 14;
  config.d2_pin = 9;
  config.d3_pin = 10;
  config.frequency_hz = 20000000ul;
  config.mode_1bit = false;
  return config;
}

padre::DualWavLoopI2sAppConfig makeAppConfig() {
  padre::DualWavLoopI2sAppConfig config;
  config.example_name = "DualSdmmc4BitWavLoopI2s";
  config.build_tag = "dual-sdmmc4-wav-i2s-r2";
  return config;
}

padre::Esp32SdMmcStorage g_storage(makeStorageConfig());
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
