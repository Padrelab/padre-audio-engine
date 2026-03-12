#include "Mpr121AdafruitDriver.h"

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
  irq_flag_ = false;
  irq_count_ = 0;
  ready_ = attachInterruptHandler();
  return ready_;
}

void Mpr121AdafruitDriver::end() {
  detachInterruptHandler();
  ready_ = false;
  irq_flag_ = false;
  irq_count_ = 0;
  touch_mask_ = 0;
}

bool Mpr121AdafruitDriver::reconfigure(uint8_t touch_threshold,
                                       uint8_t release_threshold,
                                       bool autoconfig) {
  config_.touch_threshold = touch_threshold;
  config_.release_threshold = release_threshold;
  config_.autoconfig = autoconfig;
  return begin();
}

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

const Mpr121AdafruitDriverConfig& Mpr121AdafruitDriver::config() const { return config_; }

Mpr121TouchControllerIo Mpr121AdafruitDriver::asTouchControllerIo() {
  Mpr121TouchControllerIo io;
  io.user_data = this;
  io.read_touch_mask = &Mpr121AdafruitDriver::readTouchMaskFromDevice;
  return io;
}

uint16_t Mpr121AdafruitDriver::readTouchMaskFromDevice(void* user_data) {
  auto* self = static_cast<Mpr121AdafruitDriver*>(user_data);
  return self != nullptr ? self->readTouchMask() : 0;
}

void PADRE_MPR121_ISR_ATTR Mpr121AdafruitDriver::irqThunk() {
  if (active_instance_ != nullptr) active_instance_->handleIrq();
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
  interrupt_attached_ = true;
  return true;
}

void Mpr121AdafruitDriver::detachInterruptHandler() {
  if (interrupt_attached_ && pins_.irq >= 0) {
    const int interrupt = digitalPinToInterrupt(static_cast<uint8_t>(pins_.irq));
    if (interrupt >= 0) detachInterrupt(interrupt);
  }

  interrupt_attached_ = false;
  if (active_instance_ == this) active_instance_ = nullptr;
}

void Mpr121AdafruitDriver::handleIrq() {
  irq_flag_ = true;
  ++irq_count_;
  if (irq_hook_ != nullptr) irq_hook_(irq_hook_ctx_);
}

}  // namespace padre
