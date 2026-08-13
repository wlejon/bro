#include "js/event_dispatch_internal.h"
#include "js/dom_bindings.h"
#include "js/runtime.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event_target.h"
#include "dom/event.h"

#include <algorithm>
#include <string>
#include <vector>

namespace bro::js {

bool listenerRunsInPhase(int phase, bool capture) {
    return (phase == AT_TARGET) ||
           (phase == CAPTURING_PHASE && capture) ||
           (phase == BUBBLING_PHASE && !capture);
}

// Push flags a C++ listener set on the dom::Event onto the JS event object, so
// JS listeners later in the same dispatch observe them. Only ever sets: a JS
// listener cannot un-prevent an event either.
void mirrorNativeFlagsToJs(JSContext* ctx, JSValue jsEvent,
                           const bro::dom::Event& event) {
    if (!ctx || JS_IsUndefined(jsEvent) || !JS_IsObject(jsEvent)) return;
    if (event.defaultPrevented()) {
        JS_SetPropertyStr(ctx, jsEvent, "_prevented", JS_TRUE);
        JS_SetPropertyStr(ctx, jsEvent, "defaultPrevented", JS_TRUE);
    }
    if (event.propagationStopped())
        JS_SetPropertyStr(ctx, jsEvent, "_stopped", JS_TRUE);
    if (event.immediatePropagationStopped())
        JS_SetPropertyStr(ctx, jsEvent, "_immediateStopped", JS_TRUE);
}

// Invoke one C++ listener. Returns false when dispatch at this target must
// stop immediately (stopImmediatePropagation).
// `list` may be null; it is only needed to honour ListenerOptions::once.
bool invokeNativeEntry(const NativeEntryPtr& entry,
                       bro::dom::NativeListenerList* list,
                       bro::dom::Event& event,
                       JSContext* ctx, JSValue jsEvent) {
    // Snapshots outlive removal: a listener removed by an earlier listener in
    // this same dispatch must not run.
    if (!entry || entry->removed || !entry->cb) return true;
    if (entry->opts.once && list) list->remove(bro::dom::ListenerHandle{entry->id});
    entry->cb(event);
    mirrorNativeFlagsToJs(ctx, jsEvent, event);
    return !event.immediatePropagationStopped();
}

// The registration sequence stamped on a JS listener record. Records written
// before this existed (or by code that did not stamp one) sort first, keeping
// the pre-existing "JS array order" behaviour for them.
uint64_t jsListenerSeq(JSContext* ctx, JSValueConst entry) {
    JSValue seqVal = JS_GetPropertyStr(ctx, entry, "seq");
    int64_t seq = 0;
    if (JS_IsNumber(seqVal)) JS_ToInt64(ctx, &seq, seqVal);
    JS_FreeValue(ctx, seqVal);
    return seq < 0 ? 0 : static_cast<uint64_t>(seq);
}

// phase: CAPTURING_PHASE, AT_TARGET, or BUBBLING_PHASE
void invokeListeners(JSContext* ctx, bro::dom::Element* current,
                     bro::dom::Element* retargetedTarget,
                     bro::dom::Event& event,
                     int phase,
                     JSValue originalJsEvent) {
    // --- C++ listeners on this element, for this type and phase -------------
    // Gathered first: they are the only listeners that can run when the realm
    // has no JSContext, and their presence decides whether the JS loop below
    // has to merge or can stay on its existing fast path.
    auto* nativeList = current->nativeListeners();
    std::vector<NativeEntryPtr> nativeEntries;
    if (nativeList) {
        for (auto& e : nativeList->snapshot(event.type()))
            if (listenerRunsInPhase(phase, e->opts.capture))
                nativeEntries.push_back(std::move(e));
    }

    // A C++ listener sees the target the way this scope sees it — retargeted
    // to the shadow host outside the shadow tree, exactly like the JS side's
    // event.target. Restored afterwards so the next scope retargets from the
    // real target.
    bro::dom::Element* savedTarget = event.target();
    struct TargetRestore {
        bro::dom::Event& ev; bro::dom::Element* saved;
        ~TargetRestore() { ev.setTarget(saved); }
    } targetRestore{event, savedTarget};
    if (retargetedTarget) event.setTarget(retargetedTarget);

    if (!ctx) {
        // Realm with no JS: C++ listeners are the whole of dispatch. Inline
        // on* attributes and el.onclick handlers are JS by definition and
        // cannot run here.
        for (auto& e : nativeEntries)
            if (!invokeNativeEntry(e, nativeList, event, nullptr, JS_UNDEFINED)) break;
        return;
    }

    // Check if this element has registered listeners OR an inline handler
    bool hasListeners = current->hasJsListener(event.type());
    bool hasInlineHandler = false;
    std::string attrName;
    if (phase == AT_TARGET || phase == BUBBLING_PHASE) {
        attrName = "on" + event.type();
        hasInlineHandler = !current->getAttribute(attrName).empty();
    }

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue elemMap = JS_GetPropertyStr(ctx, global, "__bro_elem_map");
    if (JS_IsUndefined(elemMap)) {
        // Map doesn't exist yet (no JS has accessed any DOM element).
        // Create it so inline handlers and wrapElement can use it.
        elemMap = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "__bro_elem_map", JS_DupValue(ctx, elemMap));
    }

