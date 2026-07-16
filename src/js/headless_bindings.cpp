#include "js/headless_bindings.h"
#include "engine/engine.h"
#include "engine/gamepad.h"
#include "canvas/canvas_scene.h"
#if BRO_WITH_3D
#include "scene/scene_renderer.h"  // scene::CullStats for perf.stats()
#endif
#include "dom/element.h"
#include "dom/element_geometry.h"
#include "dom/text_node.h"
#include "dom/node.h"
#include "dom/document.h"
#include "dom/shadow_root.h"
#include "util/log.h"

#include <qjsbind/qjsbind.h>

#include "broimage/encode.h"

#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace bro::js {

// ---------------------------------------------------------------------------
// Engine pointer stash (same pattern as Timers)
// ---------------------------------------------------------------------------

static const char* kEngineKey = "__bro_engine_ptr";

static engine::Engine* getEngine(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue val = JS_GetPropertyStr(ctx, global, kEngineKey);
    engine::Engine* e = nullptr;
    if (JS_IsNumber(val)) {
        int64_t ptr = 0;
        JS_ToInt64(ctx, &ptr, val);
        e = reinterpret_cast<engine::Engine*>(static_cast<intptr_t>(ptr));
    }
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, global);
    return e;
}

// ---------------------------------------------------------------------------
// Global functions
// ---------------------------------------------------------------------------

static JSValue js_screenshot(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "screenshot() requires a path argument");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    const char* path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    bool ok;
    if (argc >= 2 && JS_IsString(argv[1])) {
        // screenshot(path, selector) — crop to element's bounding rect
        const char* selector = JS_ToCString(ctx, argv[1]);
        if (!selector) { JS_FreeCString(ctx, path); return JS_EXCEPTION; }

        auto* el = engine->querySelector(selector);
        if (!el) {
            JS_FreeCString(ctx, path);
            return JS_ThrowTypeError(ctx, "screenshot: element not found: %s", selector);
        }
        JS_FreeCString(ctx, selector);

        // Compute absolute (transform- and scroll-correct) border-box position,
        // matching getBoundingClientRect() — see dom::absoluteBorderBox(). Also
        // correctly traverses shadow DOM boundaries via layoutParent().
        bro::dom::AbsoluteRect r = bro::dom::absoluteBorderBox(el);
        // Add menu bar inset so the crop matches getBoundingClientRect-based
        // viewport coords (mouse helpers do the same compensation).
        float ax = r.x;
        float ay = r.y + static_cast<float>(engine->contentTop());
        int w = static_cast<int>(r.width);
        int h = static_cast<int>(r.height);
        ok = engine->screenshot(path, static_cast<int>(ax), static_cast<int>(ay), w, h);
    } else {
        ok = engine->screenshot(path);
    }

    JS_FreeCString(ctx, path);
    if (ok) return JS_TRUE;
    return JS_ThrowInternalError(ctx, "screenshot failed");
}

// screenshotCanvas(path, selector) — direct snapshot of a <canvas> element's
// underlying Skia surface, preserving alpha. Bypasses the framebuffer
// composite path used by screenshot(), which clears with opaque black and
// flattens transparent canvas pixels. Required for exporting sprite/tileset
// PNGs that must keep a transparent background.
static JSValue js_screenshotCanvas(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "screenshotCanvas(path, selector) requires both arguments");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    const char* path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;
    const char* selector = JS_ToCString(ctx, argv[1]);
    if (!selector) { JS_FreeCString(ctx, path); return JS_EXCEPTION; }

    auto cleanup = [&]() { JS_FreeCString(ctx, path); JS_FreeCString(ctx, selector); };

    auto* el = engine->querySelector(selector);
    if (!el) { cleanup(); return JS_ThrowTypeError(ctx, "screenshotCanvas: element not found: %s", selector); }
    // A canvas hosting a 3D scene graph or WebGL context also carries an
    // auxiliary CanvasScene for ShapeNode/SpriteNode overlay compositing, so
    // canvasScene() alone can't distinguish "plain 2D canvas" from "scene/
    // WebGL canvas with an unused 2D overlay" — check explicitly so this
    // doesn't silently capture a blank overlay layer instead of throwing.
    if (el->sceneGraph() || el->webglContext()) {
        cleanup();
        return JS_ThrowTypeError(ctx,
            "screenshotCanvas: element has an active 3D scene or WebGL context, not a plain "
            "2D canvas: %s — use screenshot(path, selector) instead", selector);
    }
    auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
    if (!cs) { cleanup(); return JS_ThrowTypeError(ctx, "screenshotCanvas: element has no 2D canvas: %s", selector); }

    // Lay out the document so the canvas element's box matches the size the
    // app just set via canvas.width / canvas.style.width. Then drain queued
    // commands so the surface materializes at that size. screenshotCanvas is
    // typically called right after JS draws with no engine tick in between.
    engine->flush();
    cs->flush();
    auto* surf = cs->surface();
    if (!surf) { cleanup(); return JS_ThrowInternalError(ctx, "screenshotCanvas: no surface"); }
    int w = surf->width();
    int h = surf->height();
    if (w <= 0 || h <= 0) { cleanup(); return JS_ThrowInternalError(ctx, "screenshotCanvas: zero-size canvas"); }
    auto pixels = cs->getImageData(0, 0, w, h);
    if (pixels.empty()) { cleanup(); return JS_ThrowInternalError(ctx, "screenshotCanvas: read failed"); }

    bool ok = broimage::encode_png_file(path, pixels.data(), w, h, 4);
    cleanup();
    if (ok) return JS_TRUE;
    return JS_ThrowInternalError(ctx, "screenshotCanvas: write failed");
}

