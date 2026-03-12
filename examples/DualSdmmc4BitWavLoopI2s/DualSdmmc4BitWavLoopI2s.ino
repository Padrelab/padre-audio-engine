#include <Arduino.h>

#include "../DualWavLoopSketchApp.h"
#include "../../patches/media/source/Esp32SdMmcStorage.h"

namespace {

constexpr uint32_t kSdMmcFrequencyHz = 40000000ul;

enum class AudioProfilePreset {
  Balanced,
  StressSafe,
};

// Переключение между проверенными профилями делается здесь.
constexpr AudioProfilePreset kAudioProfilePreset = AudioProfilePreset::Balanced;

struct AudioProfileTuning {
  const char* build_tag;
  size_t startup_prebuffer_samples;
  size_t queue_refill_target_samples;
  size_t track_switch_retained_queue_samples;
  uint32_t service_budget_us;
  size_t voice_pcm_buffer_samples;
  uint8_t voice_pcm_max_refill_attempts;
  size_t voice_track_switch_min_queue_samples;
  uint32_t voice_track_switch_max_delay_ms;
};

constexpr AudioProfileTuning kBalancedAudioProfile = {
    "dual-sdmmc4-wav-i2s-n16r8-bal-r7",
    16384,
    28672,
    12288,
    18000,
    10240,
    5,
    12288,
    180,
};

constexpr AudioProfileTuning kStressSafeAudioProfile = {
    "dual-sdmmc4-wav-i2s-n16r8-stresssafe-r1",
    18432,
    29696,
    14336,
    22000,
    12288,
    6,
    12288,
    220,
};

const AudioProfileTuning& currentAudioProfile() {
  switch (kAudioProfilePreset) {
    case AudioProfilePreset::StressSafe:
      return kStressSafeAudioProfile;
    case AudioProfilePreset::Balanced:
    default:
      return kBalancedAudioProfile;
  }
}

padre::Esp32SdMmcStorageConfig makeStorageConfig() {
  padre::Esp32SdMmcStorageConfig config;
  config.clk_pin = 12;
  config.cmd_pin = 11;
  config.d0_pin = 13;
  config.d1_pin = 14;
  config.d2_pin = 9;
  config.d3_pin = 10;
  // Для SD_MMC 4-bit на ESP32-S3 имеет смысл стартовать с 40 MHz.
  // Если конкретная карта/разводка монтируется нестабильно, откатить до 20 MHz.
  config.frequency_hz = kSdMmcFrequencyHz;
  config.mode_1bit = false;
  return config;
}

padre::DualWavLoopI2sAppConfig makeAppConfig() {
  const AudioProfileTuning& profile = currentAudioProfile();
  padre::DualWavLoopI2sAppConfig config;
  config.example_name = "DualSdmmc4BitWavLoopI2s";
  config.build_tag = profile.build_tag;
  config.mix_chunk_samples = 1024;
  config.sink_queue_samples = 32768;
  config.sink_watermark_samples = 1024;
  config.startup_prebuffer_samples = profile.startup_prebuffer_samples;
  config.startup_prebuffer_budget_ms = 650;
  config.queue_refill_target_samples = profile.queue_refill_target_samples;
  config.loop_track_switch_retained_queue_samples = profile.track_switch_retained_queue_samples;
  config.i2s_dma_desc_num = 8;
  config.i2s_dma_frame_num = 256;
  config.i2s_work_samples = 1024;
  config.service_budget_us = profile.service_budget_us;
  config.audio_task_priority = 4;
  config.music_voice.pcm_buffer_samples = profile.voice_pcm_buffer_samples;
  config.music_voice.pcm_low_water_samples = 2048;
  config.music_voice.pcm_refill_chunk_samples = 2048;
  config.music_voice.pcm_max_refill_attempts = profile.voice_pcm_max_refill_attempts;
  config.music_voice.track_switch_min_queue_samples = profile.voice_track_switch_min_queue_samples;
  config.music_voice.track_switch_coalesce_ms = 40;
  config.music_voice.track_switch_max_delay_ms = profile.voice_track_switch_max_delay_ms;
  config.foley_voice = config.music_voice;
  return config;
}

padre::Esp32SdMmcStorage g_storage(makeStorageConfig());
padre::DualWavLoopI2sApp g_app(Serial, Wire, g_storage, makeAppConfig());

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  g_app.begin();
}

void loop() {
  g_app.loop();
}
