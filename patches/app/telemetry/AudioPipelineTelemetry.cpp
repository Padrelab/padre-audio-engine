#include "AudioPipelineTelemetry.h"

namespace padre {

namespace {

float clamp01(float value) {
  if (value < 0.0f) return 0.0f;
  if (value > 1.0f) return 1.0f;
  return value;
}

float expAverage(float current_avg, float sample, float alpha, uint32_t updates) {
  if (updates <= 1) return sample;
  return current_avg + (sample - current_avg) * alpha;
}

}  // namespace

BufferTelemetry::BufferTelemetry(float avg_alpha) : avg_alpha_(avg_alpha) { reset(); }

void BufferTelemetry::reset() { metrics_ = BufferMetrics{}; }

void BufferTelemetry::update(size_t fill_samples, size_t capacity_samples) {
  const float ratio = capacity_samples == 0
                          ? 0.0f
                          : clamp01(static_cast<float>(fill_samples) /
                                    static_cast<float>(capacity_samples));

  metrics_.fill_ratio = ratio;
  metrics_.updates++;

  if (metrics_.updates == 1) {
    metrics_.fill_ratio_min = ratio;
    metrics_.fill_ratio_max = ratio;
  } else {
    if (ratio < metrics_.fill_ratio_min) metrics_.fill_ratio_min = ratio;
    if (ratio > metrics_.fill_ratio_max) metrics_.fill_ratio_max = ratio;
  }

  metrics_.fill_ratio_avg =
      expAverage(metrics_.fill_ratio_avg, ratio, avg_alpha_, metrics_.updates);
}

void BufferTelemetry::noteUnderrun() { metrics_.underruns++; }

void BufferTelemetry::noteOverrun() { metrics_.overruns++; }

const BufferMetrics& BufferTelemetry::metrics() const { return metrics_; }

CpuTelemetry::CpuTelemetry(float avg_alpha) : avg_alpha_(avg_alpha) { reset(); }

void CpuTelemetry::reset() {
  metrics_ = CpuMetrics{};
  in_cycle_ = false;
  cycle_start_us_ = 0;
}

void CpuTelemetry::beginCycle(uint32_t now_us) {
  in_cycle_ = true;
  cycle_start_us_ = now_us;
}

void CpuTelemetry::endCycle(uint32_t now_us, uint32_t budget_us) {
  if (!in_cycle_) return;
  in_cycle_ = false;

  const uint32_t busy = elapsedUs(cycle_start_us_, now_us);
  const float load = budget_us == 0
                         ? 0.0f
                         : (100.0f * static_cast<float>(busy) /
                            static_cast<float>(budget_us));

  metrics_.cycles++;
  metrics_.busy_time_us_last = busy;
  if (busy > metrics_.busy_time_us_max) metrics_.busy_time_us_max = busy;
  metrics_.busy_time_us_avg =
      expAverage(metrics_.busy_time_us_avg, static_cast<float>(busy), avg_alpha_,
                 metrics_.cycles);

  metrics_.load_percent_last = load;
  if (load > metrics_.load_percent_max) metrics_.load_percent_max = load;
  metrics_.load_percent_avg =
      expAverage(metrics_.load_percent_avg, load, avg_alpha_, metrics_.cycles);

  if (budget_us > 0 && busy > budget_us) noteXrun();
}

void CpuTelemetry::noteXrun() { metrics_.xruns++; }

const CpuMetrics& CpuTelemetry::metrics() const { return metrics_; }

uint32_t CpuTelemetry::elapsedUs(uint32_t start_us, uint32_t end_us) {
  if (end_us >= start_us) return end_us - start_us;
  return (0xFFFFFFFFu - start_us) + end_us + 1u;
}

AudioPipelineDiagnostics::AudioPipelineDiagnostics(Print& out,
                                                   const PipelineDiagnosticsConfig& config)
    : out_(&out), config_(config) {}

void AudioPipelineDiagnostics::reset() {
  buffer_.reset();
  cpu_.reset();
  last_report_ms_ = 0;
}

void AudioPipelineDiagnostics::updateBuffer(size_t fill_samples, size_t capacity_samples) {
  buffer_.update(fill_samples, capacity_samples);
}

void AudioPipelineDiagnostics::noteUnderrun() { buffer_.noteUnderrun(); }

void AudioPipelineDiagnostics::noteOverrun() { buffer_.noteOverrun(); }

void AudioPipelineDiagnostics::beginCycle(uint32_t now_us) { cpu_.beginCycle(now_us); }

void AudioPipelineDiagnostics::endCycle(uint32_t now_us, uint32_t budget_us) {
  cpu_.endCycle(now_us, budget_us);
}

void AudioPipelineDiagnostics::noteXrun() { cpu_.noteXrun(); }

PipelineHealth AudioPipelineDiagnostics::health() const {
  const BufferMetrics& bm = buffer_.metrics();
  const CpuMetrics& cm = cpu_.metrics();

  if (bm.fill_ratio <= config_.low_buffer_critical ||
      cm.load_percent_last >= config_.high_cpu_critical || bm.underruns > 0 ||
      cm.xruns > 0) {
    return PipelineHealth::Critical;
  }

  if (bm.fill_ratio <= config_.low_buffer_warning ||
      cm.load_percent_last >= config_.high_cpu_warning || bm.overruns > 0) {
    return PipelineHealth::Warning;
  }

  return PipelineHealth::Ok;
}

void AudioPipelineDiagnostics::reportIfDue(uint32_t now_ms, bool force) {
  if (!out_) return;

  if (!force && config_.report_interval_ms > 0 &&
      static_cast<uint32_t>(now_ms - last_report_ms_) < config_.report_interval_ms) {
    return;
  }

  last_report_ms_ = now_ms;
  const BufferMetrics& bm = buffer_.metrics();
  const CpuMetrics& cm = cpu_.metrics();

  out_->printf(
      "diag health=%s buf=%.1f%% avg=%.1f%% min=%.1f%% max=%.1f%% u=%lu o=%lu cpu=%.1f%% "
      "avg=%.1f%% max=%.1f%% busy=%luus xruns=%lu\n",
      healthName(health()), static_cast<double>(bm.fill_ratio * 100.0f),
      static_cast<double>(bm.fill_ratio_avg * 100.0f),
      static_cast<double>(bm.fill_ratio_min * 100.0f),
      static_cast<double>(bm.fill_ratio_max * 100.0f),
      static_cast<unsigned long>(bm.underruns),
      static_cast<unsigned long>(bm.overruns),
      static_cast<double>(cm.load_percent_last),
      static_cast<double>(cm.load_percent_avg),
      static_cast<double>(cm.load_percent_max),
      static_cast<unsigned long>(cm.busy_time_us_last),
      static_cast<unsigned long>(cm.xruns));
}

const BufferMetrics& AudioPipelineDiagnostics::bufferMetrics() const {
  return buffer_.metrics();
}

const CpuMetrics& AudioPipelineDiagnostics::cpuMetrics() const { return cpu_.metrics(); }

const char* AudioPipelineDiagnostics::healthName(PipelineHealth state) {
  switch (state) {
    case PipelineHealth::Ok:
      return "ok";
    case PipelineHealth::Warning:
      return "warn";
    case PipelineHealth::Critical:
      return "critical";
    default:
      return "unknown";
  }
}

}  // namespace padre
