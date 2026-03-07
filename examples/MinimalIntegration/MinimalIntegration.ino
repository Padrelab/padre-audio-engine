#include "../../patches/audio/control/VolumeController.h"
#include "../../patches/audio/decoder/DecoderFacade.h"
#include "../../patches/audio/fade/FadeController.h"
#include "../../patches/input/buttons/ButtonInput.h"
#include "../../patches/input/mpr121/Mpr121Input.h"
#include "../../patches/input/pots/PotInput.h"
#include "../../patches/input/core/PressDetector.h"
#include "../../patches/audio/mixer/VoiceMixer.h"
#include "../../patches/app/persistence/PreferencesStore.h"
#include "../../patches/app/persistence/RuntimeSettingsPersistence.h"
#include "../../patches/media/playlist/PlaylistManager.h"
#include "../../patches/app/serial/SerialRuntimeConsole.h"
#include "../../patches/app/telemetry/AudioPipelineTelemetry.h"
#include "../../patches/media/source/AudioSourceRouter.h"
#include "../../patches/media/source/HttpAudioSource.h"
#include "../../patches/media/source/SdAudioSource.h"
#include "../../patches/audio/output/Esp32I2sOutputIo.h"
#include "../../patches/audio/output/BufferedI2sOutput.h"

padre::VolumeController volume;
padre::PressDetector sensor0(650);
padre::PlaylistManager playlist;
padre::VoiceMixer mixer(2);
padre::CrossfadeController crossfade({0.0f, 1.0f, 0.8f, 0.0001f});
padre::ButtonInputIo button_io{nullptr, [](void*, uint8_t) { return true; }};
padre::Mpr121InputIo touch_io{nullptr, [](void*) { return static_cast<uint16_t>(0); }};
padre::PotInputIo pot_io{nullptr, [](void*, uint8_t) { return 2048; }};

padre::ButtonInput button0(0, button_io);
padre::Mpr121Input touch0(0, touch_io);
padre::PotInput pot0(0, pot_io);

padre::PreferencesStore prefs_store;
padre::RuntimeSettingsPersistence persistence(prefs_store);

float runtime_crossfade_sec = 1.2f;
float runtime_global_gain = 0.5f;

padre::RuntimeConfigEntry runtime_entries[] = {
    {"crossfade_sec", &runtime_crossfade_sec, 0.1f, 10.0f},
    {"global_gain", &runtime_global_gain, 0.0f, 1.0f},
};

padre::PipelineDiagnosticsConfig telemetry_cfg = {2000, 0.20f, 0.08f, 70.0f, 90.0f};
padre::AudioPipelineDiagnostics telemetry(Serial, telemetry_cfg);

padre::PersistedFloatParam persisted_params[] = {
    {"crossfade_sec", &runtime_crossfade_sec, 0.1f, 10.0f},
    {"global_gain", &runtime_global_gain, 0.0f, 1.0f},
};

class ConstantVoice : public padre::IMixerVoiceSource {
 public:
  explicit ConstantVoice(int16_t sample) : sample_(sample) {}

  size_t readSamples(int16_t* dst, size_t sample_count) override {
    for (size_t i = 0; i < sample_count; ++i) dst[i] = sample_;
    return sample_count;
  }

  bool eof() const override { return false; }

 private:
  int16_t sample_ = 0;
};

ConstantVoice voice_a(900);
ConstantVoice voice_b(-700);

#if defined(ARDUINO_ARCH_ESP32)
constexpr int8_t I2S_BCLK = 14;
constexpr int8_t I2S_LRC = 15;
constexpr int8_t I2S_DOUT = 16;
constexpr size_t kSinkQueueSamples = 32768;
constexpr size_t kSinkDmaWatermarkSamples = 1024;
#endif

String nextRuntimeToken(const String& line, size_t& pos) {
  while (pos < static_cast<size_t>(line.length()) &&
         isspace(static_cast<unsigned char>(line[pos]))) {
    ++pos;
  }

  const size_t start = pos;
  while (pos < static_cast<size_t>(line.length()) &&
         !isspace(static_cast<unsigned char>(line[pos]))) {
    ++pos;
  }
  if (start >= static_cast<size_t>(line.length())) return String();
  return line.substring(start, pos);
}

