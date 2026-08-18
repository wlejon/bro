#include "js/window_bindings.h"
#include "js/event_dispatch.h"
#include "platform/clipboard.h"
#include "platform/event_loop.h"
#include "platform/sdl_window.h"
#include "util/log.h"

#include "window_polyfill.js.h"

#include <qjsbind/qjsbind.h>

#include <cstring>
#include <limits>
#include <unordered_map>

#include <SDL3/SDL_misc.h>   // SDL_OpenURL
#include <SDL3/SDL_power.h>  // SDL_GetPowerInfo
#include <SDL3/SDL_stdinc.h> // SDL_free

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

// ---------------------------------------------------------------------------
// PER-REALM window state. These used to be plain statics on the assumption of
// one process, one platform window — true until bro.window.open() gave a
// secondary window its own document realm. A host realm's window.screen,
// devicePixelRatio and bro.window.* must answer for ITS window, not the
// primary one, so the state is keyed by JSContext: each install() records the
// window that realm describes, and every accessor resolves through its own ctx.
//
// Realms that share the primary window (app, iframes, system panels) simply all
// record the same pointer, so nothing changes for them. Entries are dropped by
// cleanupWindowBindings() when a realm dies — required, not hygiene: JSContext
// addresses are recycled, and a stale entry would hand a fresh realm a dangling
// window pointer.
// ---------------------------------------------------------------------------

namespace {

struct RealmWindowState {
    platform::Window* window = nullptr;
    bool headless = false;
    // screen fallback when no window exists (--no-gpu).
    int screenFallbackW = 0;
    int screenFallbackH = 0;
};

std::unordered_map<JSContext*, RealmWindowState> s_realms;

// Seeded from the first (top-level) realm install so child realms without a
// window still report the same screen. Genuinely process-wide — it describes
// the machine, not a window.
int s_defaultFallbackW = 0;
int s_defaultFallbackH = 0;

RealmWindowState& realmFor(JSContext* ctx) {
    return s_realms[ctx];
}

platform::Window* realmWindow(JSContext* ctx) {
    auto it = s_realms.find(ctx);
    return it == s_realms.end() ? nullptr : it->second.window;
}

bool realmHeadless(JSContext* ctx) {
    auto it = s_realms.find(ctx);
    return it != s_realms.end() && it->second.headless;
}

} // namespace

// The display this realm's window currently sits on, or false if unknown.
static bool currentDisplayInfo(JSContext* ctx, platform::DisplayInfo& out) {
    platform::Window* win = realmWindow(ctx);
    if (!win) return false;
    for (auto& d : win->getDisplays()) {
        if (d.isCurrent) { out = d; return true; }
    }
    return false;
}

// navigator.clipboard backing — SDL is the only system-clipboard path bro links.
// These are the synchronous primitives; installWindowBindings wraps them in the
// Promise-returning writeText/readText the web Clipboard API exposes.
static JSValue js_clipboard_write(JSContext* ctx, JSValueConst /*this_val*/,
                                  int argc, JSValueConst* argv)
{
    const char* text = argc > 0 ? JS_ToCString(ctx, argv[0]) : nullptr;
    bool ok = bro::platform::setClipboardText(text ? text : "");
    if (text) JS_FreeCString(ctx, text);
    return JS_NewBool(ctx, ok);
}

static JSValue js_clipboard_read(JSContext* ctx, JSValueConst /*this_val*/,
                                 int /*argc*/, JSValueConst* /*argv*/)
{
    // Retried: an unretried read answers "" for a clipboard it merely
    // could not open, which reads back as an empty clipboard.
    const std::string text = bro::platform::getClipboardText();
    return JS_NewString(ctx, text.c_str());
}

// ---------------------------------------------------------------------------
// window.screen — live getters over the display the window currently sits
// on (it follows the window across monitors). Headless pins to the hidden
// window's size so test output never depends on the desk the suite runs on
// (same policy as devicePixelRatio — see engine_init.cpp).
// ---------------------------------------------------------------------------