static JSValue js_advanceTime(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "advanceTime() requires milliseconds argument");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    double ms;
    if (JS_ToFloat64(ctx, &ms, argv[0])) return JS_EXCEPTION;

    engine->advanceTime(ms);
    return JS_UNDEFINED;
}

static JSValue js_flush(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");
    engine->flush();
    return JS_UNDEFINED;
}

// Real wall-clock sleep — the only thing in headless that pauses the JS
// thread by actual time. Use when a probe needs to give a real subsystem
// (mic, network, child process) time to do work; for time-dependent app
// logic use advanceTime/sleep instead.
static JSValue js_wallSleep(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "wallSleep() requires milliseconds argument");
    double ms = 0.0;
    if (JS_ToFloat64(ctx, &ms, argv[0])) return JS_EXCEPTION;
    if (ms > 0.0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int64_t>(ms)));
    }
    return JS_UNDEFINED;
}

static JSValue js_assert(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_ToBool(ctx, argv[0])) {
        const char* msg = nullptr;
        if (argc >= 2) msg = JS_ToCString(ctx, argv[1]);

        JSValue err = JS_ThrowTypeError(ctx, "%s", msg ? msg : "Assertion failed");
        if (msg) JS_FreeCString(ctx, msg);
        return err;
    }
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Mouse input simulation — goes through the full engine input pipeline
// (hit testing, focus, event dispatch, scrollbar interaction, etc.)
// ---------------------------------------------------------------------------

// Convert DOM button convention (0=left, 1=middle, 2=right) to SDL convention
// (1=left, 2=middle, 3=right) since Engine::handleMouse*() expects SDL values.
static int domToSdlButton(int domButton) {
    switch (domButton) {
        case 0: return 1;  // DOM primary   -> SDL left
        case 1: return 2;  // DOM auxiliary  -> SDL middle
        case 2: return 3;  // DOM secondary  -> SDL right
        case 3: return 4;  // DOM back       -> SDL X1
        case 4: return 5;  // DOM forward    -> SDL X2
        default: return domButton + 1;
    }
}

// Headless mouse input takes viewport-relative coordinates (matching
// getBoundingClientRect / clientX / clientY in the DOM). The engine's
// handleMouse* methods expect screen-space coords, which include the
// menu-bar inset reserved at the top. Add contentTop() so callers can
// pass values straight from getBoundingClientRect without knowing the
// engine reserves a top inset.
static float toScreenY(engine::Engine* engine, double viewportY) {
    return static_cast<float>(viewportY) + static_cast<float>(engine->contentTop());
}

static JSValue js_mouseDown(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "mouseDown(x, y [, button]) requires x and y");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    double x, y;
    if (JS_ToFloat64(ctx, &x, argv[0])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &y, argv[1])) return JS_EXCEPTION;
    int button = 0;
    if (argc >= 3) JS_ToInt32(ctx, &button, argv[2]);

    engine->handleMouseDown(static_cast<float>(x), toScreenY(engine, y), domToSdlButton(button));
    engine->flush();
    return JS_UNDEFINED;
}

static JSValue js_mouseUp(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "mouseUp(x, y [, button]) requires x and y");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    double x, y;
    if (JS_ToFloat64(ctx, &x, argv[0])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &y, argv[1])) return JS_EXCEPTION;
    int button = 0;
    if (argc >= 3) JS_ToInt32(ctx, &button, argv[2]);

    engine->handleMouseUp(static_cast<float>(x), toScreenY(engine, y), domToSdlButton(button));
    engine->flush();
    return JS_UNDEFINED;
}

static JSValue js_mouseMove(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "mouseMove(x, y) requires x and y");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    double x, y;
    if (JS_ToFloat64(ctx, &x, argv[0])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &y, argv[1])) return JS_EXCEPTION;

    float fx = static_cast<float>(x), fy = toScreenY(engine, y);
    engine->handleMouseMove(fx, fy, fx - engine->getLastMouseX(), fy - engine->getLastMouseY());
    engine->flush();
    return JS_UNDEFINED;
}

static JSValue js_click(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "click(x, y [, button]) requires x and y");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    double x, y;
    if (JS_ToFloat64(ctx, &x, argv[0])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &y, argv[1])) return JS_EXCEPTION;
    int button = 0;
    if (argc >= 3) JS_ToInt32(ctx, &button, argv[2]);

    float fx = static_cast<float>(x), fy = toScreenY(engine, y);
    int sdlBtn = domToSdlButton(button);
    engine->handleMouseDown(fx, fy, sdlBtn);
    engine->handleMouseUp(fx, fy, sdlBtn);
    engine->flush();
    return JS_UNDEFINED;
}

static JSValue js_wheel(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "wheel(x, y, deltaY [, deltaX]) requires x, y, deltaY");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    double x, y, dy;
    if (JS_ToFloat64(ctx, &x, argv[0])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &y, argv[1])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &dy, argv[2])) return JS_EXCEPTION;
    double dx = 0;
    if (argc >= 4) JS_ToFloat64(ctx, &dx, argv[3]);

    // wheel()'s dx/dy are documented (and JS-caller-facing) as the resulting
    // WheelEvent's deltaX/deltaY, i.e. already DOM convention. handleWheel()
    // expects raw SDL-convention deltas (as real SDL wheel events carry) and
    // negates them once to produce the DOM-facing event — so negate here
    // first to cancel that out and hand callers what they actually asked for.
    engine->handleWheel(static_cast<float>(x), toScreenY(engine, y),
                        static_cast<float>(-dx), static_cast<float>(-dy));
    engine->flush();
    return JS_UNDEFINED;
}

