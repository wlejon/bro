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
    qjsbind::Class<EventData>(ctx, "Event", qjsbind::NoGlobal)
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
