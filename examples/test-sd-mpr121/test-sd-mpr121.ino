#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <math.h>
#include <vector>
#include <driver/i2s_std.h>

#include <Adafruit_MPR121.h>
#include <Audio.h>

#include "../../patches/input/InputEvent.h"
#include "../../patches/io_mpr121/Mpr121Input.h"
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

SPIClass g_sd_spi(FSPI);
Audio* g_audio = nullptr;
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
uint32_t g_last_touch_poll_ms = 0;
uint32_t g_retry_at_ms = 0;
volatile bool g_touch_irq_flag = false;

constexpr size_t kWavConvInBufSize = 4096;
constexpr size_t kWavConvCarrySize = 512;
constexpr size_t kWavConvOutBufSize = 4096;

uint8_t g_wav_conv_in_buf[kWavConvInBufSize + kWavConvCarrySize] = {0};
uint8_t g_wav_conv_out_buf[kWavConvOutBufSize] = {0};

enum class PlaybackBackend {
  None = 0,
  AudioLib,
  WavCustom,
};

PlaybackBackend g_backend = PlaybackBackend::None;
i2s_chan_handle_t g_wav_i2s_tx = nullptr;
File g_wav_file;
uint32_t g_wav_remaining = 0;
size_t g_wav_carry = 0;
uint8_t g_wav_bytes_per_sample = 0;
bool g_wav_running = false;

void IRAM_ATTR onMpr121Irq() { g_touch_irq_flag = true; }

bool hasSupportedExt(const String& path) {
  String lower = path;
  lower.toLowerCase();
  if (lower.endsWith(".pcm16.wav")) return false;
  return lower.endsWith(".wav") || lower.endsWith(".mp3") || lower.endsWith(".flac");
}

uint16_t readLe16(const uint8_t* b) {
  return static_cast<uint16_t>(b[0]) |
         (static_cast<uint16_t>(b[1]) << 8);
}

uint32_t readLe32(const uint8_t* b) {
  return static_cast<uint32_t>(b[0]) |
         (static_cast<uint32_t>(b[1]) << 8) |
         (static_cast<uint32_t>(b[2]) << 16) |
         (static_cast<uint32_t>(b[3]) << 24);
}

struct WavMeta {
  bool valid = false;
  uint16_t format_code = 0;      // Raw WAV format tag.
  uint16_t codec_tag = 0;        // Effective codec (e.g. EXTENSIBLE subformat).
  uint16_t channels = 0;
  uint32_t sample_rate = 0;
  uint16_t bits_per_sample = 0;
  uint16_t block_align = 0;
  uint32_t data_offset = 0;
  uint32_t data_size = 0;
};

WavMeta g_wav_meta;

bool readWavMeta(const String& path, WavMeta& meta) {
  meta = {};

  String lower = path;
  lower.toLowerCase();
  if (!lower.endsWith(".wav")) return false;

  File f = SD.open(path.c_str(), FILE_READ);
  if (!f) return false;

  uint8_t riff[12] = {0};
  if (f.read(riff, sizeof(riff)) != static_cast<int>(sizeof(riff))) {
    f.close();
    return false;
  }
  if (memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
    f.close();
    return false;
  }

  bool have_fmt = false;
  bool have_data = false;
  while (f.available()) {
    uint8_t chunk[8] = {0};
    if (f.read(chunk, sizeof(chunk)) != static_cast<int>(sizeof(chunk))) break;

    const uint32_t chunk_size = readLe32(chunk + 4);
    if (memcmp(chunk, "fmt ", 4) == 0) {
      if (chunk_size < 16) break;

      uint8_t fmt[64] = {0};
      const size_t fmt_to_read = min<size_t>(chunk_size, sizeof(fmt));
      if (f.read(fmt, fmt_to_read) != static_cast<int>(fmt_to_read)) break;

      meta.format_code = readLe16(fmt + 0);
      meta.codec_tag = meta.format_code;
      meta.channels = readLe16(fmt + 2);
      meta.sample_rate = readLe32(fmt + 4);
      meta.bits_per_sample = readLe16(fmt + 14);
      meta.block_align = readLe16(fmt + 12);

      // WAVE_FORMAT_EXTENSIBLE: derive actual codec from SubFormat GUID tag.
      // GUID layout begins at offset 24; first 16-bit is effective codec tag.
      if (meta.format_code == 0xFFFE && fmt_to_read >= 40) {
        meta.codec_tag = readLe16(fmt + 24);
      }
      have_fmt = true;

      const uint32_t consumed = static_cast<uint32_t>(fmt_to_read);
      if (chunk_size > consumed) {
        f.seek(f.position() + (chunk_size - consumed));
      }
      if ((chunk_size & 1u) != 0u) {
        f.seek(f.position() + 1);
      }
      continue;
    }

    if (memcmp(chunk, "data", 4) == 0) {
      meta.data_offset = static_cast<uint32_t>(f.position());
      meta.data_size = chunk_size;
      have_data = true;
      break;
    }

    f.seek(f.position() + chunk_size + (chunk_size & 1u));
  }

  f.close();
  meta.valid = have_fmt && have_data;
  return meta.valid;
}

