#include "dom/event.h"
#include "util/time.h"

namespace bro::dom {

// ---------------------------------------------------------------------------
// Event
// ---------------------------------------------------------------------------

Event::Event(const std::string& type, bool bubbles, bool cancelable)
    : type_(type)
    , bubbles_(bubbles)
    , cancelable_(cancelable)
    , timeStamp_(util::currentTimeMs())
{
}

void Event::preventDefault() {
    if (cancelable_) {
        defaultPrevented_ = true;
    }
}

void Event::stopPropagation() {
    propagationStopped_ = true;
}

void Event::stopImmediatePropagation() {
    propagationStopped_ = true;
    immediatePropagationStopped_ = true;
}

// ---------------------------------------------------------------------------
// CustomEvent
// ---------------------------------------------------------------------------

CustomEvent::CustomEvent(const std::string& type, bool bubbles, bool cancelable)
    : Event(type, bubbles, cancelable)
{
}

// ---------------------------------------------------------------------------
// MouseEvent
// ---------------------------------------------------------------------------

MouseEvent::MouseEvent(const std::string& type, bool bubbles, bool cancelable)
    : Event(type, bubbles, cancelable)
{
    setComposed(true);
}

// ---------------------------------------------------------------------------
// KeyboardEvent
// ---------------------------------------------------------------------------

KeyboardEvent::KeyboardEvent(const std::string& type, bool bubbles, bool cancelable)
    : Event(type, bubbles, cancelable)
{
    setComposed(true);
}

// ---------------------------------------------------------------------------
// FocusEvent
// ---------------------------------------------------------------------------

FocusEvent::FocusEvent(const std::string& type, bool bubbles, bool cancelable)
    : Event(type, bubbles, cancelable)
{
    setComposed(true);
}

// ---------------------------------------------------------------------------
// WheelEvent
// ---------------------------------------------------------------------------

WheelEvent::WheelEvent(const std::string& type, bool bubbles, bool cancelable)
    : MouseEvent(type, bubbles, cancelable)
{
}

// ---------------------------------------------------------------------------
// InputEvent
// ---------------------------------------------------------------------------

InputEvent::InputEvent(const std::string& type, bool bubbles, bool cancelable)
    : Event(type, bubbles, cancelable)
{
    setComposed(true);
}

CompositionEvent::CompositionEvent(const std::string& type, bool bubbles, bool cancelable)
    : Event(type, bubbles, cancelable)
{
    setComposed(true);
}

TransitionEvent::TransitionEvent(const std::string& type, bool bubbles, bool cancelable)
    : Event(type, bubbles, cancelable)
{
}

AnimationEvent::AnimationEvent(const std::string& type, bool bubbles, bool cancelable)
    : Event(type, bubbles, cancelable)
{
}

ClipboardEvent::ClipboardEvent(const std::string& type, bool bubbles, bool cancelable)
    : Event(type, bubbles, cancelable)
{
}

DragEvent::DragEvent(const std::string& type, bool bubbles, bool cancelable)
    : MouseEvent(type, bubbles, cancelable)
{
}

SubmitEvent::SubmitEvent(const std::string& type, bool bubbles, bool cancelable)
    : Event(type, bubbles, cancelable)
{
}

TouchEvent::TouchEvent(const std::string& type, bool bubbles, bool cancelable)
    : Event(type, bubbles, cancelable)
{
}

GestureEvent::GestureEvent(const std::string& type, bool bubbles, bool cancelable)
    : Event(type, bubbles, cancelable)
{
}

} // namespace bro::dom
