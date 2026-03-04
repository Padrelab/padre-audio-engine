#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <vector>
#include <driver/i2s_std.h>

#include <Adafruit_MPR121.h>

#include "../../patches/decoder/DecoderFacade.h"
#include "../../patches/input/InputEvent.h"
#include "../../patches/io_mpr121/Mpr121Input.h"
#include "../../patches/output/I2sPcm5122Output.h"
#include "../../patches/playlist/PlaylistManager.h"

namespace {

constexpr uint8_t SD_CS = 10;
constexpr uint8_t SD_SCK = 12;
constexpr uint8_t SD_MISO = 13;
constexpr uint8_t SD_MOSI = 11;

constexpr uint8_t I2S_BCLK = 14;
constexpr uint8_t I2S_LRC = 15;
constexpr uint8_t I2S_DOUT = 16;

constexpr uint8_t MPR121_SDA = 35;
constexpr uint8_t MPR121_SCL = 36;
constexpr uint8_t MPR121_IRQ = 37;
constexpr uint8_t MPR121_ADDR = 0x5A;

constexpr char kMusicDir[] = "/music";
constexpr uint8_t kMaxDirDepth = 5;

constexpr uint8_t kVolumeMin = 0;
constexpr uint8_t kVolumeMax = 21;
constexpr uint8_t kVolumeDefault = 12;

constexpr uint16_t kTouchThreshold = 12;
constexpr uint16_t kReleaseThreshold = 6;
constexpr uint32_t kTouchPollMs = 10;
constexpr uint32_t kRetryStartDelayMs = 500;
constexpr bool kTouchDebug = true;
constexpr uint32_t kI2sWriteTimeoutMs = 0;

constexpr size_t kI2sWorkSamples = 2048;
constexpr size_t kSinkQueueSamples = 32768;
constexpr size_t kPrebufferMinSamples = 8192;
constexpr uint32_t kStartPrebufferBudgetUs = 30000;
constexpr uint32_t kServiceDecodeBudgetUs = 2500;
constexpr size_t kStartReadsPerStep = 8;
constexpr size_t kServiceReadsPerStep = 8;
constexpr bool kPerfTelemetryEnabled = false;
constexpr uint32_t kPerfReportMs = 2000;
constexpr uint32_t kPerfSlowLoopUs = 5000;
constexpr uint32_t kPerfSlowDecodeUs = 1200;
constexpr size_t kPerfQueueLowSamples = 4096;

SPIClass g_sd_spi(FSPI);
Adafruit_MPR121 g_mpr121;
padre::PlaylistManager g_playlist;

std::vector<String> g_tracks;

struct TouchMaskCache {
  uint16_t mask = 0;
};

TouchMaskCache g_touch_cache;
padre::Mpr121InputIo g_touch_io{
    &g_touch_cache,
    [](void* user_data) -> uint16_t {
      auto* cache = static_cast<TouchMaskCache*>(user_data);
      return cache ? cache->mask : 0;
    },
};

padre::Mpr121Input g_touch0(0, g_touch_io);
padre::Mpr121Input g_touch1(1, g_touch_io);
padre::Mpr121Input g_touch2(2, g_touch_io);
padre::Mpr121Input g_touch3(3, g_touch_io);

bool g_paused = false;
bool g_was_running = false;
int g_volume = kVolumeDefault;
int32_t g_volume_gain_q15 = 0;
uint32_t g_last_touch_poll_ms = 0;
uint32_t g_retry_at_ms = 0;
bool g_request_next_track = false;
volatile bool g_touch_irq_flag = false;

int16_t g_i2s_work_stereo[kI2sWorkSamples] = {0};
int16_t g_i2s_work_mono_to_stereo[kI2sWorkSamples * 2] = {0};

struct I2sRuntime {
  i2s_chan_handle_t tx = nullptr;
  bool stereo_input = true;
  bool prebuffering = false;
};

I2sRuntime g_i2s_runtime;

struct PerfTelemetry {
  uint32_t next_report_ms = 0;

  uint32_t loop_calls = 0;
  uint64_t loop_total_us = 0;
  uint32_t loop_max_us = 0;
  uint32_t loop_slow = 0;

  uint32_t service_calls = 0;
  uint64_t service_total_us = 0;
  uint32_t service_max_us = 0;
  uint32_t service_budget_hits = 0;
  uint64_t service_decode_iters = 0;

