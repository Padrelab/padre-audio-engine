#include <Arduino.h>
#include <Wire.h>

#include <Adafruit_MPR121.h>
#include <algorithm>

#include "../../patches/app/playback/MultiVoiceWavPlayer.h"
#include "../../patches/input/core/InputEvent.h"
#include "../../patches/input/mpr121/Mpr121AdafruitDriver.h"
#include "../../patches/input/mpr121/Mpr121TouchController.h"
#include "../../patches/media/source/Esp32SdMmcStorage.h"

namespace {

constexpr uint32_t kSdMmcFrequencyHz = 40000000ul;

constexpr uint8_t kMusicVoiceIndex = 0;
constexpr uint8_t kFoleyVoiceIndex = 1;
constexpr uint8_t kUiVoiceIndex = 2;
constexpr size_t kInvalidTrackIndex = static_cast<size_t>(-1);

constexpr char kUiNextPath[] = "/ui/next.wav";
constexpr char kUiVolumePath[] = "/ui/vol.wav";

constexpr uint8_t kMpr121Sda = 4;
constexpr uint8_t kMpr121Scl = 5;
constexpr uint8_t kMpr121Irq = 6;
constexpr uint8_t kMpr121Addr = 0x5A;
constexpr uint16_t kTouchThreshold = 120;
constexpr uint16_t kReleaseThreshold = 100;
constexpr uint32_t kTouchPollMs = 10;
constexpr uint32_t kTouchI2cClockHz = 400000;
constexpr uint8_t kTouchElectrodes = 4;

enum class AudioProfilePreset {
  Balanced,
  StressSafe,
};

constexpr AudioProfilePreset kAudioProfilePreset = AudioProfilePreset::Balanced;

struct AudioProfileTuning {
  const char* build_tag;
  const char* runtime_name;
  size_t startup_prebuffer_samples;
  size_t queue_refill_target_samples;
  size_t loop_track_switch_retained_queue_samples;
  size_t oneshot_trigger_retained_queue_samples;
  size_t oneshot_queue_refill_target_samples;
  uint32_t service_budget_us;
  size_t loop_pcm_buffer_samples;
  uint8_t loop_pcm_max_refill_attempts;
  size_t loop_track_switch_min_queue_samples;
  uint32_t loop_track_switch_max_delay_ms;
  size_t oneshot_pcm_buffer_samples;
  size_t oneshot_pcm_low_water_samples;
  size_t oneshot_pcm_refill_chunk_samples;
  uint8_t oneshot_pcm_max_refill_attempts;
};

constexpr AudioProfileTuning kBalancedAudioProfile = {
    "dual-sdmmc4-wav-i2s-ui-n16r8-bal-r2",
    "balanced",
    16384,
    28672,
    12288,
    2048,
    6144,
    18000,
    10240,
    5,
    12288,
    180,
    2048,
    512,
    1024,
    4,
};

constexpr AudioProfileTuning kStressSafeAudioProfile = {
    "dual-sdmmc4-wav-i2s-ui-n16r8-stresssafe-r2",
    "stresssafe",
    18432,
    29696,
    14336,
    3072,
    8192,
    22000,
    12288,
    6,
    12288,
    220,
    3072,
    1024,
    1024,
    5,
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
  config.frequency_hz = kSdMmcFrequencyHz;
  config.mode_1bit = false;
  return config;
}

padre::MultiVoiceWavPlayerConfig makePlayerConfig() {
  const AudioProfileTuning& profile = currentAudioProfile();

  padre::MultiVoiceWavPlayerConfig config;
  config.player_name = "DualSdmmc4BitWavLoopI2sUi";
  config.build_tag = profile.build_tag;
  config.max_dir_depth = 5;
  config.runtime.name = profile.runtime_name;
  config.runtime.mix_chunk_samples = 1024;
  config.runtime.sink_queue_samples = 32768;
  config.runtime.sink_watermark_samples = 1024;
  config.runtime.startup_prebuffer_samples = profile.startup_prebuffer_samples;
  config.runtime.startup_prebuffer_budget_ms = 650;
  config.runtime.queue_refill_target_samples = profile.queue_refill_target_samples;
  config.runtime.loop_track_switch_retained_queue_samples =
      profile.loop_track_switch_retained_queue_samples;
  config.runtime.oneshot_trigger_retained_queue_samples =
      profile.oneshot_trigger_retained_queue_samples;
  config.runtime.oneshot_queue_refill_target_samples =
      profile.oneshot_queue_refill_target_samples;
  config.runtime.i2s_dma_desc_num = 8;
  config.runtime.i2s_dma_frame_num = 256;
  config.runtime.i2s_work_samples = 1024;
  config.runtime.service_budget_us = profile.service_budget_us;
  config.runtime.audio_task_priority = 4;
  return config;
}

std::vector<padre::MultiVoiceWavPlayerVoiceSpec> makeVoiceSpecs() {
  const AudioProfileTuning& profile = currentAudioProfile();

  padre::WavVoiceConfig loop_voice;
  loop_voice.mode = padre::WavVoiceMode::Loop;
  loop_voice.pcm_buffer_samples = profile.loop_pcm_buffer_samples;
  loop_voice.pcm_low_water_samples = 2048;
  loop_voice.pcm_refill_chunk_samples = 2048;
  loop_voice.pcm_max_refill_attempts = profile.loop_pcm_max_refill_attempts;
  loop_voice.track_switch_min_queue_samples = profile.loop_track_switch_min_queue_samples;
  loop_voice.track_switch_coalesce_ms = 40;
  loop_voice.track_switch_max_delay_ms = profile.loop_track_switch_max_delay_ms;

  padre::WavVoiceConfig ui_voice;
  ui_voice.mode = padre::WavVoiceMode::OneShot;
  ui_voice.pcm_buffer_samples = profile.oneshot_pcm_buffer_samples;
  ui_voice.pcm_low_water_samples = profile.oneshot_pcm_low_water_samples;
  ui_voice.pcm_refill_chunk_samples = profile.oneshot_pcm_refill_chunk_samples;
  ui_voice.pcm_max_refill_attempts = profile.oneshot_pcm_max_refill_attempts;

  std::vector<padre::MultiVoiceWavPlayerVoiceSpec> voices;
  voices.push_back(padre::MultiVoiceWavPlayerVoiceSpec{
      "music",
      "/music",
      0.60f,
      loop_voice,
  });
  voices.push_back(padre::MultiVoiceWavPlayerVoiceSpec{
      "foley",
      "/foley",
      0.60f,
      loop_voice,
  });
  voices.push_back(padre::MultiVoiceWavPlayerVoiceSpec{
      "ui",
      "/ui",
      0.80f,
      ui_voice,
  });
  return voices;
}

padre::Esp32SdMmcStorage g_storage(makeStorageConfig());
padre::MultiVoiceWavPlayer g_player(Serial, g_storage, makeVoiceSpecs(), makePlayerConfig());

Adafruit_MPR121 g_mpr121;
padre::Mpr121AdafruitDriver g_touch_device(
    g_mpr121,
    Wire,
    padre::Mpr121AdafruitDriverPins{
        static_cast<int8_t>(kMpr121Sda),
        static_cast<int8_t>(kMpr121Scl),
        static_cast<int8_t>(kMpr121Irq),
        kMpr121Addr,
        kTouchI2cClockHz,
    },
    padre::Mpr121AdafruitDriverConfig{
        static_cast<uint8_t>(kTouchThreshold),
        static_cast<uint8_t>(kReleaseThreshold),
        true,
    });
padre::Mpr121TouchController g_touch_controller(
    g_touch_device.asTouchControllerIo(),
    padre::Mpr121TouchControllerConfig{
        kTouchElectrodes,
        padre::Mpr121InputConfig{},
    });

bool g_touch_ready = false;
size_t g_ui_next_track_index = kInvalidTrackIndex;
size_t g_ui_volume_track_index = kInvalidTrackIndex;

void playUiCue(size_t track_index) {
  if (track_index == kInvalidTrackIndex) return;
  g_player.triggerVoiceTrack(kUiVoiceIndex, track_index);
}

void onTouchEvent(const padre::InputEvent& event) {
  if (event.type != padre::InputEventType::PressDown) return;

  switch (event.source_id) {
    case 0:
      g_player.requestNextTrack(kMusicVoiceIndex);
      playUiCue(g_ui_next_track_index);
      break;
    case 1:
      g_player.requestNextTrack(kFoleyVoiceIndex);
      playUiCue(g_ui_next_track_index);
      break;
    case 2:
      g_player.stepVolume(-1);
      playUiCue(g_ui_volume_track_index);
      break;
    case 3:
      g_player.stepVolume(1);
      playUiCue(g_ui_volume_track_index);
      break;
    default:
      break;
  }
}

void onTouchEventThunk(void*, const padre::InputEvent& event) { onTouchEvent(event); }

bool initTouch() {
  if (!g_touch_device.begin()) {
    Serial.println("MPR121 init failed, touch disabled");
    return false;
  }

  g_touch_controller.setEventHandler(nullptr, onTouchEventThunk);
  if (!g_touch_controller.begin()) {
    Serial.println("Touch controller init failed, touch disabled");
    return false;
  }

  Serial.println("MPR121 touch ready");
  return true;
}

void resolveUiTracks() {
  const int next_index = g_player.findTrackIndex(kUiVoiceIndex, kUiNextPath);
  const int volume_index = g_player.findTrackIndex(kUiVoiceIndex, kUiVolumePath);

  if (next_index >= 0) {
    g_ui_next_track_index = static_cast<size_t>(next_index);
    Serial.printf("[ui] cue next mapped to %s\n", kUiNextPath);
  } else {
    Serial.printf("[ui] warning: missing cue %s\n", kUiNextPath);
  }

  if (volume_index >= 0) {
    g_ui_volume_track_index = static_cast<size_t>(volume_index);
    Serial.printf("[ui] cue volume mapped to %s\n", kUiVolumePath);
  } else {
    Serial.printf("[ui] warning: missing cue %s\n", kUiVolumePath);
  }
}

void serviceTouch(uint32_t now_ms) {
  if (!g_touch_ready) return;

  const bool touch_irq = g_touch_device.consumeIrq();
  static uint32_t last_touch_poll_ms = 0;
  if (touch_irq || (now_ms - last_touch_poll_ms) >= kTouchPollMs) {
    g_touch_controller.poll(now_ms);
    last_touch_poll_ms = now_ms;
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);

  if (!g_player.begin()) return;
  resolveUiTracks();
  g_touch_ready = initTouch();
}

void loop() {
  serviceTouch(millis());
  g_player.loop();
}
