#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Audio.h>

#include "patches/Mpr121TapInput.h"
#include "patches/RandomPlaylist.h"

using test_sd_mpr121::Mpr121TapInput;
using test_sd_mpr121::RandomPlaylist;

// SD pins
static constexpr int SD_CS = 10;
static constexpr int SD_SCK = 12;
static constexpr int SD_MISO = 13;
static constexpr int SD_MOSI = 11;

// I2S pins
static constexpr int I2S_BCLK = 14;
static constexpr int I2S_LRC = 15;
static constexpr int I2S_DOUT = 16;

// MPR121 pins
static constexpr int MPR121_SDA = 35;
static constexpr int MPR121_SCL = 36;
static constexpr int MPR121_IRQ = 37;

// Touch settings (configurable)
static constexpr uint8_t TOUCH_THRESHOLD = 12;
static constexpr uint8_t RELEASE_THRESHOLD = 6;

static constexpr uint8_t VOLUME_MIN = 0;
static constexpr uint8_t VOLUME_MAX = 21;
static constexpr uint8_t VOLUME_STEP = 1;
static constexpr uint8_t VOLUME_DEFAULT = 12;

SPIClass sd_spi(FSPI);
TwoWire touch_wire(0);
Audio audio;
RandomPlaylist playlist;
Mpr121TapInput touch;

bool paused = false;
uint8_t volume_level = VOLUME_DEFAULT;

bool playTrack(const String& path) {
  Serial.printf("Play: %s\n", path.c_str());
  paused = false;
  return audio.connecttoFS(SD, path.c_str());
}

void playNextTrack() {
  if (playlist.empty()) {
    Serial.println("Playlist is empty");
    return;
  }

  const String& next_track = playlist.next();
  if (!playTrack(next_track)) {
    Serial.printf("Failed to play: %s\n", next_track.c_str());
  }
}

void handleTap(uint8_t sensor_id) {
  switch (sensor_id) {
    case 0:
      audio.pauseResume();
      paused = !paused;
      Serial.printf("Pause/resume -> %s\n", paused ? "pause" : "play");
      break;
    case 1:
      playNextTrack();
      break;
    case 2:
      if (volume_level >= VOLUME_STEP) {
        volume_level = max<uint8_t>(VOLUME_MIN, volume_level - VOLUME_STEP);
        audio.setVolume(volume_level);
      }
      Serial.printf("Volume: %u\n", volume_level);
      break;
    case 3:
      volume_level = min<uint8_t>(VOLUME_MAX, volume_level + VOLUME_STEP);
      audio.setVolume(volume_level);
      Serial.printf("Volume: %u\n", volume_level);
      break;
    default:
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  randomSeed(esp_random());

  sd_spi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, sd_spi)) {
    Serial.println("SD init failed");
    while (true) delay(1000);
  }

  const Mpr121TapInput::Config touch_config(TOUCH_THRESHOLD, RELEASE_THRESHOLD);
  if (!touch.begin(touch_wire,
                   MPR121_SDA,
                   MPR121_SCL,
                   MPR121_IRQ,
                   touch_config)) {
    Serial.println("MPR121 init failed");
    while (true) delay(1000);
  }

  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(volume_level);

  if (!playlist.loadFromDirectory(SD, "/music")) {
    Serial.println("No supported files in /music");
    while (true) delay(1000);
  }

  Serial.printf("Tracks found: %u\n", static_cast<unsigned>(playlist.size()));

  if (!playTrack(playlist.current())) {
    Serial.println("Initial track start failed");
    while (true) delay(1000);
  }
}

void loop() {
  audio.loop();

  if (!paused && !audio.isRunning()) {
    playNextTrack();
  }

  const uint16_t taps = touch.update();
  if (taps != 0) {
    for (uint8_t sensor = 0; sensor < 12; ++sensor) {
      if (taps & (1u << sensor)) {
        handleTap(sensor);
      }
    }
  }

  delay(2);
}
