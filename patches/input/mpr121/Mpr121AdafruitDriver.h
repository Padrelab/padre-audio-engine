#pragma once

#include <Adafruit_MPR121.h>
#include <Arduino.h>
#include <Wire.h>

#include "../../app/serial/SerialRuntimeConsole.h"
#include "Mpr121TouchController.h"

namespace padre {

enum class Mpr121DiagnosticsOutputMode : uint8_t {
  Summary = 0,
  Table = 1,
  Plot = 2,
};

struct Mpr121AdafruitDriverPins {
  int8_t sda = -1;
  int8_t scl = -1;
  int8_t irq = -1;
  uint8_t address = 0x5A;
  uint32_t i2c_clock_hz = 400000;
};

struct Mpr121AdafruitDriverConfig {
  uint8_t touch_threshold = 12;
  uint8_t release_threshold = 6;
  bool autoconfig = true;
  bool stream_enabled = false;
  uint32_t report_interval_ms = 250;
  Mpr121DiagnosticsOutputMode output_mode = Mpr121DiagnosticsOutputMode::Summary;
  bool log_transitions = true;
};

using Mpr121AdafruitDriverIrqHookFn = void (*)(void* ctx);

class Mpr121AdafruitDriver {
 public:
  struct ElectrodeSnapshot {
    uint16_t baseline = 0;
    uint16_t filtered = 0;
    int16_t delta = 0;
    bool touched = false;
  };

  Mpr121AdafruitDriver(Adafruit_MPR121& device,
                       TwoWire& wire,
                       const Mpr121AdafruitDriverPins& pins,
                       const Mpr121AdafruitDriverConfig& config = {});

  bool begin();
  void end();
  bool reconfigure(uint8_t touch_threshold, uint8_t release_threshold, bool autoconfig);

  void setDiagnosticsOutput(Print* out);
  void setIrqHook(void* ctx, Mpr121AdafruitDriverIrqHookFn hook);

  bool ready() const;
  bool consumeIrq();
  bool irqFlag() const;
  uint32_t irqCount() const;
  bool irqLineActive() const;
  uint16_t touchMask() const;

  uint16_t readTouchMask();
  uint16_t filteredData(uint8_t electrode) const;
  uint16_t baselineData(uint8_t electrode) const;

  const Mpr121AdafruitDriverConfig& config() const;

  void serviceRuntime(uint32_t now_ms);
  void scanI2c(Print& out);
  void printStatus(Print& out);
  void printElectrodeTable(Print& out);
  void printHelp(Print& out) const;
  bool handleRuntimeCommand(const String& line, Print& out);

  Mpr121TouchControllerIo asTouchControllerIo();
  RuntimeCommandEntry runtimeCommandEntry(
      const char* command = "mpr121",
      const char* help =
          "mpr121 [status|dump|scan|stream <on|off>|mode <summary|table|plot>|rate <ms>|thresholds <touch> <release>|auto <on|off>|help]");

  static bool handleRuntimeCommandEntry(void* ctx, const String& line, Print& out);

 private:
  static constexpr uint8_t kElectrodeCount = 12;

  static uint16_t readTouchMaskFromDevice(void* user_data);
  static void irqThunk();
  static String nextToken(const String& line, size_t& pos);
  static bool parseBoolToken(const String& token, bool& out_value);
  static bool parseUint8Token(const String& token, uint8_t& out_value);
  static bool parseUint32Token(const String& token, uint32_t& out_value);
  static bool parseOutputModeToken(const String& token, Mpr121DiagnosticsOutputMode& out_mode);
  static const char* outputModeName(Mpr121DiagnosticsOutputMode mode);

  bool configureWire();
  bool attachInterruptHandler();
  void detachInterruptHandler();
  void handleIrq();
  void fillElectrodeSnapshots(ElectrodeSnapshot* snapshots) const;
  void printSummaryLine(Print& out, uint32_t now_ms);
  void printPlotLine(Print& out) const;
  void logTouchChanges(Print& out, uint16_t previous_mask, uint16_t current_mask);

  Adafruit_MPR121* device_ = nullptr;
  TwoWire* wire_ = nullptr;
  Mpr121AdafruitDriverPins pins_;
  Mpr121AdafruitDriverConfig config_;
  Print* diagnostics_out_ = nullptr;
  void* irq_hook_ctx_ = nullptr;
  Mpr121AdafruitDriverIrqHookFn irq_hook_ = nullptr;
  bool wire_ready_ = false;
  bool ready_ = false;
  volatile bool irq_flag_ = false;
  volatile uint32_t irq_count_ = 0;
  uint16_t touch_mask_ = 0;
  uint16_t reported_touch_mask_ = 0;
  uint32_t last_report_ms_ = 0;

  static Mpr121AdafruitDriver* active_instance_;
};

}  // namespace padre
