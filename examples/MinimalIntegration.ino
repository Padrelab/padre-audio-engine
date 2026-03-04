#include "../patches/control/VolumeController.h"
#include "../patches/decoder/DecoderFacade.h"
#include "../patches/fade/FadeController.h"
#include "../patches/input/PressDetector.h"
#include "../patches/mixer/VoiceMixer.h"
#include "../patches/playlist/PlaylistManager.h"
#include "../patches/source/AudioSourceRouter.h"
#include "../patches/source/HttpAudioSource.h"
#include "../patches/source/SdAudioSource.h"

padre::VolumeController volume;
padre::PressDetector sensor0(650);
padre::PlaylistManager playlist;
padre::VoiceMixer mixer(2);
padre::CrossfadeController crossfade({0.0f, 1.0f, 0.8f, 0.0001f});

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

class SerialSink : public padre::IAudioSink {
 public:
  bool begin(const padre::DecoderConfig& config) override {
    Serial.printf("Sink begin: %lu Hz, %u bit, stereo=%s\n",
                  static_cast<unsigned long>(config.output_sample_rate),
                  config.output_bits,
                  config.stereo ? "yes" : "no");
    return true;
  }

  size_t write(const int16_t*, size_t sample_count) override {
    return sample_count;
  }

  void end() override { Serial.println("Sink end"); }
};

bool fakeSdOpen(const String&) { return true; }
size_t fakeSdRead(uint8_t*, size_t bytes) { return bytes; }
bool fakeSdSeek(size_t) { return true; }
size_t fakeSdPos() { return 0; }
size_t fakeSdSize() { return 1024; }
bool fakeSdEof() { return false; }
bool fakeSdIsOpen() { return true; }
void fakeSdClose() {}

bool fakeHttpOpen(const String&) { return true; }
size_t fakeHttpRead(uint8_t*, size_t bytes) { return bytes; }
bool fakeHttpSeek(size_t) { return false; }
size_t fakeHttpPos() { return 0; }
size_t fakeHttpSize() { return 0; }
bool fakeHttpEof() { return false; }
bool fakeHttpIsOpen() { return true; }
void fakeHttpClose() {}

size_t fakeExternalDecode(void*, const uint8_t*, size_t input_size, int16_t* out,
                          size_t out_capacity, bool* frame_done) {
  const size_t samples = min(input_size / 2, out_capacity);
  for (size_t i = 0; i < samples; ++i) out[i] = 0;
  if (frame_done) *frame_done = true;
  return samples;
}

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
SerialSink sink;

void setup() {
  Serial.begin(115200);

  decoder.attachMp3Decoder({nullptr, nullptr, fakeExternalDecode, nullptr});
  decoder.attachFlacDecoder({nullptr, nullptr, fakeExternalDecode, nullptr});

  volume.restore(12.0f);
  playlist.setOrder(padre::PlayOrder::Shuffle);
  playlist.setTracks({"sd:///music/a.mp3", "https://example.com/b.flac",
                      "sd:///music/c.wav"});

  mixer.attachSource(0, &voice_a);
  mixer.attachSource(1, &voice_b);
  mixer.setGlobalGain(0.5f);
  mixer.setVoiceGain(0, 1.0f);
  mixer.setVoiceGain(1, 0.0f);

  if (const String* track = playlist.current()) {
    Serial.printf("Current track: %s\n", track->c_str());

    if (padre::IAudioSource* source = source_router.resolve(*track)) {
      source->begin();
      source->open(*track);
      Serial.printf("Source type: %s\n", source->type());
      decoder.begin(*source, sink, *track);
    }
  }
}

void loop() {
  const float smooth_volume = volume.tick(10);
  (void)smooth_volume;

  const bool fake_sensor_state = false;
  const auto event = sensor0.update(fake_sensor_state, millis());

  if (event == padre::PressEvent::ShortPress) {
    volume.step(1.0f);
  } else if (event == padre::PressEvent::LongPress) {
    playlist.next();
    crossfade.start(1.2f);
  }

  const auto fade_state = crossfade.tick(10);
  mixer.setVoiceGain(0, fade_state.from_gain);
  mixer.setVoiceGain(1, fade_state.to_gain);

  decoder.process();

  int16_t mixed[64] = {0};
  const size_t mixed_samples = mixer.mix(mixed, 64);
  (void)mixed_samples;

  delay(10);
}
