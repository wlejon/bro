#include "js/event_dispatch.h"
#include "js/runtime.h"
#include "dom/element.h"
#include "dom/shadow_root.h"
#include "dom/event.h"

#include <string>
#include <cstring>
#include <vector>

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

// C-function methods for plain JS event objects.  They set flag properties
// on the JS object which are read back after each listener call and
// propagated to the C++ Event.
static JSValue js_ev_stopPropagation(JSContext* ctx, JSValueConst this_val,
                                     int /*argc*/, JSValueConst* /*argv*/) {
    JS_SetPropertyStr(ctx, this_val, "_stopped", JS_TRUE);
    return JS_UNDEFINED;
}

static JSValue js_ev_preventDefault(JSContext* ctx, JSValueConst this_val,
                                    int /*argc*/, JSValueConst* /*argv*/) {
    JS_SetPropertyStr(ctx, this_val, "_prevented", JS_TRUE);
    JS_SetPropertyStr(ctx, this_val, "defaultPrevented", JS_TRUE);
    return JS_UNDEFINED;
}

static JSValue js_ev_stopImmediatePropagation(JSContext* ctx, JSValueConst this_val,
                                              int /*argc*/, JSValueConst* /*argv*/) {
    JS_SetPropertyStr(ctx, this_val, "_stopped", JS_TRUE);
    JS_SetPropertyStr(ctx, this_val, "_immediateStopped", JS_TRUE);
    return JS_UNDEFINED;
}

// Event phases per DOM spec
static constexpr int NONE = 0;
static constexpr int CAPTURING_PHASE = 1;
static constexpr int AT_TARGET = 2;
static constexpr int BUBBLING_PHASE = 3;

// Build the event path from target up to the document root.
// Handles shadow DOM: when an element is inside a shadow root, the path
// crosses from shadow tree → host element → host's parent, etc.
// At each shadow boundary, the effective target is retargeted to the host.
struct EventPathEntry {
    bro::dom::Element* element;
    bro::dom::Element* retargetedTarget; // target visible at this scope
};

static std::vector<EventPathEntry> buildEventPath(bro::dom::Element* target) {
    std::vector<EventPathEntry> path;

    // Current target as we walk up
    bro::dom::Element* current = target;
    bro::dom::Element* effectiveTarget = target;

    while (current) {
        path.push_back({current, effectiveTarget});

        // Check if current element's parent is a ShadowRoot
        auto* parentNode = current->parentNode();
        if (parentNode && parentNode->nodeType() == bro::dom::NodeType::DocumentFragment) {
            auto* sr = dynamic_cast<bro::dom::ShadowRoot*>(parentNode);
            if (sr && sr->host()) {
                // Crossing shadow boundary: retarget to the host
                effectiveTarget = sr->host();
                current = sr->host();
                continue;
            }
        }

        // Normal parent traversal
        current = current->parentElement();
    }

    return path;
}

// composedPath() implementation — returns JS array of elements in the event path
static JSValue js_ev_composedPath(JSContext* ctx, JSValueConst this_val,
                                  int /*argc*/, JSValueConst* /*argv*/) {
    // Retrieve the stashed path array
    JSValue pathArr = JS_GetPropertyStr(ctx, this_val, "_composedPath");
    if (!JS_IsUndefined(pathArr) && !JS_IsNull(pathArr)) {
        return pathArr; // already owns a ref from GetProperty
    }
    JS_FreeValue(ctx, pathArr);
    return JS_NewArray(ctx); // empty array fallback
}

