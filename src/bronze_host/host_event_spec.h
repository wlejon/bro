// The descriptor a program hands `dispatchEvent`, read once into a struct,
// and the engine event built from it (host_event_spec.cpp). Shared by the
// element and window dispatch paths in host_dom_events.cpp.
#pragma once

#include "bronze_host/host_internal.h"

#include <functional>
#include <string>

namespace bro::dom {
class Event;
}

namespace bro::bronze_host {

struct EventSpec {
    std::string type;
    bool bubbles = false;
    bool cancelable = false;
    bool isTrusted = false;
    // CustomEvent payload — set only for a descriptor that is not one of the
    // UI event shapes, whose numeric `detail` is a click count.
    bool hasDetail = false;
    std::string detail;
    // KeyboardEvent
    std::string key;
    std::string code;
    // MouseEvent / PointerEvent / WheelEvent — read when `type` is in the
    // mouse family (isMouseType), where `detail` is the click count.
    double clientX = 0, clientY = 0, screenX = 0, screenY = 0;
    double movementX = 0, movementY = 0;
    int button = 0, buttons = 0, clickCount = 0;
    double deltaX = 0, deltaY = 0, deltaZ = 0;
    int deltaMode = 0;
    int pointerId = 0;
    std::string pointerType;
    bool isPrimary = false;
    double pressure = -1.0;
    // Modifiers, mouse and keyboard alike.
    bool ctrlKey = false, shiftKey = false, altKey = false, metaKey = false;
    bool repeat = false;
};

// Reads `{type, bubbles, cancelable, detail, key, code, clientX, ...}`.
// False leaves a pending TypeError naming what was wrong: a dispatch with
// no type is a program bug, and a silently dropped one would look exactly
// like a listener that never ran.
bool readEventSpec(Value descV, const char* what, EventSpec& out);

// Builds the engine event the spec describes — KeyboardEvent for the key
// family, MouseEvent / WheelEvent for the mouse and pointer families,
// CustomEvent when a detail was given, a plain Event otherwise — and hands
// it to `dispatch`. Answers `!defaultPrevented()`, which is what
// dispatchEvent returns.
bool dispatchEventSpec(const EventSpec& spec, const std::function<void(dom::Event&)>& dispatch);

}  // namespace bro::bronze_host