    std::string elemKey = std::to_string(current->nodeId());
    JSValue jsElem = JS_GetPropertyStr(ctx, elemMap, elemKey.c_str());
    JS_FreeValue(ctx, elemMap);

    // Check for IDL event-handler property (e.g. el.onclick = fn). Only possible
    // if a JS wrapper already exists; a fresh wrapper would have no such prop.
    bool hasPropertyHandler = false;
    if (!attrName.empty() && !JS_IsUndefined(jsElem) && !JS_IsNull(jsElem)) {
        JSValue propHandler = JS_GetPropertyStr(ctx, jsElem, attrName.c_str());
        hasPropertyHandler = JS_IsFunction(ctx, propHandler);
        JS_FreeValue(ctx, propHandler);
    }

    if (!hasListeners && !hasInlineHandler && !hasPropertyHandler &&
        nativeEntries.empty()) {
        JS_FreeValue(ctx, jsElem);
        JS_FreeValue(ctx, global);
        return;
    }

    if (JS_IsUndefined(jsElem) || JS_IsNull(jsElem)) {
        JS_FreeValue(ctx, jsElem);
        // Element has no JS wrapper yet. Create one on demand so that
        // inline event handler attributes (onclick, etc.) can fire.
        jsElem = DomBindings::wrapElement(ctx, current);
        if (JS_IsUndefined(jsElem) || JS_IsException(jsElem)) {
            JS_FreeValue(ctx, jsElem);
            JS_FreeValue(ctx, global);
            return;
        }
    }

    JSValue listenersArr = JS_GetPropertyStr(ctx, jsElem, "__bro_listeners");
    bool hasListenersArr = !JS_IsUndefined(listenersArr) && JS_IsArray(listenersArr);