enum class WavCodecClass {
  Unsupported = 0,
  PcmInt,
  Float,
  ALaw,
  MuLaw,
};

WavCodecClass codecClass(const WavMeta& meta) {
  if (meta.codec_tag == 1) return WavCodecClass::PcmInt;
  if (meta.codec_tag == 3) return WavCodecClass::Float;
  if (meta.codec_tag == 6) return WavCodecClass::ALaw;
  if (meta.codec_tag == 7) return WavCodecClass::MuLaw;
  return WavCodecClass::Unsupported;
}

bool isCodecConvertible(const WavMeta& meta) {
  if (!meta.valid) return false;
  if (meta.channels == 0) return false;
  if (meta.sample_rate == 0) return false;
  if (meta.block_align == 0) return false;

  const auto cls = codecClass(meta);
  if (cls == WavCodecClass::PcmInt) {
    return meta.bits_per_sample == 8 || meta.bits_per_sample == 16 ||
           meta.bits_per_sample == 24 || meta.bits_per_sample == 32;
  }
  if (cls == WavCodecClass::Float) {
    return meta.bits_per_sample == 32 || meta.bits_per_sample == 64;
  }
  if (cls == WavCodecClass::ALaw || cls == WavCodecClass::MuLaw) {
    return meta.bits_per_sample == 8;
  }
  return false;
}

int16_t decodeALaw(uint8_t a_val) {
  a_val ^= 0x55;

  int16_t t = static_cast<int16_t>((a_val & 0x0F) << 4);
  const uint8_t seg = static_cast<uint8_t>((a_val & 0x70) >> 4);
  switch (seg) {
    case 0:
      t += 8;
      break;
    case 1:
      t += 0x108;
      break;
    default:
      t += 0x108;
      t <<= (seg - 1);
      break;
  }
  return (a_val & 0x80) ? t : static_cast<int16_t>(-t);
}

int16_t decodeMuLaw(uint8_t u_val) {
  u_val = static_cast<uint8_t>(~u_val);
  const int sign = (u_val & 0x80) ? -1 : 1;
  const int exponent = (u_val >> 4) & 0x07;
  const int mantissa = u_val & 0x0F;
  int sample = ((mantissa << 3) + 0x84) << exponent;
  sample -= 0x84;
  sample *= sign;
  if (sample > 32767) sample = 32767;
  if (sample < -32768) sample = -32768;
  return static_cast<int16_t>(sample);
}