  uint32_t decode_calls = 0;
  uint64_t decode_total_us = 0;
  uint32_t decode_max_us = 0;
  uint32_t decode_slow = 0;
  uint64_t decode_out_samples = 0;
  uint32_t decode_zero_out = 0;

  size_t queue_min_samples = static_cast<size_t>(-1);
  uint32_t queue_low_events = 0;
  uint32_t queue_empty_events = 0;

  bool next_touch_pending = false;
  uint32_t next_touch_requested_ms = 0;
  uint32_t next_touch_count = 0;
  uint64_t next_touch_latency_total_ms = 0;
  uint32_t next_touch_latency_max_ms = 0;
};

PerfTelemetry g_perf;

void perfNoteQueue(size_t queued_samples) {
  if (!kPerfTelemetryEnabled) return;
  if (queued_samples < g_perf.queue_min_samples) {
    g_perf.queue_min_samples = queued_samples;
  }
  if (queued_samples <= kPerfQueueLowSamples) ++g_perf.queue_low_events;
  if (queued_samples == 0) ++g_perf.queue_empty_events;
}

void perfNoteNextTouchRequest(uint32_t now_ms) {
  if (!kPerfTelemetryEnabled) return;
  g_perf.next_touch_pending = true;
  g_perf.next_touch_requested_ms = now_ms;
}

void perfNoteNextTouchHandled(uint32_t now_ms) {
  if (!kPerfTelemetryEnabled) return;
  if (!g_perf.next_touch_pending) return;

  const uint32_t latency_ms = now_ms - g_perf.next_touch_requested_ms;
  g_perf.next_touch_pending = false;
  ++g_perf.next_touch_count;
  g_perf.next_touch_latency_total_ms += latency_ms;
  if (latency_ms > g_perf.next_touch_latency_max_ms) {
    g_perf.next_touch_latency_max_ms = latency_ms;
  }
}

void perfReportIfDue(uint32_t now_ms) {
  if (!kPerfTelemetryEnabled) return;

  if (g_perf.next_report_ms == 0) {
    g_perf.next_report_ms = now_ms + kPerfReportMs;
    return;
  }
  if (now_ms < g_perf.next_report_ms) return;

  const uint32_t loop_avg_us =
      g_perf.loop_calls == 0 ? 0 : static_cast<uint32_t>(g_perf.loop_total_us / g_perf.loop_calls);
  const uint32_t service_avg_us = g_perf.service_calls == 0
                                      ? 0
                                      : static_cast<uint32_t>(g_perf.service_total_us / g_perf.service_calls);
  const uint32_t decode_avg_us =
      g_perf.decode_calls == 0 ? 0 : static_cast<uint32_t>(g_perf.decode_total_us / g_perf.decode_calls);
  const uint32_t decode_iters_per_service = g_perf.service_calls == 0
                                                ? 0
                                                : static_cast<uint32_t>(g_perf.service_decode_iters /
                                                                        g_perf.service_calls);
  const uint32_t next_touch_avg_ms =
      g_perf.next_touch_count == 0
          ? 0
          : static_cast<uint32_t>(g_perf.next_touch_latency_total_ms / g_perf.next_touch_count);
  const uint32_t queue_min =
      g_perf.queue_min_samples == static_cast<size_t>(-1)
          ? 0
          : static_cast<uint32_t>(g_perf.queue_min_samples);

  Serial.printf(
      "PERF loop %lu/%luus slow=%lu | svc %lu/%luus it=%lu budget=%lu | dec %lu/%luus "
      "slow=%lu out=%llu zero=%lu | qmin=%lu low=%lu empty=%lu\n",
      static_cast<unsigned long>(loop_avg_us),
      static_cast<unsigned long>(g_perf.loop_max_us),
      static_cast<unsigned long>(g_perf.loop_slow),
      static_cast<unsigned long>(service_avg_us),
      static_cast<unsigned long>(g_perf.service_max_us),
      static_cast<unsigned long>(decode_iters_per_service),
      static_cast<unsigned long>(g_perf.service_budget_hits),
      static_cast<unsigned long>(decode_avg_us),
      static_cast<unsigned long>(g_perf.decode_max_us),
      static_cast<unsigned long>(g_perf.decode_slow),
      static_cast<unsigned long long>(g_perf.decode_out_samples),
      static_cast<unsigned long>(g_perf.decode_zero_out),
      static_cast<unsigned long>(queue_min),
      static_cast<unsigned long>(g_perf.queue_low_events),
      static_cast<unsigned long>(g_perf.queue_empty_events));

  if (g_perf.next_touch_count > 0 || g_perf.next_touch_pending) {
    Serial.printf(
        "PERF touch-next count=%lu avg/max=%lu/%lums pending=%s\n",
        static_cast<unsigned long>(g_perf.next_touch_count),
        static_cast<unsigned long>(next_touch_avg_ms),
        static_cast<unsigned long>(g_perf.next_touch_latency_max_ms),
        g_perf.next_touch_pending ? "yes" : "no");
  }

  const bool pending = g_perf.next_touch_pending;
  const uint32_t pending_since_ms = g_perf.next_touch_requested_ms;
  g_perf = {};
  g_perf.next_touch_pending = pending;
  g_perf.next_touch_requested_ms = pending_since_ms;
  g_perf.next_report_ms = now_ms + kPerfReportMs;
}

class SdFileAudioSource final : public padre::IAudioSource {
 public:
  bool begin() override { return true; }