static void screenDims(JSContext* ctx, int& w, int& h, bool workArea) {
    auto it = s_realms.find(ctx);
    const RealmWindowState* st = it == s_realms.end() ? nullptr : &it->second;
    if (!st || !st->headless) {
        platform::DisplayInfo d;
        if (currentDisplayInfo(ctx, d)) {
            w = workArea ? d.workWidth : d.width;
            h = workArea ? d.workHeight : d.height;
            return;
        }
    }
    if (st && st->window) {
        w = static_cast<int>(st->window->getWidth());
        h = static_cast<int>(st->window->getHeight());
    } else {
        w = st ? st->screenFallbackW : s_defaultFallbackW;
        h = st ? st->screenFallbackH : s_defaultFallbackH;
    }
}

static JSValue js_screen_get_width(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    int w, h; screenDims(ctx, w, h, false);
    return JS_NewInt32(ctx, w);
}
static JSValue js_screen_get_height(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    int w, h; screenDims(ctx, w, h, false);
    return JS_NewInt32(ctx, h);
}
static JSValue js_screen_get_availWidth(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    int w, h; screenDims(ctx, w, h, true);
    return JS_NewInt32(ctx, w);
}
static JSValue js_screen_get_availHeight(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    int w, h; screenDims(ctx, w, h, true);
    return JS_NewInt32(ctx, h);
}

// ---------------------------------------------------------------------------
// window.open(url) — hand the URL to the OS (default browser / mail client)
// via SDL_OpenURL. There is no Window object to return, so it always returns
// null; target/features arguments are ignored. Any scheme SDL accepts is
// allowed — bro apps are local-first and already have full fs/child_process
// access, so gating URL schemes here would protect nothing (documented in
// docs/window-api.js). Headless never shells out.
// ---------------------------------------------------------------------------

// window.focus(): raise this realm's window and ask the OS for input focus.
// Headless has nothing to raise, and neither does a realm with no window
// (--no-gpu); both simply do nothing, which is also what a browser does for a
// call the user did not initiate.
static JSValue js_window_focus(JSContext* ctx, JSValueConst /*this_val*/,
                               int /*argc*/, JSValueConst* /*argv*/)
{
    if (!realmHeadless(ctx)) {
        if (platform::Window* win = realmWindow(ctx)) win->raise();
    }
    return JS_UNDEFINED;
}

static JSValue js_window_blur(JSContext* ctx, JSValueConst /*this_val*/,
                              int /*argc*/, JSValueConst* /*argv*/)
{
    (void)ctx;
    return JS_UNDEFINED;
}

static JSValue js_window_open(JSContext* ctx, JSValueConst /*this_val*/,
                              int argc, JSValueConst* argv)
{
    if (argc < 1 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0]))
        return JS_NULL;
    const char* url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_NULL;
    if (*url) {
        if (realmHeadless(ctx)) {
            LOG_INFO("window.open('%s'): suppressed in headless mode", url);
        } else if (!SDL_OpenURL(url)) {
            LOG_INFO("window.open('%s') failed: %s", url, SDL_GetError());
        }
    }
    JS_FreeCString(ctx, url);
    return JS_NULL;
}

// ---------------------------------------------------------------------------
// navigator.getBattery() — Promise of a BatteryManager-shaped snapshot over
// SDL_GetPowerInfo. Snapshot-on-call: call again for fresh values; there are
// no change events (the listener methods are present but inert). Desktops
// without a battery report the web convention: charging, full, forever.
// Headless always reports that no-battery shape.
// ---------------------------------------------------------------------------

