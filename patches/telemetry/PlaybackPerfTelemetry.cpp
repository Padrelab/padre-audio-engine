#include "PlaybackPerfTelemetry.h"

namespace padre {

PlaybackPerfTelemetry::PlaybackPerfTelemetry(
    Print& out,
    const PlaybackPerfTelemetryConfig& config)
    : out_(&out), config_(config) {
  reset();
}

void PlaybackPerfTelemetry::setConfig(const PlaybackPerfTelemetryConfig& config) {
  config_ = config;
}

const PlaybackPerfTelemetryConfig& PlaybackPerfTelemetry::config() const {
  return config_;
}

void PlaybackPerfTelemetry::reset(uint32_t now_ms) {
  metrics_ = {};
  next_report_ms_ = 0;
  if (config_.enabled && config_.report_interval_ms > 0) {
    next_report_ms_ = now_ms + config_.report_interval_ms;
  }
}

void PlaybackPerfTelemetry::noteQueue(size_t queued_samples) {
  if (!config_.enabled) return;

  if (queued_samples < metrics_.queue_min_samples) {
    metrics_.queue_min_samples = queued_samples;
  }
  if (queued_samples <= config_.queue_low_samples) ++metrics_.queue_low_events;
  if (queued_samples == 0) ++metrics_.queue_empty_events;
}

void PlaybackPerfTelemetry::noteNextTouchRequest(uint32_t now_ms) {
  if (!config_.enabled) return;
  metrics_.next_touch_pending = true;
  metrics_.next_touch_requested_ms = now_ms;
}

void PlaybackPerfTelemetry::noteNextTouchHandled(uint32_t now_ms) {
  if (!config_.enabled || !metrics_.next_touch_pending) return;

  const uint32_t latency_ms = now_ms - metrics_.next_touch_requested_ms;
  metrics_.next_touch_pending = false;
  ++metrics_.next_touch_count;
  metrics_.next_touch_latency_total_ms += latency_ms;
  if (latency_ms > metrics_.next_touch_latency_max_ms) {
    metrics_.next_touch_latency_max_ms = latency_ms;
  }
}

void PlaybackPerfTelemetry::onServiceBegin(size_t queued_samples) {
  noteQueue(queued_samples);
}

void PlaybackPerfTelemetry::onDecodeIteration(uint32_t decode_elapsed_us,
                                              size_t produced_samples,
                                              size_t queued_samples) {
  noteQueue(queued_samples);
  if (!config_.enabled) return;

  ++metrics_.decode_calls;
  metrics_.decode_total_us += decode_elapsed_us;
  metrics_.decode_out_samples += produced_samples;
  if (decode_elapsed_us > metrics_.decode_max_us) {
    metrics_.decode_max_us = decode_elapsed_us;
  }
  if (decode_elapsed_us >= config_.slow_decode_us) ++metrics_.decode_slow;
  if (produced_samples == 0) ++metrics_.decode_zero_out;
}

void PlaybackPerfTelemetry::onServiceEnd(uint32_t service_elapsed_us,
                                         uint32_t decode_iterations,
                                         bool hit_budget) {
  if (!config_.enabled) return;

  ++metrics_.service_calls;
  metrics_.service_total_us += service_elapsed_us;
  metrics_.service_decode_iters += decode_iterations;
  if (service_elapsed_us > metrics_.service_max_us) {
    metrics_.service_max_us = service_elapsed_us;
  }
  if (hit_budget) ++metrics_.service_budget_hits;
}

void PlaybackPerfTelemetry::onLoop(uint32_t loop_elapsed_us, uint32_t now_ms) {
  if (!config_.enabled) return;

  ++metrics_.loop_calls;
  metrics_.loop_total_us += loop_elapsed_us;
  if (loop_elapsed_us > metrics_.loop_max_us) {
    metrics_.loop_max_us = loop_elapsed_us;
  }
  if (loop_elapsed_us >= config_.slow_loop_us) ++metrics_.loop_slow;
  reportIfDue(now_ms);
}

void PlaybackPerfTelemetry::reportIfDue(uint32_t now_ms) {
  if (!config_.enabled || out_ == nullptr) return;

  if (next_report_ms_ == 0) {
    next_report_ms_ = now_ms + config_.report_interval_ms;
    return;
  }
  if (now_ms < next_report_ms_) return;

  reportNow(now_ms);
}

void PlaybackPerfTelemetry::reportNow(uint32_t now_ms) {
  const uint32_t loop_avg_us =
      metrics_.loop_calls == 0
          ? 0
          : static_cast<uint32_t>(metrics_.loop_total_us / metrics_.loop_calls);
  const uint32_t service_avg_us =
      metrics_.service_calls == 0
          ? 0
          : static_cast<uint32_t>(metrics_.service_total_us / metrics_.service_calls);
  const uint32_t decode_avg_us =
      metrics_.decode_calls == 0
          ? 0
          : static_cast<uint32_t>(metrics_.decode_total_us / metrics_.decode_calls);
  const uint32_t decode_iters_per_service =
      metrics_.service_calls == 0
          ? 0
          : static_cast<uint32_t>(metrics_.service_decode_iters / metrics_.service_calls);
  const uint32_t next_touch_avg_ms =
      metrics_.next_touch_count == 0
          ? 0
          : static_cast<uint32_t>(metrics_.next_touch_latency_total_ms /
                                  metrics_.next_touch_count);
  const uint32_t queue_min =
      metrics_.queue_min_samples == static_cast<size_t>(-1)
          ? 0
          : static_cast<uint32_t>(metrics_.queue_min_samples);

  out_->printf(
      "PERF loop %lu/%luus slow=%lu | svc %lu/%luus it=%lu budget=%lu | dec %lu/%luus "
      "slow=%lu out=%llu zero=%lu | qmin=%lu low=%lu empty=%lu\n",
      static_cast<unsigned long>(loop_avg_us),
      static_cast<unsigned long>(metrics_.loop_max_us),
      static_cast<unsigned long>(metrics_.loop_slow),
      static_cast<unsigned long>(service_avg_us),
      static_cast<unsigned long>(metrics_.service_max_us),
      static_cast<unsigned long>(decode_iters_per_service),
      static_cast<unsigned long>(metrics_.service_budget_hits),
      static_cast<unsigned long>(decode_avg_us),
      static_cast<unsigned long>(metrics_.decode_max_us),
      static_cast<unsigned long>(metrics_.decode_slow),
      static_cast<unsigned long long>(metrics_.decode_out_samples),
      static_cast<unsigned long>(metrics_.decode_zero_out),
      static_cast<unsigned long>(queue_min),
      static_cast<unsigned long>(metrics_.queue_low_events),
      static_cast<unsigned long>(metrics_.queue_empty_events));

  if (metrics_.next_touch_count > 0 || metrics_.next_touch_pending) {
    out_->printf(
        "PERF touch-next count=%lu avg/max=%lu/%lums pending=%s\n",
        static_cast<unsigned long>(metrics_.next_touch_count),
        static_cast<unsigned long>(next_touch_avg_ms),
        static_cast<unsigned long>(metrics_.next_touch_latency_max_ms),
        metrics_.next_touch_pending ? "yes" : "no");
  }

  const bool pending = metrics_.next_touch_pending;
  const uint32_t pending_since_ms = metrics_.next_touch_requested_ms;
  metrics_ = {};
  metrics_.next_touch_pending = pending;
  metrics_.next_touch_requested_ms = pending_since_ms;
  next_report_ms_ = now_ms + config_.report_interval_ms;
}

}  // namespace padre
