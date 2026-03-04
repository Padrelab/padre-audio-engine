#include "../patches/audio_decoder/DecoderFacade.h"
#include "../patches/control/VolumeController.h"
#include "../patches/input/PressDetector.h"
#include "../patches/playlist/PlaylistManager.h"

padre::VolumeController volume;
padre::PressDetector sensor0(650);
padre::PlaylistManager playlist;

padre::DecoderFacade decoder;
padre::DecodedAudio decoded;

void setup() {
  Serial.begin(115200);

  volume.restore(12.0f);
  playlist.setOrder(padre::PlayOrder::Shuffle);
  playlist.setTracks({"/music/a.mp3", "/music/b.flac", "/music/c.wav"});

  if (const String* track = playlist.current()) {
    Serial.printf("Current track: %s\n", track->c_str());

    const auto result = decoder.decode_file(track->c_str(), decoded);
    if (!result.ok) {
      Serial.printf("Decode error: %s\n", result.error.c_str());
    } else {
      Serial.printf("Decoded: %u Hz, %u ch, samples=%u\n", decoded.sample_rate, decoded.channels,
                    static_cast<unsigned>(decoded.samples.size()));
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