static JSValue js_navigator_getBattery(JSContext* ctx, JSValueConst /*this_val*/,
                                       int /*argc*/, JSValueConst* /*argv*/)
{
    const double inf = std::numeric_limits<double>::infinity();
    bool charging = true;
    double chargingTime = 0.0;     // seconds; 0 = full or no battery
    double dischargingTime = inf;  // seconds until empty
    double level = 1.0;            // 0.0 .. 1.0

    if (!realmHeadless(ctx)) {
        int secs = 0, pct = 0;
        switch (SDL_GetPowerInfo(&secs, &pct)) {
            case SDL_POWERSTATE_ON_BATTERY:
                charging = false;
                chargingTime = inf;
                dischargingTime = secs >= 0 ? static_cast<double>(secs) : inf;
                if (pct >= 0) level = pct / 100.0;
                break;
            case SDL_POWERSTATE_CHARGING:
                // SDL has no time-to-full estimate — unknown is Infinity per spec.
                chargingTime = inf;
                if (pct >= 0) level = pct / 100.0;
                break;
            case SDL_POWERSTATE_CHARGED:
                if (pct >= 0) level = pct / 100.0;
                break;
            case SDL_POWERSTATE_NO_BATTERY:
            case SDL_POWERSTATE_UNKNOWN:
            case SDL_POWERSTATE_ERROR:
            default:
                break;  // keep the desktop no-battery shape
        }
    }

    JSValue b = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, b, "charging", JS_NewBool(ctx, charging));
    JS_SetPropertyStr(ctx, b, "chargingTime", JS_NewFloat64(ctx, chargingTime));
    JS_SetPropertyStr(ctx, b, "dischargingTime", JS_NewFloat64(ctx, dischargingTime));
    JS_SetPropertyStr(ctx, b, "level", JS_NewFloat64(ctx, level));
    // EventTarget veneer so spec-shaped code doesn't throw; never fires.
    static const char kNoop[] = "(function(){})";
    JS_SetPropertyStr(ctx, b, "addEventListener",
        JS_Eval(ctx, kNoop, sizeof(kNoop) - 1, "<battery>", JS_EVAL_TYPE_GLOBAL));
    JS_SetPropertyStr(ctx, b, "removeEventListener",
        JS_Eval(ctx, kNoop, sizeof(kNoop) - 1, "<battery>", JS_EVAL_TYPE_GLOBAL));
    JS_SetPropertyStr(ctx, b, "onchargingchange", JS_NULL);
    JS_SetPropertyStr(ctx, b, "onchargingtimechange", JS_NULL);
    JS_SetPropertyStr(ctx, b, "ondischargingtimechange", JS_NULL);
    JS_SetPropertyStr(ctx, b, "onlevelchange", JS_NULL);
    return qjsbind::make_resolved_promise(ctx, b);
}

