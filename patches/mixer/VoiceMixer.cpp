#include "VoiceMixer.h"

namespace padre {

VoiceMixer::VoiceMixer(size_t voice_count) { setVoiceCount(voice_count); }

void VoiceMixer::setVoiceCount(size_t voice_count) {
  voices_.assign(voice_count, {});
}

size_t VoiceMixer::voiceCount() const { return voices_.size(); }

bool VoiceMixer::attachSource(size_t voice_index, IMixerVoiceSource* source) {
  if (voice_index >= voices_.size()) return false;

  auto& voice = voices_[voice_index];
  voice.source = source;
  voice.paused = false;
  voice.active = (source != nullptr);
  return true;
}

void VoiceMixer::setGlobalGain(float gain) { global_gain_ = clampGain(gain); }

float VoiceMixer::globalGain() const { return global_gain_; }

bool VoiceMixer::setVoiceGain(size_t voice_index, float gain) {
  if (voice_index >= voices_.size()) return false;
  voices_[voice_index].gain = clampGain(gain);
  return true;
}

float VoiceMixer::voiceGain(size_t voice_index) const {
  if (voice_index >= voices_.size()) return 0.0f;
  return voices_[voice_index].gain;
}

bool VoiceMixer::pauseVoice(size_t voice_index, bool paused) {
  if (voice_index >= voices_.size()) return false;
  voices_[voice_index].paused = paused;
  return true;
}

bool VoiceMixer::stopVoice(size_t voice_index) {
  if (voice_index >= voices_.size()) return false;

  auto& voice = voices_[voice_index];
  voice.active = false;
  voice.paused = false;
  voice.source = nullptr;
  return true;
}

void VoiceMixer::pauseAll(bool paused) { paused_ = paused; }

bool VoiceMixer::isPaused() const { return paused_; }

void VoiceMixer::stopAll() {
  paused_ = false;
  for (size_t i = 0; i < voices_.size(); ++i) {
    stopVoice(i);
  }
}

size_t VoiceMixer::mix(int16_t* output, size_t sample_count) {
  if (output == nullptr || sample_count == 0) return 0;

  memset(output, 0, sample_count * sizeof(int16_t));
  if (paused_ || global_gain_ <= 0.0f) return 0;

  if (temp_.size() < sample_count) temp_.resize(sample_count);

  size_t mixed_samples = 0;
  for (auto& voice : voices_) {
    if (!voice.active || voice.paused || voice.source == nullptr || voice.gain <= 0.0f) continue;

    const size_t read = voice.source->readSamples(temp_.data(), sample_count);
    if (read == 0 || voice.source->eof()) {
      voice.active = false;
      voice.source = nullptr;
      continue;
    }

    const float gain = voice.gain * global_gain_;
    for (size_t i = 0; i < read; ++i) {
      const int32_t mixed = static_cast<int32_t>(output[i]) +
                            static_cast<int32_t>(static_cast<float>(temp_[i]) * gain);
      output[i] = clampSample(mixed);
    }

    if (read > mixed_samples) mixed_samples = read;
  }

  return mixed_samples;
}

float VoiceMixer::clampGain(float gain) {
  if (gain < 0.0f) return 0.0f;
  if (gain > 4.0f) return 4.0f;
  return gain;
}

int16_t VoiceMixer::clampSample(int32_t sample) {
  if (sample > 32767) return 32767;
  if (sample < -32768) return -32768;
  return static_cast<int16_t>(sample);
}

}  // namespace padre
