#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

namespace padre {

struct PlaybackPerfTelemetryConfig {
  bool enabled = false;
  uint32_t report_interval_ms = 2000;
  uint32_t slow_loop_us = 5000;
  uint32_t slow_decode_us = 1200;
  size_t queue_low_samples = 4096;
};

class PlaybackPerfTelemetry {
 public:
  explicit PlaybackPerfTelemetry(Print& out,
                                 const PlaybackPerfTelemetryConfig& config = {});

  void setConfig(const PlaybackPerfTelemetryConfig& config);
  const PlaybackPerfTelemetryConfig& config() const;

  void reset(uint32_t now_ms = 0);

  void noteQueue(size_t queued_samples);
  void noteNextTouchRequest(uint32_t now_ms);
  void noteNextTouchHandled(uint32_t now_ms);

  void onServiceBegin(size_t queued_samples);
  void onDecodeIteration(uint32_t decode_elapsed_us,
                         size_t produced_samples,
                         size_t queued_samples);
  void onServiceEnd(uint32_t service_elapsed_us,
                    uint32_t decode_iterations,
                    bool hit_budget);
  void onLoop(uint32_t loop_elapsed_us, uint32_t now_ms);

  void reportIfDue(uint32_t now_ms);

 private:
  struct Metrics {
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

  void reportNow(uint32_t now_ms);

  Print* out_ = nullptr;
  PlaybackPerfTelemetryConfig config_;
  Metrics metrics_;
  uint32_t next_report_ms_ = 0;
};

}  // namespace padre
