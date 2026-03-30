#include "js/event_dispatch.h"
#include "js/runtime.h"
#include "dom/element.h"
#include "dom/event.h"

#include <string>
#include <cstring>

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

static void populateJsEvent(JSContext* ctx, JSValue jsEvent, bro::dom::Event& event) {
    JS_SetPropertyStr(ctx, jsEvent, "type",
                      JS_NewString(ctx, event.type().c_str()));
    JS_SetPropertyStr(ctx, jsEvent, "timeStamp",
                      JS_NewFloat64(ctx, event.timeStamp()));
    JS_SetPropertyStr(ctx, jsEvent, "bubbles",
                      JS_NewBool(ctx, event.bubbles()));
    JS_SetPropertyStr(ctx, jsEvent, "cancelable",
                      JS_NewBool(ctx, event.cancelable()));

    // MouseEvent properties
    auto* mouseEvt = dynamic_cast<bro::dom::MouseEvent*>(&event);
    if (mouseEvt) {
        JS_SetPropertyStr(ctx, jsEvent, "clientX",
                          JS_NewFloat64(ctx, mouseEvt->clientX()));
        JS_SetPropertyStr(ctx, jsEvent, "clientY",
                          JS_NewFloat64(ctx, mouseEvt->clientY()));
        JS_SetPropertyStr(ctx, jsEvent, "button",
                          JS_NewInt32(ctx, mouseEvt->button()));
        JS_SetPropertyStr(ctx, jsEvent, "buttons",
                          JS_NewInt32(ctx, mouseEvt->buttons()));
        JS_SetPropertyStr(ctx, jsEvent, "ctrlKey",
                          JS_NewBool(ctx, mouseEvt->ctrlKey()));
        JS_SetPropertyStr(ctx, jsEvent, "shiftKey",
                          JS_NewBool(ctx, mouseEvt->shiftKey()));
        JS_SetPropertyStr(ctx, jsEvent, "altKey",
                          JS_NewBool(ctx, mouseEvt->altKey()));
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
    }
}

void dispatchDomEvent(JSContext* ctx, bro::dom::Element* target, bro::dom::Event& event) {
    if (!target || !ctx) return;

    event.setTarget(target);

    // Walk from target up to root (bubble phase)
    for (bro::dom::Element* current = target; current != nullptr;
         current = current->parentElement()) {

        if (event.propagationStopped()) break;
        event.setCurrentTarget(current);

        // Check if this element has listeners for this event type
        auto& listeners = current->listeners();
        auto it = listeners.find(event.type());
        if (it == listeners.end() || it->second.empty()) continue;

        JSValue global = JS_GetGlobalObject(ctx);
        JSValue elemMap = JS_GetPropertyStr(ctx, global, "__bro_elem_map");
        if (JS_IsUndefined(elemMap)) {
            JS_FreeValue(ctx, global);
            continue;
        }

        std::string elemKey = std::to_string(current->nodeId());
        JSValue jsElem = JS_GetPropertyStr(ctx, elemMap, elemKey.c_str());
        JS_FreeValue(ctx, elemMap);

        if (JS_IsUndefined(jsElem) || JS_IsNull(jsElem)) {
            JS_FreeValue(ctx, jsElem);
            JS_FreeValue(ctx, global);
            continue;
        }

        JSValue listenersArr = JS_GetPropertyStr(ctx, jsElem, "__bro_listeners");
        if (JS_IsUndefined(listenersArr) || !JS_IsArray(listenersArr)) {
            JS_FreeValue(ctx, listenersArr);
            JS_FreeValue(ctx, jsElem);
            JS_FreeValue(ctx, global);
            continue;
        }

        int64_t len = 0;
        JSValue lenVal = JS_GetPropertyStr(ctx, listenersArr, "length");
        JS_ToInt64(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);

        // Build the JS event object with all properties + methods
        JSValue jsEvent = JS_NewObject(ctx);
        populateJsEvent(ctx, jsEvent, event);

        // Set target and currentTarget as wrapped element references
        JS_SetPropertyStr(ctx, jsEvent, "currentTarget", JS_DupValue(ctx, jsElem));
        {
            // Find the original target element wrapper
            JSValue tgtGlobal = JS_GetGlobalObject(ctx);
            JSValue tgtMap = JS_GetPropertyStr(ctx, tgtGlobal, "__bro_elem_map");
            if (!JS_IsUndefined(tgtMap) && event.target()) {
                std::string tgtKey = std::to_string(event.target()->nodeId());
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

        for (int64_t i = 0; i < len; i++) {
            JSValue entry = JS_GetPropertyInt64(ctx, listenersArr, i);
            if (JS_IsObject(entry)) {
                JSValue typeVal = JS_GetPropertyStr(ctx, entry, "type");
                const char* entryType = JS_ToCString(ctx, typeVal);
                bool match = entryType && event.type() == entryType;
                JS_FreeCString(ctx, entryType);
                JS_FreeValue(ctx, typeVal);

                if (match) {
                    JSValue cb = JS_GetPropertyStr(ctx, entry, "cb");
                    if (JS_IsFunction(ctx, cb)) {
                        JSValue result = JS_Call(ctx, cb, jsElem, 1, &jsEvent);
                        if (JS_IsException(result)) {
                            Runtime::checkException(ctx, result);
                        }
                        JS_FreeValue(ctx, result);
                    }
                    JS_FreeValue(ctx, cb);

                    // Read back propagation flags set by JS callbacks
                    JSValue stoppedVal = JS_GetPropertyStr(ctx, jsEvent, "_stopped");
                    if (JS_ToBool(ctx, stoppedVal))
                        event.stopPropagation();
                    JS_FreeValue(ctx, stoppedVal);

                    JSValue immVal = JS_GetPropertyStr(ctx, jsEvent, "_immediateStopped");
                    bool immStopped = JS_ToBool(ctx, immVal);
                    JS_FreeValue(ctx, immVal);
                    if (immStopped) { JS_FreeValue(ctx, entry); break; }
                }
            }
            JS_FreeValue(ctx, entry);

            if (event.propagationStopped()) break;
        }

        JS_FreeValue(ctx, jsEvent);
        JS_FreeValue(ctx, listenersArr);
        JS_FreeValue(ctx, jsElem);
        JS_FreeValue(ctx, global);

        if (!event.bubbles()) break;
    }
}

} // namespace bro::js