// --- Touch input simulation ---
// Drives the same Engine::handleTouch* entry points the SDL finger-event
// path uses (the gamepad-seam pattern: inject below the JS API, above SDL),
// so pointer events, touch events, per-pointer capture, and the compat mouse
// sequence all run the real pipeline. `id` is a caller-chosen contact id
// (the SDL finger-id analog) — reuse the same id for move/up/cancel of one
// contact; distinct concurrent ids are distinct fingers. Coordinates are
// viewport-relative like the mouse helpers.

static JSValue js_touchDown(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "touchDown(id, x, y [, pressure]) requires id, x, y");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    int64_t id = 0;
    double x, y, pressure = 1.0;
    if (JS_ToInt64(ctx, &id, argv[0])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &x, argv[1])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &y, argv[2])) return JS_EXCEPTION;
    if (argc >= 4) JS_ToFloat64(ctx, &pressure, argv[3]);

    engine->handleTouchDown(static_cast<uint64_t>(id), static_cast<float>(x),
                            toScreenY(engine, y), static_cast<float>(pressure));
    engine->flush();
    return JS_UNDEFINED;
}

static JSValue js_touchMove(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "touchMove(id, x, y [, pressure]) requires id, x, y");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    int64_t id = 0;
    double x, y, pressure = 1.0;
    if (JS_ToInt64(ctx, &id, argv[0])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &x, argv[1])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &y, argv[2])) return JS_EXCEPTION;
    if (argc >= 4) JS_ToFloat64(ctx, &pressure, argv[3]);

    engine->handleTouchMove(static_cast<uint64_t>(id), static_cast<float>(x),
                            toScreenY(engine, y), static_cast<float>(pressure));
    engine->flush();
    return JS_UNDEFINED;
}

static JSValue js_touchUp(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "touchUp(id, x, y) requires id, x, y");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    int64_t id = 0;
    double x, y;
    if (JS_ToInt64(ctx, &id, argv[0])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &x, argv[1])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &y, argv[2])) return JS_EXCEPTION;

    engine->handleTouchUp(static_cast<uint64_t>(id), static_cast<float>(x),
                          toScreenY(engine, y));
    engine->flush();
    return JS_UNDEFINED;
}

static JSValue js_touchCancel(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "touchCancel(id, x, y) requires id, x, y");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    int64_t id = 0;
    double x, y;
    if (JS_ToInt64(ctx, &id, argv[0])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &x, argv[1])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &y, argv[2])) return JS_EXCEPTION;

    engine->handleTouchCancel(static_cast<uint64_t>(id), static_cast<float>(x),
                              toScreenY(engine, y));
    engine->flush();
    return JS_UNDEFINED;
}

static JSValue js_keyDown(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "keyDown(keycode [, scancode, mod, repeat])");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    int keycode = 0, scancode = 0, mod = 0;
    bool repeat = false;
    JS_ToInt32(ctx, &keycode, argv[0]);
    if (argc >= 2) JS_ToInt32(ctx, &scancode, argv[1]);
    if (argc >= 3) JS_ToInt32(ctx, &mod, argv[2]);
    if (argc >= 4) repeat = JS_ToBool(ctx, argv[3]);

    engine->handleKeyDown(keycode, scancode, mod, repeat);
    engine->flush();
    return JS_UNDEFINED;
}

static JSValue js_keyUp(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "keyUp(keycode [, scancode, mod])");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    int keycode = 0, scancode = 0, mod = 0;
    JS_ToInt32(ctx, &keycode, argv[0]);
    if (argc >= 2) JS_ToInt32(ctx, &scancode, argv[1]);
    if (argc >= 3) JS_ToInt32(ctx, &mod, argv[2]);

    engine->handleKeyUp(keycode, scancode, mod, false);
    engine->flush();
    return JS_UNDEFINED;
}

static JSValue js_textInput(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "textInput(text) requires text");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    const char* text = JS_ToCString(ctx, argv[0]);
    if (!text) return JS_EXCEPTION;

    engine->handleTextInput(text);
    JS_FreeCString(ctx, text);
    engine->flush();
    return JS_UNDEFINED;
}

// --- Clipboard simulation ---

static JSValue js_paste(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "paste(text) requires text");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    const char* text = JS_ToCString(ctx, argv[0]);
    if (!text) return JS_EXCEPTION;

    engine->simulatePaste(text);
    JS_FreeCString(ctx, text);
    engine->flush();
    return JS_UNDEFINED;
}

static JSValue js_copy(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    std::string text = engine->simulateCopy();
    engine->flush();
    return JS_NewString(ctx, text.c_str());
}

static JSValue js_cut(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    std::string text = engine->simulateCut();
    engine->flush();
    return JS_NewString(ctx, text.c_str());
}

// --- Drag & drop simulation ---

static JSValue js_dropFiles(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "dropFiles(x, y, paths) requires x, y, and an array of paths");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    double x, y;
    if (JS_ToFloat64(ctx, &x, argv[0])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &y, argv[1])) return JS_EXCEPTION;

    // Move mouse to drop position first
    float fx = static_cast<float>(x), fy = toScreenY(engine, y);
    engine->handleMouseMove(fx, fy, fx - engine->getLastMouseX(), fy - engine->getLastMouseY());

    // Drop each file path
    if (JS_IsArray(argv[2])) {
        JSValue lenVal = JS_GetPropertyStr(ctx, argv[2], "length");
        int32_t len = 0;
        JS_ToInt32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        for (int32_t i = 0; i < len; i++) {
            JSValue item = JS_GetPropertyUint32(ctx, argv[2], static_cast<uint32_t>(i));
            const char* path = JS_ToCString(ctx, item);
            if (path) {
                engine->handleDropFile(path);
                JS_FreeCString(ctx, path);
            }
            JS_FreeValue(ctx, item);
        }
    } else {
        // Single string path
        const char* path = JS_ToCString(ctx, argv[2]);
        if (path) {
            engine->handleDropFile(path);
            JS_FreeCString(ctx, path);
        }
    }

    engine->flush();
    return JS_UNDEFINED;
}