int16_t decodeToPcm16(const uint8_t* src, const WavMeta& meta, uint8_t bytes_per_sample) {
  const auto cls = codecClass(meta);
  if (cls == WavCodecClass::PcmInt) {
    if (meta.bits_per_sample == 8 && bytes_per_sample >= 1) {
      const int32_t v = static_cast<int32_t>(src[0]) - 128;
      return static_cast<int16_t>(v << 8);
    }
    if (meta.bits_per_sample == 16 && bytes_per_sample >= 2) {
      const int16_t v = static_cast<int16_t>(readLe16(src));
      return v;
    }
    if (meta.bits_per_sample == 24 && bytes_per_sample >= 3) {
      int32_t v = static_cast<int32_t>(src[0]) |
                  (static_cast<int32_t>(src[1]) << 8) |
                  (static_cast<int32_t>(src[2]) << 16);
      if (v & 0x800000) v |= ~0xFFFFFF;
      return static_cast<int16_t>(v >> 8);
    }
    if (meta.bits_per_sample == 32 && bytes_per_sample >= 4) {
      int32_t v = static_cast<int32_t>(src[0]) |
                  (static_cast<int32_t>(src[1]) << 8) |
                  (static_cast<int32_t>(src[2]) << 16) |
                  (static_cast<int32_t>(src[3]) << 24);
      return static_cast<int16_t>(v >> 16);
    }
    return 0;
  }

  if (cls == WavCodecClass::Float) {
    float sample = 0.0f;
    if (meta.bits_per_sample == 32 && bytes_per_sample >= 4) {
      memcpy(&sample, src, sizeof(float));
    } else if (meta.bits_per_sample == 64 && bytes_per_sample >= 8) {
      double d = 0.0;
      memcpy(&d, src, sizeof(double));
      sample = static_cast<float>(d);
    } else {
      return 0;
    }

    if (!isfinite(sample)) sample = 0.0f;
    if (sample > 1.0f) sample = 1.0f;
    if (sample < -1.0f) sample = -1.0f;
    return static_cast<int16_t>(sample * 32767.0f);
  }

  if (cls == WavCodecClass::ALaw && bytes_per_sample >= 1) {
    return decodeALaw(src[0]);
  }

  if (cls == WavCodecClass::MuLaw && bytes_per_sample >= 1) {
    return decodeMuLaw(src[0]);
  }

  return 0;
}

bool isTrackSupported(const String& path) {
  String lower = path;
  lower.toLowerCase();
  if (!lower.endsWith(".wav")) return true;

  WavMeta meta;
  if (!readWavMeta(path, meta)) {
    Serial.printf("Skip broken WAV: %s\n", path.c_str());
    return false;
  }

  if (!isCodecConvertible(meta)) {
    Serial.printf("Skip unsupported WAV (fmt=%u codec=%u bits=%u ch=%u): %s\n",
                  static_cast<unsigned>(meta.format_code),
                  static_cast<unsigned>(meta.codec_tag),
                  static_cast<unsigned>(meta.bits_per_sample),
                  static_cast<unsigned>(meta.channels),
                  path.c_str());
    return false;
  }
  return true;
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
      if (isTrackSupported(path)) {
        g_tracks.push_back(path);
      }
    }

    entry.close();
    entry = dir.openNextFile();
  }

  dir.close();
}

void stopWavBackend() {
  if (g_wav_file) g_wav_file.close();
  if (g_wav_i2s_tx != nullptr) {
    i2s_channel_disable(g_wav_i2s_tx);
    i2s_del_channel(g_wav_i2s_tx);
    g_wav_i2s_tx = nullptr;
  }
  g_wav_meta = {};
  g_wav_remaining = 0;
  g_wav_carry = 0;
  g_wav_bytes_per_sample = 0;
  g_wav_running = false;
  if (g_backend == PlaybackBackend::WavCustom) {
    g_backend = PlaybackBackend::None;
  }
}

void stopAudioBackend() {
  if (g_audio == nullptr) return;
  g_audio->stopSong();
  delete g_audio;
  g_audio = nullptr;
  if (g_backend == PlaybackBackend::AudioLib) {
    g_backend = PlaybackBackend::None;
  }
}

bool ensureAudioBackend() {
  bool created = false;
  if (g_audio == nullptr) {
    g_audio = new Audio(I2S_NUM_0);
    if (g_audio == nullptr) return false;
    created = true;
  }
  if (!g_audio->setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT)) {
    if (created) {
      delete g_audio;
      g_audio = nullptr;
    }
    return false;
  }
  g_audio->setVolume(static_cast<uint8_t>(g_volume));
  return true;
}

int16_t applyVolumeToSample(int16_t sample) {
  const int32_t vol = static_cast<int32_t>(g_volume);
  const int32_t maxv = static_cast<int32_t>(kVolumeMax);
  const int32_t denom = maxv * maxv;
  const int32_t gain_num = vol * vol;
  const int32_t scaled = (static_cast<int32_t>(sample) * gain_num) / denom;
  if (scaled > 32767) return 32767;
  if (scaled < -32768) return -32768;
  return static_cast<int16_t>(scaled);
}

