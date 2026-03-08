#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <vector>

#include <Adafruit_MPR121.h>

#include "../../patches/app/composites/FsLibraryFacade.h"
#include "../../patches/app/composites/Mpr121InputComposite.h"
#include "../../patches/app/serial/SerialRuntimeConsole.h"
#include "../../patches/app/composites/PlaybackEngine.h"
#include "../../patches/app/telemetry/PlaybackPerfTelemetry.h"
#include "../../patches/input/core/PlaybackInputActions.h"
#include "../../patches/input/mpr121/Mpr121AdafruitDriver.h"
#include "../../patches/audio/output/BufferedI2sOutput.h"
#include "../../patches/audio/output/Esp32StdI2sOutputIo.h"

namespace {

constexpr uint8_t SD_CS = 10;
constexpr uint8_t SD_SCK = 12;
constexpr uint8_t SD_MISO = 13;
constexpr uint8_t SD_MOSI = 11;

constexpr uint8_t I2S_BCLK = 14;
constexpr uint8_t I2S_LRC = 15;
constexpr uint8_t I2S_DOUT = 16;

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
constexpr bool kTouchDebug = true;
constexpr uint32_t kI2sWriteTimeoutMs = 0;

constexpr size_t kI2sWorkSamples = 2048;
constexpr size_t kSinkQueueSamples = 32768;
constexpr size_t kPrebufferMinSamples = 8192;
constexpr uint32_t kStartPrebufferBudgetUs = 30000;
constexpr uint32_t kServiceDecodeBudgetUs = 2500;
constexpr size_t kStartReadsPerStep = 8;
constexpr size_t kServiceReadsPerStep = 8;
constexpr bool kPerfTelemetryEnabled = false;
constexpr uint32_t kPerfReportMs = 2000;
constexpr uint32_t kPerfSlowLoopUs = 5000;
constexpr uint32_t kPerfSlowDecodeUs = 1200;
constexpr size_t kPerfQueueLowSamples = 4096;

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

padre::PlaybackPerfTelemetry g_perf(
    Serial,
    padre::PlaybackPerfTelemetryConfig{
        kPerfTelemetryEnabled,
        kPerfReportMs,
        kPerfSlowLoopUs,
        kPerfSlowDecodeUs,
        kPerfQueueLowSamples,
    });

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
        false,
        250,
        padre::Mpr121DiagnosticsOutputMode::Summary,
        true,
    });

padre::Mpr121InputComposite g_touch_input(
    g_touch_device.asTouchControllerIo(),
    padre::Mpr121TouchControllerConfig{
        4,
        padre::Mpr121InputConfig{},
        kTouchDebug,
        &Serial,
    });

void playbackSetPrebuffering(void* ctx, bool enabled) {
  auto* io = static_cast<padre::Esp32StdI2sOutputIo*>(ctx);
  if (io != nullptr) io->setPrebuffering(enabled);
}

void playbackOnTrackStarted(void*, const String& path) {
  Serial.printf("Now playing: %s\n", path.c_str());
}

void playbackTelemetryOnServiceBegin(void*, uint32_t) {
  g_perf.onServiceBegin(g_sink.queuedSamples());
}

void playbackTelemetryOnDecodeIteration(void*,
                                        uint32_t decode_elapsed_us,
                                        size_t produced_samples,
                                        size_t queued_samples,
                                        size_t) {
  g_perf.onDecodeIteration(decode_elapsed_us, produced_samples, queued_samples);
}

void playbackTelemetryOnServiceEnd(void*,
                                   uint32_t service_elapsed_us,
                                   uint32_t decode_iters,
                                   bool hit_budget) {
  g_perf.onServiceEnd(service_elapsed_us, decode_iters, hit_budget);
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
    },
    padre::PlaybackControllerTelemetryCallbacks{
        nullptr,
        playbackTelemetryOnServiceBegin,
        playbackTelemetryOnDecodeIteration,
        playbackTelemetryOnServiceEnd,
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
  g_perf.noteNextTouchRequest(millis());
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

padre::RuntimeCommandEntry g_runtime_commands[] = {
    {"mpr121",
     padre::Mpr121AdafruitDriver::handleRuntimeCommandEntry,
     &g_touch_device,
     "mpr121 [status|dump|scan|stream <on|off>|mode <summary|table|plot>|rate <ms>|thresholds <touch> <release>|auto <on|off>|help]"},
};

padre::SerialRuntimeConsole g_runtime_console(
    nullptr,
    0,
    Serial,
    g_runtime_commands,
    sizeof(g_runtime_commands) / sizeof(g_runtime_commands[0]));

bool initSd() {
  g_sd_spi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, g_sd_spi, 20000000)) {
    Serial.println("SD.begin failed");
    return false;
  }
  return true;
}

bool initMpr121() {
  g_touch_device.setDiagnosticsOutput(&Serial);
  g_touch_device.setIrqHook(&g_sink, onTouchIrqWake);
  g_touch_device.scanI2c(Serial);
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
  Serial.println("Serial runtime console: help/mpr121 ...");

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
  g_perf.reset(millis());

  initPlaylist();
  g_playback_engine.start(millis(), true);
}

void loop() {
  g_runtime_console.poll(Serial);

  const uint32_t loop_start_us = micros();
  const uint32_t now_ms = millis();
  const bool touch_irq = g_touch_device.consumeIrq();
  if (touch_irq || (now_ms - g_last_touch_poll_ms >= kTouchPollMs)) {
    g_touch_input.poll(now_ms);
    g_last_touch_poll_ms = now_ms;
    g_touch_device.serviceRuntime(now_ms);
  }

  const padre::PlaybackAutoAdvanceStep playback_step =
      g_playback_engine.step(now_ms);
  if (playback_step.handled_next_request) {
    g_perf.noteNextTouchHandled(now_ms);
    if (!playback_step.next_started) {
      Serial.println("No next track available");
    }
  }

  if (kPerfTelemetryEnabled) {
    const uint32_t loop_elapsed_us = static_cast<uint32_t>(micros() - loop_start_us);
    g_perf.onLoop(loop_elapsed_us, now_ms);
  }

  delay(1);
}
