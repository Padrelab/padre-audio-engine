#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>

#include <Adafruit_MPR121.h>

#include "../../patches/app/composites/FsLibraryFacade.h"
#include "../../patches/app/composites/Mpr121InputComposite.h"
#include "../../patches/app/composites/PlaybackEngine.h"
#include "../../patches/input/core/PlaybackInputActions.h"
#include "../../patches/input/mpr121/Mpr121AdafruitDriver.h"
#include "../../patches/audio/output/BufferedI2sOutput.h"
#include "../../patches/audio/output/Esp32StdI2sOutputIo.h"

namespace {

constexpr uint8_t SD_CS = 10;
constexpr uint8_t SD_SCK = 12;
constexpr uint8_t SD_MISO = 13;
constexpr uint8_t SD_MOSI = 11;

constexpr uint8_t I2S_BCLK = 41;
constexpr uint8_t I2S_LRC = 42;
constexpr uint8_t I2S_DOUT = 40;

constexpr uint8_t MPR121_SDA = 4;
constexpr uint8_t MPR121_SCL = 5;
constexpr uint8_t MPR121_IRQ = 6;
constexpr uint8_t MPR121_ADDR = 0x5A;

constexpr char kMusicDir[] = "/music";
constexpr uint8_t kMaxDirDepth = 5;

constexpr uint8_t kVolumeMin = 0;
constexpr uint8_t kVolumeMax = 21;
constexpr uint8_t kVolumeDefault = 12;

constexpr uint16_t kTouchThreshold = 12;
constexpr uint16_t kReleaseThreshold = 6;
constexpr uint32_t kTouchPollMs = 10;
constexpr uint32_t kRetryStartDelayMs = 500;
constexpr uint32_t kI2sWriteTimeoutMs = 0;

constexpr size_t kI2sWorkSamples = 2048;
constexpr size_t kSinkQueueSamples = 32768;
constexpr size_t kPrebufferMinSamples = 8192;
constexpr uint32_t kStartPrebufferBudgetUs = 30000;
constexpr uint32_t kServiceDecodeBudgetUs = 2500;
constexpr size_t kStartReadsPerStep = 8;
constexpr size_t kServiceReadsPerStep = 8;

SPIClass g_sd_spi(FSPI);
Adafruit_MPR121 g_mpr121;
padre::FsLibraryFacade g_library(
    SD,
    padre::FsLibraryFacadeConfig{
        padre::FsAudioSourceConfig{"sd"},
        padre::AudioFileScannerOptions{kMaxDirDepth},
    });

std::vector<String> g_tracks;

int g_volume = kVolumeDefault;
int32_t g_volume_gain_q15 = 0;
uint32_t g_last_touch_poll_ms = 0;

padre::DecoderFacade g_decoder;

int16_t applyVolumeToSample(int16_t sample) {
  const int32_t scaled = (static_cast<int32_t>(sample) * g_volume_gain_q15) >> 15;
  if (scaled > 32767) return 32767;
  if (scaled < -32768) return -32768;
  return static_cast<int16_t>(scaled);
}

void updateVolumeGain() {
  const int32_t vol = static_cast<int32_t>(g_volume);
  const int32_t maxv = static_cast<int32_t>(kVolumeMax);
  const int32_t denom = maxv * maxv;
  const int32_t gain_num = vol * vol;
  g_volume_gain_q15 = (gain_num * 32767 + (denom / 2)) / denom;
}

int16_t i2sApplyVolumeSample(void*, int16_t sample) {
  return applyVolumeToSample(sample);
}

padre::Esp32StdI2sOutputIo g_i2s_io(
    padre::Esp32StdI2sPins{I2S_BCLK, I2S_LRC, I2S_DOUT, -1},
    padre::Esp32StdI2sOutputConfig{
        8,
        256,
        2,
        kI2sWriteTimeoutMs,
        kI2sWorkSamples,
    },
    padre::Esp32StdI2sSampleTransform{
        nullptr,
        i2sApplyVolumeSample,
    });

padre::BufferedI2sOutput g_sink(g_i2s_io.asIo(), padre::I2sOutputConfig{kSinkQueueSamples});

void onTouchIrqWake(void* ctx) {
  auto* sink = static_cast<padre::BufferedI2sOutput*>(ctx);
  if (sink != nullptr) sink->requestPumpFromIsr();
}

padre::Mpr121AdafruitDriver g_touch_device(
    g_mpr121,
    Wire,
    padre::Mpr121AdafruitDriverPins{
        static_cast<int8_t>(MPR121_SDA),
        static_cast<int8_t>(MPR121_SCL),
        static_cast<int8_t>(MPR121_IRQ),
        MPR121_ADDR,
        400000,
    },
    padre::Mpr121AdafruitDriverConfig{
        static_cast<uint8_t>(kTouchThreshold),
        static_cast<uint8_t>(kReleaseThreshold),
        true,
    });

padre::Mpr121InputComposite g_touch_input(
    g_touch_device.asTouchControllerIo(),
    padre::Mpr121TouchControllerConfig{
        4,
        padre::Mpr121InputConfig{},
    });

void playbackSetPrebuffering(void* ctx, bool enabled) {
  auto* io = static_cast<padre::Esp32StdI2sOutputIo*>(ctx);
  if (io != nullptr) io->setPrebuffering(enabled);
}

void playbackOnTrackStarted(void*, const String& path) {
  Serial.printf("Now playing: %s\n", path.c_str());
}

padre::PlaybackEngine g_playback_engine(
    g_decoder,
    g_library.source(),
    g_sink,
    padre::PlaybackEngineConfig{
        padre::PlaybackControllerConfig{
            kPrebufferMinSamples,
            kStartPrebufferBudgetUs,
            kStartReadsPerStep,
            kServiceDecodeBudgetUs,
            kServiceReadsPerStep,
        },
        padre::PlaybackAutoAdvanceConfig{
            kRetryStartDelayMs,
        },
        padre::PlayOrder::Shuffle,
        0,
    },
    padre::PlaybackControllerHooks{
        &g_i2s_io,
        playbackSetPrebuffering,
        playbackOnTrackStarted,
    });

void applyVolume() {
  updateVolumeGain();
  Serial.printf("Volume: %d\n", g_volume);
}

bool actionIsPlaybackRunning(void*) { return g_playback_engine.isRunning(); }

bool actionTogglePaused(void*) { return g_playback_engine.togglePaused(); }

void actionOnPauseChanged(void*, bool paused) {
  Serial.println(paused ? "Paused" : "Resumed");
}

void actionRequestNextTrack(void*) {
  g_playback_engine.requestNextTrack();
}

bool actionStepVolume(void*, int delta, int* out_new_value) {
  const int next_volume = max<int>(kVolumeMin, min<int>(kVolumeMax, g_volume + delta));
  if (next_volume == g_volume) return false;

  g_volume = next_volume;
  if (out_new_value != nullptr) *out_new_value = g_volume;
  return true;
}

void actionOnVolumeChanged(void*, int) { applyVolume(); }

padre::PlaybackInputActions g_touch_actions(
    padre::PlaybackInputBindings{
        padre::InputEventType::PressDown,
        0,
        1,
        2,
        3,
    },
    padre::PlaybackInputActionsCallbacks{
        nullptr,
        actionIsPlaybackRunning,
        actionTogglePaused,
        actionOnPauseChanged,
        actionRequestNextTrack,
        actionStepVolume,
        actionOnVolumeChanged,
    });

bool initSd() {
  g_sd_spi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, g_sd_spi, 20000000)) {
    Serial.println("SD.begin failed");
    return false;
  }
  return true;
}

