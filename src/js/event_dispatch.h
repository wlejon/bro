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

namespace bro::dom { class Document; class Element; class Event; class ShadowRoot; }

namespace bro::js {

/// Dispatch a DOM event through the listener system with bubbling.
/// Shared by both the windowed Engine and the Headless tool.
/// Supports shadow DOM event retargeting: when an event crosses a shadow
/// boundary during bubbling, the target is retargeted to the host element.
/// If originalJsEvent is provided (not JS_UNDEFINED), it is used as the event
/// object passed to listeners (with target/currentTarget updated), preserving
/// custom properties like CustomEvent.detail.
///
/// Runs both the JS listeners and the C++ listeners registered with
/// dom::Element::addEventListener(type, callback), merged in registration
/// order across the two — see dom/event_target.h.
///
/// `ctx` may be null for a realm with no JS: the same event path, phases and
/// retargeting are used, and only the C++ listeners run (inline `on*`
/// attributes and `el.onclick` properties are JS by definition and cannot).
void dispatchDomEvent(JSContext* ctx, bro::dom::Element* target, bro::dom::Event& event,
                      JSValue originalJsEvent = JS_UNDEFINED);

/// Fire `event` at one realm's window — the C++ entry point for the same
/// dispatch `globalThis.__bro_dispatch_window_event(type, event)` performs.
/// Runs the realm's JS window listeners and the C++ listeners on
/// `doc->windowListeners()`, merged in registration order. Both capture and
/// bubble listeners fire (a window event fired directly at the window has no
/// propagation path to split them across).
///
/// `doc` may be null when `ctx` is not — it is then resolved from the realm.
/// `ctx` may be null for a realm with no JS, in which case `doc` is required
/// and only the C++ listeners run.
///
/// `originalJsEvent`, when provided, is the object handed to the JS
/// listeners; otherwise one is built from `event` (type, timeStamp, bubbles,
/// cancelable, isTrusted, target = window, and the preventDefault /
/// stopPropagation / stopImmediatePropagation / composedPath methods).
void dispatchWindowEvent(JSContext* ctx, bro::dom::Document* doc,
                         bro::dom::Event& event,
                         JSValue originalJsEvent = JS_UNDEFINED);

/// Install `__bro_dispatch_window_event` and `__bro_listener_seq` on `ctx`.
/// Called by installWindowBindings after the window polyfill is evaluated —
/// the polyfill supplies the JS listener storage, this supplies the dispatcher
/// that runs it alongside the realm's C++ window listeners.
void installWindowEventDispatch(JSContext* ctx);

} // namespace bro::js
