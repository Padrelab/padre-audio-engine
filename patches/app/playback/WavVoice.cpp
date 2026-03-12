#include "WavVoice.h"

#include <algorithm>
#include <cstring>

namespace padre {

namespace {

size_t alignVoiceStereoSamples(size_t sample_count) {
  return sample_count & ~static_cast<size_t>(1);
}

}  // namespace

WavVoice::WavVoice(const char* label,
                   fs::FS& fs,
                   const char* source_type_name,
                   WavVoiceConfig config)
    : label_(label),
      source_(fs, FsAudioSourceConfig{source_type_name}),
      config_(config) {}

void WavVoice::setTracks(const std::vector<String>& tracks) {
  stopCurrentStream();
  tracks_ = tracks;
  track_index_ = 0;
  active_track_ = String();
}

bool WavVoice::configure(uint32_t output_sample_rate) {
  output_sample_rate_ = output_sample_rate;
  return output_sample_rate_ > 0;
}

WavVoiceMode WavVoice::mode() const { return config_.mode; }

bool WavVoice::hasTracks() const { return !tracks_.empty(); }

const char* WavVoice::label() const { return label_; }

const String& WavVoice::activeTrack() const { return active_track_; }

bool WavVoice::isTrackRunning() const {
  return decoder_.isRunning() || (trigger_armed_ && config_.mode == WavVoiceMode::OneShot);
}

bool WavVoice::hasPendingNextRequest() const {
  return config_.mode == WavVoiceMode::Loop && pending_next_requests_ != 0;
}

bool WavVoice::canApplyPendingNextRequest(size_t queue_samples, uint32_t now_ms) const {
  if (config_.mode != WavVoiceMode::Loop || pending_next_requests_ == 0) return false;

  const uint32_t pending_age_ms = static_cast<uint32_t>(now_ms - pending_next_request_ms_);
  if (pending_age_ms < config_.track_switch_coalesce_ms) return false;
  if (queue_samples < config_.track_switch_min_queue_samples &&
      pending_age_ms < config_.track_switch_max_delay_ms) {
    return false;
  }
  return true;
}

size_t WavVoice::currentTrackIndex() const { return track_index_; }

size_t WavVoice::playlistSize() const { return tracks_.size(); }

size_t WavVoice::currentFilePosition() const { return source_.position(); }

size_t WavVoice::currentFileSize() const { return source_.size(); }

uint32_t WavVoice::outputSampleRate() const { return output_sample_rate_; }

void WavVoice::stop() {
  stopCurrentStream();
  pending_next_requests_ = 0;
  pending_next_request_ms_ = 0;
}

bool WavVoice::trigger() {
  if (tracks_.empty()) return false;

  stopCurrentStream();
  pending_next_requests_ = 0;
  pending_next_request_ms_ = 0;
  trigger_armed_ = true;
  return true;
}

bool WavVoice::selectTrackIndex(size_t track_index) {
  if (tracks_.empty() || track_index >= tracks_.size()) return false;

  stopCurrentStream();
  pending_next_requests_ = 0;
  pending_next_request_ms_ = 0;
  track_index_ = track_index;
  return true;
}

void WavVoice::requestNextTrack() {
  queueNextTracks(1, millis());
}

void WavVoice::queueNextTracks(size_t steps, uint32_t request_ms) {
  if (tracks_.empty() || steps == 0) return;

  if (config_.mode == WavVoiceMode::Loop) {
    pending_next_requests_ += steps;
    pending_next_request_ms_ = request_ms;
    return;
  }

  advanceTrack(steps);
}

void WavVoice::selectNextTracks(size_t steps) { advanceTrack(steps); }

bool WavVoice::servicePendingNextRequest(uint32_t now_ms,
                                         size_t queue_samples_at_switch,
                                         size_t trimmed_queue_samples) {
  (void)queue_samples_at_switch;
  (void)trimmed_queue_samples;

  if (config_.mode != WavVoiceMode::Loop || tracks_.empty() ||
      !canApplyPendingNextRequest(queue_samples_at_switch, now_ms)) {
    return false;
  }

  const size_t pending_steps = pending_next_requests_ % tracks_.size();
  pending_next_requests_ = 0;
  stopCurrentStream();
  if (pending_steps != 0) {
    advanceTrack(pending_steps);
  }
  return true;
}

size_t WavVoice::readSamples(int16_t* dst, size_t sample_count) {
  if (dst == nullptr || sample_count == 0 || output_sample_rate_ == 0 || tracks_.empty()) {
    return 0;
  }

  sample_count = alignVoiceStereoSamples(sample_count);
  const size_t pcm_capacity = std::max(config_.pcm_buffer_samples, sample_count);
  ensurePcmBufferCapacity(pcm_capacity);

  if (pcm_buffered_samples_ < sample_count) {
    refillPcmBuffer(pcm_capacity);
  } else if (pcm_buffered_samples_ < config_.pcm_low_water_samples) {
    refillPcmBuffer(pcm_capacity);
  }

  const size_t produced_total = std::min(sample_count, pcm_buffered_samples_);
  if (produced_total > 0) {
    memcpy(dst, pcm_buffer_.data(), produced_total * sizeof(int16_t));
    consumePcmBuffer(produced_total);
  }

  return produced_total;
}

bool WavVoice::eof() const {
  if (tracks_.empty()) return true;
  if (config_.mode == WavVoiceMode::Loop) return false;
  return !decoder_.isRunning() && !trigger_armed_ && pcm_buffered_samples_ == 0;
}

bool WavVoice::ensureTrackOpen() {
  if (decoder_.isRunning()) return true;
  if (tracks_.empty() || output_sample_rate_ == 0) return false;

  if (config_.mode == WavVoiceMode::OneShot) {
    if (!trigger_armed_) return false;
    if (openTrack(tracks_[track_index_])) return true;
    trigger_armed_ = false;
    return false;
  }

  const size_t track_count = tracks_.size();
  for (size_t attempt = 0; attempt < track_count; ++attempt) {
    if (openTrack(tracks_[track_index_])) return true;
    advanceTrack();
  }

  return false;
}

bool WavVoice::openTrack(const String& path) {
  decoder_.stop();
  source_.close();

  if (!source_.begin()) return false;
  if (!source_.open(path)) return false;
  if (!decoder_.begin(source_)) {
    source_.close();
    return false;
  }

  const WavStreamInfo& info = decoder_.streamInfo();
  if (info.sample_rate != output_sample_rate_) {
    decoder_.stop();
    source_.close();
    return false;
  }

  active_track_ = path;
  return true;
}

void WavVoice::advanceTrack(size_t steps) {
  if (tracks_.empty() || steps == 0) return;
  track_index_ = (track_index_ + steps) % tracks_.size();
}

bool WavVoice::ensurePcmBufferCapacity(size_t capacity_samples) {
  if (pcm_buffer_.size() >= capacity_samples) return true;
  pcm_buffer_.resize(capacity_samples);
  return pcm_buffer_.size() >= capacity_samples;
}

void WavVoice::clearPcmBuffer() { pcm_buffered_samples_ = 0; }

void WavVoice::consumePcmBuffer(size_t consumed_samples) {
  if (consumed_samples >= pcm_buffered_samples_) {
    pcm_buffered_samples_ = 0;
    return;
  }

  const size_t remain = pcm_buffered_samples_ - consumed_samples;
  memmove(pcm_buffer_.data(),
          pcm_buffer_.data() + consumed_samples,
          remain * sizeof(int16_t));
  pcm_buffered_samples_ = remain;
}

void WavVoice::refillPcmBuffer(size_t target_samples) {
  if (pcm_buffer_.empty()) return;

  const size_t capped_target = std::min(target_samples, pcm_buffer_.size());
  uint8_t attempts = 0;

  while (pcm_buffered_samples_ < capped_target && attempts < config_.pcm_max_refill_attempts) {
    const size_t free_capacity = pcm_buffer_.size() - pcm_buffered_samples_;
    if (free_capacity < 2) break;

    const size_t request =
        alignVoiceStereoSamples(std::min(free_capacity, config_.pcm_refill_chunk_samples));
    if (request < 2) break;

    const size_t produced_now = decodePcmSamples(pcm_buffer_.data() + pcm_buffered_samples_, request);
    if (produced_now == 0) break;

    pcm_buffered_samples_ += produced_now;
    ++attempts;
    if (produced_now < request) break;
  }
}

size_t WavVoice::decodePcmSamples(int16_t* dst, size_t sample_count) {
  if (dst == nullptr || sample_count == 0) return 0;

  sample_count = alignVoiceStereoSamples(sample_count);
  size_t produced_total = 0;

  while (produced_total < sample_count) {
    if (!ensureTrackOpen()) break;

    const WavStreamInfo& info = decoder_.streamInfo();
    size_t produced_now = 0;

    if (info.output_channels >= 2) {
      const size_t request = alignVoiceStereoSamples(sample_count - produced_total);
      produced_now = alignVoiceStereoSamples(decoder_.decode(dst + produced_total, request));
    } else {
      const size_t stereo_room = sample_count - produced_total;
      const size_t mono_capacity = stereo_room / 2;
      if (mono_capacity == 0) break;

      if (mono_decode_buffer_.size() < mono_capacity) {
        mono_decode_buffer_.resize(mono_capacity);
      }

      const size_t mono_samples = decoder_.decode(mono_decode_buffer_.data(), mono_capacity);
      for (size_t i = 0; i < mono_samples; ++i) {
        const int16_t sample = mono_decode_buffer_[i];
        dst[produced_total + (i * 2)] = sample;
        dst[produced_total + (i * 2) + 1] = sample;
      }
      produced_now = mono_samples * 2;
    }

    produced_total += produced_now;

    if (decoder_.isRunning()) {
      if (produced_now == 0) break;
      continue;
    }

    source_.close();
    if (config_.mode == WavVoiceMode::Loop) {
      advanceTrack();
      if (produced_now == 0) continue;
      continue;
    }

    trigger_armed_ = false;
    if (produced_now == 0) break;
    break;
  }

  return produced_total;
}

void WavVoice::stopCurrentStream() {
  decoder_.stop();
  source_.close();
  clearPcmBuffer();
  active_track_ = String();
  trigger_armed_ = false;
}

}  // namespace padre