    int64_t len = 0;
    if (hasListenersArr) {
        JSValue lenVal = JS_GetPropertyStr(ctx, listenersArr, "length");
        JS_ToInt64(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
    }

    // Build the JS event object — reuse original if provided (preserves detail, etc.)
    bool ownsEvent = JS_IsUndefined(originalJsEvent);
    JSValue jsEvent;
    if (ownsEvent) {
        jsEvent = JS_NewObject(ctx);
        populateJsEvent(ctx, jsEvent, event);
    } else {
        jsEvent = JS_DupValue(ctx, originalJsEvent);
    }

    // Set eventPhase
    JS_SetPropertyStr(ctx, jsEvent, "eventPhase", JS_NewInt32(ctx, phase));

    // Set currentTarget to the current element
    JS_SetPropertyStr(ctx, jsEvent, "currentTarget", JS_DupValue(ctx, jsElem));

    // Set target to the retargeted target (may be the shadow host from outside)
    {
        JSValue tgtGlobal = JS_GetGlobalObject(ctx);
        JSValue tgtMap = JS_GetPropertyStr(ctx, tgtGlobal, "__bro_elem_map");
        if (!JS_IsUndefined(tgtMap) && retargetedTarget) {
            std::string tgtKey = std::to_string(retargetedTarget->nodeId());
            JSValue tgtElem = JS_GetPropertyStr(ctx, tgtMap, tgtKey.c_str());
            if (JS_IsUndefined(tgtElem) || JS_IsNull(tgtElem)) {
                JS_FreeValue(ctx, tgtElem);
                tgtElem = DomBindings::wrapElement(ctx, retargetedTarget);
            }
            JS_SetPropertyStr(ctx, jsEvent, "target", tgtElem);
        } else {
            JS_SetPropertyStr(ctx, jsEvent, "target", JS_DupValue(ctx, jsElem));
        }
        JS_FreeValue(ctx, tgtMap);
        JS_FreeValue(ctx, tgtGlobal);
    }

    installJsEventMethods(ctx, jsEvent);

    // Collect indices of "once" listeners to remove after dispatch
    std::vector<int64_t> onceIndices;

    // Invoke the JS listener record at array index `i`. Returns false when
    // dispatch at this element must stop (stopImmediatePropagation).
    auto invokeJsEntryAt = [&](int64_t i) -> bool {
        JSValue entry = JS_GetPropertyInt64(ctx, listenersArr, i);
        if (JS_IsObject(entry)) {
            JSValue typeVal = JS_GetPropertyStr(ctx, entry, "type");
            const char* entryType = JS_ToCString(ctx, typeVal);
            bool match = entryType && event.type() == entryType;
            JS_FreeCString(ctx, entryType);
            JS_FreeValue(ctx, typeVal);

            if (match) {
                // Check capture flag on listener
                JSValue captureVal = JS_GetPropertyStr(ctx, entry, "capture");
                bool isCapture = JS_ToBool(ctx, captureVal);
                JS_FreeValue(ctx, captureVal);

                // During capture phase, only invoke capture listeners.
                // During bubble phase, only invoke non-capture listeners.
                // At target, invoke all listeners regardless of capture flag.
                bool shouldInvoke = (phase == AT_TARGET) ||
                                    (phase == CAPTURING_PHASE && isCapture) ||
                                    (phase == BUBBLING_PHASE && !isCapture);

                if (shouldInvoke) {
                    JSValue cb = JS_GetPropertyStr(ctx, entry, "cb");
                    if (JS_IsFunction(ctx, cb)) {
                        JSValue result = Runtime::callJs(ctx, cb, jsElem, 1, &jsEvent,
                            ErrorOrigin::listener(describeListener(event.type(), current)));
                        JS_FreeValue(ctx, result);
                    }
                    JS_FreeValue(ctx, cb);

                    // Check if this is a "once" listener
                    JSValue onceVal = JS_GetPropertyStr(ctx, entry, "once");
                    if (JS_ToBool(ctx, onceVal)) {
                        onceIndices.push_back(i);
                    }
                    JS_FreeValue(ctx, onceVal);

                    // Check propagation flags
                    JSValue stoppedVal = JS_GetPropertyStr(ctx, jsEvent, "_stopped");
                    if (JS_ToBool(ctx, stoppedVal))
                        event.stopPropagation();
                    JS_FreeValue(ctx, stoppedVal);

                    // Check preventDefault from JS side
                    JSValue preventedVal = JS_GetPropertyStr(ctx, jsEvent, "_prevented");
                    if (JS_ToBool(ctx, preventedVal))
                        event.preventDefault();
                    JS_FreeValue(ctx, preventedVal);

                    JSValue immVal = JS_GetPropertyStr(ctx, jsEvent, "_immediateStopped");
                    bool immStopped = JS_ToBool(ctx, immVal);
                    JS_FreeValue(ctx, immVal);
                    if (immStopped) {
                        event.stopImmediatePropagation();
                        JS_FreeValue(ctx, entry);
                        return false;
                    }
                }
            }
        }
        JS_FreeValue(ctx, entry);
        return true;
    };

    if (nativeEntries.empty()) {
        // No C++ listeners here — the ordinary path, unchanged.
        for (int64_t i = 0; i < len; i++) {
            if (!invokeJsEntryAt(i)) break;
            if (event.propagationStopped()) break;
        }
    } else {
        // Merge the two lists on the shared registration sequence so C++ and
        // JS listeners on this element fire in the order they were added.
        struct Slot { uint64_t seq; int64_t jsIndex; const NativeEntryPtr* native; };
        std::vector<Slot> slots;
        slots.reserve(static_cast<size_t>(len) + nativeEntries.size());
        for (int64_t i = 0; i < len; i++) {
            JSValue entry = JS_GetPropertyInt64(ctx, listenersArr, i);
            if (JS_IsObject(entry)) {
                JSValue typeVal = JS_GetPropertyStr(ctx, entry, "type");
                const char* entryType = JS_ToCString(ctx, typeVal);
                bool match = entryType && event.type() == entryType;
                JS_FreeCString(ctx, entryType);
                JS_FreeValue(ctx, typeVal);
                if (match) slots.push_back({jsListenerSeq(ctx, entry), i, nullptr});
            }
            JS_FreeValue(ctx, entry);
        }
        for (const auto& e : nativeEntries) slots.push_back({e->seq, -1, &e});
        std::stable_sort(slots.begin(), slots.end(),
                         [](const Slot& a, const Slot& b) { return a.seq < b.seq; });

        for (const auto& slot : slots) {
            if (slot.native) {
                if (!invokeNativeEntry(*slot.native, nativeList, event, ctx, jsEvent))
                    break;
            } else {
                if (!invokeJsEntryAt(slot.jsIndex)) break;
            }
            if (event.propagationStopped()) break;
        }
    }

    // Remove "once" listeners by compacting the array (splice out holes)
    if (!onceIndices.empty()) {
        // Mark slots as undefined
        for (auto it2 = onceIndices.rbegin(); it2 != onceIndices.rend(); ++it2) {
            JS_SetPropertyInt64(ctx, listenersArr, *it2, JS_UNDEFINED);
        }
        // Compact: shift valid entries down, then truncate
        int64_t dst = 0;
        for (int64_t src = 0; src < len; ++src) {
            JSValue v = JS_GetPropertyInt64(ctx, listenersArr, src);
            if (!JS_IsUndefined(v)) {
                if (dst != src)
                    JS_SetPropertyInt64(ctx, listenersArr, dst, v);
                else
                    JS_FreeValue(ctx, v);
                ++dst;
            } else {
                JS_FreeValue(ctx, v);
            }
        }
        // Truncate by setting length
        JS_SetPropertyStr(ctx, listenersArr, "length", JS_NewInt64(ctx, dst));
        // A reaped `once` listener is gone for the same reasons an explicitly
        // removed one is, so the element's per-type count has to come down with
        // it — otherwise firing a one-shot listener would leave its type looking
        // permanently subscribed. Every index in onceIndices matched
        // event.type() to be invoked at all.
        for (size_t k = 0; k < onceIndices.size(); ++k)
            current->removeJsListener(event.type());
    }

    // --- Inline event handler: IDL property (el.onclick = fn) ---
    // Per DOM spec, fires after registered listeners during AT_TARGET/BUBBLING.
    // Prefer the JS property over the HTML attribute when both are set (matches
    // browser behavior: assigning el.onclick overrides the attribute).
    bool propertyHandlerFired = false;
    if ((phase == AT_TARGET || phase == BUBBLING_PHASE) && !event.propagationStopped()) {
        JSValue propHandler = JS_GetPropertyStr(ctx, jsElem, attrName.c_str());
        if (JS_IsFunction(ctx, propHandler)) {
            JSValue result = Runtime::callJs(ctx, propHandler, jsElem, 1, &jsEvent,
                ErrorOrigin::listener(describeListener(event.type(), current) + " (." + attrName + ")"));
            if (JS_IsBool(result) && !JS_ToBool(ctx, result)) {
                event.preventDefault();
            }
            JS_FreeValue(ctx, result);
            propertyHandlerFired = true;

            JSValue stoppedVal = JS_GetPropertyStr(ctx, jsEvent, "_stopped");
            if (JS_ToBool(ctx, stoppedVal))
                event.stopPropagation();
            JS_FreeValue(ctx, stoppedVal);

            JSValue preventedVal = JS_GetPropertyStr(ctx, jsEvent, "_prevented");
            if (JS_ToBool(ctx, preventedVal))
                event.preventDefault();
            JS_FreeValue(ctx, preventedVal);
        }
        JS_FreeValue(ctx, propHandler);
    }

    // --- Inline event handler attributes (onclick, onmouseover, etc.) ---
    // Per DOM spec, inline handlers fire during AT_TARGET or BUBBLING phase,
    // after any registered listeners on the same element. Skipped when an IDL
    // property handler already fired (the property overrides the attribute).
    if ((phase == AT_TARGET || phase == BUBBLING_PHASE) && !event.propagationStopped() && !propertyHandlerFired) {
        std::string handlerCode = current->getAttribute(attrName);
        if (!handlerCode.empty()) {
            // Compile the handler as a function body with 'event' parameter.
            // 'this' is bound to the element.
            std::string funcSource = "(function(event){" + handlerCode + "\n})";
            JSValue func = JS_Eval(ctx, funcSource.c_str(), funcSource.size(),
                                   attrName.c_str(), JS_EVAL_TYPE_GLOBAL);
            if (JS_IsFunction(ctx, func)) {
                JSValue result = Runtime::callJs(ctx, func, jsElem, 1, &jsEvent,
                    ErrorOrigin::listener(describeListener(event.type(), current) + " (" + attrName + " attr)"));
                // onclick returning false means preventDefault
                if (JS_IsBool(result) && !JS_ToBool(ctx, result)) {
                    event.preventDefault();
                }
                JS_FreeValue(ctx, result);
            } else if (JS_IsException(func)) {
                Runtime::checkException(ctx, func, ErrorOrigin::eval(attrName));
            }
            JS_FreeValue(ctx, func);

            // Check propagation flags set by handler
            JSValue stoppedVal2 = JS_GetPropertyStr(ctx, jsEvent, "_stopped");
            if (JS_ToBool(ctx, stoppedVal2))
                event.stopPropagation();
            JS_FreeValue(ctx, stoppedVal2);

            JSValue preventedVal2 = JS_GetPropertyStr(ctx, jsEvent, "_prevented");
            if (JS_ToBool(ctx, preventedVal2))
                event.preventDefault();
            JS_FreeValue(ctx, preventedVal2);
        }
    }

    JS_FreeValue(ctx, jsEvent);
    JS_FreeValue(ctx, listenersArr);
    JS_FreeValue(ctx, jsElem);
    JS_FreeValue(ctx, global);
}

} // namespace bro::js
