// Example-local touch wrapper over the generic multivoice WAV player.
#pragma once

#include <Adafruit_MPR121.h>
#include <Arduino.h>
#include <Wire.h>

#include <vector>

#include "../patches/app/playback/MultiVoiceWavPlayer.h"
#include "../patches/input/core/InputEvent.h"
#include "../patches/input/mpr121/Mpr121AdafruitDriver.h"
#include "../patches/input/mpr121/Mpr121TouchController.h"
#include "../patches/media/source/FsStorageBackend.h"

namespace padre {

struct DualWavLoopI2sPins {
  uint8_t i2s_bclk = 41;
  uint8_t i2s_lrc = 42;
  uint8_t i2s_dout = 40;
  uint8_t mpr121_sda = 4;
  uint8_t mpr121_scl = 5;
  uint8_t mpr121_irq = 6;
  uint8_t mpr121_addr = 0x5A;
};

struct DualWavLoopI2sTouchConfig {
  uint16_t touch_threshold = 120;
  uint16_t release_threshold = 100;
  uint32_t poll_ms = 10;
  uint32_t i2c_clock_hz = 400000;
  uint8_t active_electrodes = 4;
};

struct DualWavLoopI2sAppConfig {
  const char* example_name = "DualSdWavLoopI2s";
  const char* build_tag = "dual-sd-wav-i2s";
  DualWavLoopI2sPins pins = {};
  const char* music_dir = "/music";
  const char* foley_dir = "/foley";
  uint8_t max_dir_depth = 5;
  size_t mix_chunk_samples = 512;
  size_t sink_queue_samples = 49152;
  size_t sink_watermark_samples = 1024;
  size_t startup_prebuffer_samples = 24576;
  uint32_t startup_prebuffer_budget_ms = 800;
  size_t queue_refill_target_samples = 36864;
  size_t loop_track_switch_retained_queue_samples = 8192;
  size_t oneshot_trigger_retained_queue_samples = 0;
  size_t oneshot_queue_refill_target_samples = 0;
  float music_gain = 0.60f;
  float foley_gain = 0.60f;
  MultiVoiceWavPlayerVolumeConfig volume = {};
  DualWavLoopI2sTouchConfig touch = {};
  uint32_t i2s_write_timeout_ms = 0;
  uint8_t i2s_dma_desc_num = 16;
  uint16_t i2s_dma_frame_num = 256;
  size_t i2s_work_samples = 2048;
  uint32_t service_budget_us = 5000;
  uint32_t startup_sample_rate_hint = 48000;
  MultiVoiceWavPlayerAdaptiveProfileRules adaptive = {};
  WavVoiceConfig music_voice = {};
  WavVoiceConfig foley_voice = {};
  uint32_t audio_task_stack_bytes = 8192;
  UBaseType_t audio_task_priority = 3;
  uint32_t audio_task_loop_delay_ms = 1;
#if CONFIG_FREERTOS_UNICORE
  BaseType_t audio_task_core = 0;
#else
  BaseType_t audio_task_core = 0;
#endif
};

class DualWavLoopI2sApp {
 public:
  DualWavLoopI2sApp(Print& serial,
                    TwoWire& wire,
                    FsStorageBackend& storage,
                    DualWavLoopI2sAppConfig config = {})
      : serial_(&serial),
        config_(config),
        touch_device_(
            mpr121_,
            wire,
            Mpr121AdafruitDriverPins{
                static_cast<int8_t>(config_.pins.mpr121_sda),
                static_cast<int8_t>(config_.pins.mpr121_scl),
                static_cast<int8_t>(config_.pins.mpr121_irq),
                config_.pins.mpr121_addr,
                config_.touch.i2c_clock_hz,
            },
            Mpr121AdafruitDriverConfig{
                static_cast<uint8_t>(config_.touch.touch_threshold),
                static_cast<uint8_t>(config_.touch.release_threshold),
                true,
            }),
        touch_controller_(
            touch_device_.asTouchControllerIo(),
            Mpr121TouchControllerConfig{
                config_.touch.active_electrodes,
                Mpr121InputConfig{},
            }),
        player_(serial, storage, makeVoiceSpecs(config_), makePlayerConfig(config_)) {}