static JSValue js_dropText(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "dropText(x, y, text) requires x, y, and text");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    double x, y;
    if (JS_ToFloat64(ctx, &x, argv[0])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &y, argv[1])) return JS_EXCEPTION;

    const char* text = JS_ToCString(ctx, argv[2]);
    if (!text) return JS_EXCEPTION;

    // Move mouse to drop position first
    float fx = static_cast<float>(x), fy = toScreenY(engine, y);
    engine->handleMouseMove(fx, fy, fx - engine->getLastMouseX(), fy - engine->getLastMouseY());

    engine->handleDropText(text);
    JS_FreeCString(ctx, text);
    engine->flush();
    return JS_UNDEFINED;
}

// --- Gamepad simulation ---
// Injects a virtual controller at the engine layer (below navigator.getGamepads
// and above SDL), so connection events, snapshots, and bro.settings action
// dispatch all run the real path without hardware.

// gamepadConnect([id]) -> slot index
static JSValue js_gamepadConnect(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    std::string id;
    if (argc >= 1 && JS_IsString(argv[0])) {
        const char* s = JS_ToCString(ctx, argv[0]);
        if (s) { id = s; JS_FreeCString(ctx, s); }
    }
    int index = engine->gamepadConnectVirtual(id);
    engine->flush();
    return JS_NewInt32(ctx, index);
}

static JSValue js_gamepadDisconnect(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "gamepadDisconnect(index) requires the slot index");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    int index = 0;
    if (JS_ToInt32(ctx, &index, argv[0])) return JS_EXCEPTION;
    bool ok = engine->gamepadDisconnectVirtual(index);
    engine->flush();
    if (!ok) return JS_ThrowTypeError(ctx, "gamepadDisconnect: no virtual gamepad at index %d", index);
    return JS_UNDEFINED;
}

// Shared: resolve a button/axis argument that may be a name string or an index.
static int gamepadResolveIndex(JSContext* ctx, JSValueConst arg,
                               int (*fromName)(const std::string&)) {
    if (JS_IsString(arg)) {
        const char* s = JS_ToCString(ctx, arg);
        if (!s) return -1;
        int idx = fromName(s);
        JS_FreeCString(ctx, s);
        return idx;
    }
    int idx = -1;
    if (JS_ToInt32(ctx, &idx, arg)) return -1;
    return idx;
}

// gamepadButton(index, button, pressed [, value]) — button by W3C index or
// name ("south", "start", "lefttrigger", ...). value gives triggers an
// analog level; it defaults to pressed ? 1 : 0.
static JSValue js_gamepadButton(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "gamepadButton(index, button, pressed [, value])");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    int index = 0;
    if (JS_ToInt32(ctx, &index, argv[0])) return JS_EXCEPTION;
    int button = gamepadResolveIndex(ctx, argv[1], engine::gamepadButtonIndex);
    if (button < 0 || button >= engine::kGamepadButtonCount)
        return JS_ThrowTypeError(ctx, "gamepadButton: unknown button");
    bool pressed = JS_ToBool(ctx, argv[2]);
    double value = -1.0;
    if (argc >= 4 && JS_ToFloat64(ctx, &value, argv[3])) return JS_EXCEPTION;

    bool ok = engine->gamepadSetVirtualButton(index, button, pressed,
                                              static_cast<float>(value));
    engine->flush();
    if (!ok) return JS_ThrowTypeError(ctx, "gamepadButton: no virtual gamepad at index %d", index);
    return JS_UNDEFINED;
}

// gamepadAxis(index, axis, value) — axis by W3C index or name
// ("leftx", "lefty", "rightx", "righty"); value -1..1.
static JSValue js_gamepadAxis(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "gamepadAxis(index, axis, value)");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    int index = 0;
    if (JS_ToInt32(ctx, &index, argv[0])) return JS_EXCEPTION;
    int axis = gamepadResolveIndex(ctx, argv[1], engine::gamepadAxisIndex);
    if (axis < 0 || axis >= engine::kGamepadAxisCount)
        return JS_ThrowTypeError(ctx, "gamepadAxis: unknown axis");
    double value = 0.0;
    if (JS_ToFloat64(ctx, &value, argv[2])) return JS_EXCEPTION;

    bool ok = engine->gamepadSetVirtualAxis(index, axis, static_cast<float>(value));
    engine->flush();
    if (!ok) return JS_ThrowTypeError(ctx, "gamepadAxis: no virtual gamepad at index %d", index);
    return JS_UNDEFINED;
}

static JSValue js_resize(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "resize(w, h) requires width and height");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    int w, h;
    if (JS_ToInt32(ctx, &w, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &h, argv[1])) return JS_EXCEPTION;

    engine->handleResize(w, h);
    engine->flush();
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// CSS/Layout inspection
// ---------------------------------------------------------------------------

// Format a float, trimming trailing zeros: 10.00 -> "10", 10.50 -> "10.5"
static std::string fmtF(float v) {
    if (v == 0.0f) return "0";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    return s;
}

