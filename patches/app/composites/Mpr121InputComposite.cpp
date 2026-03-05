#include "Mpr121InputComposite.h"

namespace padre {

Mpr121InputComposite::Mpr121InputComposite(const Mpr121TouchControllerIo& io,
                                           const Mpr121TouchControllerConfig& config)
    : controller_(io, config) {}

bool Mpr121InputComposite::begin() {
  if (!controller_.begin()) return false;
  controller_.setEventHandler(this, &Mpr121InputComposite::handleTouchEvent);
  return true;
}

void Mpr121InputComposite::end() { controller_.end(); }

void Mpr121InputComposite::poll(uint32_t now_ms) { controller_.poll(now_ms); }

void Mpr121InputComposite::bindActions(PlaybackInputActions* actions) { actions_ = actions; }

void Mpr121InputComposite::setEventHandler(void* ctx, InputEventHandlerFn handler) {
  event_ctx_ = ctx;
  event_handler_ = handler;
}

Mpr121TouchController& Mpr121InputComposite::controller() { return controller_; }

const Mpr121TouchController& Mpr121InputComposite::controller() const {
  return controller_;
}

void Mpr121InputComposite::handleTouchEvent(void* ctx, const InputEvent& event) {
  auto* self = static_cast<Mpr121InputComposite*>(ctx);
  if (self == nullptr || event.type == InputEventType::None) return;

  if (self->actions_ != nullptr) {
    self->actions_->handle(event);
  }
  if (self->event_handler_ != nullptr) {
    self->event_handler_(self->event_ctx_, event);
  }
}

}  // namespace padre
