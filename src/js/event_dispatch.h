#pragma once

#include <cstdint>

#if defined(QUICKJS_H)
// quickjs.h already included
#elif __has_include("quickjs.h")
extern "C" {
#include "quickjs.h"
}
#else
struct JSContext;
#if defined(__x86_64__) || defined(_M_X64)
typedef uint64_t JSValue;
#else
typedef struct JSValue JSValue;
#endif
#ifndef JS_UNDEFINED
#define JS_UNDEFINED 0
#endif
#endif

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
