#pragma once

// Events: the `on<type>` + addEventListener lists on host objects that fire
// events of their own (host_events.cpp), and the listeners on host objects
// whose identity is a dom::Element, wired to the engine's dispatch
// (host_dom_events.cpp).

#include "embed/embed.h"
#include "dom/event_target.h"

#include <functional>
#include <string>
#include <vector>

namespace bro::dom {
class Document;
class Element;
class Event;
}  // namespace bro::dom

namespace bro::bronze_host {

namespace ev = bronze::embed;
using Value = bronze::Value;

// host_builder.h owns it; a file that only names it needs nothing more.
struct ObjectBuilder;

// ---------------------------------------------------------------------------
// Events (host_events.cpp)
// ---------------------------------------------------------------------------

// A private list stored ON a host object under `key`: a plain object with
// numeric keys and a `length`, because the embed API builds plain objects and
// has no array constructor. This is the shape every "things to call later" list
// in this layer takes, and it is on the object rather than in host memory for
// the reason the GC rule gives — a host-side table would need Persistents owned
// by the object's finalizer, which is the one thing a finalizer may not own.
// The snapshot is taken whole before anything runs, so a list mutated by one
// entry does not disturb the run in progress.
void hostListAppend(ev::Persistent& obj, const std::string& key, Value v);
std::vector<ev::Persistent> hostListSnapshot(const ev::Persistent& obj,
                                             const std::string& key);

// The `on<type>` slot plus the addEventListener list, for the host objects that
// fire events. Both live as ordinary properties ON THE OBJECT — see above.
void addHostListener(ev::Persistent& obj, const std::string& type, Value fn);
void removeHostListener(ev::Persistent& obj, const std::string& type, Value fn);

// Fire `type` at `target`: the `on<type>` property first, then every
// addEventListener listener in registration order, each called with `target` as
// the receiver and a minimal `{type, target}` event object. A listener list
// mutated during dispatch does not disturb the run in progress (the list is
// snapshotted first) — three.js's ImageLoader removes its own listeners from
// inside them, so this is load-bearing, not defensive.
//
// This layer does NOT model the web's single registration-ordered listener list
// spanning both spellings: an `onload` assigned after an addEventListener('load')
// still runs first. Nothing three.js does can see the difference, and the
// alternative is a host-side registration counter that only a finalizer could
// own.
void dispatchHostEvent(ev::Persistent target, const std::string& type);

// ---------------------------------------------------------------------------
// DOM events (host_dom_events.cpp)
// ---------------------------------------------------------------------------

// addEventListener / removeEventListener / dispatchEvent on a host object
// whose real identity is a dom::Element, wired to the ENGINE's listener
// registry (dom::Element::addEventListener) rather than to a list of this
// layer's own. That is the whole point: the engine already runs one dispatch
// walk that dispatches listeners in registration order
// (dom/event_dispatch.cpp), so a compiled listener registered here fires from
// a real click, in the right phase, beside the page's own listeners — instead
// of from a second dispatch system that nothing would ever call.
//
// `source` is resolved at each call rather than captured as a pointer: the
// document's element target is documentElement, which does not exist yet when
// the globals are registered. `what` names the object in diagnostics.
using ElementSource = std::function<dom::Element*()>;
void installElementEventTarget(ObjectBuilder& b, ElementSource source,
                               const char* what);
dom::ListenerOptions readOptions(Value optV);

// Hand `evt` to one compiled listener as PLAIN DATA — a fresh object carrying
// the fields for the event's kind (coordinates, key, button, deltas, the
// string detail of a CustomEvent), the target identity, and the three
// propagation methods, which write through to the live dom::Event for the
// duration of this call and refuse afterwards. Nothing from either heap
// crosses; every field is copied. A throw out of the listener is reported
// through reportBronzeError under `origin` and dispatch continues.
void callBronzeListener(const ev::Persistent& fn, const ev::Persistent& thisObj,
                        dom::Event& evt, const char* origin,
                        dom::Document* docOverride = nullptr);

// `dispatchEvent(desc)` from compiled code, where `desc` is a plain
// `{type, bubbles, cancelable, detail}` object rather than a `new
// CustomEvent(...)`. That was forced when nothing here could be built on a
// chosen prototype; it no longer is — embed::makeHandle takes one, and
// host_image.cpp works the shape end to end — so a real CustomEvent class
// is buildable and merely unwritten. The descriptor stays until it is: it is
// the documented channel and compiled code already speaks it. Answers
// `!defaultPrevented`, as the web's dispatchEvent does, or a pending
// TypeError for a descriptor without a string `type`.
Value hostDispatchToElement(ElementSource source, const char* what, Value desc);
Value hostDispatchToWindow(Value desc);
// element.focus()'s steps (host_element_interaction.cpp): focus, blur,
// focusin, focusout. A no-op for an inert element (outside a modal dialog).
void hostFocusElement(dom::Element* el);
// The same at one document's window — a secondary window's, an iframe's.
Value hostDispatchToWindowOf(dom::Document* doc, Value desc);

}  // namespace bro::bronze_host