  bool open(const String& uri) override {
    close();
    file_ = SD.open(uri.c_str(), FILE_READ);
    return static_cast<bool>(file_);
  }

  size_t read(uint8_t* dst, size_t bytes) override {
    if (!file_ || dst == nullptr || bytes == 0) return 0;
    const int got = file_.read(dst, bytes);
    return got > 0 ? static_cast<size_t>(got) : 0;
  }

  bool seek(size_t offset) override { return file_ ? file_.seek(offset) : false; }

  size_t position() const override {
    return file_ ? static_cast<size_t>(file_.position()) : 0;
  }

  size_t size() const override {
    return file_ ? static_cast<size_t>(file_.size()) : 0;
  }

  bool eof() const override {
    if (!file_) return true;
    return file_.position() >= file_.size();
  }

  bool isOpen() const override { return static_cast<bool>(file_); }

  void close() override {
    if (file_) file_.close();
  }

  const char* type() const override { return "sd"; }

 private:
  mutable File file_;
};

SdFileAudioSource g_audio_source;
padre::DecoderFacade g_decoder;

int16_t applyVolumeToSample(int16_t sample) {
  const int32_t scaled = (static_cast<int32_t>(sample) * g_volume_gain_q15) >> 15;
  if (scaled > 32767) return 32767;
  if (scaled < -32768) return -32768;
  return static_cast<int16_t>(scaled);
}

void updateVolumeGain() {
  const int32_t vol = static_cast<int32_t>(g_volume);
  const int32_t maxv = static_cast<int32_t>(kVolumeMax);
  const int32_t denom = maxv * maxv;
  const int32_t gain_num = vol * vol;
  g_volume_gain_q15 = (gain_num * 32767 + (denom / 2)) / denom;
}

bool sinkI2sBegin(void* ctx, uint32_t sample_rate, uint8_t bits, bool stereo) {
  if (bits != 16) return false;

  auto* runtime = static_cast<I2sRuntime*>(ctx);
  if (runtime == nullptr) return false;

  if (runtime->tx != nullptr) {
    i2s_channel_disable(runtime->tx);
    i2s_del_channel(runtime->tx);
    runtime->tx = nullptr;
  }

  i2s_chan_config_t chan_cfg = {};
  chan_cfg.id = I2S_NUM_0;
  chan_cfg.role = I2S_ROLE_MASTER;
  chan_cfg.dma_desc_num = 8;
  chan_cfg.dma_frame_num = 256;
  chan_cfg.auto_clear = true;
  chan_cfg.intr_priority = 2;

  if (i2s_new_channel(&chan_cfg, &runtime->tx, nullptr) != ESP_OK) {
    runtime->tx = nullptr;
    return false;
  }

  i2s_std_config_t std_cfg = {};
  std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
  std_cfg.slot_cfg =
      I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  std_cfg.gpio_cfg.bclk = static_cast<gpio_num_t>(I2S_BCLK);
  std_cfg.gpio_cfg.ws = static_cast<gpio_num_t>(I2S_LRC);
  std_cfg.gpio_cfg.dout = static_cast<gpio_num_t>(I2S_DOUT);
  std_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
  std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
  std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
  std_cfg.gpio_cfg.invert_flags.ws_inv = false;

  if (i2s_channel_init_std_mode(runtime->tx, &std_cfg) != ESP_OK) {
    i2s_del_channel(runtime->tx);
    runtime->tx = nullptr;
    return false;
  }

  if (i2s_channel_enable(runtime->tx) != ESP_OK) {
    i2s_del_channel(runtime->tx);
    runtime->tx = nullptr;
    return false;
  }

  runtime->stereo_input = stereo;
  runtime->prebuffering = false;
  return true;
}

size_t sinkI2sAvailableForWrite(void* ctx) {
  auto* runtime = static_cast<I2sRuntime*>(ctx);
  if (runtime != nullptr && runtime->prebuffering) return 0;
  return kI2sWorkSamples;
}

size_t sinkI2sWrite(void* ctx, const int16_t* samples, size_t sample_count) {
  auto* runtime = static_cast<I2sRuntime*>(ctx);
  if (runtime == nullptr || runtime->tx == nullptr || samples == nullptr || sample_count == 0) {
    return 0;
  }

  size_t consumed_input_samples = 0;

  if (runtime->stereo_input) {
    while (consumed_input_samples < sample_count) {
      size_t chunk_samples = min(kI2sWorkSamples, sample_count - consumed_input_samples);
      if ((chunk_samples & 1u) != 0u && chunk_samples > 1) {
        --chunk_samples;
      }
      if (chunk_samples == 0) break;

      for (size_t i = 0; i < chunk_samples; ++i) {
        g_i2s_work_stereo[i] = applyVolumeToSample(samples[consumed_input_samples + i]);
      }

      size_t written_bytes = 0;
      const size_t total_bytes = chunk_samples * sizeof(int16_t);
      const esp_err_t err = i2s_channel_write(
          runtime->tx, g_i2s_work_stereo, total_bytes, &written_bytes, kI2sWriteTimeoutMs);
      if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        return consumed_input_samples;
      }

      const size_t written_samples = written_bytes / sizeof(int16_t);
      consumed_input_samples += written_samples;
      if (written_samples < chunk_samples) break;
    }

    return consumed_input_samples;
  }

