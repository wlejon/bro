#include "js/window_bindings.h"
#include "platform/event_loop.h"

#include "window_polyfill.js.h"

#include <cstring>

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

void installWindowBindings(JSContext* ctx, int viewportWidth, int viewportHeight)
{
    JSValue global = JS_GetGlobalObject(ctx);

    // window = globalThis
    JS_SetPropertyStr(ctx, global, "window", JS_DupValue(ctx, global));
    JS_SetPropertyStr(ctx, global, "devicePixelRatio", JS_NewFloat64(ctx, 1.0));
    JS_SetPropertyStr(ctx, global, "innerWidth", JS_NewInt32(ctx, viewportWidth));
    JS_SetPropertyStr(ctx, global, "innerHeight", JS_NewInt32(ctx, viewportHeight));

    // navigator — extend existing (brokit may have created it) rather than replace
    JSValue nav = JS_GetPropertyStr(ctx, global, "navigator");
    if (JS_IsUndefined(nav) || JS_IsNull(nav)) {
        JS_FreeValue(ctx, nav);
        nav = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "navigator", JS_DupValue(ctx, nav));
    }
    JS_SetPropertyStr(ctx, nav, "userAgent", JS_NewString(ctx, "Bro/1.0"));
    JS_SetPropertyStr(ctx, nav, "platform", JS_NewString(ctx, "Win32"));
    JS_SetPropertyStr(ctx, nav, "language", JS_NewString(ctx, "en-US"));
    JS_FreeValue(ctx, nav);

    // location (initial values — polyfill adds methods)
    JSValue loc = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, loc, "href",     JS_NewString(ctx, "bro://app/"));
    JS_SetPropertyStr(ctx, loc, "origin",   JS_NewString(ctx, "bro://app"));
    JS_SetPropertyStr(ctx, loc, "protocol", JS_NewString(ctx, "bro:"));
    JS_SetPropertyStr(ctx, loc, "host",     JS_NewString(ctx, "app"));
    JS_SetPropertyStr(ctx, loc, "hostname", JS_NewString(ctx, "app"));
    JS_SetPropertyStr(ctx, loc, "port",     JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, loc, "pathname", JS_NewString(ctx, "/"));
    JS_SetPropertyStr(ctx, loc, "search",   JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, loc, "hash",     JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, global, "location", loc);

    // history (initial values — polyfill adds methods)
    JSValue history = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, history, "state", JS_NULL);
    JS_SetPropertyStr(ctx, history, "length", JS_NewInt32(ctx, 1));
    JS_SetPropertyStr(ctx, global, "history", history);

    // Window events + SPA history/location polyfill
    JSValue r = JS_Eval(ctx, js_window_polyfill, strlen(js_window_polyfill),
                        "<window-bindings>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx, r);

    JS_FreeValue(ctx, global);
}

void installWindowClose(JSContext* ctx, platform::EventLoop* eventLoop) {
    if (!eventLoop) return;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ptrVal = JS_NewInt64(ctx, static_cast<int64_t>(
        reinterpret_cast<intptr_t>(eventLoop)));
    JS_SetPropertyStr(ctx, global, "close",
        JS_NewCFunctionData(ctx, [](JSContext*, JSValue, int, JSValue*,
                                   int, JSValue* fdata) -> JSValue {
            int64_t p = 0;
            JS_ToInt64(nullptr, &p, fdata[0]);
            auto* loop = reinterpret_cast<platform::EventLoop*>(
                static_cast<intptr_t>(p));
            if (loop) loop->requestQuit();
            return JS_UNDEFINED;
        }, 0, 0, 1, &ptrVal));
    JS_FreeValue(ctx, ptrVal);
    JS_FreeValue(ctx, global);
}

} // namespace bro::js
