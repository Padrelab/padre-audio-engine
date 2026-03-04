#pragma once

#include <Arduino.h>

namespace padre {

struct BufferMetrics {
  float fill_ratio = 0.0f;
  float fill_ratio_min = 1.0f;
  float fill_ratio_max = 0.0f;
  float fill_ratio_avg = 0.0f;
  uint32_t updates = 0;
  uint32_t underruns = 0;
  uint32_t overruns = 0;
};

class BufferTelemetry {
 public:
  explicit BufferTelemetry(float avg_alpha = 0.15f);

  void reset();
  void update(size_t fill_samples, size_t capacity_samples);
  void noteUnderrun();
  void noteOverrun();

  const BufferMetrics& metrics() const;

 private:
  float avg_alpha_ = 0.15f;
  BufferMetrics metrics_;
};

struct CpuMetrics {
  uint32_t cycles = 0;
  uint32_t xruns = 0;
  uint32_t busy_time_us_last = 0;
  uint32_t busy_time_us_max = 0;
  float busy_time_us_avg = 0.0f;
  float load_percent_last = 0.0f;
  float load_percent_avg = 0.0f;
  float load_percent_max = 0.0f;
};

class CpuTelemetry {
 public:
  explicit CpuTelemetry(float avg_alpha = 0.12f);

  void reset();
  void beginCycle(uint32_t now_us);
  void endCycle(uint32_t now_us, uint32_t budget_us);
  void noteXrun();

  const CpuMetrics& metrics() const;

 private:
  static uint32_t elapsedUs(uint32_t start_us, uint32_t end_us);

  float avg_alpha_ = 0.12f;
  bool in_cycle_ = false;
  uint32_t cycle_start_us_ = 0;
  CpuMetrics metrics_;
};

enum class PipelineHealth {
  Ok,
  Warning,
  Critical,
};

struct PipelineDiagnosticsConfig {
  uint32_t report_interval_ms = 1000;
  float low_buffer_warning = 0.10f;
  float low_buffer_critical = 0.03f;
  float high_cpu_warning = 85.0f;
  float high_cpu_critical = 95.0f;
};

class AudioPipelineDiagnostics {
 public:
  AudioPipelineDiagnostics(Print& out, const PipelineDiagnosticsConfig& config = {});

  void reset();
  void updateBuffer(size_t fill_samples, size_t capacity_samples);
  void noteUnderrun();
  void noteOverrun();
  void beginCycle(uint32_t now_us);
  void endCycle(uint32_t now_us, uint32_t budget_us);
  void noteXrun();

  PipelineHealth health() const;
  void reportIfDue(uint32_t now_ms, bool force = false);

  const BufferMetrics& bufferMetrics() const;
  const CpuMetrics& cpuMetrics() const;

 private:
  static const char* healthName(PipelineHealth state);

  Print* out_ = nullptr;
  PipelineDiagnosticsConfig config_;
  BufferTelemetry buffer_;
  CpuTelemetry cpu_;
  uint32_t last_report_ms_ = 0;
};

}  // namespace padre