bool initWavI2s(uint32_t sample_rate) {
  stopWavBackend();

  i2s_chan_config_t chan_cfg = {};
  chan_cfg.id = I2S_NUM_0;
  chan_cfg.role = I2S_ROLE_MASTER;
  chan_cfg.dma_desc_num = 8;
  chan_cfg.dma_frame_num = 256;
  chan_cfg.auto_clear = true;
  chan_cfg.intr_priority = 2;

  if (i2s_new_channel(&chan_cfg, &g_wav_i2s_tx, nullptr) != ESP_OK) {
    g_wav_i2s_tx = nullptr;
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

  if (i2s_channel_init_std_mode(g_wav_i2s_tx, &std_cfg) != ESP_OK) {
    stopWavBackend();
    return false;
  }
  if (i2s_channel_enable(g_wav_i2s_tx) != ESP_OK) {
    stopWavBackend();
    return false;
  }
  return true;
}

bool startWavBackend(const String& path) {
  WavMeta meta;
  if (!readWavMeta(path, meta) || !isCodecConvertible(meta)) {
    return false;
  }

  File f = SD.open(path.c_str(), FILE_READ);
  if (!f) return false;
  if (!f.seek(meta.data_offset)) {
    f.close();
    return false;
  }

  if (!initWavI2s(meta.sample_rate)) {
    f.close();
    return false;
  }

  g_wav_file = f;
  g_wav_meta = meta;
  g_wav_remaining = meta.data_size;
  g_wav_carry = 0;
  g_wav_bytes_per_sample =
      static_cast<uint8_t>(max<uint16_t>(1, meta.block_align / meta.channels));
  g_wav_running = true;

  Serial.printf("Now playing (wav-custom): %s\n", path.c_str());
  Serial.printf("WAV cfg: fmt=%u codec=%u bits=%u ch=%u sr=%lu\n",
                static_cast<unsigned>(meta.format_code),
                static_cast<unsigned>(meta.codec_tag),
                static_cast<unsigned>(meta.bits_per_sample),
                static_cast<unsigned>(meta.channels),
                static_cast<unsigned long>(meta.sample_rate));
  return true;
}

bool processWavBackend() {
  if (!g_wav_running) return false;
  if (g_paused) return true;

  const size_t total_in_buf = kWavConvInBufSize + kWavConvCarrySize;
  const size_t room = total_in_buf - g_wav_carry;
  const size_t want = g_wav_remaining > 0 ? min<size_t>(g_wav_remaining, room) : 0;
  const int got = want > 0 ? g_wav_file.read(g_wav_conv_in_buf + g_wav_carry, want) : 0;
  if (got < 0) {
    stopWavBackend();
    return false;
  }
  if (want > 0 && got == 0) {
    stopWavBackend();
    return false;
  }

  const size_t available = g_wav_carry + static_cast<size_t>(got);
  const size_t frame_count = available / g_wav_meta.block_align;
  const size_t process_bytes = frame_count * g_wav_meta.block_align;

  size_t out_pos = 0;
  for (size_t fidx = 0; fidx < frame_count; ++fidx) {
    const uint8_t* frame = g_wav_conv_in_buf + (fidx * g_wav_meta.block_align);
    const int16_t left_raw = decodeToPcm16(frame, g_wav_meta, g_wav_bytes_per_sample);
    const int16_t left = applyVolumeToSample(left_raw);

    const uint8_t right_channel = g_wav_meta.channels >= 2 ? 1 : 0;
    const int16_t right_raw = decodeToPcm16(
        frame + (right_channel * g_wav_bytes_per_sample), g_wav_meta, g_wav_bytes_per_sample);
    const int16_t right = applyVolumeToSample(right_raw);

    g_wav_conv_out_buf[out_pos++] = static_cast<uint8_t>(left & 0xFF);
    g_wav_conv_out_buf[out_pos++] = static_cast<uint8_t>((left >> 8) & 0xFF);
    g_wav_conv_out_buf[out_pos++] = static_cast<uint8_t>(right & 0xFF);
    g_wav_conv_out_buf[out_pos++] = static_cast<uint8_t>((right >> 8) & 0xFF);

    if (out_pos + 4 >= kWavConvOutBufSize || fidx + 1 == frame_count) {
      size_t write_off = 0;
      while (write_off < out_pos) {
        size_t written = 0;
        const esp_err_t err = i2s_channel_write(
            g_wav_i2s_tx, g_wav_conv_out_buf + write_off, out_pos - write_off, &written, 20);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
          stopWavBackend();
          return false;
        }
        write_off += written;
        if (written == 0) delay(0);
      }
      out_pos = 0;
    }
  }

  g_wav_carry = available - process_bytes;
  if (g_wav_carry > 0) {
    memmove(g_wav_conv_in_buf, g_wav_conv_in_buf + process_bytes, g_wav_carry);
  }
  g_wav_remaining -= static_cast<uint32_t>(got > 0 ? got : 0);

  if (g_wav_remaining == 0 && g_wav_carry < g_wav_meta.block_align) {
    stopWavBackend();
    return false;
  }
  return true;
}