  while (consumed_input_samples < sample_count) {
    const size_t chunk_input_samples = min(kI2sWorkSamples, sample_count - consumed_input_samples);
    for (size_t i = 0; i < chunk_input_samples; ++i) {
      const int16_t scaled = applyVolumeToSample(samples[consumed_input_samples + i]);
      g_i2s_work_mono_to_stereo[i * 2] = scaled;
      g_i2s_work_mono_to_stereo[i * 2 + 1] = scaled;
    }

    size_t written_bytes = 0;
    const size_t total_bytes = chunk_input_samples * 2 * sizeof(int16_t);
    const esp_err_t err = i2s_channel_write(
        runtime->tx, g_i2s_work_mono_to_stereo, total_bytes, &written_bytes, kI2sWriteTimeoutMs);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
      return consumed_input_samples;
    }

    const size_t written_output_samples = written_bytes / sizeof(int16_t);
    const size_t written_input_samples = written_output_samples / 2;
    consumed_input_samples += written_input_samples;
    if (written_input_samples < chunk_input_samples) break;
  }

  return consumed_input_samples;
}

void sinkI2sEnd(void* ctx) {
  auto* runtime = static_cast<I2sRuntime*>(ctx);
  if (runtime == nullptr || runtime->tx == nullptr) return;

  i2s_channel_disable(runtime->tx);
  i2s_del_channel(runtime->tx);
  runtime->tx = nullptr;
  runtime->prebuffering = false;
}

padre::I2sPcm5122Output g_sink({
    &g_i2s_runtime,
    sinkI2sBegin,
    sinkI2sAvailableForWrite,
    sinkI2sWrite,
    sinkI2sEnd,
}, padre::I2sOutputConfig{kSinkQueueSamples});

void IRAM_ATTR onMpr121Irq() { g_touch_irq_flag = true; }

bool hasSupportedExt(const String& path) {
  String lower = path;
  lower.toLowerCase();
  return lower.endsWith(".wav") || lower.endsWith(".mp3") || lower.endsWith(".flac");
}