// Format edges: "5" if all equal, "5 10" if top/bottom == left/right, "5 10 5 10" otherwise
static std::string fmtEdges(const htmlayout::layout::Edges& e) {
    if (e.top == e.right && e.right == e.bottom && e.bottom == e.left)
        return fmtF(e.top);
    if (e.top == e.bottom && e.left == e.right)
        return fmtF(e.top) + " " + fmtF(e.right);
    return fmtF(e.top) + " " + fmtF(e.right) + " " + fmtF(e.bottom) + " " + fmtF(e.left);
}

// Build element descriptor: <tag#id.class1.class2>
static std::string elemDesc(bro::dom::Element* el) {
    std::string s = "<" + el->tagName();
    std::string id = el->id();
    if (!id.empty()) s += "#" + id;
    std::string cls = el->getAttribute("class");
    if (!cls.empty()) {
        // Split classes by whitespace, join with dots
        std::istringstream iss(cls);
        std::string tok;
        while (iss >> tok) s += "." + tok;
    }
    s += ">";
    return s;
}

// Compute absolute position by walking layout parent chain
static void absolutePos(bro::dom::Element* el, float& ax, float& ay) {
    auto& box = el->layoutBox();
    ax = box.contentRect.x - box.padding.left - box.border.left;
    ay = box.contentRect.y - box.padding.top - box.border.top;
    for (auto* lp = el->layoutParent(); lp; lp = lp->layoutParent()) {
        auto& pb = lp->layoutBox();
        ax += pb.contentRect.x;
        ay += pb.contentRect.y;
        ay -= lp->scrollTopValue();
    }
}

// Build full inspection string for a single element
static std::string buildInspectString(bro::dom::Element* el, bool verbose) {
    std::ostringstream out;
    auto& box = el->layoutBox();
    float ax, ay;
    absolutePos(el, ax, ay);

    // Header
    out << elemDesc(el) << "\n";

    // Box model
    out << "  Box Model:\n";
    out << "    content:  " << fmtF(box.contentRect.width) << " x "
        << fmtF(box.contentRect.height) << "\n";
    out << "    padding:  " << fmtEdges(box.padding) << "\n";
    out << "    border:   " << fmtEdges(box.border) << "\n";
    out << "    margin:   " << fmtEdges(box.margin) << "\n";
    out << "    full:     " << fmtF(box.fullWidth()) << " x "
        << fmtF(box.fullHeight()) << "\n";

    // Position
    out << "  Position:\n";
    out << "    relative: (" << fmtF(box.contentRect.x) << ", "
        << fmtF(box.contentRect.y) << ")\n";
    out << "    absolute: (" << fmtF(ax) << ", " << fmtF(ay) << ")\n";

    // Scroll state
    float scrollTop = el->scrollTopValue();
    float natH = box.naturalHeight;
    if (scrollTop > 0 || natH > box.contentRect.height + 0.5f) {
        out << "  Scroll:\n";
        out << "    scrollTop:    " << fmtF(scrollTop) << "\n";
        out << "    scrollHeight: " << fmtF(natH) << "\n";
        out << "    overflow:     " << fmtF(natH - box.contentRect.height) << "px hidden\n";
    }

    // Flags
    if (box.textTruncated)
        out << "  Flags: text-truncated\n";

    // Computed styles
    auto& styles = el->computedStyle();
    if (!styles.empty()) {
        // In verbose mode, show all. Otherwise show key layout properties.
        out << "  Computed Styles:\n";
        if (verbose) {
            // Sort for stable output
            std::vector<std::pair<std::string, std::string>> sorted(styles.begin(), styles.end());
            std::sort(sorted.begin(), sorted.end());
            for (auto& [k, v] : sorted) {
                out << "    " << k << ": " << v << "\n";
            }
        } else {
            // Key layout/visual properties
            static const char* keys[] = {
                "display", "position", "flex-direction", "justify-content", "align-items",
                "width", "height", "min-width", "min-height", "max-width", "max-height",
                "overflow", "overflow-x", "overflow-y",
                "color", "background-color", "background",
                "font-size", "font-family", "font-weight",
                "opacity", "visibility", "z-index", "transform",
                "box-sizing", "text-align", "vertical-align",
                "white-space", "text-overflow",
                "gap", "row-gap", "column-gap", "flex-wrap", "flex-grow", "flex-shrink",
                "grid-template-columns", "grid-template-rows",
            };
            for (auto* key : keys) {
                auto it = styles.find(key);
                if (it != styles.end() && !it->second.empty()) {
                    out << "    " << key << ": " << it->second << "\n";
                }
            }
        }
    }

    // Inline styles
    auto& inlineStyle = el->style();
    if (!inlineStyle.empty()) {
        out << "  Inline Styles:\n";
        out << "    " << inlineStyle.cssText() << "\n";
    }

    // Attributes (non-style, non-class, non-id)
    auto& attrs = el->attributes();
    bool hasExtra = false;
    for (auto& [k, v] : attrs) {
        if (k == "id" || k == "class" || k == "style") continue;
        if (!hasExtra) { out << "  Attributes:\n"; hasExtra = true; }
        out << "    " << k << "=\"" << v << "\"\n";
    }

    // Children summary
    int elemCount = 0, textCount = 0;
    for (auto* child : el->childNodes()) {
        if (child->nodeType() == bro::dom::NodeType::Element) ++elemCount;
        else if (child->nodeType() == bro::dom::NodeType::Text) ++textCount;
    }
    out << "  Children: " << elemCount << " element" << (elemCount != 1 ? "s" : "")
        << ", " << textCount << " text node" << (textCount != 1 ? "s" : "") << "\n";

    // Shadow DOM
    if (el->hasShadow()) {
        auto* sr = el->shadowRoot();
        out << "  Shadow DOM: " << (sr->mode() == bro::dom::ShadowRoot::Mode::Open ? "open" : "closed") << "\n";
    }

    return out.str();
}

