#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <vector>

namespace padre {

class IMixerVoiceSource {
 public:
  virtual ~IMixerVoiceSource() = default;
  virtual size_t readSamples(int16_t* dst, size_t sample_count) = 0;
  virtual bool eof() const = 0;
};

class VoiceMixer {
 public:
  explicit VoiceMixer(size_t voice_count = 4);

  void setVoiceCount(size_t voice_count);
  size_t voiceCount() const;

  bool attachSource(size_t voice_index, IMixerVoiceSource* source);

  void setGlobalGain(float gain);
  float globalGain() const;

  bool setVoiceGain(size_t voice_index, float gain);
  float voiceGain(size_t voice_index) const;

  bool pauseVoice(size_t voice_index, bool paused = true);
  bool stopVoice(size_t voice_index);

  void pauseAll(bool paused = true);
  bool isPaused() const;
  void stopAll();

  size_t mix(int32_t* output, size_t sample_count);

 private:
  struct VoiceState {
    IMixerVoiceSource* source = nullptr;
    float gain = 1.0f;
    bool paused = false;
    bool active = false;
  };

  static float clampGain(float gain);
  std::vector<VoiceState> voices_;
  std::vector<int16_t> temp_;
  float global_gain_ = 1.0f;
  bool paused_ = false;
};

}  // namespace padre
