#include "DualWavLoopRuntimeDiagnostics.h"

namespace padre {

void RuntimeDurationStats::add(uint32_t elapsed_us) {
  ++count;
  total_us += elapsed_us;
  if (elapsed_us > max_us) max_us = elapsed_us;
}

uint32_t RuntimeDurationStats::avgUs() const {
  return count == 0 ? 0 : static_cast<uint32_t>(total_us / count);
}

void RuntimeDurationStats::reset() {
  count = 0;
  total_us = 0;
  max_us = 0;
}

void RuntimeSampleStats::add(size_t value) {
  last_value = value;
  ++count;
  total += value;
  if (count == 1 || value < min_value) min_value = value;
  if (value > max_value) max_value = value;
}

size_t RuntimeSampleStats::avg() const {
  return count == 0 ? 0 : static_cast<size_t>(total / count);
}

size_t RuntimeSampleStats::minValue() const {
  return count == 0 ? 0 : min_value;
}

void RuntimeSampleStats::reset() {
  count = 0;
  total = 0;
  min_value = SIZE_MAX;
  max_value = 0;
  last_value = 0;
}

DualWavLoopRuntimeDiagnostics::DualWavLoopRuntimeDiagnostics(DualWavLoopRuntimeDiagnosticsConfig cfg)
    : config(cfg) {}

void DualWavLoopRuntimeDiagnostics::noteQueue(size_t queued_samples) {
  queue_samples.add(queued_samples);

  if (queued_samples >= config.queue_rearm_samples) {
    snapshot_rearm_wait = false;
  }

  const bool is_low = queued_samples <= config.queue_low_samples;
  if (!snapshot_rearm_wait && is_low && !queue_low_active) {
    ++queue_low_events;
    ++queue_low_events_total;
    snapshot_low_pending = true;
  }
  queue_low_active = is_low;

  const bool is_empty = queued_samples == 0;
  if (!snapshot_rearm_wait && is_empty && !queue_empty_active) {
    ++queue_empty_events;
    ++queue_empty_events_total;
    snapshot_empty_pending = true;
  }
  queue_empty_active = is_empty;

  if ((snapshot_low_pending || snapshot_empty_pending) &&
      (snapshot_trigger_queue_min == SIZE_MAX || queued_samples < snapshot_trigger_queue_min)) {
    snapshot_trigger_queue_min = queued_samples;
  }
}

void DualWavLoopRuntimeDiagnostics::noteLoop(uint32_t elapsed_us) {
  loop_us.add(elapsed_us);
  if (elapsed_us >= config.slow_loop_us) ++loop_slow;
}

void DualWavLoopRuntimeDiagnostics::noteService(uint32_t elapsed_us) {
  service_us.add(elapsed_us);
  if (elapsed_us >= config.slow_service_us) {
    ++service_slow;
    snapshot_service_pending = true;
  }
}

void DualWavLoopRuntimeDiagnostics::notePump(size_t queued_before,
                                             size_t pumped_samples,
                                             uint32_t elapsed_us) {
  ++pump_calls;
  ++pump_calls_total;
  pump_samples += pumped_samples;
  pump_samples_total += pumped_samples;
  pump_us.add(elapsed_us);
  if (elapsed_us >= config.slow_pump_us) ++pump_slow;
  if (queued_before > 0 && pumped_samples == 0) {
    ++pump_zero_with_data;
    ++pump_zero_with_data_total;
  }
}

void DualWavLoopRuntimeDiagnostics::noteWrite(size_t requested_samples,
                                              size_t written_samples_now) {
  ++write_calls;
  ++write_calls_total;
  written_samples += written_samples_now;
  written_samples_total += written_samples_now;
  if (written_samples_now == 0) {
    ++zero_writes;
    ++zero_writes_total;
  }
  if (written_samples_now < requested_samples) {
    ++short_writes;
    ++short_writes_total;
  }
}

void DualWavLoopRuntimeDiagnostics::noteMix(size_t mixed_samples_now) {
  ++mix_calls;
  ++mix_calls_total;
  mixed_samples += mixed_samples_now;
  mixed_samples_total += mixed_samples_now;
  if (mixed_samples_now == 0) {
    ++mix_zero;
    ++mix_zero_total;
  }
}

void DualWavLoopRuntimeDiagnostics::noteRefill(uint32_t iterations, bool made_progress) {
  ++refill_loops;
  ++refill_loops_total;
  refill_iterations += iterations;
  refill_iterations_total += iterations;
  if (iterations > refill_peak_iterations) refill_peak_iterations = iterations;
  if (!made_progress) {
    ++refill_no_progress;
    ++refill_no_progress_total;
  }
}

void DualWavLoopRuntimeDiagnostics::noteServiceBudgetHit() {
  ++service_budget_hits;
  ++service_budget_hits_total;
  snapshot_budget_pending = true;
}

void DualWavLoopRuntimeDiagnostics::noteTouchPoll(uint32_t now_ms) {
  ++touch_polls;
  ++touch_polls_total;
  if (last_touch_poll_ms != 0) {
    const uint32_t gap_ms = now_ms - last_touch_poll_ms;
    if (gap_ms > max_touch_poll_gap_ms) max_touch_poll_gap_ms = gap_ms;
  }
  last_touch_poll_ms = now_ms;
}

void DualWavLoopRuntimeDiagnostics::noteTouchEvent(uint8_t electrode) {
  ++touch_events;
  ++touch_events_total;
  if (electrode < 4) {
    ++touch_event_counts[electrode];
    ++touch_event_counts_total[electrode];
  }
}

void DualWavLoopRuntimeDiagnostics::noteStartupPrefill(uint32_t elapsed_ms,
                                                       size_t queued_samples) {
  startup_prefill_done = true;
  startup_prefill_ms = elapsed_ms;
  startup_prefill_samples = queued_samples;
}

void DualWavLoopRuntimeDiagnostics::noteReport(uint32_t elapsed_us) {
  ++reports;
  ++reports_total;
  report_us.add(elapsed_us);
  report_last_us = elapsed_us;
  if (elapsed_us >= config.slow_report_us) ++report_slow;
}

bool DualWavLoopRuntimeDiagnostics::snapshotPending() const {
  return snapshot_low_pending || snapshot_empty_pending || snapshot_budget_pending ||
         snapshot_service_pending;
}

const char* DualWavLoopRuntimeDiagnostics::snapshotEventName() const {
  if (snapshot_empty_pending) return "queue-empty";
  if (snapshot_low_pending) return "queue-low";
  if (snapshot_budget_pending) return "budget-hit";
  if (snapshot_service_pending) return "slow-service";
  return "manual";
}

size_t DualWavLoopRuntimeDiagnostics::snapshotTriggerQueueMin() const {
  return snapshot_trigger_queue_min == SIZE_MAX ? 0 : snapshot_trigger_queue_min;
}

bool DualWavLoopRuntimeDiagnostics::reportDue(uint32_t now_ms, bool force) const {
  if (force) return true;
  if (!snapshotPending()) return false;
  if (last_report_ms == 0) return true;
  return static_cast<uint32_t>(now_ms - last_report_ms) >= config.report_cooldown_ms;
}

void DualWavLoopRuntimeDiagnostics::clearSnapshotPending() {
  snapshot_low_pending = false;
  snapshot_empty_pending = false;
  snapshot_budget_pending = false;
  snapshot_service_pending = false;
  snapshot_trigger_queue_min = SIZE_MAX;
  snapshot_rearm_wait = true;
}

void DualWavLoopRuntimeDiagnostics::resetWindow() {
  loop_us.reset();
  service_us.reset();
  pump_us.reset();
  queue_samples.reset();
  loop_slow = 0;
  service_slow = 0;
  pump_slow = 0;
  queue_low_events = 0;
  queue_empty_events = 0;
  pump_calls = 0;
  pump_samples = 0;
  pump_zero_with_data = 0;
  write_calls = 0;
  written_samples = 0;
  short_writes = 0;
  zero_writes = 0;
  mix_calls = 0;
  mixed_samples = 0;
  mix_zero = 0;
  refill_loops = 0;
  refill_iterations = 0;
  refill_peak_iterations = 0;
  refill_no_progress = 0;
  service_budget_hits = 0;
  touch_polls = 0;
  touch_events = 0;
  max_touch_poll_gap_ms = 0;
  reports = 0;
  for (uint8_t i = 0; i < 4; ++i) {
    touch_event_counts[i] = 0;
  }
}

}  // namespace padre
