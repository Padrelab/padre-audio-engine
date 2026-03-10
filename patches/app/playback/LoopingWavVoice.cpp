#include "LoopingWavVoice.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace padre {

namespace {

size_t alignVoiceStereoSamples(size_t sample_count) {
  return sample_count & ~static_cast<size_t>(1);
}

}  // namespace

void LoopingWavVoiceDurationStats::add(uint32_t elapsed_us) {
  ++count;
  total_us += elapsed_us;
  if (elapsed_us > max_us) max_us = elapsed_us;
}

uint32_t LoopingWavVoiceDurationStats::avgUs() const {
  return count == 0 ? 0 : static_cast<uint32_t>(total_us / count);
}

void LoopingWavVoiceDurationStats::reset() {
  count = 0;
  total_us = 0;
  max_us = 0;
}

LoopingWavVoice::LoopingWavVoice(const char* label,
                                 fs::FS& fs,
                                 const char* source_type_name,
                                 LoopingWavVoiceConfig config,
                                 Print* log)
    : label_(label),
      source_(fs, FsAudioSourceConfig{source_type_name}),
      config_(config),
      log_(log) {
  syncDebugSnapshot();
}

void LoopingWavVoice::setLog(Print* log) { log_ = log; }

void LoopingWavVoice::setTracks(const std::vector<String>& tracks) {
  stop();
  tracks_ = tracks;
  track_index_ = 0;
  syncDebugSnapshot();
}

bool LoopingWavVoice::configure(uint32_t output_sample_rate) {
  output_sample_rate_ = output_sample_rate;
  return output_sample_rate_ > 0;
}

bool LoopingWavVoice::hasTracks() const { return !tracks_.empty(); }

const char* LoopingWavVoice::label() const { return label_; }

const LoopingWavVoice::Stats& LoopingWavVoice::stats() const { return stats_; }

const String& LoopingWavVoice::activeTrack() const { return active_track_; }

bool LoopingWavVoice::isTrackRunning() const { return decoder_.isRunning(); }

bool LoopingWavVoice::hasPendingNextRequest() const { return pending_next_requests_ != 0; }

bool LoopingWavVoice::canApplyPendingNextRequest(size_t queue_samples, uint32_t now_ms) const {
  if (pending_next_requests_ == 0) return false;

  const uint32_t pending_age_ms = static_cast<uint32_t>(now_ms - pending_next_request_ms_);
  if (pending_age_ms < config_.track_switch_coalesce_ms) return false;
  if (queue_samples < config_.track_switch_min_queue_samples &&
      pending_age_ms < config_.track_switch_max_delay_ms) {
    return false;
  }
  return true;
}

size_t LoopingWavVoice::currentTrackIndex() const { return track_index_; }

size_t LoopingWavVoice::playlistSize() const { return tracks_.size(); }

size_t LoopingWavVoice::currentFilePosition() const { return source_.position(); }

size_t LoopingWavVoice::currentFileSize() const { return source_.size(); }

uint32_t LoopingWavVoice::outputSampleRate() const { return output_sample_rate_; }

void LoopingWavVoice::stop() {
  decoder_.stop();
  closeSourceTimed();
  track_index_ = 0;
  active_track_ = String();
  pending_next_requests_ = 0;
  pending_next_request_ms_ = 0;
  clearPcmBuffer();
  syncDebugSnapshot();
}

void LoopingWavVoice::requestNextTrack() {
  queueNextTracks(1, millis());
}

void LoopingWavVoice::queueNextTracks(size_t steps, uint32_t request_ms) {
  if (tracks_.empty() || steps == 0) return;

  stats_.manual_next_requests += static_cast<uint32_t>(steps);
  pending_next_requests_ += steps;
  pending_next_request_ms_ = request_ms;
  syncDebugSnapshot();
}