// Build tree view recursively
static void buildTreeString(std::ostringstream& out, bro::dom::Element* el,
                            int depth, int maxDepth, const std::string& indent) {
    auto& box = el->layoutBox();
    float ax, ay;
    absolutePos(el, ax, ay);

    out << indent << elemDesc(el) << "  "
        << fmtF(box.fullWidth()) << "x" << fmtF(box.fullHeight())
        << " @ (" << fmtF(ax) << ", " << fmtF(ay) << ")";

    // Show key style hints inline
    auto& styles = el->computedStyle();
    auto displayIt = styles.find("display");
    if (displayIt != styles.end() && displayIt->second != "block")
        out << " [" << displayIt->second << "]";
    auto posIt = styles.find("position");
    if (posIt != styles.end() && posIt->second != "static")
        out << " [" << posIt->second << "]";
    auto ovIt = styles.find("overflow");
    if (ovIt != styles.end() && ovIt->second != "visible")
        out << " [overflow:" << ovIt->second << "]";
    if (el->hasShadow())
        out << " [shadow]";

    out << "\n";

    if (depth >= maxDepth) return;

    // Show children (use composed tree to see through shadow DOM)
    std::string childIndent = indent + "  ";
    for (auto* child : el->composedChildNodes()) {
        if (child->nodeType() == bro::dom::NodeType::Element) {
            buildTreeString(out, static_cast<bro::dom::Element*>(child),
                           depth + 1, maxDepth, childIndent);
        } else if (child->nodeType() == bro::dom::NodeType::Text) {
            auto* text = static_cast<bro::dom::TextNode*>(child);
            std::string data = text->data();
            // Trim whitespace
            size_t start = data.find_first_not_of(" \t\n\r");
            if (start == std::string::npos) continue; // skip whitespace-only
            size_t end = data.find_last_not_of(" \t\n\r");
            data = data.substr(start, end - start + 1);
            if (data.size() > 40) data = data.substr(0, 37) + "...";
            out << childIndent << "#text \"" << data << "\"\n";
        }
    }
}

static JSValue js_inspect(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "inspect(selector [, verbose]) requires a selector");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    const char* selector = JS_ToCString(ctx, argv[0]);
    if (!selector) return JS_EXCEPTION;

    // Flush layout so box info is current
    engine->flush();

    auto* el = engine->querySelector(selector);
    if (!el) {
        JS_FreeCString(ctx, selector);
        return JS_ThrowTypeError(ctx, "inspect: no element matches '%s'", selector);
    }
    JS_FreeCString(ctx, selector);

    bool verbose = false;
    if (argc >= 2) verbose = JS_ToBool(ctx, argv[1]);

    std::string result = buildInspectString(el, verbose);
    return JS_NewString(ctx, result.c_str());
}

static JSValue js_inspectTree(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "inspectTree(selector [, depth]) requires a selector");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    const char* selector = JS_ToCString(ctx, argv[0]);
    if (!selector) return JS_EXCEPTION;

    // Flush layout so box info is current
    engine->flush();

    auto* el = engine->querySelector(selector);
    if (!el) {
        JS_FreeCString(ctx, selector);
        return JS_ThrowTypeError(ctx, "inspectTree: no element matches '%s'", selector);
    }
    JS_FreeCString(ctx, selector);

    int maxDepth = 3;
    if (argc >= 2) JS_ToInt32(ctx, &maxDepth, argv[1]);

    std::ostringstream out;
    buildTreeString(out, el, 0, maxDepth, "");
    return JS_NewString(ctx, out.str().c_str());
}

static JSValue js_computedStyle(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "computedStyle(selector [, property]) requires a selector");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    const char* selector = JS_ToCString(ctx, argv[0]);
    if (!selector) return JS_EXCEPTION;

    // Flush layout so styles are current
    engine->flush();

    auto* el = engine->querySelector(selector);
    if (!el) {
        JS_FreeCString(ctx, selector);
        return JS_ThrowTypeError(ctx, "computedStyle: no element matches '%s'", selector);
    }
    JS_FreeCString(ctx, selector);

    auto& styles = el->computedStyle();

    // If a specific property is requested, return just that value
    if (argc >= 2 && JS_IsString(argv[1])) {
        const char* prop = JS_ToCString(ctx, argv[1]);
        if (!prop) return JS_EXCEPTION;
        std::string propStr(prop);
        JS_FreeCString(ctx, prop);

        auto it = styles.find(propStr);
        if (it != styles.end())
            return JS_NewString(ctx, it->second.c_str());
        return JS_NewString(ctx, "");
    }

    // No property specified — return all as a JS object
    JSValue obj = JS_NewObject(ctx);
    for (auto& [k, v] : styles) {
        JS_SetPropertyStr(ctx, obj, k.c_str(), JS_NewString(ctx, v.c_str()));
    }
    return obj;
}

static JSValue js_elements(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "elements(selector) requires a selector");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    const char* selector = JS_ToCString(ctx, argv[0]);
    if (!selector) return JS_EXCEPTION;

    engine->flush();

    auto* doc = engine->document();
    if (!doc || !doc->documentElement()) {
        JS_FreeCString(ctx, selector);
        return JS_NewString(ctx, "(no document)\n");
    }

    auto results = doc->documentElement()->querySelectorAll(std::string(selector));
    JS_FreeCString(ctx, selector);

    std::ostringstream out;
    out << results.size() << " match" << (results.size() != 1 ? "es" : "") << ":\n";
    for (size_t i = 0; i < results.size(); ++i) {
        auto* el = results[i];
        auto& box = el->layoutBox();
        float ax, ay;
        absolutePos(el, ax, ay);
        out << "  [" << i << "] " << elemDesc(el) << "  "
            << fmtF(box.fullWidth()) << "x" << fmtF(box.fullHeight())
            << " @ (" << fmtF(ax) << ", " << fmtF(ay) << ")\n";
    }
    return JS_NewString(ctx, out.str().c_str());
}

