#include <Arduino.h>
#include <Wire.h>
#include <ctype.h>
#include <stdlib.h>

#include <Adafruit_MPR121.h>

#if defined(ARDUINO_ARCH_ESP32)
#define PADRE_ISR_ATTR IRAM_ATTR
#else
#define PADRE_ISR_ATTR
#endif

namespace {

constexpr uint8_t MPR121_SDA = 4;
constexpr uint8_t MPR121_SCL = 5;
constexpr uint8_t MPR121_IRQ = 6;
constexpr uint8_t MPR121_ADDR = 0x5A;

constexpr uint8_t kElectrodeCount = 12;
constexpr uint32_t kTouchPollMs = 10;
constexpr uint32_t kDefaultReportIntervalMs = 250;
constexpr uint8_t kDefaultTouchThreshold = 12;
constexpr uint8_t kDefaultReleaseThreshold = 6;

enum class OutputMode : uint8_t {
  Summary = 0,
  Table = 1,
  Plot = 2,
};

struct RuntimeConfig {
  uint8_t touch_threshold = kDefaultTouchThreshold;
  uint8_t release_threshold = kDefaultReleaseThreshold;
  bool autoconfig = true;
  bool stream_enabled = true;
  uint32_t report_interval_ms = kDefaultReportIntervalMs;
  OutputMode output_mode = OutputMode::Summary;
};

struct ElectrodeSample {
  uint16_t baseline = 0;
  uint16_t filtered = 0;
  int16_t delta = 0;
  bool touched = false;
};

Adafruit_MPR121 g_mpr121;
RuntimeConfig g_config;

bool g_touch_ready = false;
uint16_t g_last_touch_mask = 0;
uint32_t g_last_poll_ms = 0;
uint32_t g_last_report_ms = 0;
String g_command_line;

volatile bool g_touch_irq_flag = false;
volatile uint32_t g_touch_irq_count = 0;

const char* outputModeName(OutputMode mode) {
  switch (mode) {
    case OutputMode::Summary:
      return "summary";
    case OutputMode::Table:
      return "table";
    case OutputMode::Plot:
      return "plot";
  }
  return "unknown";
}

String nextToken(const String& line, size_t& pos) {
  while (pos < static_cast<size_t>(line.length()) &&
         isspace(static_cast<unsigned char>(line[pos])) != 0) {
    ++pos;
  }

  const size_t start = pos;
  while (pos < static_cast<size_t>(line.length()) &&
         isspace(static_cast<unsigned char>(line[pos])) == 0) {
    ++pos;
  }

  if (start >= static_cast<size_t>(line.length())) return String();
  return line.substring(start, pos);
}

bool parseUint32Token(const String& token, uint32_t& out_value) {
  if (token.length() == 0) return false;

  char* end = nullptr;
  const unsigned long value = strtoul(token.c_str(), &end, 10);
  if (end == token.c_str() || *end != '\0') return false;

  out_value = static_cast<uint32_t>(value);
  return true;
}

bool parseUint8Token(const String& token, uint8_t& out_value) {
  uint32_t value = 0;
  if (!parseUint32Token(token, value) || value > 255u) return false;

  out_value = static_cast<uint8_t>(value);
  return true;
}

bool parseOnOffToken(const String& token, bool& out_value) {
  if (token.equalsIgnoreCase("on")) {
    out_value = true;
    return true;
  }
  if (token.equalsIgnoreCase("off")) {
    out_value = false;
    return true;
  }
  return false;
}

bool parseOutputModeToken(const String& token, OutputMode& out_mode) {
  if (token.equalsIgnoreCase("summary")) {
    out_mode = OutputMode::Summary;
    return true;
  }
  if (token.equalsIgnoreCase("table")) {
    out_mode = OutputMode::Table;
    return true;
  }
  if (token.equalsIgnoreCase("plot")) {
    out_mode = OutputMode::Plot;
    return true;
  }
  return false;
}

void PADRE_ISR_ATTR onMpr121Irq() {
  g_touch_irq_flag = true;
  ++g_touch_irq_count;
}

void printHelp() {
  Serial.println("Commands:");
  Serial.println("  help                     - show this help");
  Serial.println("  scan                     - scan I2C bus");
  Serial.println("  status                   - print current config and registers");
  Serial.println("  dump                     - print one full electrode table");
  Serial.println("  stream on|off            - enable/disable periodic reports");
  Serial.println("  mode summary|table|plot  - report format");
  Serial.println("  rate <ms>                - report interval, minimum 20 ms");
  Serial.println("  thresholds <touch> <release> - apply new thresholds and reinit");
  Serial.println("  auto on|off              - toggle MPR121 autoconfig and reinit");
  Serial.println("  reset                    - reinitialize MPR121");
}

void printPinout() {
  Serial.printf("MPR121 diag pins: sda=%u scl=%u irq=%u addr=0x%02X\n",
                MPR121_SDA,
                MPR121_SCL,
                MPR121_IRQ,
                MPR121_ADDR);
}

void scanI2cBus() {
  Serial.println("I2C scan:");
  bool found_any = false;
  bool found_mpr121 = false;

  for (uint8_t addr = 1; addr < 127; ++addr) {
    Wire.beginTransmission(addr);
    const uint8_t error = Wire.endTransmission();
    if (error != 0) continue;

    found_any = true;
    if (addr == MPR121_ADDR) found_mpr121 = true;
    Serial.printf("  0x%02X%s\n", addr, addr == MPR121_ADDR ? "  <- expected MPR121" : "");
  }

  if (!found_any) {
    Serial.println("  no devices found");
  } else if (!found_mpr121) {
    Serial.printf("  expected MPR121 address 0x%02X not found\n", MPR121_ADDR);
  }
}

void fillElectrodeSamples(uint16_t touch_mask, ElectrodeSample* out_samples) {
  if (out_samples == nullptr) return;

  for (uint8_t i = 0; i < kElectrodeCount; ++i) {
    ElectrodeSample& sample = out_samples[i];
    sample.baseline = g_mpr121.baselineData(i);
    sample.filtered = g_mpr121.filteredData(i);
    sample.delta = static_cast<int16_t>(sample.baseline) -
                   static_cast<int16_t>(sample.filtered);
    sample.touched = (touch_mask & (static_cast<uint16_t>(1u) << i)) != 0;
  }
}

void printStatus() {
  Serial.printf("Config: touch=%u release=%u autoconfig=%s stream=%s rate=%lu mode=%s\n",
                static_cast<unsigned>(g_config.touch_threshold),
                static_cast<unsigned>(g_config.release_threshold),
                g_config.autoconfig ? "on" : "off",
                g_config.stream_enabled ? "on" : "off",
                static_cast<unsigned long>(g_config.report_interval_ms),
                outputModeName(g_config.output_mode));

  Serial.printf("State: ready=%s mask=0x%03X irq_pin=%s irq_count=%lu irq_flag=%s\n",
                g_touch_ready ? "yes" : "no",
                static_cast<unsigned>(g_last_touch_mask),
                digitalRead(MPR121_IRQ) == LOW ? "LOW" : "HIGH",
                static_cast<unsigned long>(g_touch_irq_count),
                g_touch_irq_flag ? "yes" : "no");

  if (!g_touch_ready) return;

  Serial.printf(
      "Regs: ECR=0x%02X CONFIG1=0x%02X CONFIG2=0x%02X AUTOCONFIG0=0x%02X TOUCH0=%u RELEASE0=%u DEBOUNCE=0x%02X\n",
      g_mpr121.readRegister8(MPR121_ECR),
      g_mpr121.readRegister8(MPR121_CONFIG1),
      g_mpr121.readRegister8(MPR121_CONFIG2),
      g_mpr121.readRegister8(MPR121_AUTOCONFIG0),
      g_mpr121.readRegister8(MPR121_TOUCHTH_0),
      g_mpr121.readRegister8(MPR121_RELEASETH_0),
      g_mpr121.readRegister8(MPR121_DEBOUNCE));
}

void printElectrodeTable(uint16_t touch_mask, const ElectrodeSample* samples) {
  if (samples == nullptr) return;

  Serial.printf("Electrodes at %lu ms, mask=0x%03X\n",
                static_cast<unsigned long>(millis()),
                static_cast<unsigned>(touch_mask));
  Serial.println(" idx touch baseline filtered delta");
  for (uint8_t i = 0; i < kElectrodeCount; ++i) {
    const ElectrodeSample& sample = samples[i];
    Serial.printf(" %3u %5s %8u %8u %+5d\n",
                  static_cast<unsigned>(i),
                  sample.touched ? "yes" : "no",
                  static_cast<unsigned>(sample.baseline),
                  static_cast<unsigned>(sample.filtered),
                  static_cast<int>(sample.delta));
  }
}

void printSummaryLine(uint16_t touch_mask, const ElectrodeSample* samples) {
  if (samples == nullptr) return;

  Serial.printf("%8lu ms mask=0x%03X irq=%lu pin=%s d=[",
                static_cast<unsigned long>(millis()),
                static_cast<unsigned>(touch_mask),
                static_cast<unsigned long>(g_touch_irq_count),
                digitalRead(MPR121_IRQ) == LOW ? "LOW" : "HIGH");

  for (uint8_t i = 0; i < kElectrodeCount; ++i) {
    Serial.print(static_cast<int>(samples[i].delta));
    if (i + 1 != kElectrodeCount) Serial.print(',');
  }
  Serial.println(']');
}

void printPlotLine(const ElectrodeSample* samples) {
  if (samples == nullptr) return;

  for (uint8_t i = 0; i < kElectrodeCount; ++i) {
    Serial.print(static_cast<int>(samples[i].delta));
    if (i + 1 != kElectrodeCount) {
      Serial.print('\t');
    } else {
      Serial.println();
    }
  }
}

bool initMpr121() {
  detachInterrupt(digitalPinToInterrupt(MPR121_IRQ));
  pinMode(MPR121_IRQ, INPUT_PULLUP);

  g_touch_ready = false;
  g_touch_irq_flag = false;
  g_touch_irq_count = 0;
  g_last_touch_mask = 0;

  Serial.printf("Initializing MPR121: touch=%u release=%u autoconfig=%s\n",
                static_cast<unsigned>(g_config.touch_threshold),
                static_cast<unsigned>(g_config.release_threshold),
                g_config.autoconfig ? "on" : "off");

  if (!g_mpr121.begin(MPR121_ADDR,
                      &Wire,
                      g_config.touch_threshold,
                      g_config.release_threshold,
                      g_config.autoconfig)) {
    Serial.println("MPR121 init failed");
    return false;
  }

  attachInterrupt(digitalPinToInterrupt(MPR121_IRQ), onMpr121Irq, FALLING);
  g_last_touch_mask = g_mpr121.touched();
  g_last_poll_ms = millis();
  g_last_report_ms = 0;
  g_touch_ready = true;

  Serial.println("MPR121 ready");
  printStatus();

  ElectrodeSample samples[kElectrodeCount];
  fillElectrodeSamples(g_last_touch_mask, samples);
  printElectrodeTable(g_last_touch_mask, samples);
  return true;
}

void reportTouchData(uint16_t touch_mask, const ElectrodeSample* samples, bool force) {
  if (!g_config.stream_enabled && !force) return;

  const uint32_t now_ms = millis();
  if (!force && (now_ms - g_last_report_ms) < g_config.report_interval_ms) return;
  g_last_report_ms = now_ms;

  switch (g_config.output_mode) {
    case OutputMode::Summary:
      printSummaryLine(touch_mask, samples);
      break;
    case OutputMode::Table:
      printElectrodeTable(touch_mask, samples);
      break;
    case OutputMode::Plot:
      printPlotLine(samples);
      break;
  }
}

void logTouchChanges(uint16_t previous_mask,
                     uint16_t touch_mask,
                     const ElectrodeSample* samples) {
  const uint16_t changed = previous_mask ^ touch_mask;
  if (changed == 0 || samples == nullptr) return;

  for (uint8_t i = 0; i < kElectrodeCount; ++i) {
    const uint16_t bit = static_cast<uint16_t>(1u) << i;
    if ((changed & bit) == 0) continue;

    const ElectrodeSample& sample = samples[i];
    Serial.printf("E%u %s baseline=%u filtered=%u delta=%d irq_pin=%s\n",
                  static_cast<unsigned>(i),
                  sample.touched ? "TOUCH" : "RELEASE",
                  static_cast<unsigned>(sample.baseline),
                  static_cast<unsigned>(sample.filtered),
                  static_cast<int>(sample.delta),
                  digitalRead(MPR121_IRQ) == LOW ? "LOW" : "HIGH");
  }
}

void pollTouch() {
  if (!g_touch_ready) return;

  const uint32_t now_ms = millis();
  const bool had_irq = g_touch_irq_flag;
  if (had_irq) g_touch_irq_flag = false;

  if (!had_irq && (now_ms - g_last_poll_ms) < kTouchPollMs) return;

  const uint16_t previous_mask = g_last_touch_mask;
  const uint16_t touch_mask = g_mpr121.touched();
  g_last_poll_ms = now_ms;
  g_last_touch_mask = touch_mask;

  ElectrodeSample samples[kElectrodeCount];
  fillElectrodeSamples(touch_mask, samples);
  logTouchChanges(previous_mask, touch_mask, samples);
  reportTouchData(touch_mask, samples, false);
}

void handleScanCommand() {
  scanI2cBus();
}

void handleDumpCommand() {
  if (!g_touch_ready) {
    Serial.println("MPR121 not ready");
    return;
  }

  const uint16_t touch_mask = g_mpr121.touched();
  g_last_touch_mask = touch_mask;

  ElectrodeSample samples[kElectrodeCount];
  fillElectrodeSamples(touch_mask, samples);
  printStatus();
  printElectrodeTable(touch_mask, samples);
}

void handleRateCommand(const String& value_token) {
  uint32_t rate_ms = 0;
  if (!parseUint32Token(value_token, rate_ms)) {
    Serial.println("Usage: rate <ms>");
    return;
  }

  if (rate_ms < 20u) rate_ms = 20u;
  g_config.report_interval_ms = rate_ms;
  Serial.printf("Report interval set to %lu ms\n",
                static_cast<unsigned long>(g_config.report_interval_ms));
}

void handleStreamCommand(const String& value_token) {
  bool enabled = false;
  if (!parseOnOffToken(value_token, enabled)) {
    Serial.println("Usage: stream on|off");
    return;
  }

  g_config.stream_enabled = enabled;
  Serial.printf("Stream %s\n", enabled ? "enabled" : "disabled");
}

void handleModeCommand(const String& value_token) {
  OutputMode output_mode = OutputMode::Summary;
  if (!parseOutputModeToken(value_token, output_mode)) {
    Serial.println("Usage: mode summary|table|plot");
    return;
  }

  g_config.output_mode = output_mode;
  Serial.printf("Mode set to %s\n", outputModeName(g_config.output_mode));
}

void handleThresholdsCommand(const String& touch_token, const String& release_token) {
  uint8_t touch_threshold = 0;
  uint8_t release_threshold = 0;
  if (!parseUint8Token(touch_token, touch_threshold) ||
      !parseUint8Token(release_token, release_threshold)) {
    Serial.println("Usage: thresholds <touch> <release>");
    return;
  }

  g_config.touch_threshold = touch_threshold;
  g_config.release_threshold = release_threshold;
  initMpr121();
}

void handleAutoconfigCommand(const String& value_token) {
  bool enabled = false;
  if (!parseOnOffToken(value_token, enabled)) {
    Serial.println("Usage: auto on|off");
    return;
  }

  g_config.autoconfig = enabled;
  initMpr121();
}

void handleCommand(const String& line) {
  String trimmed = line;
  trimmed.trim();
  if (trimmed.length() == 0) return;

  size_t pos = 0;
  const String command = nextToken(trimmed, pos);

  if (command.equalsIgnoreCase("help")) {
    printHelp();
    return;
  }
  if (command.equalsIgnoreCase("scan")) {
    handleScanCommand();
    return;
  }
  if (command.equalsIgnoreCase("status")) {
    printStatus();
    return;
  }
  if (command.equalsIgnoreCase("dump")) {
    handleDumpCommand();
    return;
  }
  if (command.equalsIgnoreCase("reset")) {
    initMpr121();
    return;
  }

  if (command.equalsIgnoreCase("stream")) {
    handleStreamCommand(nextToken(trimmed, pos));
    return;
  }
  if (command.equalsIgnoreCase("mode")) {
    handleModeCommand(nextToken(trimmed, pos));
    return;
  }
  if (command.equalsIgnoreCase("rate")) {
    handleRateCommand(nextToken(trimmed, pos));
    return;
  }
  if (command.equalsIgnoreCase("auto")) {
    handleAutoconfigCommand(nextToken(trimmed, pos));
    return;
  }
  if (command.equalsIgnoreCase("thresholds")) {
    handleThresholdsCommand(nextToken(trimmed, pos), nextToken(trimmed, pos));
    return;
  }

  Serial.printf("Unknown command: %s\n", command.c_str());
  printHelp();
}

void pollSerial() {
  while (Serial.available() > 0) {
    const char ch = static_cast<char>(Serial.read());
    if (ch == '\r') continue;

    if (ch == '\n') {
      handleCommand(g_command_line);
      g_command_line = "";
      continue;
    }

    if (g_command_line.length() < 127) {
      g_command_line += ch;
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("MPR121 diagnostics");
  printPinout();

  pinMode(MPR121_IRQ, INPUT_PULLUP);
  Wire.begin(MPR121_SDA, MPR121_SCL);
  Wire.setClock(400000);

  scanI2cBus();
  printHelp();
  initMpr121();
}

void loop() {
  pollSerial();
  pollTouch();
  delay(1);
}
