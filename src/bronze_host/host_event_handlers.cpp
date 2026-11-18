// The event handler IDL attributes of the window and the document
// (`window.onload`, `window.onresize`, `document.onkeydown`,
// `document.onreadystatechange`, ...): HTML §8.1.8.1. Element's own are
// host_element.cpp's installInlineEventHandler.
//
// A handler is one listener in the target's list, added when the attribute
// is first set to a function and removed when it is set to anything else; a
// function set over a function takes the place of the first, so the handler
// keeps its position among the target's listeners as the web's does. The
// listener reads the slot when the event comes, so it calls whatever the
// attribute holds then.
//
// The slots are the document's (dom::Document::hostSlots): a window's under
// "window:<type>", the document's own under "document:<type>". They go with
// the document, and so does the listener, which lives in a list the document
// owns (its window's list, its root element's).
//
// Not here: the window's onerror, onmessage and onunhandledrejection, which
// their own dispatches read as properties (host_error_events.cpp,
// host_window_message.cpp, host_rejection_events.cpp).

#include "bronze_host/host_events.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_compartment.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_web_globals.h"

#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "engine/engine.h"

#include <memory>
#include <string>

namespace bro::bronze_host {

namespace {

// GlobalEventHandlers: the types the window and the document both carry.
constexpr const char* kGlobalHandlerTypes[] = {
    "abort", "animationend", "animationiteration", "animationstart", "auxclick", "blur", "cancel",
    "change", "click", "close", "contextmenu", "copy", "cut", "dblclick", "drag", "dragend",
    "dragenter", "dragleave", "dragover", "dragstart", "drop", "focus", "gotpointercapture", "input",
    "invalid", "keydown", "keypress", "keyup", "load", "lostpointercapture", "mousedown",
    "mouseenter", "mouseleave", "mousemove", "mouseout", "mouseover", "mouseup", "paste",
    "pointercancel", "pointerdown", "pointerenter", "pointerleave", "pointermove", "pointerout",
    "pointerover", "pointerup", "reset", "resize", "scroll", "scrollend", "select", "selectionchange",
    "submit", "toggle", "touchcancel", "touchend", "touchmove", "touchstart", "transitionend", "wheel",
};

// WindowEventHandlers and the window's own, less the three read elsewhere.
constexpr const char* kWindowHandlerTypes[] = {
    "afterprint", "beforeprint", "beforeunload", "gamepadconnected", "gamepaddisconnected",
    "hashchange", "languagechange", "offline", "online", "orientationchange", "pagehide",
    "pageshow", "popstate", "storage", "unload",
};

// The document's own (DocumentAndElementEventHandlers aside, which the
// global list has).
constexpr const char* kDocumentHandlerTypes[] = {
    "error", "fullscreenchange", "fullscreenerror", "pointerlockchange", "pointerlockerror",
    "readystatechange", "visibilitychange",
};

struct HandlerSlot {
    ev::Persistent fn;    // the attribute's value; undefined when it holds none
    ev::Persistent self;  // the object the attribute was set on: the handler's `this`
    dom::ListenerHandle handle;
    dom::Element* element = nullptr;  // the listener's element (a document's root), or null for a window's
};

std::shared_ptr<HandlerSlot> findSlot(dom::Document* doc, const std::string& key) {
    if (!doc) return nullptr;
    auto it = doc->hostSlots().find(key);
    return it == doc->hostSlots().end() ? nullptr : std::static_pointer_cast<HandlerSlot>(it->second);
}

void dropSlot(dom::Document* doc, const std::string& key) {
    auto slot = findSlot(doc, key);
    if (!slot) return;
    if (slot->element) slot->element->removeEventListener(slot->handle);
    else doc->windowListeners().remove(slot->handle);
    doc->hostSlots().erase(key);
}

// One attribute. `targetOf` answers the document whose slot it is and, for a
// document's attribute, the element its listener goes on.
using TargetOf = std::function<dom::Document*(dom::Element*& element)>;

void installHandlerAttribute(ObjectBuilder& b, const std::string& type, const std::string& keyPrefix,
                             const TargetOf& targetOf) {
    const std::string key = keyPrefix + type;
    const std::string prop = "on" + type;
    b.accessor(
        prop.c_str(),
        [key, targetOf](Value, std::span<const Value>) -> Value {
            dom::Element* el = nullptr;
            auto slot = findSlot(targetOf(el), key);
            return slot && ev::isFunction(slot->fn.get()) ? slot->fn.get() : ev::null();
        },
        [key, type, targetOf](Value self_, std::span<const Value> a) -> Value {
            ev::Persistent self(self_);
            ev::Persistent fn(argAt(a, 0));
            dom::Element* el = nullptr;
            dom::Document* doc = targetOf(el);
            if (!doc) return ev::undefined();
            if (!ev::isFunction(fn.get())) {
                dropSlot(doc, key);
                return ev::undefined();
            }
            if (auto slot = findSlot(doc, key)) {
                slot->fn.set(fn.get());
                slot->self.set(self.get());
                return ev::undefined();
            }
            auto slot = std::make_shared<HandlerSlot>();
            slot->fn.set(fn.get());
            slot->self.set(self.get());
            slot->element = el;
            std::weak_ptr<HandlerSlot> weak = slot;
            const std::string origin = (el ? "document on" : "window on") + type + " handler";
            auto call = [weak, origin, doc](dom::Event& evt) {
                auto s = weak.lock();
                if (!s || !ev::isFunction(s->fn.get())) return;
                // Copies: the handler may set the attribute again, which
                // replaces what the slot holds while it runs.
                ev::Persistent handler(s->fn.get());
                ev::Persistent thisObj(s->self.get());
                callBronzeListener(handler, thisObj, evt, origin.c_str(), doc);
            };
            slot->handle = el ? el->addEventListener(type, call) : doc->windowListeners().add(type, call);
            if (!slot->handle) return ev::undefined();
            doc->hostSlots()[key] = slot;
            return ev::undefined();
        });
}

}  // namespace

void installWindowEventHandlers(ObjectBuilder& window) {
    // The window's document is its realm's, whichever realm sets the
    // attribute (`frame.contentWindow.onload = f` is the frame's).
    const uint64_t home = currentCompartmentId();
    const TargetOf target = [home](dom::Element*& el) {
        el = nullptr;
        return compartmentDocument(home);
    };
    for (const char* t : kGlobalHandlerTypes) installHandlerAttribute(window, t, "window:", target);
    for (const char* t : kWindowHandlerTypes) installHandlerAttribute(window, t, "window:", target);
}

void installDocumentEventHandlers(ObjectBuilder& document, const ElementSource& root) {
    const TargetOf target = [root](dom::Element*& el) -> dom::Document* {
        el = root ? root() : nullptr;
        return el ? el->document() : nullptr;
    };
    for (const char* t : kGlobalHandlerTypes) installHandlerAttribute(document, t, "document:", target);
    for (const char* t : kDocumentHandlerTypes) installHandlerAttribute(document, t, "document:", target);
}

}  // namespace bro::bronze_host
