#include "../../patches/control/VolumeController.h"
#include "../../patches/decoder/DecoderFacade.h"
#include "../../patches/fade/FadeController.h"
#include "../../patches/io_buttons/ButtonInput.h"
#include "../../patches/io_mpr121/Mpr121Input.h"
#include "../../patches/io_pots/PotInput.h"
#include "../../patches/input/PressDetector.h"
#include "../../patches/mixer/VoiceMixer.h"
#include "../../patches/persistence/PreferencesStore.h"
#include "../../patches/persistence/RuntimeSettingsPersistence.h"
#include "../../patches/playlist/PlaylistManager.h"
#include "../../patches/serial/SerialRuntimeConsole.h"
#include "../../patches/telemetry/AudioPipelineTelemetry.h"
#include "../../patches/source/AudioSourceRouter.h"
#include "../../patches/source/HttpAudioSource.h"
#include "../../patches/source/SdAudioSource.h"
#include "../../patches/output/I2sPcm5122Output.h"

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

padre::SerialRuntimeConsole runtime_console(
    runtime_entries, sizeof(runtime_entries) / sizeof(runtime_entries[0]), Serial);

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
padre::I2sPcm5122Output sink({
    nullptr,
    fakeI2sBegin,
    fakeI2sAvailable,
    fakeI2sWrite,
    fakeI2sEnd,
});

void setup() {
  Serial.begin(115200);
  Serial.println("Serial runtime console: help/list/set/get/debug");

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
  if (Serial.available()) {
    const String line = Serial.readStringUntil('\n');
    runtime_console.handleLine(line);
  }

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
