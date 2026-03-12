#pragma once

#include <Arduino.h>

#include <memory>
#include <vector>

#include "../../audio/mixer/VoiceMixer.h"
#include "../../audio/output/BufferedI2sOutput.h"
#include "../../audio/output/Esp32StdI2sOutputIo.h"
#include "../../media/source/FsStorageBackend.h"
#include "WavVoice.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace padre {

struct MultiVoiceWavPlayerPins {
  uint8_t i2s_bclk = 41;
  uint8_t i2s_lrc = 42;
  uint8_t i2s_dout = 40;
};

struct MultiVoiceWavPlayerVolumeConfig {
  int min = 0;
  int max = 20;
  int initial = 18;
};

struct MultiVoiceWavPlayerRuntimeProfile {
  const char* name = "default";
  size_t mix_chunk_samples = 512;
  size_t sink_queue_samples = 49152;
  size_t sink_watermark_samples = 1024;
  size_t startup_prebuffer_samples = 24576;
  uint32_t startup_prebuffer_budget_ms = 800;
  size_t queue_refill_target_samples = 36864;
  size_t loop_track_switch_retained_queue_samples = 8192;
  uint32_t i2s_write_timeout_ms = 0;
  uint8_t i2s_dma_desc_num = 16;
  uint16_t i2s_dma_frame_num = 256;
  size_t i2s_work_samples = 2048;
  uint32_t service_budget_us = 5000;
  uint32_t startup_sample_rate_hint = 48000;
  uint32_t audio_task_stack_bytes = 8192;
  UBaseType_t audio_task_priority = 3;
  uint32_t audio_task_loop_delay_ms = 1;
#if CONFIG_FREERTOS_UNICORE
  BaseType_t audio_task_core = 0;
#else
  BaseType_t audio_task_core = 0;
#endif
  // Если > 0, при триггере oneshot software-queue подрезается до этого хвоста,
  // чтобы новый звук начал звучать раньше уже набитого буфера loop-трека.
  size_t oneshot_trigger_retained_queue_samples = 0;
  // Если > 0, пока активен хотя бы один oneshot, очередь дозаполняется только
  // до этого уровня вместо общего queue_refill_target_samples.
  size_t oneshot_queue_refill_target_samples = 0;
};

struct MultiVoiceWavPlayerAdaptiveProfileRules {
  bool enabled = true;
  uint8_t oneshot_voice_weight = 2;
  size_t mix_chunk_per_weighted_voice_samples = 256;
  size_t sink_queue_per_weighted_voice_samples = 8192;
  size_t startup_prebuffer_per_weighted_voice_samples = 4096;
  size_t queue_refill_per_weighted_voice_samples = 6144;
  size_t loop_switch_retained_per_loop_voice_samples = 4096;
  uint32_t service_budget_per_weighted_voice_us = 1500;
  uint32_t high_sample_rate_threshold = 48000;
  size_t mix_chunk_high_sample_rate_bonus_samples = 0;
  size_t sink_queue_high_sample_rate_bonus_samples = 8192;
  size_t startup_prebuffer_high_sample_rate_bonus_samples = 8192;
  size_t queue_refill_high_sample_rate_bonus_samples = 8192;
  size_t loop_switch_retained_high_sample_rate_bonus_samples = 2048;
  uint32_t service_budget_high_sample_rate_bonus_us = 2000;
};

struct MultiVoiceWavPlayerConfig {
  const char* player_name = "MultiVoiceWavPlayer";
  const char* build_tag = "multivoice-wav-player";
  MultiVoiceWavPlayerPins pins = {};
  uint8_t max_dir_depth = 5;
  MultiVoiceWavPlayerVolumeConfig volume = {};
  MultiVoiceWavPlayerRuntimeProfile runtime = {};
  MultiVoiceWavPlayerAdaptiveProfileRules adaptive = {};
};

struct MultiVoiceWavPlayerVoiceSpec {
  const char* label = "voice";
  const char* tracks_dir = "/";
  float gain = 1.0f;
  WavVoiceConfig voice = {};
};