void scanMusicDir(const String& dir_path, uint8_t depth_left) {
  File dir = SD.open(dir_path.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  File entry = dir.openNextFile();
  while (entry) {
    String path = entry.path();
    if (path.length() == 0) {
      path = dir_path;
      if (!path.endsWith("/")) path += "/";
      path += entry.name();
    }

    if (entry.isDirectory()) {
      if (depth_left > 0) scanMusicDir(path, static_cast<uint8_t>(depth_left - 1));
    } else if (hasSupportedExt(path)) {
      g_tracks.push_back(path);
    }

    entry.close();
    entry = dir.openNextFile();
  }

  dir.close();
}

void stopPlayback() {
  g_decoder.stop();
  g_audio_source.close();
}

bool startTrack(const String& path) {
  g_paused = false;
  stopPlayback();

  if (!g_audio_source.open(path)) {
    g_was_running = false;
    return false;
  }

  if (!g_decoder.begin(g_audio_source, g_sink, path)) {
    g_audio_source.close();
    g_was_running = false;
    return false;
  }

  g_i2s_runtime.prebuffering = true;
  const uint32_t prebuffer_start_us = micros();
  size_t stagnant_iters = 0;
  while (g_decoder.isRunning()) {
    if (g_sink.queuedSamples() >= kPrebufferMinSamples) break;
    if (static_cast<uint32_t>(micros() - prebuffer_start_us) >= kStartPrebufferBudgetUs) break;

    const size_t before = g_sink.queuedSamples();
    g_decoder.process(kStartReadsPerStep);
    if (g_sink.queuedSamples() == before) {
      ++stagnant_iters;
      if (stagnant_iters >= 2) break;
      delay(0);
      continue;
    }
    stagnant_iters = 0;
  }
  g_i2s_runtime.prebuffering = false;
  g_sink.pump();

  Serial.printf("Now playing: %s\n", path.c_str());
  g_retry_at_ms = 0;
  g_was_running = true;
  return true;
}

bool playCurrentTrack() {
  const String* track = g_playlist.current();
  if (track == nullptr) return false;
  return startTrack(*track);
}

bool playNextTrack() {
  if (g_tracks.empty()) return false;

  for (size_t attempt = 0; attempt < g_tracks.size(); ++attempt) {
    const String* track = g_playlist.next(true);
    if (track == nullptr) continue;
    if (startTrack(*track)) return true;
  }
  return false;
}

bool servicePlayback() {
  if (!g_decoder.isRunning()) return false;
  if (g_paused) return true;

  const uint32_t service_start_us = micros();
  uint32_t decode_iters = 0;
  bool hit_budget = false;
  perfNoteQueue(g_sink.queuedSamples());
  while (g_decoder.isRunning()) {
    if (static_cast<uint32_t>(micros() - service_start_us) >= kServiceDecodeBudgetUs) {
      hit_budget = true;
      break;
    }

    g_sink.pump();
    const uint32_t decode_start_us = micros();
    const size_t produced = g_decoder.process(kServiceReadsPerStep);
    const uint32_t decode_elapsed_us = static_cast<uint32_t>(micros() - decode_start_us);
    ++decode_iters;

    if (kPerfTelemetryEnabled) {
      ++g_perf.decode_calls;
      g_perf.decode_total_us += decode_elapsed_us;
      g_perf.decode_out_samples += produced;
      if (decode_elapsed_us > g_perf.decode_max_us) {
        g_perf.decode_max_us = decode_elapsed_us;
      }
      if (decode_elapsed_us >= kPerfSlowDecodeUs) ++g_perf.decode_slow;
      if (produced == 0) ++g_perf.decode_zero_out;
    }

    g_sink.pump();
    perfNoteQueue(g_sink.queuedSamples());

    if (g_sink.writableSamples() == 0) break;
  }

  if (kPerfTelemetryEnabled) {
    const uint32_t service_elapsed_us = static_cast<uint32_t>(micros() - service_start_us);
    ++g_perf.service_calls;
    g_perf.service_total_us += service_elapsed_us;
    g_perf.service_decode_iters += decode_iters;
    if (service_elapsed_us > g_perf.service_max_us) {
      g_perf.service_max_us = service_elapsed_us;
    }
    if (hit_budget) ++g_perf.service_budget_hits;
  }

  if (!g_decoder.isRunning()) {
    g_audio_source.close();
    return false;
  }

  return true;
}

void applyVolume() {
  updateVolumeGain();
  Serial.printf("Volume: %d\n", g_volume);
}

void handleTouchEvent(const padre::InputEvent& event) {
  if (event.type != padre::InputEventType::PressDown) return;

  if (event.source_id == 0) {
    if (g_decoder.isRunning()) {
      g_paused = !g_paused;
      Serial.println(g_paused ? "Paused" : "Resumed");
    }
    return;
  }

  if (event.source_id == 1) {
    perfNoteNextTouchRequest(millis());
    g_request_next_track = true;
    return;
  }

  if (event.source_id == 2) {
    const int next_volume = max<int>(kVolumeMin, g_volume - 1);
    if (next_volume != g_volume) {
      g_volume = next_volume;
      applyVolume();
    }
    return;
  }

  if (event.source_id == 3) {
    const int next_volume = min<int>(kVolumeMax, g_volume + 1);
    if (next_volume != g_volume) {
      g_volume = next_volume;
      applyVolume();
    }
  }
}

void pollTouchAndHandle(uint32_t now_ms) {
  const uint16_t new_mask = g_mpr121.touched();
  if (kTouchDebug && new_mask != g_touch_cache.mask) {
    Serial.printf("Touch mask: 0x%03X\n", static_cast<unsigned>(new_mask));
  }
  g_touch_cache.mask = new_mask;

  handleTouchEvent(g_touch0.update(now_ms));
  handleTouchEvent(g_touch1.update(now_ms));
  handleTouchEvent(g_touch2.update(now_ms));
  handleTouchEvent(g_touch3.update(now_ms));
}

bool initSd() {
  g_sd_spi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, g_sd_spi, 20000000)) {
    Serial.println("SD.begin failed");
    return false;
  }
  return true;
}

