#pragma once

extern "C" {
#include "quickjs.h"
}

namespace bro::dom { class Element; class Event; class ShadowRoot; }

namespace bro::js {

/// Dispatch a DOM event through the JS listener system with bubbling.
/// Shared by both the windowed Engine and the Headless tool.
/// Supports shadow DOM event retargeting: when an event crosses a shadow
/// boundary during bubbling, the target is retargeted to the host element.
/// If originalJsEvent is provided (not JS_UNDEFINED), it is used as the event
/// object passed to listeners (with target/currentTarget updated), preserving
/// custom properties like CustomEvent.detail.
void dispatchDomEvent(JSContext* ctx, bro::dom::Element* target, bro::dom::Event& event,
                      JSValue originalJsEvent = JS_UNDEFINED);

} // namespace bro::js
