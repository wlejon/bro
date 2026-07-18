#include "js/window_bindings.h"
#include "platform/event_loop.h"
#include "platform/sdl_window.h"

#include "window_polyfill.js.h"

#include <cstring>

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_stdinc.h> // SDL_free

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

// ---------------------------------------------------------------------------
// Shared window state — one process, one platform window (may be null under
// --no-gpu headless). Set once per install; every realm (app, iframes,
// system panels) sees the same physical window, so plain statics are the
// right shape (same pattern as dialog_bindings' s_window).
// ---------------------------------------------------------------------------

static platform::Window* s_platWindow = nullptr;
static bool s_headless = false;

// navigator.clipboard backing — SDL is the only system-clipboard path bro links.
// These are the synchronous primitives; installWindowBindings wraps them in the
// Promise-returning writeText/readText the web Clipboard API exposes.
static JSValue js_clipboard_write(JSContext* ctx, JSValueConst /*this_val*/,
                                  int argc, JSValueConst* argv)
{
    const char* text = argc > 0 ? JS_ToCString(ctx, argv[0]) : nullptr;
    bool ok = SDL_SetClipboardText(text ? text : "");
    if (text) JS_FreeCString(ctx, text);
    return JS_NewBool(ctx, ok);
}

static JSValue js_clipboard_read(JSContext* ctx, JSValueConst /*this_val*/,
                                 int /*argc*/, JSValueConst* /*argv*/)
{
    char* text = SDL_GetClipboardText(); // never NULL — "" on empty/failure
    JSValue s = JS_NewString(ctx, text ? text : "");
    if (text) SDL_free(text);
    return s;
}