#if defined(ARDUINO_ARCH_ESP32)
struct I2sProfilePreset {
  const char* name = nullptr;
  padre::Esp32I2sDmaProfile profile = {};
};

constexpr I2sProfilePreset kI2sProfilePresets[] = {
    {"loop", padre::Esp32I2sDmaProfile{12, 512, 0, 3072}},
    {"balanced", padre::Esp32I2sDmaProfile{8, 256, 0, 1024}},
    {"oneshot", padre::Esp32I2sDmaProfile{4, 128, 0, 256}},
};

class I2sRuntimeCommandContext {
 public:
  padre::Esp32I2sOutputIo* io = nullptr;
  padre::BufferedI2sOutput* sink = nullptr;
  const char* active_profile = "balanced";
};
#endif

#if !defined(ARDUINO_ARCH_ESP32)
bool fakeI2sBegin(void*, uint32_t sample_rate, uint8_t bits, bool stereo) {
  Serial.printf("I2S begin: %lu Hz, %u bit, stereo=%s\n",
                static_cast<unsigned long>(sample_rate),
                bits,
                stereo ? "yes" : "no");
  return true;
}

size_t fakeI2sAvailable(void*) { return 512; }
size_t fakeI2sWrite(void*, const int16_t*, size_t sample_count) { return sample_count; }
void fakeI2sEnd(void*) { Serial.println("I2S end"); }
#endif

bool fakeSdOpen(const String&) { return false; }
size_t fakeSdRead(uint8_t*, size_t bytes) { return bytes; }
bool fakeSdSeek(size_t) { return true; }
size_t fakeSdPos() { return 0; }
size_t fakeSdSize() { return 1024; }
bool fakeSdEof() { return false; }
bool fakeSdIsOpen() { return true; }
void fakeSdClose() {}

bool fakeHttpOpen(const String&) { return false; }
size_t fakeHttpRead(uint8_t*, size_t bytes) { return bytes; }
bool fakeHttpSeek(size_t) { return false; }
size_t fakeHttpPos() { return 0; }
size_t fakeHttpSize() { return 0; }
bool fakeHttpEof() { return false; }
bool fakeHttpIsOpen() { return true; }
void fakeHttpClose() {}

padre::SdAudioSource sd_source({
    nullptr, fakeSdOpen, fakeSdRead, fakeSdSeek, fakeSdPos,
    fakeSdSize, fakeSdEof, fakeSdIsOpen, fakeSdClose,
});

padre::HttpAudioSource http_source({
    nullptr, fakeHttpOpen, fakeHttpRead, fakeHttpSeek, fakeHttpPos,
    fakeHttpSize, fakeHttpEof, fakeHttpIsOpen, fakeHttpClose,
});

padre::AudioSourceRouter::Entry source_entries[] = {
    {"sd", &sd_source},
    {"http", &http_source},
    {"https", &http_source},
};

padre::AudioSourceRouter source_router(source_entries,
                                       sizeof(source_entries) /
                                           sizeof(source_entries[0]));

padre::DecoderFacade decoder;
#if defined(ARDUINO_ARCH_ESP32)
padre::Esp32I2sOutputIo esp32_i2s(
    padre::Esp32I2sPins{I2S_BCLK, I2S_LRC, I2S_DOUT, -1},
    padre::Esp32I2sDriverConfig{
        I2S_NUM_0,
        8,
        256,
        0,
        false,
        true,
        true,
        kSinkDmaWatermarkSamples,
    });

padre::BufferedI2sOutput sink(
    esp32_i2s.asIo(),
    padre::I2sOutputConfig{
        kSinkQueueSamples,
        esp32_i2s.dmaWatermarkSamples(),
    });
#else
padre::BufferedI2sOutput sink({
    nullptr,
    fakeI2sBegin,
    fakeI2sAvailable,
    fakeI2sWrite,
    fakeI2sEnd,
});
#endif

#if defined(ARDUINO_ARCH_ESP32)
I2sRuntimeCommandContext i2s_runtime_ctx{&esp32_i2s, &sink, "balanced"};