MultiVoiceWavPlayerRuntimeProfile deriveAdaptiveRuntimeProfile(
    const MultiVoiceWavPlayerRuntimeProfile& base,
    const MultiVoiceWavPlayerAdaptiveProfileRules& rules,
    size_t voice_count,
    size_t loop_voice_count,
    size_t oneshot_voice_count,
    uint32_t sample_rate);

class MultiVoiceWavPlayer {
 public:
  MultiVoiceWavPlayer(Print& serial,
                      FsStorageBackend& storage,
                      std::vector<MultiVoiceWavPlayerVoiceSpec> voice_specs,
                      MultiVoiceWavPlayerConfig config = {});

  bool begin();
  void loop();

  size_t voiceCount() const;
  WavVoiceMode voiceMode(size_t voice_index) const;
  bool activateVoice(size_t voice_index);
  bool requestNextTrack(size_t voice_index, size_t steps = 1);
  bool triggerVoice(size_t voice_index);
  bool triggerVoiceTrack(size_t voice_index, size_t track_index);
  bool stepVolume(int delta);
  int volume() const;
  const MultiVoiceWavPlayerRuntimeProfile& activeRuntimeProfile() const;
  size_t queuedOutputSamples() const;
  const String& activeTrack(size_t voice_index) const;
  int findTrackIndex(size_t voice_index, const char* path) const;

 private:
  struct VoiceSlot {
    MultiVoiceWavPlayerVoiceSpec spec = {};
    std::vector<String> tracks;
    std::unique_ptr<WavVoice> voice;
  };

  struct PendingControlState {
    std::vector<uint32_t> next_requests;
    std::vector<uint32_t> trigger_requests;
    std::vector<int32_t> selected_track_indices;
    std::vector<uint8_t> selected_track_trigger_requests;
    int32_t volume_delta = 0;
  };

  static int16_t applyVolumeSampleThunk(void* ctx, int16_t sample);
  static void audioTaskEntry(void* ctx);

  int16_t applyVolumeToSample(int16_t sample) const;
  void updateVolumeGain();
  void applyVolume();
  bool stepVolumeInternal(int delta);
  void printConfigSummary() const;
  bool initStorage();
  bool preparePlaylists(uint32_t& out_sample_rate);
  bool buildAudioGraph(uint32_t sample_rate);
  bool startAudio(uint32_t sample_rate);
  bool startAudioTask();
  void audioTaskMain();
  void notifyAudioTask();
  void queueNextTrackRequest(size_t voice_index, size_t steps);
  void queueTriggerRequest(size_t voice_index);
  void queueTriggerTrackRequest(size_t voice_index, size_t track_index);
  void queueVolumeDelta(int delta);
  PendingControlState takePendingControls();
  bool hasPendingControls();
  void applyPendingControls(uint32_t now_ms);
  size_t pumpSink();
  size_t mixVoices(size_t request_samples);
  size_t writeMixedSamples(size_t sample_count);
  bool servicePendingTrackSwitches();
  void serviceAudio();
  size_t currentQueueRefillTargetSamples() const;
  size_t oneShotRetainedQueueSamples() const;
  bool hasActiveOneShotVoice() const;
  size_t loopVoiceCount() const;
  size_t oneShotVoiceCount() const;

  Print* serial_ = nullptr;
  FsStorageBackend* storage_ = nullptr;
  MultiVoiceWavPlayerConfig config_;
  MultiVoiceWavPlayerRuntimeProfile active_runtime_;
  std::vector<VoiceSlot> voice_slots_;
  String empty_track_;
  bool audio_ready_ = false;
  TaskHandle_t audio_task_handle_ = nullptr;
  portMUX_TYPE control_mux_ = portMUX_INITIALIZER_UNLOCKED;
  std::vector<uint32_t> pending_next_requests_;
  std::vector<uint32_t> pending_trigger_requests_;
  std::vector<int32_t> pending_selected_track_indices_;
  std::vector<uint8_t> pending_selected_track_trigger_requests_;
  int32_t pending_volume_delta_ = 0;
  std::unique_ptr<Esp32StdI2sOutputIo> i2s_io_;
  std::unique_ptr<BufferedI2sOutput> sink_;
  std::unique_ptr<VoiceMixer> mixer_;
  std::vector<int16_t> mix_buffer_;
  int volume_ = 0;
  int32_t volume_gain_q15_ = 32767;
};

}  // namespace padre