void installWindowBindings(JSContext* ctx, int viewportWidth, int viewportHeight,
                           double devicePixelRatio)
{
    JSValue global = JS_GetGlobalObject(ctx);

    // window = globalThis
    JS_SetPropertyStr(ctx, global, "window", JS_DupValue(ctx, global));
    JS_SetPropertyStr(ctx, global, "devicePixelRatio",
                      JS_NewFloat64(ctx, devicePixelRatio));
    JS_SetPropertyStr(ctx, global, "innerWidth", JS_NewInt32(ctx, viewportWidth));
    JS_SetPropertyStr(ctx, global, "innerHeight", JS_NewInt32(ctx, viewportHeight));
    JS_SetPropertyStr(ctx, global, "outerWidth", JS_NewInt32(ctx, viewportWidth));
    JS_SetPropertyStr(ctx, global, "outerHeight", JS_NewInt32(ctx, viewportHeight));
    JS_SetPropertyStr(ctx, global, "screenX", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, global, "screenY", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, global, "scrollX", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, global, "scrollY", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, global, "pageXOffset", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, global, "pageYOffset", JS_NewInt32(ctx, 0));

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

    // navigator.clipboard — the system clipboard over SDL. The native helpers are
    // synchronous; the JS wrapper below presents them as the Promise-returning
    // writeText/readText apps expect from the web Clipboard API.
    JSValue clip = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, clip, "__write",
                      JS_NewCFunction(ctx, js_clipboard_write, "writeText", 1));
    JS_SetPropertyStr(ctx, clip, "__read",
                      JS_NewCFunction(ctx, js_clipboard_read, "readText", 0));
    JS_SetPropertyStr(ctx, nav, "clipboard", clip);
    JS_FreeValue(ctx, nav);

    static const char kClipboardJs[] =
        "(function(){var c=navigator.clipboard;"
        "c.writeText=function(t){return c.__write(String(t==null?'':t))"
        "?Promise.resolve():Promise.reject(new Error('clipboard write failed'));};"
        "c.readText=function(){return Promise.resolve(c.__read());};})();";
    JSValue clipR = JS_Eval(ctx, kClipboardJs, strlen(kClipboardJs),
                            "<clipboard-bindings>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx, clipR);

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

// ---------------------------------------------------------------------------
// bro.window.* — runtime window management. Documented in docs/window-api.js.
//
// Headless policy: state-affecting ops (minimize/maximize/restore,
// setPosition) no-op so a test can never disturb the hidden window the whole
// pipeline renders through; flag/limit setters (borderless, alwaysOnTop,
// min/max size) still apply — they're pure window state, so tests can
// round-trip them without visible effect. With no window at all (--no-gpu)
// every query returns its pinned default and every mutator no-ops.
// ---------------------------------------------------------------------------

static JSValue js_brw_get_state(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    const char* state = "normal";
    if (s_platWindow) {
        if (s_platWindow->isMinimized())       state = "minimized";
        else if (s_platWindow->isFullscreen()) state = "fullscreen";
        else if (s_platWindow->isMaximized())  state = "maximized";
    }
    return JS_NewString(ctx, state);
}

static JSValue js_brw_get_borderless(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewBool(ctx, s_platWindow && s_platWindow->isBorderless());
}

static JSValue js_brw_set_borderless(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (s_platWindow && argc >= 1)
        s_platWindow->setBorderless(JS_ToBool(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_brw_get_alwaysOnTop(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewBool(ctx, s_platWindow && s_platWindow->isAlwaysOnTop());
}

static JSValue js_brw_set_alwaysOnTop(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (s_platWindow && argc >= 1)
        s_platWindow->setAlwaysOnTop(JS_ToBool(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_brw_minimize(JSContext*, JSValueConst, int, JSValueConst*) {
    if (s_platWindow && !s_headless) s_platWindow->minimize();
    return JS_UNDEFINED;
}

static JSValue js_brw_maximize(JSContext*, JSValueConst, int, JSValueConst*) {
    if (s_platWindow && !s_headless) s_platWindow->maximize();
    return JS_UNDEFINED;
}

static JSValue js_brw_restore(JSContext*, JSValueConst, int, JSValueConst*) {
    if (s_platWindow && !s_headless) s_platWindow->restore();
    return JS_UNDEFINED;
}

static JSValue js_brw_getPosition(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    int x = 0, y = 0;
    if (s_platWindow) s_platWindow->getPosition(x, y);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "x", JS_NewInt32(ctx, x));
    JS_SetPropertyStr(ctx, obj, "y", JS_NewInt32(ctx, y));
    return obj;
}

static JSValue js_brw_setPosition(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_platWindow || s_headless || argc < 2) return JS_UNDEFINED;
    int32_t x = 0, y = 0;
    if (JS_ToInt32(ctx, &x, argv[0]) || JS_ToInt32(ctx, &y, argv[1]))
        return JS_EXCEPTION;
    s_platWindow->setPosition(x, y);
    return JS_UNDEFINED;
}

static JSValue sizePairToJS(JSContext* ctx, int w, int h) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "width", JS_NewInt32(ctx, w));
    JS_SetPropertyStr(ctx, obj, "height", JS_NewInt32(ctx, h));
    return obj;
}

static JSValue js_brw_getMinSize(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    int w = 0, h = 0;
    if (s_platWindow) s_platWindow->getMinimumSize(w, h);
    return sizePairToJS(ctx, w, h);
}

static JSValue js_brw_setMinSize(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_platWindow || argc < 2) return JS_UNDEFINED;
    int32_t w = 0, h = 0;
    if (JS_ToInt32(ctx, &w, argv[0]) || JS_ToInt32(ctx, &h, argv[1]))
        return JS_EXCEPTION;
    s_platWindow->setMinimumSize(w, h);
    return JS_UNDEFINED;
}

static JSValue js_brw_getMaxSize(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    int w = 0, h = 0;
    if (s_platWindow) s_platWindow->getMaximumSize(w, h);
    return sizePairToJS(ctx, w, h);
}

static JSValue js_brw_setMaxSize(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_platWindow || argc < 2) return JS_UNDEFINED;
    int32_t w = 0, h = 0;
    if (JS_ToInt32(ctx, &w, argv[0]) || JS_ToInt32(ctx, &h, argv[1]))
        return JS_EXCEPTION;
    s_platWindow->setMaximumSize(w, h);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_brw_funcs[] = {
    JS_CFUNC_DEF("minimize", 0, js_brw_minimize),
    JS_CFUNC_DEF("maximize", 0, js_brw_maximize),
    JS_CFUNC_DEF("restore", 0, js_brw_restore),
    JS_CFUNC_DEF("getPosition", 0, js_brw_getPosition),
    JS_CFUNC_DEF("setPosition", 2, js_brw_setPosition),
    JS_CFUNC_DEF("getMinSize", 0, js_brw_getMinSize),
    JS_CFUNC_DEF("setMinSize", 2, js_brw_setMinSize),
    JS_CFUNC_DEF("getMaxSize", 0, js_brw_getMaxSize),
    JS_CFUNC_DEF("setMaxSize", 2, js_brw_setMaxSize),
};

void installBroWindowBindings(JSContext* ctx, platform::Window* window,
                              bool headless)
{
    if (window) s_platWindow = window;
    s_headless = headless;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue bro = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(bro) || JS_IsNull(bro)) {
        JS_FreeValue(ctx, bro);
        bro = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, bro));
    }

    JSValue win = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, win, js_brw_funcs,
                               sizeof(js_brw_funcs) / sizeof(js_brw_funcs[0]));

    auto defineGetSet = [&](const char* name, JSCFunction* getter,
                            JSCFunction* setter) {
        JSAtom atom = JS_NewAtom(ctx, name);
        JS_DefinePropertyGetSet(ctx, win, atom,
            JS_NewCFunction(ctx, getter, name, 0),
            setter ? JS_NewCFunction(ctx, setter, name, 1) : JS_UNDEFINED,
            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, atom);
    };
    defineGetSet("state",       js_brw_get_state,       nullptr);
    defineGetSet("borderless",  js_brw_get_borderless,  js_brw_set_borderless);
    defineGetSet("alwaysOnTop", js_brw_get_alwaysOnTop, js_brw_set_alwaysOnTop);

    JS_SetPropertyStr(ctx, bro, "window", win);
    JS_FreeValue(ctx, bro);
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