void printI2sProfileStatus(void* user_ctx, Print& out) {
  auto* ctx = static_cast<I2sRuntimeCommandContext*>(user_ctx);
  if (ctx == nullptr || ctx->io == nullptr || ctx->sink == nullptr) {
    out.println("i2s: runtime context is not initialized");
    return;
  }

  out.printf("i2s profile=%s running=%s dma=%u x %u watermark=%lu timeout=%lums queue=%lu/%lu\n",
             ctx->active_profile ? ctx->active_profile : "custom",
             ctx->io->isRunning() ? "yes" : "no",
             static_cast<unsigned>(ctx->io->dmaBufferCount()),
             static_cast<unsigned>(ctx->io->dmaBufferSamples()),
             static_cast<unsigned long>(ctx->io->dmaWatermarkSamples()),
             static_cast<unsigned long>(ctx->io->writeTimeoutMs()),
             static_cast<unsigned long>(ctx->sink->queuedSamples()),
             static_cast<unsigned long>(ctx->sink->queueCapacity()));
}

void printI2sProfileList(Print& out) {
  out.println("i2s profiles:");
  for (size_t i = 0; i < sizeof(kI2sProfilePresets) / sizeof(kI2sProfilePresets[0]); ++i) {
    const I2sProfilePreset& preset = kI2sProfilePresets[i];
    out.printf("  %s: dma=%u x %u watermark=%lu timeout=%lums\n",
               preset.name,
               static_cast<unsigned>(preset.profile.dma_buffer_count),
               static_cast<unsigned>(preset.profile.dma_buffer_samples),
               static_cast<unsigned long>(preset.profile.dma_watermark_samples),
               static_cast<unsigned long>(preset.profile.write_timeout_ms));
  }
}

bool handleI2sRuntimeCommand(void* user_ctx, const String& line, Print& out) {
  auto* ctx = static_cast<I2sRuntimeCommandContext*>(user_ctx);
  if (ctx == nullptr || ctx->io == nullptr || ctx->sink == nullptr) {
    out.println("i2s: runtime context is not initialized");
    return false;
  }

  size_t pos = 0;
  const String command = nextRuntimeToken(line, pos);
  if (!command.equalsIgnoreCase("i2s")) return false;

  const String action = nextRuntimeToken(line, pos);
  if (action.length() == 0 || action.equalsIgnoreCase("status")) {
    printI2sProfileStatus(ctx, out);
    return true;
  }

  if (action.equalsIgnoreCase("list")) {
    printI2sProfileList(out);
    return true;
  }

  if (action.equalsIgnoreCase("profile")) {
    const String profile_name = nextRuntimeToken(line, pos);
    if (profile_name.length() == 0) {
      printI2sProfileStatus(ctx, out);
      printI2sProfileList(out);
      return true;
    }

    const I2sProfilePreset* preset = nullptr;
    for (size_t i = 0; i < sizeof(kI2sProfilePresets) / sizeof(kI2sProfilePresets[0]); ++i) {
      if (profile_name.equalsIgnoreCase(kI2sProfilePresets[i].name)) {
        preset = &kI2sProfilePresets[i];
        break;
      }
    }
    if (preset == nullptr) {
      out.printf("i2s: unknown profile '%s'\n", profile_name.c_str());
      printI2sProfileList(out);
      return false;
    }

    if (!ctx->io->applyDmaProfile(preset->profile)) {
      out.printf("i2s: failed to apply profile '%s'\n", preset->name);
      return false;
    }

    ctx->sink->setDmaWatermarkSamples(ctx->io->dmaWatermarkSamples());
    ctx->active_profile = preset->name;
    printI2sProfileStatus(ctx, out);
    return true;
  }

  out.println("i2s: usage i2s [status|list|profile <loop|balanced|oneshot>]");
  return false;
}
#else
bool handleI2sRuntimeCommand(void*, const String&, Print& out) {
  out.println("i2s: unsupported on this target");
  return false;
}
#endif

padre::RuntimeCommandEntry runtime_commands[] = {
    {"i2s", handleI2sRuntimeCommand, 
#if defined(ARDUINO_ARCH_ESP32)
     &i2s_runtime_ctx,
#else
     nullptr,
#endif
     "i2s [status|list|profile <loop|balanced|oneshot>]"},
};

padre::SerialRuntimeConsole runtime_console(
    runtime_entries,
    sizeof(runtime_entries) / sizeof(runtime_entries[0]),
    Serial,
    runtime_commands,
    sizeof(runtime_commands) / sizeof(runtime_commands[0]));