bool LoopingWavVoice::servicePendingNextRequest(uint32_t now_ms,
                                                size_t queue_samples_at_switch,
                                                size_t trimmed_queue_samples) {
  if (!canApplyPendingNextRequest(queue_samples_at_switch, now_ms)) return false;

  const uint32_t pending_age_ms = static_cast<uint32_t>(now_ms - pending_next_request_ms_);
  const uint32_t request_start_us = micros();
  const size_t requested_steps = pending_next_requests_;
  const size_t pending_steps = requested_steps % tracks_.size();
  pending_next_requests_ = 0;
  decoder_.stop();
  closeSourceTimed();
  clearPcmBuffer();
  if (pending_steps != 0) {
    advanceTrack(pending_steps);
  }
  const uint32_t elapsed_us = static_cast<uint32_t>(micros() - request_start_us);
  stats_.request_next_us.add(elapsed_us);
  stats_.request_next_last_us = elapsed_us;
  stats_.request_next_age_last_ms = pending_age_ms;
  if (pending_age_ms > stats_.request_next_age_max_ms) {
    stats_.request_next_age_max_ms = pending_age_ms;
  }
  stats_.request_next_queue_last_samples = queue_samples_at_switch;
  if (queue_samples_at_switch > stats_.request_next_queue_max_samples) {
    stats_.request_next_queue_max_samples = queue_samples_at_switch;
  }
  stats_.request_next_trimmed_last_samples = trimmed_queue_samples;
  if (trimmed_queue_samples > stats_.request_next_trimmed_max_samples) {
    stats_.request_next_trimmed_max_samples = trimmed_queue_samples;
  }
  syncDebugSnapshot();
  return true;
}

void LoopingWavVoice::copyDebugSnapshot(DebugSnapshot& out) const {
  portENTER_CRITICAL(&debug_snapshot_mux_);
  out = debug_snapshot_;
  portEXIT_CRITICAL(&debug_snapshot_mux_);
}

