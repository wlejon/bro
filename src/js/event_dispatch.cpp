#include "js/event_dispatch.h"
#include "js/event_dispatch_internal.h"
#include "js/dom_bindings.h"
#include "js/dom_bindings_internal.h"
#include "js/runtime.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event_target.h"
#include "dom/event.h"

#include <algorithm>
#include <string>
#include <vector>

namespace bro::js {

// ---------------------------------------------------------------------------
// Window dispatch
//
// A realm's window listeners live in two places: the JS ones in the window
// polyfill's __bro_win_listeners side map, the C++ ones in the realm
// Document's windowListeners(). This is the single loop that runs both, in
// registration order (see the block above invokeListeners).
//
// It is also what globalThis.__bro_dispatch_window_event is bound to — the
// polyfill no longer implements that function — so every existing dispatch
// site, C++ (resize, gamepad, message, DOMContentLoaded) and JS (popstate,
// hashchange, visibilitychange, window.dispatchEvent) alike, reaches C++
// listeners without knowing about them.
// ---------------------------------------------------------------------------

// captureFilter: 1 = capture listeners only, 0 = bubble listeners only,
// -1 = both (the legacy one-shot dispatch sites, and window.dispatchEvent).
void dispatchWindowEventCore(JSContext* ctx, bro::dom::Document* doc,
                             bro::dom::Event& event, JSValue originalJsEvent,
                             int captureFilter, bro::dom::Element* target) {
    if (!doc && ctx) doc = getDocumentForCtx(ctx);
    // No JS realm to ask: the target element knows its document, and that
    // document owns the realm's C++ window listeners.
    if (!doc && target) doc = target->document();

    std::vector<NativeEntryPtr> nativeEntries;
    if (doc) {
        for (auto& e : doc->windowListeners().snapshot(event.type())) {
            if (captureFilter >= 0 && e->opts.capture != (captureFilter == 1)) continue;
            nativeEntries.push_back(std::move(e));
        }
    }
    auto* nativeList = doc ? &doc->windowListeners() : nullptr;

    if (!ctx) {
        // Realm with no JS: only the C++ listeners exist.
        for (auto& e : nativeEntries)
            if (!invokeNativeEntry(e, nativeList, event, nullptr, JS_UNDEFINED)) break;
        return;
    }

    JSValue global = JS_GetGlobalObject(ctx);

    // The realm's JS window listener array for this type.
    JSValue winMap = JS_GetPropertyStr(ctx, global, "__bro_win_listeners");
    JSValue liveArr = JS_UNDEFINED;
    if (JS_IsObject(winMap))
        liveArr = JS_GetPropertyStr(ctx, winMap, event.type().c_str());
    JS_FreeValue(ctx, winMap);
    if (!JS_IsArray(liveArr)) {
        JS_FreeValue(ctx, liveArr);
        liveArr = JS_UNDEFINED;
    }

    if (nativeEntries.empty() && JS_IsUndefined(liveArr)) {
        JS_FreeValue(ctx, liveArr);
        JS_FreeValue(ctx, global);
        return;
    }

    bool ownsEvent = JS_IsUndefined(originalJsEvent);
    JSValue jsEvent;
    if (ownsEvent) {
        jsEvent = JS_NewObject(ctx);
        populateJsEvent(ctx, jsEvent, event);
        installJsEventMethods(ctx, jsEvent);

        // Resolve target to its JS wrapper. Window events fired at the window
        // itself have no element target; the window is the target then.
        JSValue elemMap = JS_GetPropertyStr(ctx, global, "__bro_elem_map");
        if (!JS_IsUndefined(elemMap) && target) {
            std::string tgtKey = std::to_string(target->nodeId());
            JSValue tgtElem = JS_GetPropertyStr(ctx, elemMap, tgtKey.c_str());
            if (JS_IsUndefined(tgtElem) || JS_IsNull(tgtElem)) {
                JS_FreeValue(ctx, tgtElem);
                tgtElem = DomBindings::wrapElement(ctx, target);
            }
            JS_SetPropertyStr(ctx, jsEvent, "target", tgtElem);
        } else if (target) {
            JS_SetPropertyStr(ctx, jsEvent, "target", JS_NULL);
        } else {
            JS_SetPropertyStr(ctx, jsEvent, "target", JS_DupValue(ctx, global));
        }
        JS_FreeValue(ctx, elemMap);
    } else {
        jsEvent = JS_DupValue(ctx, originalJsEvent);
    }

    JS_SetPropertyStr(ctx, jsEvent, "currentTarget", JS_DupValue(ctx, global));
    if (captureFilter >= 0) {
        JS_SetPropertyStr(ctx, jsEvent, "eventPhase",
            JS_NewInt32(ctx, captureFilter == 1 ? CAPTURING_PHASE : BUBBLING_PHASE));
    }

    // Snapshot the JS listener records the way the polyfill used to: a handler
    // may add or remove listeners (commonly its own) mid-dispatch, and neither
    // may corrupt this iteration.
    struct JsSlot { JSValue entry; uint64_t seq; };
    std::vector<JsSlot> jsSlots;
    int64_t liveLen = 0;
    if (!JS_IsUndefined(liveArr)) {
        JSValue lenVal = JS_GetPropertyStr(ctx, liveArr, "length");
        JS_ToInt64(ctx, &liveLen, lenVal);
        JS_FreeValue(ctx, lenVal);
        for (int64_t i = 0; i < liveLen; ++i) {
            JSValue e = JS_GetPropertyInt64(ctx, liveArr, i);
            if (JS_IsObject(e)) jsSlots.push_back({e, jsListenerSeq(ctx, e)});
            else JS_FreeValue(ctx, e);
        }
    }

    // Index of `entry` in the live array, or -1 if it has been removed.
    auto liveIndexOf = [&](JSValueConst entry) -> int64_t {
        if (JS_IsUndefined(liveArr)) return -1;
        int64_t len = 0;
        JSValue lenVal = JS_GetPropertyStr(ctx, liveArr, "length");
        JS_ToInt64(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        for (int64_t i = 0; i < len; ++i) {
            JSValue e = JS_GetPropertyInt64(ctx, liveArr, i);
            bool same = JS_VALUE_GET_PTR(e) == JS_VALUE_GET_PTR(entry);
            JS_FreeValue(ctx, e);
            if (same) return i;
        }
        return -1;
    };

    struct Slot { uint64_t seq; int jsSlot; const NativeEntryPtr* native; };
    std::vector<Slot> slots;
    slots.reserve(jsSlots.size() + nativeEntries.size());
    for (size_t i = 0; i < jsSlots.size(); ++i)
        slots.push_back({jsSlots[i].seq, static_cast<int>(i), nullptr});
    for (const auto& e : nativeEntries) slots.push_back({e->seq, -1, &e});
    std::stable_sort(slots.begin(), slots.end(),
                     [](const Slot& a, const Slot& b) { return a.seq < b.seq; });

    for (const auto& slot : slots) {
        if (slot.native) {
            if (!invokeNativeEntry(*slot.native, nativeList, event, ctx, jsEvent)) break;
            continue;
        }
        JSValue entry = jsSlots[static_cast<size_t>(slot.jsSlot)].entry;

        if (captureFilter >= 0) {
            JSValue capVal = JS_GetPropertyStr(ctx, entry, "capture");
            bool isCapture = JS_ToBool(ctx, capVal);
            JS_FreeValue(ctx, capVal);
            if (isCapture != (captureFilter == 1)) continue;
        }
        if (liveIndexOf(entry) < 0) continue;   // removed since the snapshot

        JSValue fn = JS_GetPropertyStr(ctx, entry, "fn");
        if (JS_IsFunction(ctx, fn)) {
            JSValue ret = Runtime::callJs(ctx, fn, global, 1, &jsEvent,
                ErrorOrigin::listener(event.type() + " on window"));
            JS_FreeValue(ctx, ret);
        }
        JS_FreeValue(ctx, fn);

        JSValue onceVal = JS_GetPropertyStr(ctx, entry, "once");
        bool once = JS_ToBool(ctx, onceVal);
        JS_FreeValue(ctx, onceVal);
        if (once) {
            int64_t idx = liveIndexOf(entry);
            if (idx >= 0) {
                int64_t len = 0;
                JSValue lenVal = JS_GetPropertyStr(ctx, liveArr, "length");
                JS_ToInt64(ctx, &len, lenVal);
                JS_FreeValue(ctx, lenVal);
                for (int64_t j = idx; j < len - 1; ++j) {
                    JSValue next = JS_GetPropertyInt64(ctx, liveArr, j + 1);
                    JS_SetPropertyInt64(ctx, liveArr, j, next);
                }
                JS_SetPropertyStr(ctx, liveArr, "length", JS_NewInt64(ctx, len - 1));
            }
        }

        readJsFlagsBack(ctx, jsEvent, event);
        if (event.immediatePropagationStopped()) break;
    }

    for (auto& s : jsSlots) JS_FreeValue(ctx, s.entry);
    JS_FreeValue(ctx, liveArr);
    JS_FreeValue(ctx, jsEvent);
    JS_FreeValue(ctx, global);
}

// Dispatch event to window-level listeners (set on globalThis via
// addEventListener). Per DOM spec, window is the outermost node in the
// propagation path: it receives capture first and bubble last.
void dispatchToWindow(JSContext* ctx, bro::dom::Element* target,
                      bro::dom::Event& event,
                      JSValue originalJsEvent, bool isCapture) {
    dispatchWindowEventCore(ctx, nullptr, event, originalJsEvent,
                            isCapture ? 1 : 0, target);
}

void dispatchWindowEvent(JSContext* ctx, bro::dom::Document* doc,
                         bro::dom::Event& event, JSValue originalJsEvent) {
    dispatchWindowEventCore(ctx, doc, event, originalJsEvent,
                            /*captureFilter=*/-1, /*target=*/nullptr);
}

// globalThis.__bro_listener_seq() — the shared registration counter, so the
// window polyfill can stamp its listener records with the same sequence C++
// registrations take. Without it, C++ and JS window listeners could not be
// ordered against each other.
static JSValue js_listener_seq(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewInt64(ctx, static_cast<int64_t>(bro::dom::nextListenerSeq()));
}

// globalThis.__bro_dispatch_window_event(type, event, capture)
//
// Same signature the polyfill used to define, so every existing caller is
// unchanged. C++ listeners get a real dom::Event synthesized from the JS one:
// its type, bubbles/cancelable and defaultPrevented cross over, and anything
// the C++ listener does to it (preventDefault, stopImmediatePropagation)
// crosses back onto the JS object. Payload a JS caller put on the event —
// CustomEvent.detail, gamepad, PopStateEvent.state — is NOT visible on the
// C++ side; a C++ listener that needs it must be registered for an event the
// host itself dispatches (js::dispatchWindowEvent), which carries the real
// dom::Event through.
static JSValue js_dispatch_window_event(JSContext* ctx, JSValueConst,
                                        int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    const char* typeC = JS_ToCString(ctx, argv[0]);
    if (!typeC) return JS_UNDEFINED;
    std::string type(typeC);
    JS_FreeCString(ctx, typeC);

    JSValue jsEvent = (argc >= 2) ? argv[1] : JS_UNDEFINED;

    int captureFilter = -1;
    if (argc >= 3 && !JS_IsUndefined(argv[2]))
        captureFilter = JS_ToBool(ctx, argv[2]) ? 1 : 0;

    bool bubbles = false, cancelable = true, trusted = false, prevented = false;
    if (JS_IsObject(jsEvent)) {
        JSValue v = JS_GetPropertyStr(ctx, jsEvent, "bubbles");
        if (!JS_IsUndefined(v)) bubbles = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, jsEvent, "cancelable");
        if (!JS_IsUndefined(v)) cancelable = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, jsEvent, "isTrusted");
        trusted = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, jsEvent, "defaultPrevented");
        prevented = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
    }

    // A string `detail` is promoted to a real dom::CustomEvent so the realm's
    // C++ window listeners see the payload; anything else stays a plain event
    // for them, and the JS listeners get the caller's object regardless.
    std::string detail;
    const bool hasDetail = jsEventStringDetail(ctx, jsEvent, detail);
    bro::dom::CustomEvent customEvent(type, bubbles, cancelable);
    bro::dom::Event plainEvent(type, bubbles, cancelable);
    bro::dom::Event& event = hasDetail ? static_cast<bro::dom::Event&>(customEvent)
                                       : plainEvent;
    if (hasDetail) customEvent.setDetail(detail);
    event.setIsTrusted(trusted);
    if (prevented) event.preventDefault();

    dispatchWindowEventCore(ctx, nullptr, event, jsEvent, captureFilter, nullptr);
    return JS_UNDEFINED;
}