void setup() {
  Serial.begin(115200);
  Serial.println("Serial runtime console: help/list/set/get/debug/i2s");

  if (persistence.begin("audio", false)) {
    persistence.loadVolume(volume, "volume");
    persistence.loadParams(persisted_params,
                           sizeof(persisted_params) / sizeof(persisted_params[0]));
    persistence.end();
  } else {
    volume.restore(12.0f);
  }
  playlist.setOrder(padre::PlayOrder::Shuffle);
  playlist.setTracks({"sd:///music/a.mp3", "https://example.com/b.flac",
                      "sd:///music/c.wav"});

  mixer.attachSource(0, &voice_a);
  mixer.attachSource(1, &voice_b);
  mixer.setGlobalGain(runtime_global_gain);
  mixer.setVoiceGain(0, 1.0f);
  mixer.setVoiceGain(1, 0.0f);

  if (const String* track = playlist.current()) {
    Serial.printf("Current track: %s\n", track->c_str());

    if (padre::IAudioSource* source = source_router.resolve(*track)) {
      if (source->begin() && source->open(*track)) {
        Serial.printf("Source type: %s\n", source->type());
        if (!decoder.begin(*source, sink, *track)) {
          Serial.println("Decoder begin failed");
        }
      } else {
        Serial.println("Source stub is not openable in MinimalIntegration");
      }
    }
  }
}

void loop() {
  static uint32_t last_save_ms = 0;
  static float last_saved_volume = -1.0f;
  static float last_saved_crossfade = -1.0f;
  static float last_saved_global_gain = -1.0f;
  runtime_console.poll(Serial);

  const float smooth_volume = volume.tick(10);
  (void)smooth_volume;

  const bool fake_sensor_state = false;
  const auto event = sensor0.update(fake_sensor_state, millis());

  if (event == padre::PressEvent::ShortPress) {
    volume.step(1.0f);
  } else if (event == padre::PressEvent::LongPress) {
    playlist.next();
    crossfade.start(runtime_crossfade_sec);
  }

  const auto button_event = button0.update(millis());
  const auto touch_event = touch0.update(millis());
  const auto pot_event = pot0.update(millis());

  if (button_event.type == padre::InputEventType::ShortPress ||
      touch_event.type == padre::InputEventType::ShortPress) {
    volume.step(1.0f);
  }
  if (pot_event.type == padre::InputEventType::ValueChanged) {
    runtime_global_gain = pot_event.value;
  }

  telemetry.beginCycle(micros());

  const auto fade_state = crossfade.tick(10);
  mixer.setVoiceGain(0, fade_state.from_gain);
  mixer.setVoiceGain(1, fade_state.to_gain);
  mixer.setGlobalGain(runtime_global_gain);

  decoder.process();
  sink.pump();

  if (runtime_console.debugEnabled()) {
    Serial.printf("dbg vol=%.2f fade=%.2f->%.2f\n", smooth_volume, fade_state.from_gain,
                  fade_state.to_gain);
  }

  int16_t mixed[64] = {0};
  const size_t mixed_samples = mixer.mix(mixed, 64);

  telemetry.updateBuffer(mixed_samples, 64);
  if (mixed_samples == 0) telemetry.noteUnderrun();
  if (mixed_samples >= 64) telemetry.noteOverrun();

  telemetry.endCycle(micros(), 10000);
  telemetry.reportIfDue(millis());

  const bool changed =
      fabsf(last_saved_volume - volume.target()) > 0.01f ||
      fabsf(last_saved_crossfade - runtime_crossfade_sec) > 0.01f ||
      fabsf(last_saved_global_gain - runtime_global_gain) > 0.01f;

  if (changed && millis() - last_save_ms >= 2000) {
    if (persistence.begin("audio", false)) {
      persistence.saveVolume(volume, "volume");
      persistence.saveParams(persisted_params,
                             sizeof(persisted_params) / sizeof(persisted_params[0]));
      persistence.end();

      last_saved_volume = volume.target();
      last_saved_crossfade = runtime_crossfade_sec;
      last_saved_global_gain = runtime_global_gain;
      last_save_ms = millis();
    }
  }

  delay(10);
}
