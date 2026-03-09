#pragma once

#include <Arduino.h>

namespace padre {

struct RuntimeDurationStats {
  uint32_t count = 0;
  uint64_t total_us = 0;
  uint32_t max_us = 0;

  void add(uint32_t elapsed_us);
  uint32_t avgUs() const;
  void reset();
};

struct RuntimeSampleStats {
  uint32_t count = 0;
  uint64_t total = 0;
  size_t min_value = SIZE_MAX;
  size_t max_value = 0;
  size_t last_value = 0;

  void add(size_t value);
  size_t avg() const;
  size_t minValue() const;
  void reset();
};

struct DualWavLoopRuntimeDiagnosticsConfig {
  uint32_t slow_loop_us = 10000;
  uint32_t slow_service_us = 10000;
  uint32_t slow_pump_us = 2000;
  uint32_t slow_report_us = 50000;
  uint32_t report_cooldown_ms = 250;
  size_t queue_low_samples = 8192;
  size_t queue_rearm_samples = 24576;
};

struct DualWavLoopRuntimeDiagnostics {
  explicit DualWavLoopRuntimeDiagnostics(DualWavLoopRuntimeDiagnosticsConfig cfg = {});

  void noteQueue(size_t queued_samples);
  void noteLoop(uint32_t elapsed_us);
  void noteService(uint32_t elapsed_us);
  void notePump(size_t queued_before, size_t pumped_samples, uint32_t elapsed_us);
  void noteWrite(size_t requested_samples, size_t written_samples_now);
  void noteMix(size_t mixed_samples_now);
  void noteRefill(uint32_t iterations, bool made_progress);
  void noteServiceBudgetHit();
  void noteTouchPoll(uint32_t now_ms);
  void noteTouchEvent(uint8_t electrode);
  void noteStartupPrefill(uint32_t elapsed_ms, size_t queued_samples);
  void noteReport(uint32_t elapsed_us);

  bool snapshotPending() const;
  const char* snapshotEventName() const;
  size_t snapshotTriggerQueueMin() const;
  bool reportDue(uint32_t now_ms, bool force) const;
  void clearSnapshotPending();
  void resetWindow();

  DualWavLoopRuntimeDiagnosticsConfig config;

  RuntimeDurationStats loop_us;
  RuntimeDurationStats service_us;
  RuntimeDurationStats pump_us;
  RuntimeSampleStats queue_samples;

  uint32_t loop_slow = 0;
  uint32_t service_slow = 0;
  uint32_t pump_slow = 0;

  uint32_t queue_low_events = 0;
  uint32_t queue_low_events_total = 0;
  uint32_t queue_empty_events = 0;
  uint32_t queue_empty_events_total = 0;
  bool snapshot_low_pending = false;
  bool snapshot_empty_pending = false;
  bool snapshot_budget_pending = false;
  bool snapshot_service_pending = false;
  size_t snapshot_trigger_queue_min = SIZE_MAX;
  bool snapshot_rearm_wait = false;

  uint32_t pump_calls = 0;
  uint32_t pump_calls_total = 0;
  uint64_t pump_samples = 0;
  uint64_t pump_samples_total = 0;
  uint32_t pump_zero_with_data = 0;
  uint32_t pump_zero_with_data_total = 0;

  uint32_t write_calls = 0;
  uint32_t write_calls_total = 0;
  uint64_t written_samples = 0;
  uint64_t written_samples_total = 0;
  uint32_t short_writes = 0;
  uint32_t short_writes_total = 0;
  uint32_t zero_writes = 0;
  uint32_t zero_writes_total = 0;

  uint32_t mix_calls = 0;
  uint32_t mix_calls_total = 0;
  uint64_t mixed_samples = 0;
  uint64_t mixed_samples_total = 0;
  uint32_t mix_zero = 0;
  uint32_t mix_zero_total = 0;

  uint32_t refill_loops = 0;
  uint32_t refill_loops_total = 0;
  uint32_t refill_iterations = 0;
  uint32_t refill_iterations_total = 0;
  uint32_t refill_peak_iterations = 0;
  uint32_t refill_no_progress = 0;
  uint32_t refill_no_progress_total = 0;
  uint32_t service_budget_hits = 0;
  uint32_t service_budget_hits_total = 0;

  uint32_t touch_polls = 0;
  uint32_t touch_polls_total = 0;
  uint32_t touch_events = 0;
  uint32_t touch_events_total = 0;
  uint32_t touch_event_counts[4] = {0, 0, 0, 0};
  uint32_t touch_event_counts_total[4] = {0, 0, 0, 0};
  uint32_t max_touch_poll_gap_ms = 0;
  uint32_t last_touch_poll_ms = 0;

  size_t startup_prefill_samples = 0;
  uint32_t startup_prefill_ms = 0;
  bool startup_prefill_done = false;

  uint32_t last_report_ms = 0;
  bool queue_low_active = false;
  bool queue_empty_active = false;
  RuntimeDurationStats report_us;
  uint32_t report_last_us = 0;
  uint32_t report_slow = 0;
  uint32_t reports = 0;
  uint32_t reports_total = 0;
};

}  // namespace padre