// ---------------------------------------------------------------------------
// Overlay panel inspection
// ---------------------------------------------------------------------------

// inspectOverlay(panelName, selector [, verbose])
static JSValue js_inspectOverlay(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "inspectOverlay(panelName, selector [, verbose])");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    const char* panel = JS_ToCString(ctx, argv[0]);
    const char* selector = JS_ToCString(ctx, argv[1]);
    if (!panel || !selector) {
        if (panel) JS_FreeCString(ctx, panel);
        if (selector) JS_FreeCString(ctx, selector);
        return JS_EXCEPTION;
    }

    auto* el = engine->overlayQuerySelector(panel, selector);
    if (!el) {
        auto result = std::string("inspectOverlay: no element matches '") + selector +
                      "' in panel '" + panel + "'";
        JS_FreeCString(ctx, panel);
        JS_FreeCString(ctx, selector);
        return JS_ThrowTypeError(ctx, "%s", result.c_str());
    }
    JS_FreeCString(ctx, panel);
    JS_FreeCString(ctx, selector);

    bool verbose = false;
    if (argc >= 3) verbose = JS_ToBool(ctx, argv[2]);

    std::string result = buildInspectString(el, verbose);
    return JS_NewString(ctx, result.c_str());
}

// inspectOverlayTree(panelName, selector [, depth])
static JSValue js_inspectOverlayTree(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "inspectOverlayTree(panelName, selector [, depth])");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    const char* panel = JS_ToCString(ctx, argv[0]);
    const char* selector = JS_ToCString(ctx, argv[1]);
    if (!panel || !selector) {
        if (panel) JS_FreeCString(ctx, panel);
        if (selector) JS_FreeCString(ctx, selector);
        return JS_EXCEPTION;
    }

    auto* el = engine->overlayQuerySelector(panel, selector);
    if (!el) {
        auto result = std::string("inspectOverlayTree: no element matches '") + selector +
                      "' in panel '" + panel + "'";
        JS_FreeCString(ctx, panel);
        JS_FreeCString(ctx, selector);
        return JS_ThrowTypeError(ctx, "%s", result.c_str());
    }
    JS_FreeCString(ctx, panel);
    JS_FreeCString(ctx, selector);

    int maxDepth = 3;
    if (argc >= 3) JS_ToInt32(ctx, &maxDepth, argv[2]);

    std::ostringstream out;
    buildTreeString(out, el, 0, maxDepth, "");
    return JS_NewString(ctx, out.str().c_str());
}

// getPixel(x, y) — returns {r, g, b, a} for the pixel at (x, y)
static JSValue js_getPixel(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "getPixel(x, y) requires x and y");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    int x, y;
    if (JS_ToInt32(ctx, &x, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &y, argv[1])) return JS_EXCEPTION;

    auto pixels = engine->capturePixels();
    int w = engine->viewportWidth();
    int h = engine->viewportHeight();

    if (pixels.empty() || x < 0 || y < 0 || x >= w || y >= h) {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "r", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, obj, "g", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, obj, "b", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, obj, "a", JS_NewInt32(ctx, 0));
        return obj;
    }

    size_t offset = (static_cast<size_t>(y) * w + x) * 4;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "r", JS_NewInt32(ctx, pixels[offset]));
    JS_SetPropertyStr(ctx, obj, "g", JS_NewInt32(ctx, pixels[offset + 1]));
    JS_SetPropertyStr(ctx, obj, "b", JS_NewInt32(ctx, pixels[offset + 2]));
    JS_SetPropertyStr(ctx, obj, "a", JS_NewInt32(ctx, pixels[offset + 3]));
    return obj;
}

// overlayPanels() — list all overlay panel names
static JSValue js_overlayPanels(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* engine = getEngine(ctx);
    if (!engine) return JS_NewArray(ctx);

    auto names = engine->overlayPanelNames();
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < names.size(); i++) {
        JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i),
                             JS_NewString(ctx, names[i].c_str()));
    }
    return arr;
}

// ---------------------------------------------------------------------------
// perf — where the style and layout time of a change actually goes
// ---------------------------------------------------------------------------

// Real wall clock. performance.now() rides headless *virtual* time (advanceTime
// drives it), so a benchmark built on it silently measures nothing at all — it
// reports 0ms for work that took a second. This is the clock to time with.
static JSValue js_perf_now(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto t = std::chrono::steady_clock::now().time_since_epoch();
    return JS_NewFloat64(ctx, std::chrono::duration<double, std::milli>(t).count());
}

static JSValue js_perf_reset(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* engine = getEngine(ctx);
    if (!engine || !engine->document()) return JS_ThrowInternalError(ctx, "No document");
    engine->document()->resetPerf();
    return JS_UNDEFINED;
}