bool initMpr121() {
  pinMode(MPR121_IRQ, INPUT_PULLUP);
  Wire.begin(MPR121_SDA, MPR121_SCL);
  Wire.setClock(400000);

  Serial.println("I2C scan:");
  for (uint8_t addr = 1; addr < 127; ++addr) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf(" - 0x%02X\n", addr);
    }
  }

  const bool ok = g_mpr121.begin(MPR121_ADDR, &Wire, kTouchThreshold, kReleaseThreshold, true);
  if (!ok) {
    Serial.println("MPR121 init failed");
    return false;
  }
  attachInterrupt(digitalPinToInterrupt(MPR121_IRQ), onMpr121Irq, FALLING);
  return true;
}

void initPlaylist() {
  g_tracks.clear();
  scanMusicDir(kMusicDir, kMaxDirDepth);

  Serial.printf("Found %u audio file(s)\n", static_cast<unsigned>(g_tracks.size()));
  for (const auto& track : g_tracks) {
    Serial.printf(" - %s\n", track.c_str());
  }

  g_playlist.setOrder(padre::PlayOrder::Shuffle);
  g_playlist.setTracks(g_tracks);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("test-sd-mpr121");

  randomSeed(static_cast<uint32_t>(micros()));

  if (!initSd()) return;
  if (!initMpr121()) return;

  g_volume = kVolumeDefault;
  applyVolume();
  if (kPerfTelemetryEnabled) {
    g_perf.next_report_ms = millis() + kPerfReportMs;
  }

  initPlaylist();
  if (!g_tracks.empty() && !playCurrentTrack() && !playNextTrack()) {
    g_retry_at_ms = millis() + kRetryStartDelayMs;
  }
}

void loop() {
  const uint32_t loop_start_us = micros();
  const uint32_t now_ms = millis();
  const bool touch_irq = g_touch_irq_flag;
  if (touch_irq) g_touch_irq_flag = false;
  if (touch_irq || (now_ms - g_last_touch_poll_ms >= kTouchPollMs)) {
    pollTouchAndHandle(now_ms);
    g_last_touch_poll_ms = now_ms;
  }

  if (g_request_next_track) {
    g_request_next_track = false;
    const bool next_started = playNextTrack();
    perfNoteNextTouchHandled(now_ms);
    if (!next_started) {
      Serial.println("No next track available");
    }
  }

  const bool running = servicePlayback();
  if (g_was_running && !running && !g_paused) {
    if (!playNextTrack()) {
      g_retry_at_ms = now_ms + kRetryStartDelayMs;
    }
  }
  g_was_running = running;

  if (!running && !g_paused && g_retry_at_ms != 0 && now_ms >= g_retry_at_ms) {
    g_retry_at_ms = 0;
    if (!playNextTrack()) {
      g_retry_at_ms = now_ms + kRetryStartDelayMs;
    }
  }

  if (kPerfTelemetryEnabled) {
    const uint32_t loop_elapsed_us = static_cast<uint32_t>(micros() - loop_start_us);
    ++g_perf.loop_calls;
    g_perf.loop_total_us += loop_elapsed_us;
    if (loop_elapsed_us > g_perf.loop_max_us) {
      g_perf.loop_max_us = loop_elapsed_us;
    }
    if (loop_elapsed_us >= kPerfSlowLoopUs) ++g_perf.loop_slow;
    perfReportIfDue(now_ms);
  }

  delay(1);
}
