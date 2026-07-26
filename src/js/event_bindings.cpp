#include "js/dom_bindings_internal.h"

#include <qjsbind/qjsbind.h>

namespace bro::js {

// ===========================================================================
// Event wrapper
// ===========================================================================

struct EventData {
    std::string type;
    bro::dom::Element* target        = nullptr;
    bro::dom::Element* currentTarget = nullptr;
    bool bubbles          = false;
    bool cancelable       = false;
    bool defaultPrevented = false;
    double timeStamp      = 0;
    bool propagationStopped = false;
};

JSValue wrapEvent(JSContext* ctx, const std::string& type,
                  bro::dom::Element* target)
{
    auto* data = new EventData();
    data->type          = type;
    data->target        = target;
    data->currentTarget = target;
    data->bubbles       = true;
    data->cancelable    = true;
    return qjsbind::wrap<EventData>(ctx, data);
}

// ===========================================================================
// Registration
// ===========================================================================

void installEventBindings(JSContext* ctx) {
    // `new Event(type, {bubbles, cancelable})`, per the DOM spec. Without a
    // global constructor, synthesising an event — the normal way to drive a
    // component from a test, or to notify listeners of a state change a
    // script made itself — meant hand-rolling an object literal with a `type`
    // property and hoping dispatchEvent kept accepting it.
    qjsbind::Class<EventData>(ctx, "Event")
        .constructor([](JSContext* cx, int argc, JSValueConst* argv) -> EventData* {
            auto* e = new EventData();
            if (argc >= 1) {
                if (const char* t = JS_ToCString(cx, argv[0])) {
                    e->type = t;
                    JS_FreeCString(cx, t);
                }
            }
            if (argc >= 2 && JS_IsObject(argv[1])) {
                JSValue b = JS_GetPropertyStr(cx, argv[1], "bubbles");
                e->bubbles = JS_ToBool(cx, b);
                JS_FreeValue(cx, b);
                JSValue c = JS_GetPropertyStr(cx, argv[1], "cancelable");
                e->cancelable = JS_ToBool(cx, c);
                JS_FreeValue(cx, c);
            }
            return e;
        })
        .get("type", [](EventData* e) -> std::string { return e->type; })
        .get("target", [](EventData* e, JSContext* cx) -> JSValue {
            if (!e->target) return JS_NULL;
            return DomBindings::wrapElement(cx, e->target);
        })
        .get("currentTarget", [](EventData* e, JSContext* cx) -> JSValue {
            if (!e->currentTarget) return JS_NULL;
            return DomBindings::wrapElement(cx, e->currentTarget);
        })
        .get("bubbles", [](EventData* e) -> bool { return e->bubbles; })
        .get("cancelable", [](EventData* e) -> bool { return e->cancelable; })
        .get("defaultPrevented", [](EventData* e) -> bool { return e->defaultPrevented; })
        .get("timeStamp", [](EventData* e) -> double { return e->timeStamp; })
        .method("preventDefault", [](EventData* e) {
            if (e->cancelable) e->defaultPrevented = true;
        })
        .method("stopPropagation", [](EventData* e) {
            e->propagationStopped = true;
        });

    js_event_class_id = qjsbind::class_id<EventData>();
}

} // namespace bro::js