void installWindowBindings(JSContext* ctx, int viewportWidth, int viewportHeight,
                           double devicePixelRatio,
                           platform::Window* window, bool headless)
{
    if (s_defaultFallbackW == 0) {
        s_defaultFallbackW = viewportWidth;
        s_defaultFallbackH = viewportHeight;
    }
    {
        RealmWindowState& st = realmFor(ctx);
        if (window) st.window = window;
        st.headless = headless;
        if (st.screenFallbackW == 0) {
            st.screenFallbackW = s_defaultFallbackW;
            st.screenFallbackH = s_defaultFallbackH;
        }
    }

    JSValue global = JS_GetGlobalObject(ctx);

    // window = globalThis
    JS_SetPropertyStr(ctx, global, "window", JS_DupValue(ctx, global));
    // self = globalThis. Standard WindowOrWorkerGlobalScope.self, and the
    // document realm is the only one that was missing it — worker realms
    // already install it (src/js/worker.cpp). Libraries feature-detect on it
    // constantly: three.js guards `if (typeof self !== 'undefined')` before
    // wiring its animation loop, so without this every three.js app dies on
    // renderer.setAnimationLoop() with a null-context deref.
    JS_SetPropertyStr(ctx, global, "self", JS_DupValue(ctx, global));
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

    // window.open — shell out to the OS URL handler (see js_window_open).
    JS_SetPropertyStr(ctx, global, "open",
                      JS_NewCFunction(ctx, js_window_open, "open", 1));

    // window.focus() / window.blur(). Raising the OS window is what focus()
    // means; blur() is a no-op, as it is in every browser that still honours
    // it at all. Both exist mainly because library code calls them mid-handler
    // and a missing function is a TypeError that takes the rest of the handler
    // with it — CodeMirror calls window.focus() partway through its mousedown,
    // so its editor never took focus, moved a cursor, or accepted a keystroke.
    JS_SetPropertyStr(ctx, global, "focus",
                      JS_NewCFunction(ctx, js_window_focus, "focus", 0));
    JS_SetPropertyStr(ctx, global, "blur",
                      JS_NewCFunction(ctx, js_window_blur, "blur", 0));

    // window.screen — live accessors so the values track the display the
    // window actually sits on (constant in practice unless the user drags
    // the window to another monitor).
    {
        JSValue screen = JS_NewObject(ctx);
        auto defineGetter = [&](const char* name, JSCFunction* getter) {
            JSAtom atom = JS_NewAtom(ctx, name);
            JS_DefinePropertyGetSet(ctx, screen, atom,
                JS_NewCFunction(ctx, getter, name, 0), JS_UNDEFINED,
                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
            JS_FreeAtom(ctx, atom);
        };
        defineGetter("width",       js_screen_get_width);
        defineGetter("height",      js_screen_get_height);
        defineGetter("availWidth",  js_screen_get_availWidth);
        defineGetter("availHeight", js_screen_get_availHeight);
        JS_SetPropertyStr(ctx, screen, "colorDepth", JS_NewInt32(ctx, 24));
        JS_SetPropertyStr(ctx, screen, "pixelDepth", JS_NewInt32(ctx, 24));
        JS_SetPropertyStr(ctx, global, "screen", screen);
    }

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
    JS_SetPropertyStr(ctx, nav, "getBattery",
                      JS_NewCFunction(ctx, js_navigator_getBattery, "getBattery", 0));

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

    // __bro_dispatch_window_event + __bro_listener_seq. Installed BEFORE the
    // polyfill so the polyfill sees the real dispatcher and skips its
    // standalone fallback; the polyfill supplies the JS listener storage this
    // dispatcher reads.
    installWindowEventDispatch(ctx);

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
// setPosition, moveToDisplay) no-op so a test can never disturb the hidden
// window the whole pipeline renders through; flag/limit setters (borderless,
// alwaysOnTop, min/max size) still apply — they're pure window state, so
// tests can round-trip them without visible effect. With no window at all
// (--no-gpu) every query returns its pinned default and every mutator no-ops.
// ---------------------------------------------------------------------------

static JSValue js_brw_get_state(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    const char* state = "normal";
    if (realmWindow(ctx)) {
        if (realmWindow(ctx)->isMinimized())       state = "minimized";
        else if (realmWindow(ctx)->isFullscreen()) state = "fullscreen";
        else if (realmWindow(ctx)->isMaximized())  state = "maximized";
    }
    return JS_NewString(ctx, state);
}

static JSValue js_brw_get_borderless(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewBool(ctx, realmWindow(ctx) && realmWindow(ctx)->isBorderless());
}

static JSValue js_brw_set_borderless(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (realmWindow(ctx) && argc >= 1)
        realmWindow(ctx)->setBorderless(JS_ToBool(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_brw_get_alwaysOnTop(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewBool(ctx, realmWindow(ctx) && realmWindow(ctx)->isAlwaysOnTop());
}

static JSValue js_brw_set_alwaysOnTop(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (realmWindow(ctx) && argc >= 1)
        realmWindow(ctx)->setAlwaysOnTop(JS_ToBool(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_brw_minimize(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (realmWindow(ctx) && !realmHeadless(ctx)) realmWindow(ctx)->minimize();
    return JS_UNDEFINED;
}

static JSValue js_brw_maximize(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (realmWindow(ctx) && !realmHeadless(ctx)) realmWindow(ctx)->maximize();
    return JS_UNDEFINED;
}

static JSValue js_brw_restore(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (realmWindow(ctx) && !realmHeadless(ctx)) realmWindow(ctx)->restore();
    return JS_UNDEFINED;
}

static JSValue js_brw_getPosition(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    int x = 0, y = 0;
    if (realmWindow(ctx)) realmWindow(ctx)->getPosition(x, y);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "x", JS_NewInt32(ctx, x));
    JS_SetPropertyStr(ctx, obj, "y", JS_NewInt32(ctx, y));
    return obj;
}

static JSValue js_brw_setPosition(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!realmWindow(ctx) || realmHeadless(ctx) || argc < 2) return JS_UNDEFINED;
    int32_t x = 0, y = 0;
    if (JS_ToInt32(ctx, &x, argv[0]) || JS_ToInt32(ctx, &y, argv[1]))
        return JS_EXCEPTION;
    realmWindow(ctx)->setPosition(x, y);
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
    if (realmWindow(ctx)) realmWindow(ctx)->getMinimumSize(w, h);
    return sizePairToJS(ctx, w, h);
}

static JSValue js_brw_setMinSize(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!realmWindow(ctx) || argc < 2) return JS_UNDEFINED;
    int32_t w = 0, h = 0;
    if (JS_ToInt32(ctx, &w, argv[0]) || JS_ToInt32(ctx, &h, argv[1]))
        return JS_EXCEPTION;
    realmWindow(ctx)->setMinimumSize(w, h);
    return JS_UNDEFINED;
}

static JSValue js_brw_getMaxSize(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    int w = 0, h = 0;
    if (realmWindow(ctx)) realmWindow(ctx)->getMaximumSize(w, h);
    return sizePairToJS(ctx, w, h);
}

static JSValue js_brw_setMaxSize(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!realmWindow(ctx) || argc < 2) return JS_UNDEFINED;
    int32_t w = 0, h = 0;
    if (JS_ToInt32(ctx, &w, argv[0]) || JS_ToInt32(ctx, &h, argv[1]))
        return JS_EXCEPTION;
    realmWindow(ctx)->setMaximumSize(w, h);
    return JS_UNDEFINED;
}

static JSValue js_brw_getDisplays(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue arr = JS_NewArray(ctx);
    if (!realmWindow(ctx)) return arr;

    auto displays = realmWindow(ctx)->getDisplays();
    for (size_t i = 0; i < displays.size(); i++) {
        const auto& d = displays[i];
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "id", JS_NewUint32(ctx, d.id));
        JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, d.name.c_str()));

        JSValue bounds = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, bounds, "x", JS_NewInt32(ctx, d.x));
        JS_SetPropertyStr(ctx, bounds, "y", JS_NewInt32(ctx, d.y));
        JS_SetPropertyStr(ctx, bounds, "width", JS_NewInt32(ctx, d.width));
        JS_SetPropertyStr(ctx, bounds, "height", JS_NewInt32(ctx, d.height));
        JS_SetPropertyStr(ctx, obj, "bounds", bounds);

        JSValue work = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, work, "x", JS_NewInt32(ctx, d.workX));
        JS_SetPropertyStr(ctx, work, "y", JS_NewInt32(ctx, d.workY));
        JS_SetPropertyStr(ctx, work, "width", JS_NewInt32(ctx, d.workWidth));
        JS_SetPropertyStr(ctx, work, "height", JS_NewInt32(ctx, d.workHeight));
        JS_SetPropertyStr(ctx, obj, "workArea", work);

        JS_SetPropertyStr(ctx, obj, "refreshRate", JS_NewFloat64(ctx, d.refreshRate));
        JS_SetPropertyStr(ctx, obj, "contentScale", JS_NewFloat64(ctx, d.contentScale));
        JS_SetPropertyStr(ctx, obj, "isPrimary", JS_NewBool(ctx, d.isPrimary));
        JS_SetPropertyStr(ctx, obj, "isCurrent", JS_NewBool(ctx, d.isCurrent));
        JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), obj);
    }
    return arr;
}

static JSValue js_brw_moveToDisplay(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!realmWindow(ctx) || realmHeadless(ctx) || argc < 1) return JS_NewBool(ctx, false);
    uint32_t id = 0;
    if (JS_ToUint32(ctx, &id, argv[0]))
        return JS_EXCEPTION;
    return JS_NewBool(ctx, realmWindow(ctx)->moveToDisplay(id));
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
    JS_CFUNC_DEF("getDisplays", 0, js_brw_getDisplays),
    JS_CFUNC_DEF("moveToDisplay", 1, js_brw_moveToDisplay),
};

void installBroWindowBindings(JSContext* ctx, platform::Window* window,
                              bool headless)
{
    {
        RealmWindowState& st = realmFor(ctx);
        if (window) st.window = window;
        st.headless = headless;
    }

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

void cleanupWindowBindings(JSContext* ctx) {
    s_realms.erase(ctx);
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