size_t LoopingWavVoice::readSamples(int16_t* dst, size_t sample_count) {
  const uint32_t read_start_us = micros();

  if (dst == nullptr || sample_count == 0 || output_sample_rate_ == 0 || tracks_.empty()) {
    const uint32_t elapsed_us = static_cast<uint32_t>(micros() - read_start_us);
    noteReadStats(0, elapsed_us);
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

  if (produced_total < sample_count && (decoder_.isRunning() || hasTracks())) {
    ++stats_.short_reads;
  }

  const uint32_t elapsed_us = static_cast<uint32_t>(micros() - read_start_us);
  noteReadStats(produced_total, elapsed_us);
  syncDebugSnapshot();
  return produced_total;
}

bool LoopingWavVoice::eof() const { return tracks_.empty(); }

bool LoopingWavVoice::ensureTrackOpen() {
  if (decoder_.isRunning()) return true;
  if (tracks_.empty() || output_sample_rate_ == 0) return false;

  const size_t track_count = tracks_.size();
  for (size_t attempt = 0; attempt < track_count; ++attempt) {
    if (openTrack(tracks_[track_index_])) return true;
    advanceTrack();
  }

  return false;
}

bool LoopingWavVoice::openTrack(const String& path) {
  ++stats_.track_open_attempts;
  decoder_.stop();
  closeSourceTimed();

  if (!source_.begin()) {
    ++stats_.source_begin_failures;
    logf("[%s] source init failed\n", label_);
    return false;
  }

  const uint32_t open_start_us = micros();
  const bool open_ok = source_.open(path);
  const uint32_t open_elapsed_us = static_cast<uint32_t>(micros() - open_start_us);
  stats_.open_us.add(open_elapsed_us);
  stats_.open_last_us = open_elapsed_us;

  if (!open_ok) {
    ++stats_.open_failures;
    logf("[%s] open failed: %s\n", label_, path.c_str());
    return false;
  }

  const uint32_t decoder_begin_start_us = micros();
  const bool decoder_begin_ok = decoder_.begin(source_);
  const uint32_t decoder_begin_elapsed_us =
      static_cast<uint32_t>(micros() - decoder_begin_start_us);
  stats_.decoder_begin_us.add(decoder_begin_elapsed_us);
  stats_.decoder_begin_last_us = decoder_begin_elapsed_us;

  if (!decoder_begin_ok) {
    ++stats_.invalid_wav;
    logf("[%s] invalid WAV: %s\n", label_, path.c_str());
    closeSourceTimed();
    return false;
  }

  const WavStreamInfo& info = decoder_.streamInfo();
  if (info.sample_rate != output_sample_rate_) {
    ++stats_.sample_rate_mismatches;
    logf("[%s] sample rate mismatch: %s (%lu Hz)\n",
         label_,
         path.c_str(),
         static_cast<unsigned long>(info.sample_rate));
    decoder_.stop();
    closeSourceTimed();
    return false;
  }

  ++stats_.track_opened;
  stats_.last_track_start_ms = millis();
  stats_.last_track_sample_rate = info.sample_rate;
  stats_.last_track_channels = info.output_channels;
  if (info.output_channels >= 2) {
    ++stats_.stereo_tracks;
  } else {
    ++stats_.mono_tracks;
  }

  active_track_ = path;
  if (config_.runtime_action_logs_enabled) {
    logf("[%s] now playing: %s\n", label_, active_track_.c_str());
  }
  syncDebugSnapshot();
  return true;
}

void LoopingWavVoice::advanceTrack(size_t steps) {
  if (tracks_.empty() || steps == 0) return;

  const size_t track_count = tracks_.size();
  const size_t wrapped_index = track_index_ + steps;
  const size_t wraps = wrapped_index / track_count;
  if ((wrapped_index % track_count) == 0 && wrapped_index != 0) {
    ++stats_.playlist_wraps;
    if (wraps > 1) stats_.playlist_wraps += static_cast<uint32_t>(wraps - 1);
  } else if (wraps > 0) {
    stats_.playlist_wraps += static_cast<uint32_t>(wraps);
  }
  track_index_ = wrapped_index % track_count;
}

void LoopingWavVoice::noteReadStats(size_t produced_samples, uint32_t elapsed_us) {
  ++stats_.read_calls;
  stats_.read_total_us += elapsed_us;
  stats_.last_read_us = elapsed_us;
  stats_.last_read_samples = produced_samples;
  stats_.buffer_last_samples = pcm_buffered_samples_;
  if (pcm_buffered_samples_ > stats_.buffer_peak_samples) {
    stats_.buffer_peak_samples = pcm_buffered_samples_;
  }
  stats_.produced_samples += produced_samples;
  if (elapsed_us > stats_.read_max_us) stats_.read_max_us = elapsed_us;
  if (elapsed_us >= config_.slow_read_threshold_us) ++stats_.slow_reads;

  if (produced_samples == 0) {
    ++stats_.zero_reads;
    ++stats_.consecutive_zero_reads;
  } else {
    stats_.consecutive_zero_reads = 0;
  }
}

void LoopingWavVoice::closeSourceTimed() {
  if (!source_.isOpen()) return;

  const uint32_t close_start_us = micros();
  source_.close();
  const uint32_t close_elapsed_us = static_cast<uint32_t>(micros() - close_start_us);
  stats_.close_us.add(close_elapsed_us);
  stats_.close_last_us = close_elapsed_us;
}

bool LoopingWavVoice::ensurePcmBufferCapacity(size_t capacity_samples) {
  if (pcm_buffer_.size() >= capacity_samples) return true;
  pcm_buffer_.resize(capacity_samples);
  return pcm_buffer_.size() >= capacity_samples;
}

void LoopingWavVoice::clearPcmBuffer() {
  pcm_buffered_samples_ = 0;
  stats_.buffer_last_samples = 0;
}

void LoopingWavVoice::consumePcmBuffer(size_t consumed_samples) {
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

void LoopingWavVoice::refillPcmBuffer(size_t target_samples) {
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
    if (pcm_buffered_samples_ > stats_.buffer_peak_samples) {
      stats_.buffer_peak_samples = pcm_buffered_samples_;
    }

    ++attempts;
    if (produced_now < request) break;
  }
}

size_t LoopingWavVoice::decodePcmSamples(int16_t* dst, size_t sample_count) {
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

    closeSourceTimed();
    ++stats_.track_ended;
    advanceTrack();

    if (produced_now == 0) continue;
  }

  return produced_total;
}

void LoopingWavVoice::syncDebugSnapshot() {
  DebugSnapshot snapshot;
  snapshot.stats = stats_;
  snapshot.current_track_index = track_index_;
  snapshot.playlist_size = tracks_.size();
  snapshot.current_file_position = source_.position();
  snapshot.current_file_size = source_.size();
  snapshot.is_track_running = decoder_.isRunning();
  if (active_track_.length() > 0) {
    snprintf(snapshot.active_track, sizeof(snapshot.active_track), "%s", active_track_.c_str());
  } else {
    snapshot.active_track[0] = '\0';
  }

  portENTER_CRITICAL(&debug_snapshot_mux_);
  debug_snapshot_ = snapshot;
  portEXIT_CRITICAL(&debug_snapshot_mux_);
}

void LoopingWavVoice::logf(const char* format, ...) const {
  if (log_ == nullptr || format == nullptr) return;

  char buffer[192];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  log_->print(buffer);
}

}  // namespace padre
