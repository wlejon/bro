#pragma once

#include "js/event_dispatch.h"
#include "dom/element.h"
#include "dom/event.h"
#include "dom/event_target.h"
#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "quickjs.h"
}

namespace bro::dom {
class Document;
class Element;
class Event;
}

namespace bro::js {

// Event phases per DOM spec
inline constexpr int NONE = 0;
inline constexpr int CAPTURING_PHASE = 1;
inline constexpr int AT_TARGET = 2;
inline constexpr int BUBBLING_PHASE = 3;

using NativeEntryPtr = bro::dom::NativeListenerList::EntryPtr;

struct EventPathEntry {
    bro::dom::Element* element;
    bro::dom::Element* retargetedTarget; // target visible at this scope
};

// Build a short "click on #my-id" / "click on div.foo" / "click on div" description.
std::string describeListener(const std::string& evtType, const bro::dom::Element* el);

// Build the event path from target up to document root with shadow DOM retargeting.
std::vector<EventPathEntry> buildEventPath(bro::dom::Element* target);

// Stash composedPath as a JS array on the event object.
void stashComposedPath(JSContext* ctx, JSValue jsEvent, const std::vector<EventPathEntry>& path);

// Populate properties of a JS event object from a C++ dom::Event.
void populateJsEvent(JSContext* ctx, JSValue jsEvent, bro::dom::Event& event);

// Install standard stopPropagation/preventDefault/stopImmediatePropagation/composedPath methods on a JS event object.
void installJsEventMethods(JSContext* ctx, JSValue jsEvent);

// Read flags JS listeners set back onto the dom::Event.
void readJsFlagsBack(JSContext* ctx, JSValue jsEvent, bro::dom::Event& event);

// Push flags a C++ listener set on dom::Event onto the JS event object.
void mirrorNativeFlagsToJs(JSContext* ctx, JSValue jsEvent, const bro::dom::Event& event);

// Invoke one C++ listener entry.
bool invokeNativeEntry(const NativeEntryPtr& entry,
                       bro::dom::NativeListenerList* list,
                       bro::dom::Event& event,
                       JSContext* ctx, JSValue jsEvent);

// Check whether a listener with the given capture flag runs in the specified phase.
bool listenerRunsInPhase(int phase, bool capture);

// Get registration sequence from a JS listener record.
uint64_t jsListenerSeq(JSContext* ctx, JSValueConst entry);

// Invoke listeners for a single element during the specified phase.
void invokeListeners(JSContext* ctx, bro::dom::Element* current,
                     bro::dom::Element* retargetedTarget,
                     bro::dom::Event& event,
                     int phase,
                     JSValue originalJsEvent = JS_UNDEFINED);

// Core window-level event dispatch.
void dispatchWindowEventCore(JSContext* ctx, bro::dom::Document* doc,
                             bro::dom::Event& event, JSValue originalJsEvent,
                             int captureFilter, bro::dom::Element* target);

// Dispatch to window during capture or bubble phases.
void dispatchToWindow(JSContext* ctx, bro::dom::Element* target,
                      bro::dom::Event& event,
                      JSValue originalJsEvent, bool isCapture);

} // namespace bro::js
