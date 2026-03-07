#include "Mpr121AdafruitDriver.h"

#include <stdlib.h>

#if defined(ARDUINO_ARCH_ESP32)
#define PADRE_MPR121_ISR_ATTR IRAM_ATTR
#else
#define PADRE_MPR121_ISR_ATTR
#endif

namespace padre {

Mpr121AdafruitDriver* Mpr121AdafruitDriver::active_instance_ = nullptr;

Mpr121AdafruitDriver::Mpr121AdafruitDriver(Adafruit_MPR121& device,
                                           TwoWire& wire,
                                           const Mpr121AdafruitDriverPins& pins,
                                           const Mpr121AdafruitDriverConfig& config)
    : device_(&device), wire_(&wire), pins_(pins), config_(config) {}

bool Mpr121AdafruitDriver::begin() {
  end();
  if (device_ == nullptr || wire_ == nullptr) return false;
  if (!configureWire()) return false;

  if (pins_.irq >= 0) {
    pinMode(static_cast<uint8_t>(pins_.irq), INPUT_PULLUP);
  }

  if (!device_->begin(pins_.address,
                      wire_,
                      config_.touch_threshold,
                      config_.release_threshold,
                      config_.autoconfig)) {
    return false;
  }

  touch_mask_ = device_->touched();
  reported_touch_mask_ = touch_mask_;
  irq_flag_ = false;
  irq_count_ = 0;
  last_report_ms_ = 0;
  ready_ = attachInterruptHandler();
  return ready_;
}

void Mpr121AdafruitDriver::end() {
  detachInterruptHandler();
  ready_ = false;
  irq_flag_ = false;
  irq_count_ = 0;
  touch_mask_ = 0;
  reported_touch_mask_ = 0;
  last_report_ms_ = 0;
}

bool Mpr121AdafruitDriver::reconfigure(uint8_t touch_threshold,
                                       uint8_t release_threshold,
                                       bool autoconfig) {
  config_.touch_threshold = touch_threshold;
  config_.release_threshold = release_threshold;
  config_.autoconfig = autoconfig;
  return begin();
}

void Mpr121AdafruitDriver::setDiagnosticsOutput(Print* out) { diagnostics_out_ = out; }

void Mpr121AdafruitDriver::setIrqHook(void* ctx, Mpr121AdafruitDriverIrqHookFn hook) {
  irq_hook_ctx_ = ctx;
  irq_hook_ = hook;
}

bool Mpr121AdafruitDriver::ready() const { return ready_; }

bool Mpr121AdafruitDriver::consumeIrq() {
  noInterrupts();
  const bool pending = irq_flag_;
  irq_flag_ = false;
  interrupts();
  return pending;
}

bool Mpr121AdafruitDriver::irqFlag() const {
  noInterrupts();
  const bool pending = irq_flag_;
  interrupts();
  return pending;
}

uint32_t Mpr121AdafruitDriver::irqCount() const {
  noInterrupts();
  const uint32_t count = irq_count_;
  interrupts();
  return count;
}

bool Mpr121AdafruitDriver::irqLineActive() const {
  if (pins_.irq < 0) return false;
  return digitalRead(static_cast<uint8_t>(pins_.irq)) == LOW;
}

uint16_t Mpr121AdafruitDriver::touchMask() const { return touch_mask_; }

uint16_t Mpr121AdafruitDriver::readTouchMask() {
  if (!ready_ || device_ == nullptr) return 0;

  touch_mask_ = device_->touched();
  return touch_mask_;
}

uint16_t Mpr121AdafruitDriver::filteredData(uint8_t electrode) const {
  if (!ready_ || device_ == nullptr || electrode >= kElectrodeCount) return 0;
  return device_->filteredData(electrode);
}

uint16_t Mpr121AdafruitDriver::baselineData(uint8_t electrode) const {
  if (!ready_ || device_ == nullptr || electrode >= kElectrodeCount) return 0;
  return device_->baselineData(electrode);
}

const Mpr121AdafruitDriverConfig& Mpr121AdafruitDriver::config() const { return config_; }

void Mpr121AdafruitDriver::serviceRuntime(uint32_t now_ms) {
  if (!ready_ || diagnostics_out_ == nullptr) return;

  if (config_.log_transitions && reported_touch_mask_ != touch_mask_) {
    logTouchChanges(*diagnostics_out_, reported_touch_mask_, touch_mask_);
  }
  reported_touch_mask_ = touch_mask_;

  if (!config_.stream_enabled) return;
  if (config_.report_interval_ms == 0 ||
      (now_ms - last_report_ms_) < config_.report_interval_ms) {
    return;
  }

  last_report_ms_ = now_ms;
  switch (config_.output_mode) {
    case Mpr121DiagnosticsOutputMode::Summary:
      printSummaryLine(*diagnostics_out_, now_ms);
      break;
    case Mpr121DiagnosticsOutputMode::Table:
      printElectrodeTable(*diagnostics_out_);
      break;
    case Mpr121DiagnosticsOutputMode::Plot:
      printPlotLine(*diagnostics_out_);
      break;
  }
}

void Mpr121AdafruitDriver::scanI2c(Print& out) {
  if (!configureWire()) {
    out.println("mpr121: I2C bus init failed");
    return;
  }

  out.println("mpr121: I2C scan");
  bool found_any = false;
  bool found_device = false;
  for (uint8_t addr = 1; addr < 127; ++addr) {
    wire_->beginTransmission(addr);
    if (wire_->endTransmission() != 0) continue;

    found_any = true;
    if (addr == pins_.address) found_device = true;
    out.printf("  0x%02X%s\n",
               addr,
               addr == pins_.address ? "  <- expected MPR121" : "");
  }

  if (!found_any) {
    out.println("  no devices found");
  } else if (!found_device) {
    out.printf("  expected MPR121 address 0x%02X not found\n", pins_.address);
  }
}

void Mpr121AdafruitDriver::printStatus(Print& out) {
  out.printf(
      "Config: touch=%u release=%u autoconfig=%s stream=%s rate=%lu mode=%s\n",
      static_cast<unsigned>(config_.touch_threshold),
      static_cast<unsigned>(config_.release_threshold),
      config_.autoconfig ? "on" : "off",
      config_.stream_enabled ? "on" : "off",
      static_cast<unsigned long>(config_.report_interval_ms),
      outputModeName(config_.output_mode));

  out.printf("State: ready=%s mask=0x%03X irq_pin=%s irq_count=%lu irq_flag=%s\n",
             ready_ ? "yes" : "no",
             static_cast<unsigned>(touch_mask_),
             irqLineActive() ? "LOW" : "HIGH",
             static_cast<unsigned long>(irqCount()),
             irqFlag() ? "yes" : "no");

  if (!ready_ || device_ == nullptr) return;

  out.printf(
      "Regs: ECR=0x%02X CONFIG1=0x%02X CONFIG2=0x%02X AUTOCONFIG0=0x%02X TOUCH0=%u RELEASE0=%u DEBOUNCE=0x%02X\n",
      device_->readRegister8(MPR121_ECR),
      device_->readRegister8(MPR121_CONFIG1),
      device_->readRegister8(MPR121_CONFIG2),
      device_->readRegister8(MPR121_AUTOCONFIG0),
      device_->readRegister8(MPR121_TOUCHTH_0),
      device_->readRegister8(MPR121_RELEASETH_0),
      device_->readRegister8(MPR121_DEBOUNCE));
}

void Mpr121AdafruitDriver::printElectrodeTable(Print& out) {
  if (!ready_) {
    out.println("mpr121: not ready");
    return;
  }

  ElectrodeSnapshot snapshots[kElectrodeCount];
  fillElectrodeSnapshots(snapshots);

  out.printf("Electrodes at %lu ms, mask=0x%03X\n",
             static_cast<unsigned long>(millis()),
             static_cast<unsigned>(touch_mask_));
  out.println(" idx touch baseline filtered delta");
  for (uint8_t i = 0; i < kElectrodeCount; ++i) {
    const ElectrodeSnapshot& snapshot = snapshots[i];
    out.printf(" %3u %5s %8u %8u %+5d\n",
               static_cast<unsigned>(i),
               snapshot.touched ? "yes" : "no",
               static_cast<unsigned>(snapshot.baseline),
               static_cast<unsigned>(snapshot.filtered),
               static_cast<int>(snapshot.delta));
  }
}

void Mpr121AdafruitDriver::printHelp(Print& out) const {
  out.println("mpr121 commands:");
  out.println("  mpr121 help");
  out.println("  mpr121 status");
  out.println("  mpr121 dump");
  out.println("  mpr121 scan");
  out.println("  mpr121 stream <on|off>");
  out.println("  mpr121 mode <summary|table|plot>");
  out.println("  mpr121 rate <ms>");
  out.println("  mpr121 thresholds <touch> <release>");
  out.println("  mpr121 auto <on|off>");
}

bool Mpr121AdafruitDriver::handleRuntimeCommand(const String& line, Print& out) {
  size_t pos = 0;
  String command = nextToken(line, pos);
  if (command.equalsIgnoreCase("mpr121")) {
    command = nextToken(line, pos);
  }

  if (command.length() == 0 || command.equalsIgnoreCase("help")) {
    printHelp(out);
    return true;
  }

  if (command.equalsIgnoreCase("status")) {
    printStatus(out);
    return true;
  }
  if (command.equalsIgnoreCase("dump")) {
    printStatus(out);
    printElectrodeTable(out);
    return true;
  }
  if (command.equalsIgnoreCase("scan")) {
    scanI2c(out);
    return true;
  }
  if (command.equalsIgnoreCase("stream")) {
    bool enabled = false;
    if (!parseBoolToken(nextToken(line, pos), enabled)) {
      out.println("mpr121: usage mpr121 stream <on|off>");
      return false;
    }
    config_.stream_enabled = enabled;
    out.printf("mpr121: stream=%s\n", enabled ? "on" : "off");
    return true;
  }
  if (command.equalsIgnoreCase("mode")) {
    Mpr121DiagnosticsOutputMode mode = Mpr121DiagnosticsOutputMode::Summary;
    if (!parseOutputModeToken(nextToken(line, pos), mode)) {
      out.println("mpr121: usage mpr121 mode <summary|table|plot>");
      return false;
    }
    config_.output_mode = mode;
    out.printf("mpr121: mode=%s\n", outputModeName(config_.output_mode));
    return true;
  }
  if (command.equalsIgnoreCase("rate")) {
    uint32_t interval_ms = 0;
    if (!parseUint32Token(nextToken(line, pos), interval_ms)) {
      out.println("mpr121: usage mpr121 rate <ms>");
      return false;
    }
    if (interval_ms < 20u) interval_ms = 20u;
    config_.report_interval_ms = interval_ms;
    out.printf("mpr121: rate=%lu ms\n", static_cast<unsigned long>(config_.report_interval_ms));
    return true;
  }
  if (command.equalsIgnoreCase("thresholds")) {
    uint8_t touch_threshold = 0;
    uint8_t release_threshold = 0;
    if (!parseUint8Token(nextToken(line, pos), touch_threshold) ||
        !parseUint8Token(nextToken(line, pos), release_threshold)) {
      out.println("mpr121: usage mpr121 thresholds <touch> <release>");
      return false;
    }

    if (!reconfigure(touch_threshold, release_threshold, config_.autoconfig)) {
      out.println("mpr121: reconfigure failed");
      return false;
    }
    printStatus(out);
    return true;
  }
  if (command.equalsIgnoreCase("auto")) {
    bool enabled = false;
    if (!parseBoolToken(nextToken(line, pos), enabled)) {
      out.println("mpr121: usage mpr121 auto <on|off>");
      return false;
    }

    if (!reconfigure(config_.touch_threshold, config_.release_threshold, enabled)) {
      out.println("mpr121: reconfigure failed");
      return false;
    }
    printStatus(out);
    return true;
  }

  out.printf("mpr121: unknown command '%s'\n", command.c_str());
  printHelp(out);
  return false;
}

Mpr121TouchControllerIo Mpr121AdafruitDriver::asTouchControllerIo() {
  Mpr121TouchControllerIo io;
  io.user_data = this;
  io.read_touch_mask = &Mpr121AdafruitDriver::readTouchMaskFromDevice;
  return io;
}

RuntimeCommandEntry Mpr121AdafruitDriver::runtimeCommandEntry(const char* command,
                                                              const char* help) {
  RuntimeCommandEntry entry;
  entry.command = command;
  entry.handler = &Mpr121AdafruitDriver::handleRuntimeCommandEntry;
  entry.ctx = this;
  entry.help = help;
  return entry;
}

bool Mpr121AdafruitDriver::handleRuntimeCommandEntry(void* ctx,
                                                     const String& line,
                                                     Print& out) {
  auto* self = static_cast<Mpr121AdafruitDriver*>(ctx);
  return self != nullptr && self->handleRuntimeCommand(line, out);
}

uint16_t Mpr121AdafruitDriver::readTouchMaskFromDevice(void* user_data) {
  auto* self = static_cast<Mpr121AdafruitDriver*>(user_data);
  return self != nullptr ? self->readTouchMask() : 0;
}

void PADRE_MPR121_ISR_ATTR Mpr121AdafruitDriver::irqThunk() {
  if (active_instance_ != nullptr) active_instance_->handleIrq();
}

String Mpr121AdafruitDriver::nextToken(const String& line, size_t& pos) {
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

bool Mpr121AdafruitDriver::parseBoolToken(const String& token, bool& out_value) {
  if (token.equalsIgnoreCase("on") || token == "1" || token.equalsIgnoreCase("true")) {
    out_value = true;
    return true;
  }
  if (token.equalsIgnoreCase("off") || token == "0" || token.equalsIgnoreCase("false")) {
    out_value = false;
    return true;
  }
  return false;
}

bool Mpr121AdafruitDriver::parseUint8Token(const String& token, uint8_t& out_value) {
  uint32_t parsed = 0;
  if (!parseUint32Token(token, parsed) || parsed > 255u) return false;

  out_value = static_cast<uint8_t>(parsed);
  return true;
}

bool Mpr121AdafruitDriver::parseUint32Token(const String& token, uint32_t& out_value) {
  if (token.length() == 0) return false;

  char* end = nullptr;
  const unsigned long parsed = strtoul(token.c_str(), &end, 10);
  if (end == token.c_str() || *end != '\0') return false;

  out_value = static_cast<uint32_t>(parsed);
  return true;
}

bool Mpr121AdafruitDriver::parseOutputModeToken(const String& token,
                                                Mpr121DiagnosticsOutputMode& out_mode) {
  if (token.equalsIgnoreCase("summary")) {
    out_mode = Mpr121DiagnosticsOutputMode::Summary;
    return true;
  }
  if (token.equalsIgnoreCase("table")) {
    out_mode = Mpr121DiagnosticsOutputMode::Table;
    return true;
  }
  if (token.equalsIgnoreCase("plot")) {
    out_mode = Mpr121DiagnosticsOutputMode::Plot;
    return true;
  }
  return false;
}

const char* Mpr121AdafruitDriver::outputModeName(Mpr121DiagnosticsOutputMode mode) {
  switch (mode) {
    case Mpr121DiagnosticsOutputMode::Summary:
      return "summary";
    case Mpr121DiagnosticsOutputMode::Table:
      return "table";
    case Mpr121DiagnosticsOutputMode::Plot:
      return "plot";
  }
  return "unknown";
}

bool Mpr121AdafruitDriver::configureWire() {
  if (wire_ == nullptr) return false;
  if (wire_ready_) return true;

#if defined(ARDUINO_ARCH_ESP32)
  if (pins_.sda >= 0 && pins_.scl >= 0) {
    wire_->begin(static_cast<int>(pins_.sda), static_cast<int>(pins_.scl));
  } else {
    wire_->begin();
  }
#else
  wire_->begin();
#endif

  wire_->setClock(pins_.i2c_clock_hz);
  wire_ready_ = true;
  return true;
}

bool Mpr121AdafruitDriver::attachInterruptHandler() {
  if (pins_.irq < 0) return true;

  const int interrupt = digitalPinToInterrupt(static_cast<uint8_t>(pins_.irq));
  if (interrupt < 0) return false;

  active_instance_ = this;
  attachInterrupt(interrupt, &Mpr121AdafruitDriver::irqThunk, FALLING);
  return true;
}

void Mpr121AdafruitDriver::detachInterruptHandler() {
  if (pins_.irq >= 0) {
    const int interrupt = digitalPinToInterrupt(static_cast<uint8_t>(pins_.irq));
    if (interrupt >= 0) detachInterrupt(interrupt);
  }

  if (active_instance_ == this) active_instance_ = nullptr;
}

void Mpr121AdafruitDriver::handleIrq() {
  irq_flag_ = true;
  ++irq_count_;
  if (irq_hook_ != nullptr) irq_hook_(irq_hook_ctx_);
}

void Mpr121AdafruitDriver::fillElectrodeSnapshots(ElectrodeSnapshot* snapshots) const {
  if (snapshots == nullptr || !ready_) return;

  for (uint8_t i = 0; i < kElectrodeCount; ++i) {
    ElectrodeSnapshot& snapshot = snapshots[i];
    snapshot.baseline = baselineData(i);
    snapshot.filtered = filteredData(i);
    snapshot.delta = static_cast<int16_t>(snapshot.baseline) -
                     static_cast<int16_t>(snapshot.filtered);
    snapshot.touched = (touch_mask_ & (static_cast<uint16_t>(1u) << i)) != 0;
  }
}

void Mpr121AdafruitDriver::printSummaryLine(Print& out, uint32_t now_ms) {
  ElectrodeSnapshot snapshots[kElectrodeCount];
  fillElectrodeSnapshots(snapshots);

  out.printf("%8lu ms mask=0x%03X irq=%lu pin=%s d=[",
             static_cast<unsigned long>(now_ms),
             static_cast<unsigned>(touch_mask_),
             static_cast<unsigned long>(irqCount()),
             irqLineActive() ? "LOW" : "HIGH");

  for (uint8_t i = 0; i < kElectrodeCount; ++i) {
    out.print(static_cast<int>(snapshots[i].delta));
    if (i + 1 != kElectrodeCount) out.print(',');
  }
  out.println(']');
}

void Mpr121AdafruitDriver::printPlotLine(Print& out) const {
  ElectrodeSnapshot snapshots[kElectrodeCount];
  fillElectrodeSnapshots(snapshots);

  for (uint8_t i = 0; i < kElectrodeCount; ++i) {
    out.print(static_cast<int>(snapshots[i].delta));
    if (i + 1 != kElectrodeCount) {
      out.print('\t');
    } else {
      out.println();
    }
  }
}

void Mpr121AdafruitDriver::logTouchChanges(Print& out,
                                           uint16_t previous_mask,
                                           uint16_t current_mask) {
  const uint16_t changed = previous_mask ^ current_mask;
  if (changed == 0) return;

  ElectrodeSnapshot snapshots[kElectrodeCount];
  fillElectrodeSnapshots(snapshots);

  for (uint8_t i = 0; i < kElectrodeCount; ++i) {
    const uint16_t bit = static_cast<uint16_t>(1u) << i;
    if ((changed & bit) == 0) continue;

    const ElectrodeSnapshot& snapshot = snapshots[i];
    out.printf("E%u %s baseline=%u filtered=%u delta=%d irq_pin=%s\n",
               static_cast<unsigned>(i),
               snapshot.touched ? "TOUCH" : "RELEASE",
               static_cast<unsigned>(snapshot.baseline),
               static_cast<unsigned>(snapshot.filtered),
               static_cast<int>(snapshot.delta),
               irqLineActive() ? "LOW" : "HIGH");
  }
}

}  // namespace padre