static JSValue js_perf_stats(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* engine = getEngine(ctx);
    if (!engine || !engine->document()) return JS_ThrowInternalError(ctx, "No document");
    const auto& p = engine->document()->perf();
    JSValue o = JS_NewObject(ctx);
    auto num = [&](const char* k, double v) {
        JS_SetPropertyStr(ctx, o, k, JS_NewFloat64(ctx, v));
    };
    num("styleMs", p.styleMs);
    num("buildMs", p.buildMs);
    num("invalidateMs", p.invalidateMs);
    num("layoutMs", p.layoutMs);
    num("layoutTreeMs", p.layoutTreeMs);
    num("layoutAbsMs", p.layoutAbsMs);
    num("layoutHitMs", p.layoutHitMs);
    num("syncMs", p.syncMs);
    num("totalMs", p.totalMs());
    num("passes", static_cast<double>(p.passes));
    num("treeRebuilds", static_cast<double>(p.treeRebuilds));
    num("elementsStyled", static_cast<double>(p.elementsStyled));
    num("nodesLaidOut", static_cast<double>(p.nodesLaidOut));
    num("nodeVisits", static_cast<double>(p.nodeVisits));
    num("nodesReused", static_cast<double>(p.nodesReused));
    num("measureCalls", static_cast<double>(p.measureCalls));
    num("styleLookups", static_cast<double>(p.styleLookups));
    num("reuseFailDirty", static_cast<double>(p.reuseFailDirty));
    num("reuseFailAvailW", static_cast<double>(p.reuseFailAvailW));
    num("reuseFailAvailH", static_cast<double>(p.reuseFailAvailH));
    num("reuseFailOverride", static_cast<double>(p.reuseFailOverride));
#if BRO_WITH_3D
    // Frustum-culling counters summed across every scene graph's most recent
    // render (see scene::CullStats). Zeros when no 3D content rendered.
    {
        scene::CullStats s = engine->sceneCullStats();
        JSValue sc = JS_NewObject(ctx);
        auto snum = [&](const char* k, int v) {
            JS_SetPropertyStr(ctx, sc, k, JS_NewInt32(ctx, v));
        };
        snum("meshDrawn", s.meshDrawn);
        snum("meshCulled", s.meshCulled);
        snum("instancedDrawn", s.instancedDrawn);
        snum("instancedCulled", s.instancedCulled);
        snum("splatDrawn", s.splatDrawn);
        snum("splatCulled", s.splatCulled);
        snum("particlesDrawn", s.particlesDrawn);
        snum("particlesCulled", s.particlesCulled);
        snum("billboardsDrawn", s.billboardsDrawn);
        snum("billboardsCulled", s.billboardsCulled);
        snum("shadowDrawn", s.shadowDrawn);
        snum("shadowCulled", s.shadowCulled);
        JS_SetPropertyStr(ctx, o, "scene", sc);
    }
#endif
    return o;
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void installCanvasSnapshotBinding(JSContext* ctx, engine::Engine* engine) {
    qjsbind::Global(ctx)
        .value(kEngineKey, JS_NewInt64(ctx, static_cast<int64_t>(
                               reinterpret_cast<intptr_t>(engine))))
        .function("screenshotCanvas", js_screenshotCanvas, 2);
}

void installHeadlessBindings(JSContext* ctx, engine::Engine* engine) {
    // The canvas-snapshot binding is also installed by Engine in both modes;
    // re-installing is harmless (same engine pointer, same function).
    installCanvasSnapshotBinding(ctx, engine);

    // perf.{now,reset,stats}: the numbers behind a slow frame. See docs/headless.md.
    JSValue perf = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, perf, "now",   JS_NewCFunction(ctx, js_perf_now, "now", 0));
    JS_SetPropertyStr(ctx, perf, "reset", JS_NewCFunction(ctx, js_perf_reset, "reset", 0));
    JS_SetPropertyStr(ctx, perf, "stats", JS_NewCFunction(ctx, js_perf_stats, "stats", 0));
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "perf", perf);
    JS_FreeValue(ctx, global);

    qjsbind::Global(ctx)
        // Core
        .function("screenshot", js_screenshot, 1)
        .function("advanceTime", js_advanceTime, 1)
        .function("flush", js_flush, 0)
        .function("sleep", js_advanceTime, 1)
        .function("wallSleep", js_wallSleep, 1)
        .function("assert", js_assert, 2)
        // Mouse input simulation
        .function("mouseDown", js_mouseDown, 3)
        .function("mouseUp", js_mouseUp, 3)
        .function("mouseMove", js_mouseMove, 2)
        .function("click", js_click, 3)
        .function("wheel", js_wheel, 4)
        // Touch input simulation
        .function("touchDown", js_touchDown, 4)
        .function("touchMove", js_touchMove, 4)
        .function("touchUp", js_touchUp, 3)
        .function("touchCancel", js_touchCancel, 3)
        // Keyboard input simulation
        .function("keyDown", js_keyDown, 4)
        .function("keyUp", js_keyUp, 3)
        .function("textInput", js_textInput, 1)
        // Clipboard simulation
        .function("paste", js_paste, 1)
        .function("copy", js_copy, 0)
        .function("cut", js_cut, 0)
        // Drag & drop simulation
        .function("dropFiles", js_dropFiles, 3)
        .function("dropText", js_dropText, 3)
        // Gamepad simulation
        .function("gamepadConnect", js_gamepadConnect, 1)
        .function("gamepadDisconnect", js_gamepadDisconnect, 1)
        .function("gamepadButton", js_gamepadButton, 4)
        .function("gamepadAxis", js_gamepadAxis, 3)
        // Viewport
        .function("resize", js_resize, 2)
        // CSS/Layout inspection
        .function("inspect", js_inspect, 2)
        .function("inspectTree", js_inspectTree, 2)
        .function("computedStyle", js_computedStyle, 2)
        .function("elements", js_elements, 1)
        // Overlay panel inspection
        .function("inspectOverlay", js_inspectOverlay, 3)
        .function("inspectOverlayTree", js_inspectOverlayTree, 3)
        .function("overlayPanels", js_overlayPanels, 0)
        // Pixel inspection
        .function("getPixel", js_getPixel, 2);
}

} // namespace bro::js