void installWindowEventDispatch(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "__bro_listener_seq",
        JS_NewCFunction(ctx, js_listener_seq, "__bro_listener_seq", 0));
    JS_SetPropertyStr(ctx, global, "__bro_dispatch_window_event",
        JS_NewCFunction(ctx, js_dispatch_window_event,
                        "__bro_dispatch_window_event", 3));
    JS_FreeValue(ctx, global);
}

void dispatchDomEvent(JSContext* ctx, bro::dom::Element* target, bro::dom::Event& event,
                      JSValue originalJsEvent) {
    // ctx may be null: a realm with no JS still dispatches to the C++
    // listeners on the path, through this same algorithm.
    if (!target) return;

    event.setTarget(target);

    // Build the full event path including shadow DOM retargeting.
    // path[0] = target, path[N-1] = root ancestor
    auto path = buildEventPath(target);
    if (path.empty()) return;

    // Stash composed path on the original JS event if provided
    if (ctx && !JS_IsUndefined(originalJsEvent)) {
        stashComposedPath(ctx, originalJsEvent, path);
    }

    // --- Capture phase: window → root → target (exclusive) ---
    // Window sits outside the DOM tree but is the outermost EventTarget per
    // spec, so it captures first.
    if (!event.propagationStopped()) {
        dispatchToWindow(ctx, target, event, originalJsEvent, /*isCapture=*/true);
    }
    for (int i = static_cast<int>(path.size()) - 1; i > 0; --i) {
        if (event.propagationStopped()) break;
        event.setCurrentTarget(path[i].element);
        event.setEventPhase(CAPTURING_PHASE);
        invokeListeners(ctx, path[i].element, path[i].retargetedTarget,
                        event, CAPTURING_PHASE, originalJsEvent);
    }

    // --- At-target phase ---
    if (!event.propagationStopped()) {
        event.setCurrentTarget(path[0].element);
        event.setEventPhase(AT_TARGET);
        invokeListeners(ctx, path[0].element, path[0].retargetedTarget,
                        event, AT_TARGET, originalJsEvent);
    }

    // --- Bubble phase: target parent → root → window ---
    if (event.bubbles()) {
        for (size_t i = 1; i < path.size(); ++i) {
            if (event.propagationStopped()) break;
            event.setCurrentTarget(path[i].element);
            event.setEventPhase(BUBBLING_PHASE);
            invokeListeners(ctx, path[i].element, path[i].retargetedTarget,
                            event, BUBBLING_PHASE, originalJsEvent);
        }
        if (!event.propagationStopped()) {
            dispatchToWindow(ctx, target, event, originalJsEvent, /*isCapture=*/false);
        }
    }

    // Reset phase after dispatch
    event.setEventPhase(NONE);
    event.setCurrentTarget(nullptr);
}

} // namespace bro::js