  bool begin() {
    serial_->printf("Touch:  sda=%u scl=%u irq=%u addr=0x%02X poll=%lums\n",
                    config_.pins.mpr121_sda,
                    config_.pins.mpr121_scl,
                    config_.pins.mpr121_irq,
                    config_.pins.mpr121_addr,
                    static_cast<unsigned long>(config_.touch.poll_ms));
    serial_->printf("Voices: music=%s foley=%s\n",
                    voiceModeName(config_.music_voice.mode),
                    voiceModeName(config_.foley_voice.mode));

    initTouch();
    return player_.begin();
  }

  void loop() {
    serviceTouch(millis());
    player_.loop();
  }

 private:
  static const char* voiceModeName(WavVoiceMode mode) {
    switch (mode) {
      case WavVoiceMode::OneShot:
        return "oneshot";
      case WavVoiceMode::Loop:
      default:
        return "loop";
    }
  }

  static MultiVoiceWavPlayerConfig makePlayerConfig(const DualWavLoopI2sAppConfig& config) {
    MultiVoiceWavPlayerConfig player_config;
    player_config.player_name = config.example_name;
    player_config.build_tag = config.build_tag;
    player_config.pins = {
        config.pins.i2s_bclk,
        config.pins.i2s_lrc,
        config.pins.i2s_dout,
    };
    player_config.max_dir_depth = config.max_dir_depth;
    player_config.volume = config.volume;
    player_config.runtime = {
        "default",
        config.mix_chunk_samples,
        config.sink_queue_samples,
        config.sink_watermark_samples,
        config.startup_prebuffer_samples,
        config.startup_prebuffer_budget_ms,
        config.queue_refill_target_samples,
        config.loop_track_switch_retained_queue_samples,
        config.i2s_write_timeout_ms,
        config.i2s_dma_desc_num,
        config.i2s_dma_frame_num,
        config.i2s_work_samples,
        config.service_budget_us,
        config.startup_sample_rate_hint,
        config.audio_task_stack_bytes,
        config.audio_task_priority,
        config.audio_task_loop_delay_ms,
        config.audio_task_core,
        config.oneshot_trigger_retained_queue_samples,
        config.oneshot_queue_refill_target_samples,
    };
    player_config.adaptive = config.adaptive;
    return player_config;
  }

  static std::vector<MultiVoiceWavPlayerVoiceSpec> makeVoiceSpecs(
      const DualWavLoopI2sAppConfig& config) {
    std::vector<MultiVoiceWavPlayerVoiceSpec> voices;
    voices.push_back(MultiVoiceWavPlayerVoiceSpec{
        "music",
        config.music_dir,
        config.music_gain,
        config.music_voice,
    });
    voices.push_back(MultiVoiceWavPlayerVoiceSpec{
        "foley",
        config.foley_dir,
        config.foley_gain,
        config.foley_voice,
    });
    return voices;
  }

  static void onTouchEventThunk(void* ctx, const InputEvent& event) {
    auto* self = static_cast<DualWavLoopI2sApp*>(ctx);
    if (self == nullptr) return;
    self->onTouchEvent(event);
  }

  bool initTouch() {
    if (!touch_device_.begin()) {
      serial_->println("MPR121 init failed, touch disabled");
      return false;
    }

    touch_controller_.setEventHandler(this, onTouchEventThunk);
    if (!touch_controller_.begin()) {
      serial_->println("Touch controller init failed, touch disabled");
      return false;
    }

    touch_ready_ = true;
    serial_->println("MPR121 touch ready");
    return true;
  }

  void onTouchEvent(const InputEvent& event) {
    if (event.type != InputEventType::PressDown) return;

    switch (event.source_id) {
      case 0:
        player_.activateVoice(0);
        break;
      case 1:
        player_.activateVoice(1);
        break;
      case 2:
        player_.stepVolume(-1);
        break;
      case 3:
        player_.stepVolume(1);
        break;
      default:
        break;
    }
  }

  void serviceTouch(uint32_t now_ms) {
    if (!touch_ready_) return;

    const bool touch_irq = touch_device_.consumeIrq();
    if (touch_irq || (now_ms - last_touch_poll_ms_) >= config_.touch.poll_ms) {
      touch_controller_.poll(now_ms);
      last_touch_poll_ms_ = now_ms;
    }
  }

  Print* serial_ = nullptr;
  DualWavLoopI2sAppConfig config_;
  Adafruit_MPR121 mpr121_;
  uint32_t last_touch_poll_ms_ = 0;
  bool touch_ready_ = false;
  Mpr121AdafruitDriver touch_device_;
  Mpr121TouchController touch_controller_;
  MultiVoiceWavPlayer player_;
};

}  // namespace padre
