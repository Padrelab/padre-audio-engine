#include "../patches/audio_decoder/DecoderFacade.h"
#include "../patches/control/VolumeController.h"
#include "../patches/input/PressDetector.h"
#include "../patches/playlist/PlaylistManager.h"
#include "../patches/source/AudioSourceRouter.h"
#include "../patches/source/HttpAudioSource.h"
#include "../patches/source/SdAudioSource.h"

padre::VolumeController volume;
padre::PressDetector sensor0(650);
padre::PlaylistManager playlist;

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

void setup() {
  Serial.begin(115200);

  volume.restore(12.0f);
  playlist.setOrder(padre::PlayOrder::Shuffle);
  playlist.setTracks({"sd:///music/a.mp3", "https://example.com/b.flac",
                      "sd:///music/c.wav"});

  if (const String* track = playlist.current()) {
    Serial.printf("Current track: %s\n", track->c_str());

    if (padre::IAudioSource* source = source_router.resolve(*track)) {
      source->begin();
      source->open(*track);
      Serial.printf("Source type: %s\n", source->type());
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
  }

  delay(10);
}