bool initMpr121() {
  g_touch_device.setIrqHook(&g_sink, onTouchIrqWake);
  if (!g_touch_device.begin()) {
    Serial.println("MPR121 init failed");
    return false;
  }
  return true;
}

void initPlaylist() {
  g_tracks.clear();
  g_library.scan(kMusicDir, g_tracks);

  Serial.printf("Found %u audio file(s)\n", static_cast<unsigned>(g_tracks.size()));
  for (const auto& track : g_tracks) {
    Serial.printf(" - %s\n", track.c_str());
  }

  g_playback_engine.setTracks(g_tracks, padre::PlayOrder::Shuffle);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("test-sd-mpr121");

  randomSeed(static_cast<uint32_t>(micros()));

  if (!initSd()) return;
  if (!initMpr121()) return;
  if (!g_touch_input.begin()) {
    Serial.println("Touch controller init failed");
    return;
  }
  g_touch_input.bindActions(&g_touch_actions);

  g_volume = kVolumeDefault;
  applyVolume();

  initPlaylist();
  g_playback_engine.start(millis(), true);
}

void loop() {
  const uint32_t now_ms = millis();
  const bool touch_irq = g_touch_device.consumeIrq();
  if (touch_irq || (now_ms - g_last_touch_poll_ms >= kTouchPollMs)) {
    g_touch_input.poll(now_ms);
    g_last_touch_poll_ms = now_ms;
  }

  const padre::PlaybackAutoAdvanceStep playback_step = g_playback_engine.step(now_ms);
  if (playback_step.handled_next_request && !playback_step.next_started) {
    Serial.println("No next track available");
  }

  delay(1);
}
