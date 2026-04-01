#include "js/dom_bindings_internal.h"

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

static void js_event_finalizer(JSRuntime* /*rt*/, JSValue val)
{
    auto* data = static_cast<EventData*>(
        JS_GetOpaque(val, js_event_class_id));
    delete data;
}

static JSClassDef js_event_class = {
    "Event",
    js_event_finalizer,
    nullptr, nullptr, nullptr
};

static JSValue js_event_get_type(JSContext* ctx, JSValueConst this_val)
{
    auto* e = static_cast<EventData*>(JS_GetOpaque(this_val, js_event_class_id));
    if (!e) return JS_UNDEFINED;
    return JS_NewString(ctx, e->type.c_str());
}

static JSValue js_event_get_target(JSContext* ctx, JSValueConst this_val)
{
    auto* e = static_cast<EventData*>(JS_GetOpaque(this_val, js_event_class_id));
    if (!e || !e->target) return JS_NULL;
    return DomBindings::wrapElement(ctx, e->target);
}

static JSValue js_event_get_currentTarget(JSContext* ctx, JSValueConst this_val)
{
    auto* e = static_cast<EventData*>(JS_GetOpaque(this_val, js_event_class_id));
    if (!e || !e->currentTarget) return JS_NULL;
    return DomBindings::wrapElement(ctx, e->currentTarget);
}

static JSValue js_event_get_bubbles(JSContext* ctx, JSValueConst this_val)
{
    auto* e = static_cast<EventData*>(JS_GetOpaque(this_val, js_event_class_id));
    if (!e) return JS_UNDEFINED;
    return JS_NewBool(ctx, e->bubbles);
}

static JSValue js_event_get_cancelable(JSContext* ctx, JSValueConst this_val)
{
    auto* e = static_cast<EventData*>(JS_GetOpaque(this_val, js_event_class_id));
    if (!e) return JS_UNDEFINED;
    return JS_NewBool(ctx, e->cancelable);
}

static JSValue js_event_get_defaultPrevented(JSContext* ctx, JSValueConst this_val)
{
    auto* e = static_cast<EventData*>(JS_GetOpaque(this_val, js_event_class_id));
    if (!e) return JS_UNDEFINED;
    return JS_NewBool(ctx, e->defaultPrevented);
}

static JSValue js_event_get_timeStamp(JSContext* ctx, JSValueConst this_val)
{
    auto* e = static_cast<EventData*>(JS_GetOpaque(this_val, js_event_class_id));
    if (!e) return JS_UNDEFINED;
    return JS_NewFloat64(ctx, e->timeStamp);
}

static JSValue js_event_preventDefault(JSContext* ctx, JSValueConst this_val,
                                       int /*argc*/, JSValueConst* /*argv*/)
{
    auto* e = static_cast<EventData*>(JS_GetOpaque(this_val, js_event_class_id));
    if (e && e->cancelable) e->defaultPrevented = true;
    (void)ctx;
    return JS_UNDEFINED;
}

static JSValue js_event_stopPropagation(JSContext* ctx, JSValueConst this_val,
                                        int /*argc*/, JSValueConst* /*argv*/)
{
    auto* e = static_cast<EventData*>(JS_GetOpaque(this_val, js_event_class_id));
    if (e) e->propagationStopped = true;
    (void)ctx;
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_event_proto_funcs[] = {
    JS_CGETSET_DEF("type",             js_event_get_type,             nullptr),
    JS_CGETSET_DEF("target",           js_event_get_target,           nullptr),
    JS_CGETSET_DEF("currentTarget",    js_event_get_currentTarget,    nullptr),
    JS_CGETSET_DEF("bubbles",          js_event_get_bubbles,          nullptr),
    JS_CGETSET_DEF("cancelable",       js_event_get_cancelable,       nullptr),
    JS_CGETSET_DEF("defaultPrevented", js_event_get_defaultPrevented, nullptr),
    JS_CGETSET_DEF("timeStamp",        js_event_get_timeStamp,        nullptr),
    JS_CFUNC_DEF("preventDefault",  0, js_event_preventDefault),
    JS_CFUNC_DEF("stopPropagation", 0, js_event_stopPropagation),
};

JSValue wrapEvent(JSContext* ctx, const std::string& type,
                  bro::dom::Element* target)
{
    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_event_class_id));
    if (JS_IsException(obj)) return obj;

    auto* data = new EventData();
    data->type          = type;
    data->target        = target;
    data->currentTarget = target;
    data->bubbles       = true;
    data->cancelable    = true;
    JS_SetOpaque(obj, data);
    return obj;
}

// ===========================================================================
// Registration
// ===========================================================================

void registerEventClasses(JSRuntime* rt) {
    JS_NewClass(rt, js_event_class_id, &js_event_class);
}

void installEventPrototypes(JSContext* ctx) {
    JSValue evt_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, evt_proto, js_event_proto_funcs,
                               sizeof(js_event_proto_funcs) / sizeof(js_event_proto_funcs[0]));
    JS_SetClassProto(ctx, js_event_class_id, evt_proto);
}

} // namespace bro::js