bool isWavPath(const String& path) {
  String lower = path;
  lower.toLowerCase();
  return lower.endsWith(".wav");
}

void applyVolume() {
  if (g_audio != nullptr) {
    g_audio->setVolume(static_cast<uint8_t>(g_volume));
  }
  Serial.printf("Volume: %d\n", g_volume);
}

bool startTrack(const String& path) {
  g_paused = false;

  if (isWavPath(path)) {
    stopAudioBackend();
    if (!startWavBackend(path)) {
      g_backend = PlaybackBackend::None;
      g_was_running = false;
      return false;
    }
    g_backend = PlaybackBackend::WavCustom;
    g_was_running = true;
    g_retry_at_ms = 0;
    return true;
  }

  stopWavBackend();
  if (!ensureAudioBackend()) {
    g_backend = PlaybackBackend::None;
    g_was_running = false;
    return false;
  }

  Serial.printf("Now playing: %s\n", path.c_str());
  const bool ok = g_audio->connecttoFS(SD, path.c_str());
  if (!ok) {
    Serial.printf("Failed to start: %s\n", path.c_str());
    g_backend = PlaybackBackend::None;
  } else {
    g_backend = PlaybackBackend::AudioLib;
    g_retry_at_ms = 0;
  }
  g_was_running = ok;
  return ok;
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
  if (g_backend == PlaybackBackend::WavCustom) {
    return processWavBackend();
  }
  if (g_backend == PlaybackBackend::AudioLib) {
    if (g_audio == nullptr) {
      g_backend = PlaybackBackend::None;
      return false;
    }
    g_audio->loop();
    return g_audio->isRunning();
  }
  return false;
}

void handleTouchEvent(const padre::InputEvent& event) {
  if (event.type != padre::InputEventType::PressDown) return;

  if (event.source_id == 0) {
    if (g_backend == PlaybackBackend::AudioLib && g_audio != nullptr &&
        g_audio->pauseResume()) {
      g_paused = !g_paused;
      Serial.println(g_paused ? "Paused" : "Resumed");
    } else if (g_backend == PlaybackBackend::WavCustom && g_wav_running) {
      g_paused = !g_paused;
      Serial.println(g_paused ? "Paused" : "Resumed");
    }
    return;
  }

  if (event.source_id == 1) {
    if (!playNextTrack()) {
      Serial.println("No next track available");
    }
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

void onAudioInfo(Audio::msg_t msg) {
  if (msg.e == Audio::evt_info || msg.e == Audio::evt_eof || msg.e == Audio::evt_log) {
    Serial.printf("%s: %s\n", msg.s, msg.msg);
  }
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
  Audio::audio_info_callback = onAudioInfo;

  if (!initSd()) return;
  if (!initMpr121()) return;

  g_volume = kVolumeDefault;
  applyVolume();

  initPlaylist();
  if (!g_tracks.empty() && !playCurrentTrack() && !playNextTrack()) {
    g_retry_at_ms = millis() + kRetryStartDelayMs;
  }
}

void loop() {
  const uint32_t now_ms = millis();
  const bool touch_irq = g_touch_irq_flag;
  if (touch_irq) g_touch_irq_flag = false;
  if (touch_irq || (now_ms - g_last_touch_poll_ms >= kTouchPollMs)) {
    pollTouchAndHandle(now_ms);
    g_last_touch_poll_ms = now_ms;
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

  delay(1);
}
