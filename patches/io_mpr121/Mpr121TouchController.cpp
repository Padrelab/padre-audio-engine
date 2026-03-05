#include "Mpr121TouchController.h"

namespace padre {

Mpr121TouchController::Mpr121TouchController(const Mpr121TouchControllerIo& io,
                                             const Mpr121TouchControllerConfig& config)
    : io_(io), config_(config), electrode_count_(clampElectrodeCount(config.electrode_count)) {}

Mpr121TouchController::~Mpr121TouchController() { end(); }

bool Mpr121TouchController::begin() {
  end();
  if (io_.read_touch_mask == nullptr || electrode_count_ == 0) return false;

  Mpr121InputIo input_io;
  input_io.user_data = &cache_;
  input_io.read_touch_mask = &Mpr121TouchController::readTouchMaskFromCache;

  for (uint8_t i = 0; i < electrode_count_; ++i) {
    inputs_[i] = new Mpr121Input(i, input_io, config_.input_config);
    if (inputs_[i] == nullptr) {
      end();
      return false;
    }
  }

  cache_.mask = 0;
  return true;
}

void Mpr121TouchController::end() {
  for (uint8_t i = 0; i < 12; ++i) {
    delete inputs_[i];
    inputs_[i] = nullptr;
  }
  cache_.mask = 0;
}

void Mpr121TouchController::setEventHandler(void* ctx, InputEventHandlerFn handler) {
  event_ctx_ = ctx;
  event_handler_ = handler;
}

void Mpr121TouchController::poll(uint32_t now_ms) {
  if (io_.read_touch_mask == nullptr) return;

  const uint16_t new_mask = io_.read_touch_mask(io_.user_data);
  if (config_.debug_touch_mask && config_.debug_out != nullptr && new_mask != cache_.mask) {
    config_.debug_out->printf("Touch mask: 0x%03X\n", static_cast<unsigned>(new_mask));
  }
  cache_.mask = new_mask;

  for (uint8_t i = 0; i < electrode_count_; ++i) {
    if (inputs_[i] == nullptr) continue;
    dispatchEvent(inputs_[i]->update(now_ms));
  }
}

uint16_t Mpr121TouchController::touchMask() const { return cache_.mask; }

uint8_t Mpr121TouchController::electrodeCount() const { return electrode_count_; }

uint16_t Mpr121TouchController::readTouchMaskFromCache(void* user_data) {
  auto* cache = static_cast<TouchMaskCache*>(user_data);
  return cache ? cache->mask : 0;
}

uint8_t Mpr121TouchController::clampElectrodeCount(uint8_t requested) {
  if (requested == 0) return 1;
  if (requested > 12) return 12;
  return requested;
}

void Mpr121TouchController::dispatchEvent(const InputEvent& event) {
  if (event_handler_ == nullptr || event.type == InputEventType::None) return;
  event_handler_(event_ctx_, event);
}

}  // namespace padre