static void populateJsEvent(JSContext* ctx, JSValue jsEvent, bro::dom::Event& event) {
    JS_SetPropertyStr(ctx, jsEvent, "type",
                      JS_NewString(ctx, event.type().c_str()));
    JS_SetPropertyStr(ctx, jsEvent, "timeStamp",
                      JS_NewFloat64(ctx, event.timeStamp()));
    JS_SetPropertyStr(ctx, jsEvent, "bubbles",
                      JS_NewBool(ctx, event.bubbles()));
    JS_SetPropertyStr(ctx, jsEvent, "cancelable",
                      JS_NewBool(ctx, event.cancelable()));
    JS_SetPropertyStr(ctx, jsEvent, "composed",
                      JS_NewBool(ctx, event.composed()));
    JS_SetPropertyStr(ctx, jsEvent, "isTrusted",
                      JS_NewBool(ctx, event.isTrusted()));
    JS_SetPropertyStr(ctx, jsEvent, "eventPhase",
                      JS_NewInt32(ctx, NONE));
    JS_SetPropertyStr(ctx, jsEvent, "defaultPrevented",
                      JS_NewBool(ctx, false));

    // MouseEvent properties
    auto* mouseEvt = dynamic_cast<bro::dom::MouseEvent*>(&event);
    if (mouseEvt) {
        JS_SetPropertyStr(ctx, jsEvent, "clientX",
                          JS_NewFloat64(ctx, mouseEvt->clientX()));
        JS_SetPropertyStr(ctx, jsEvent, "clientY",
                          JS_NewFloat64(ctx, mouseEvt->clientY()));
        JS_SetPropertyStr(ctx, jsEvent, "pageX",
                          JS_NewFloat64(ctx, mouseEvt->pageX()));
        JS_SetPropertyStr(ctx, jsEvent, "pageY",
                          JS_NewFloat64(ctx, mouseEvt->pageY()));
        JS_SetPropertyStr(ctx, jsEvent, "screenX",
                          JS_NewFloat64(ctx, mouseEvt->screenX()));
        JS_SetPropertyStr(ctx, jsEvent, "screenY",
                          JS_NewFloat64(ctx, mouseEvt->screenY()));
        JS_SetPropertyStr(ctx, jsEvent, "offsetX",
                          JS_NewFloat64(ctx, mouseEvt->offsetX()));
        JS_SetPropertyStr(ctx, jsEvent, "offsetY",
                          JS_NewFloat64(ctx, mouseEvt->offsetY()));
        JS_SetPropertyStr(ctx, jsEvent, "movementX",
                          JS_NewFloat64(ctx, mouseEvt->movementX()));
        JS_SetPropertyStr(ctx, jsEvent, "movementY",
                          JS_NewFloat64(ctx, mouseEvt->movementY()));
        JS_SetPropertyStr(ctx, jsEvent, "button",
                          JS_NewInt32(ctx, mouseEvt->button()));
        JS_SetPropertyStr(ctx, jsEvent, "buttons",
                          JS_NewInt32(ctx, mouseEvt->buttons()));
        JS_SetPropertyStr(ctx, jsEvent, "detail",
                          JS_NewInt32(ctx, mouseEvt->detail()));
        JS_SetPropertyStr(ctx, jsEvent, "ctrlKey",
                          JS_NewBool(ctx, mouseEvt->ctrlKey()));
        JS_SetPropertyStr(ctx, jsEvent, "shiftKey",
                          JS_NewBool(ctx, mouseEvt->shiftKey()));
        JS_SetPropertyStr(ctx, jsEvent, "altKey",
                          JS_NewBool(ctx, mouseEvt->altKey()));
        JS_SetPropertyStr(ctx, jsEvent, "metaKey",
                          JS_NewBool(ctx, mouseEvt->metaKey()));
        if (mouseEvt->relatedTarget()) {
            JSValue global = JS_GetGlobalObject(ctx);
            JSValue elemMap = JS_GetPropertyStr(ctx, global, "__bro_elem_map");
            if (!JS_IsUndefined(elemMap)) {
                std::string key = std::to_string(mouseEvt->relatedTarget()->nodeId());
                JSValue rtElem = JS_GetPropertyStr(ctx, elemMap, key.c_str());
                JS_SetPropertyStr(ctx, jsEvent, "relatedTarget", rtElem);
            } else {
                JS_SetPropertyStr(ctx, jsEvent, "relatedTarget", JS_NULL);
            }
            JS_FreeValue(ctx, elemMap);
            JS_FreeValue(ctx, global);
        } else {
            JS_SetPropertyStr(ctx, jsEvent, "relatedTarget", JS_NULL);
        }

        // WheelEvent properties
        auto* wheelEvt = dynamic_cast<bro::dom::WheelEvent*>(&event);
        if (wheelEvt) {
            JS_SetPropertyStr(ctx, jsEvent, "deltaX",
                              JS_NewFloat64(ctx, wheelEvt->deltaX()));
            JS_SetPropertyStr(ctx, jsEvent, "deltaY",
                              JS_NewFloat64(ctx, wheelEvt->deltaY()));
            JS_SetPropertyStr(ctx, jsEvent, "deltaZ",
                              JS_NewFloat64(ctx, wheelEvt->deltaZ()));
            JS_SetPropertyStr(ctx, jsEvent, "deltaMode",
                              JS_NewInt32(ctx, wheelEvt->deltaMode()));
        }
    }

    // KeyboardEvent properties
    auto* keyEvt = dynamic_cast<bro::dom::KeyboardEvent*>(&event);
    if (keyEvt) {
        JS_SetPropertyStr(ctx, jsEvent, "key",
                          JS_NewString(ctx, keyEvt->key().c_str()));
        JS_SetPropertyStr(ctx, jsEvent, "code",
                          JS_NewString(ctx, keyEvt->code().c_str()));
        JS_SetPropertyStr(ctx, jsEvent, "ctrlKey",
                          JS_NewBool(ctx, keyEvt->ctrlKey()));
        JS_SetPropertyStr(ctx, jsEvent, "shiftKey",
                          JS_NewBool(ctx, keyEvt->shiftKey()));
        JS_SetPropertyStr(ctx, jsEvent, "altKey",
                          JS_NewBool(ctx, keyEvt->altKey()));
        JS_SetPropertyStr(ctx, jsEvent, "metaKey",
                          JS_NewBool(ctx, keyEvt->metaKey()));
        JS_SetPropertyStr(ctx, jsEvent, "repeat",
                          JS_NewBool(ctx, keyEvt->repeat()));
        JS_SetPropertyStr(ctx, jsEvent, "isComposing",
                          JS_NewBool(ctx, keyEvt->isComposing()));
        JS_SetPropertyStr(ctx, jsEvent, "location",
                          JS_NewInt32(ctx, keyEvt->location()));
        // Legacy properties (deprecated but widely used)
        JS_SetPropertyStr(ctx, jsEvent, "keyCode",
                          JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, jsEvent, "charCode",
                          JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, jsEvent, "which",
                          JS_NewInt32(ctx, 0));
    }

    // FocusEvent properties
    auto* focusEvt = dynamic_cast<bro::dom::FocusEvent*>(&event);
    if (focusEvt) {
        if (focusEvt->relatedTarget()) {
            JSValue global = JS_GetGlobalObject(ctx);
            JSValue elemMap = JS_GetPropertyStr(ctx, global, "__bro_elem_map");
            if (!JS_IsUndefined(elemMap)) {
                std::string key = std::to_string(focusEvt->relatedTarget()->nodeId());
                JSValue rtElem = JS_GetPropertyStr(ctx, elemMap, key.c_str());
                JS_SetPropertyStr(ctx, jsEvent, "relatedTarget", rtElem);
            } else {
                JS_SetPropertyStr(ctx, jsEvent, "relatedTarget", JS_NULL);
            }
            JS_FreeValue(ctx, elemMap);
            JS_FreeValue(ctx, global);
        } else {
            JS_SetPropertyStr(ctx, jsEvent, "relatedTarget", JS_NULL);
        }
    }

    // InputEvent properties
    auto* inputEvt = dynamic_cast<bro::dom::InputEvent*>(&event);
    if (inputEvt) {
        if (inputEvt->data().empty()) {
            JS_SetPropertyStr(ctx, jsEvent, "data", JS_NULL);
        } else {
            JS_SetPropertyStr(ctx, jsEvent, "data",
                              JS_NewString(ctx, inputEvt->data().c_str()));
        }
        JS_SetPropertyStr(ctx, jsEvent, "inputType",
                          JS_NewString(ctx, inputEvt->inputType().c_str()));
        JS_SetPropertyStr(ctx, jsEvent, "isComposing",
                          JS_NewBool(ctx, inputEvt->isComposing()));
    }
}

