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

void setup() {
  Serial.begin(115200);

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

  const auto fade_state = crossfade.tick(10);
  mixer.setVoiceGain(0, fade_state.from_gain);
  mixer.setVoiceGain(1, fade_state.to_gain);
  mixer.setGlobalGain(runtime_global_gain);

  decoder.process();
  sink.pump();

  int16_t mixed[64] = {0};
  const size_t mixed_samples = mixer.mix(mixed, 64);
  (void)mixed_samples;

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
