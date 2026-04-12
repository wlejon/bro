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
// MouseEvent
// ---------------------------------------------------------------------------

MouseEvent::MouseEvent(const std::string& type, bool bubbles, bool cancelable)
    : Event(type, bubbles, cancelable)
{
}

// ---------------------------------------------------------------------------
// KeyboardEvent
// ---------------------------------------------------------------------------

KeyboardEvent::KeyboardEvent(const std::string& type, bool bubbles, bool cancelable)
    : Event(type, bubbles, cancelable)
{
}

// ---------------------------------------------------------------------------
// FocusEvent
// ---------------------------------------------------------------------------

FocusEvent::FocusEvent(const std::string& type, bool bubbles, bool cancelable)
    : Event(type, bubbles, cancelable)
{
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

} // namespace bro::dom