// Stash the composedPath as a JS array on the event object
static void stashComposedPath(JSContext* ctx, JSValue jsEvent,
                              const std::vector<EventPathEntry>& path) {
    JSValue arr = JS_NewArray(ctx);
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue elemMap = JS_GetPropertyStr(ctx, global, "__bro_elem_map");

    if (!JS_IsUndefined(elemMap)) {
        for (size_t i = 0; i < path.size(); ++i) {
            std::string key = std::to_string(path[i].element->nodeId());
            JSValue elem = JS_GetPropertyStr(ctx, elemMap, key.c_str());
            if (!JS_IsUndefined(elem) && !JS_IsNull(elem)) {
                JS_SetPropertyInt64(ctx, arr, static_cast<int64_t>(i), elem);
            } else {
                JS_FreeValue(ctx, elem);
            }
        }
    }

    JS_FreeValue(ctx, elemMap);
    JS_FreeValue(ctx, global);
    JS_SetPropertyStr(ctx, jsEvent, "_composedPath", arr);
}

// phase: CAPTURING_PHASE, AT_TARGET, or BUBBLING_PHASE
static void invokeListeners(JSContext* ctx, bro::dom::Element* current,
                            bro::dom::Element* retargetedTarget,
                            bro::dom::Event& event,
                            int phase,
                            JSValue originalJsEvent = JS_UNDEFINED) {
    auto& listeners = current->listeners();
    auto it = listeners.find(event.type());
    if (it == listeners.end() || it->second.empty()) return;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue elemMap = JS_GetPropertyStr(ctx, global, "__bro_elem_map");
    if (JS_IsUndefined(elemMap)) {
        JS_FreeValue(ctx, global);
        return;
    }

    std::string elemKey = std::to_string(current->nodeId());
    JSValue jsElem = JS_GetPropertyStr(ctx, elemMap, elemKey.c_str());
    JS_FreeValue(ctx, elemMap);

    if (JS_IsUndefined(jsElem) || JS_IsNull(jsElem)) {
        JS_FreeValue(ctx, jsElem);
        JS_FreeValue(ctx, global);
        return;
    }

    JSValue listenersArr = JS_GetPropertyStr(ctx, jsElem, "__bro_listeners");
    if (JS_IsUndefined(listenersArr) || !JS_IsArray(listenersArr)) {
        JS_FreeValue(ctx, listenersArr);
        JS_FreeValue(ctx, jsElem);
        JS_FreeValue(ctx, global);
        return;
    }

    int64_t len = 0;
    JSValue lenVal = JS_GetPropertyStr(ctx, listenersArr, "length");
    JS_ToInt64(ctx, &len, lenVal);
    JS_FreeValue(ctx, lenVal);

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
            JS_SetPropertyStr(ctx, jsEvent, "target", tgtElem);
        } else {
            JS_SetPropertyStr(ctx, jsEvent, "target", JS_DupValue(ctx, jsElem));
        }
        JS_FreeValue(ctx, tgtMap);
        JS_FreeValue(ctx, tgtGlobal);
    }

    JS_SetPropertyStr(ctx, jsEvent, "stopPropagation",
        JS_NewCFunction(ctx, js_ev_stopPropagation, "stopPropagation", 0));
    JS_SetPropertyStr(ctx, jsEvent, "preventDefault",
        JS_NewCFunction(ctx, js_ev_preventDefault, "preventDefault", 0));
    JS_SetPropertyStr(ctx, jsEvent, "stopImmediatePropagation",
        JS_NewCFunction(ctx, js_ev_stopImmediatePropagation, "stopImmediatePropagation", 0));
    JS_SetPropertyStr(ctx, jsEvent, "composedPath",
        JS_NewCFunction(ctx, js_ev_composedPath, "composedPath", 0));

    // Collect indices of "once" listeners to remove after dispatch
    std::vector<int64_t> onceIndices;

    for (int64_t i = 0; i < len; i++) {
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
                        JSValue result = JS_Call(ctx, cb, jsElem, 1, &jsEvent);
                        if (JS_IsException(result)) {
                            Runtime::checkException(ctx, result);
                        }
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
                        break;
                    }
                }
            }
        }
        JS_FreeValue(ctx, entry);

        if (event.propagationStopped()) break;
    }

    // Remove "once" listeners (iterate in reverse to preserve indices)
    for (auto it2 = onceIndices.rbegin(); it2 != onceIndices.rend(); ++it2) {
        JS_SetPropertyInt64(ctx, listenersArr, *it2, JS_UNDEFINED);
    }

    JS_FreeValue(ctx, jsEvent);
    JS_FreeValue(ctx, listenersArr);
    JS_FreeValue(ctx, jsElem);
    JS_FreeValue(ctx, global);
}

void dispatchDomEvent(JSContext* ctx, bro::dom::Element* target, bro::dom::Event& event,
                      JSValue originalJsEvent) {
    if (!target || !ctx) return;

    event.setTarget(target);

    // Build the full event path including shadow DOM retargeting.
    // path[0] = target, path[N-1] = root ancestor
    auto path = buildEventPath(target);
    if (path.empty()) return;

    // Stash composed path on the original JS event if provided
    if (!JS_IsUndefined(originalJsEvent)) {
        stashComposedPath(ctx, originalJsEvent, path);
    }

    // --- Capture phase: root → target (exclusive) ---
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

    // --- Bubble phase: target parent → root ---
    if (event.bubbles()) {
        for (size_t i = 1; i < path.size(); ++i) {
            if (event.propagationStopped()) break;
            event.setCurrentTarget(path[i].element);
            event.setEventPhase(BUBBLING_PHASE);
            invokeListeners(ctx, path[i].element, path[i].retargetedTarget,
                            event, BUBBLING_PHASE, originalJsEvent);
        }
    }

    // Reset phase after dispatch
    event.setEventPhase(NONE);
    event.setCurrentTarget(nullptr);
}

} // namespace bro::js
