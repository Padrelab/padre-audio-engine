#pragma once

#include <Adafruit_MPR121.h>
#include <Arduino.h>
#include <Wire.h>

#include <vector>

#include "../playback/LoopingWavVoice.h"
#include "../../audio/mixer/VoiceMixer.h"
#include "../../audio/output/BufferedI2sOutput.h"
#include "../../audio/output/Esp32StdI2sOutputIo.h"
#include "../../input/core/InputEvent.h"
#include "../../input/mpr121/Mpr121AdafruitDriver.h"
#include "../../input/mpr121/Mpr121TouchController.h"
#include "../../media/source/FsStorageBackend.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

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

struct DualWavLoopI2sVolumeConfig {
  int min = 0;
  int max = 20;
  int initial = 18;
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
  size_t track_switch_retained_queue_samples = 8192;
  float music_gain = 0.60f;
  float foley_gain = 0.60f;
  DualWavLoopI2sVolumeConfig volume = {};
  DualWavLoopI2sTouchConfig touch = {};
  uint32_t i2s_write_timeout_ms = 0;
  uint8_t i2s_dma_desc_num = 16;
  uint16_t i2s_dma_frame_num = 256;
  size_t i2s_work_samples = 2048;
  uint32_t service_budget_us = 5000;
  uint32_t startup_sample_rate_hint = 48000;
  LoopingWavVoiceConfig voice = {};
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
  DualWavLoopI2sApp(HardwareSerial& serial,
                    TwoWire& wire,
                    FsStorageBackend& storage,
                    DualWavLoopI2sAppConfig config = {});

  bool begin();
  void loop();

 private:
  struct PendingControlState {
    uint32_t music_next_requests = 0;
    uint32_t foley_next_requests = 0;
    int32_t volume_delta = 0;
  };

  static int16_t applyVolumeSampleThunk(void* ctx, int16_t sample);
  static void audioTaskEntry(void* ctx);
  static void onTouchEventThunk(void* ctx, const InputEvent& event);

  int16_t applyVolumeToSample(int16_t sample) const;
  void updateVolumeGain();
  void applyVolume();
  bool stepVolume(int delta);
  void printPinout();
  bool initStorage();
  bool initTouch();
  bool preparePlaylists(uint32_t& out_sample_rate);
  bool startAudio(uint32_t sample_rate);
  bool startAudioTask();
  void audioTaskMain();
  void notifyAudioTask();
  void queueMusicNextRequest();
  void queueFoleyNextRequest();
  void queueVolumeDelta(int delta);
  PendingControlState takePendingControls();
  bool hasPendingControls();
  void applyPendingControls(uint32_t now_ms);
  void onTouchEvent(const InputEvent& event);
  void serviceTouch(uint32_t now_ms);
  size_t pumpSink();
  size_t mixVoices(size_t request_samples);
  size_t writeMixedSamples(size_t sample_count);
  bool servicePendingTrackSwitches();
  void serviceAudio();

  HardwareSerial* serial_ = nullptr;
  TwoWire* wire_ = nullptr;
  FsStorageBackend* storage_ = nullptr;
  DualWavLoopI2sAppConfig config_;

  Adafruit_MPR121 mpr121_;
  uint32_t last_touch_poll_ms_ = 0;
  bool touch_ready_ = false;
  volatile bool audio_ready_ = false;
  TaskHandle_t audio_task_handle_ = nullptr;
  portMUX_TYPE control_mux_ = portMUX_INITIALIZER_UNLOCKED;
  PendingControlState pending_controls_;

  Mpr121AdafruitDriver touch_device_;
  Mpr121TouchController touch_controller_;
  Esp32StdI2sOutputIo i2s_io_;
  BufferedI2sOutput sink_;
  VoiceMixer mixer_;
  LoopingWavVoice music_voice_;
  LoopingWavVoice foley_voice_;
  std::vector<String> music_tracks_;
  std::vector<String> foley_tracks_;
  std::vector<int16_t> mix_buffer_;
  int volume_ = 0;
  int32_t volume_gain_q15_ = 32767;
};

}  // namespace padre
