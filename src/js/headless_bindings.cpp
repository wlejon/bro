#include "js/headless_bindings.h"
#include "js/dialog_bindings.h"
#include "js/anchor_download.h"
#include "engine/engine.h"
#include "engine/capture_path.h"
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
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <thread>

#include <SDL3/SDL_keyboard.h>

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

    if (!ok) {
        JSValue err = JS_ThrowInternalError(ctx, "screenshot: could not write %s", path);
        JS_FreeCString(ctx, path);
        return err;
    }
    JS_FreeCString(ctx, path);
    return JS_TRUE;
}

// writeFile(path, data) — write a string (UTF-8) or ArrayBuffer/TypedArray to
// disk. The counterpart to screenshot(): headless is the project's build and
// test harness, so a tool that bakes an asset needs somewhere to put it. It is
// deliberately headless-only — a shipped app gets no filesystem write.
// Missing parent directories are created, matching screenshot().
static JSValue js_writeFile(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "writeFile(path, data) requires both arguments");

    const char* path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    const std::uint8_t* bytes = nullptr;
    std::size_t len = 0;
    std::string text;
    std::size_t byteOff = 0, byteLen = 0, bytesPer = 0;

    // A TypedArray view has to be resolved to its buffer plus offset; handing
    // over the whole buffer would silently write the wrong range. Both probes
    // throw when the value is the wrong shape, so each miss clears the pending
    // exception before the next one -- leaving one set would surface later as a
    // spurious throw from unrelated JS.
    JSValue buf = JS_GetTypedArrayBuffer(ctx, argv[1], &byteOff, &byteLen, &bytesPer);
    if (!JS_IsException(buf)) {
        std::size_t total = 0;
        std::uint8_t* base = JS_GetArrayBuffer(ctx, &total, buf);
        if (base) { bytes = base + byteOff; len = byteLen; }
        JS_FreeValue(ctx, buf);
    } else {
        JS_FreeValue(ctx, JS_GetException(ctx));
        std::size_t total = 0;
        std::uint8_t* base = JS_GetArrayBuffer(ctx, &total, argv[1]);
        if (base) {
            bytes = base;
            len = total;
        } else {
            JS_FreeValue(ctx, JS_GetException(ctx));
            // Anything else object-shaped (a DataView, a plain array, a detached
            // buffer) would stringify to "[object Object]" and write a file that
            // looks like it worked. Say so instead.
            if (JS_IsObject(argv[1])) {
                JS_FreeCString(ctx, path);
                return JS_ThrowTypeError(
                    ctx, "writeFile: data must be a string, ArrayBuffer, or TypedArray");
            }
            const char* s = JS_ToCString(ctx, argv[1]);
            if (!s) { JS_FreeCString(ctx, path); return JS_EXCEPTION; }
            text = s;
            JS_FreeCString(ctx, s);
            bytes = reinterpret_cast<const std::uint8_t*>(text.data());
            len = text.size();
        }
    }

    std::error_code ec;
    std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);

    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) {
        JSValue err = JS_ThrowInternalError(ctx, "writeFile: could not open %s", path);
        JS_FreeCString(ctx, path);
        return err;
    }
    if (len) out.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(len));
    out.close();
    if (!out) {
        JSValue err = JS_ThrowInternalError(ctx, "writeFile: could not write %s", path);
        JS_FreeCString(ctx, path);
        return err;
    }

    JS_FreeCString(ctx, path);
    return JS_NewInt64(ctx, static_cast<int64_t>(len));
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

    bool ok = bro::ensureParentDir(path) &&
              broimage::encode_png_file(path, pixels.data(), w, h, 4);
    if (!ok) {
        JSValue err = JS_ThrowInternalError(ctx, "screenshotCanvas: could not write %s", path);
        cleanup();
        return err;
    }
    cleanup();
    return JS_TRUE;
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

// --- Optional trailing `windowId` on every input seam ---------------------
//
// Omitted / 0 / undefined = the MAIN window, so every pre-multiwindow test
// keeps working byte-for-byte. Any other value is a bro.window.open() handle
// id (`win.id`) and routes the event into that secondary window's document
// through Engine::host*(). Unknown ids reach a no-op in the engine.
static uint64_t argWindowId(JSContext* ctx, int argc, JSValueConst* argv, int idx) {
    if (argc <= idx || JS_IsUndefined(argv[idx]) || JS_IsNull(argv[idx])) return 0;
    int64_t v = 0;
    if (JS_ToInt64(ctx, &v, argv[idx])) return 0;
    return v > 0 ? static_cast<uint64_t>(v) : 0;
}

// Input y for the target window. The main window reserves a top inset (menu
// bar) that viewport-relative test coordinates must skip past; a secondary
// window carries no engine chrome, so its window space IS its document space
// and the coordinate passes through untouched.
static float toWindowY(engine::Engine* engine, double y, uint64_t windowId) {
    return windowId ? static_cast<float>(y) : toScreenY(engine, y);
}

static JSValue js_mouseDown(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "mouseDown(x, y [, button, windowId]) requires x and y");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    double x, y;
    if (JS_ToFloat64(ctx, &x, argv[0])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &y, argv[1])) return JS_EXCEPTION;
    int button = 0;
    if (argc >= 3) JS_ToInt32(ctx, &button, argv[2]);

    const uint64_t wid = argWindowId(ctx, argc, argv, 3);
    const float fy = toWindowY(engine, y, wid);
    if (wid) engine->hostMouseDown(wid, static_cast<float>(x), fy, domToSdlButton(button));
    else engine->handleMouseDown(static_cast<float>(x), fy, domToSdlButton(button));
    engine->flush();
    return JS_UNDEFINED;
}

static JSValue js_mouseUp(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "mouseUp(x, y [, button, windowId]) requires x and y");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    double x, y;
    if (JS_ToFloat64(ctx, &x, argv[0])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &y, argv[1])) return JS_EXCEPTION;
    int button = 0;
    if (argc >= 3) JS_ToInt32(ctx, &button, argv[2]);

    const uint64_t wid = argWindowId(ctx, argc, argv, 3);
    const float fy = toWindowY(engine, y, wid);
    if (wid) engine->hostMouseUp(wid, static_cast<float>(x), fy, domToSdlButton(button));
    else engine->handleMouseUp(static_cast<float>(x), fy, domToSdlButton(button));
    engine->flush();
    return JS_UNDEFINED;
}

static JSValue js_mouseMove(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "mouseMove(x, y [, windowId]) requires x and y");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    double x, y;
    if (JS_ToFloat64(ctx, &x, argv[0])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &y, argv[1])) return JS_EXCEPTION;

    const uint64_t wid = argWindowId(ctx, argc, argv, 2);
    float fx = static_cast<float>(x), fy = toWindowY(engine, y, wid);
    if (wid) {
        // Same self-computed delta the main-window path uses: there is no real
        // pointing device here, so movementX/Y measures from this window's own
        // last injected position.
        auto* h = engine->windowHostById(wid);
        float lx = h ? h->lastMouseX : fx, ly = h ? h->lastMouseY : fy;
        engine->hostMouseMove(wid, fx, fy, fx - lx, fy - ly);
    } else {
        engine->handleMouseMove(fx, fy, fx - engine->getLastMouseX(),
                                fy - engine->getLastMouseY());
    }
    engine->flush();
    return JS_UNDEFINED;
}

// currentCursor() → the resolved OS cursor shape name for the current hover
// target ("default", "pointer", "text", ..., "none"). Drive a mouseMove()
// first; the engine re-resolves the hovered element's computed `cursor` on
// every move (in headless the mapping runs without touching a real cursor).
static JSValue js_currentCursor(JSContext* ctx, JSValueConst, int argc,
                                JSValueConst* argv) {
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");
    // currentCursor([windowId]) — each window resolves its own cursor from its
    // own hover target, so a `cursor: pointer` element in a palette window
    // never changes what the main window reports.
    return JS_NewString(ctx,
        engine->resolvedCursor(argWindowId(ctx, argc, argv, 0)).c_str());
}

static JSValue js_click(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "click(x, y [, button, windowId]) requires x and y");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    double x, y;
    if (JS_ToFloat64(ctx, &x, argv[0])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &y, argv[1])) return JS_EXCEPTION;
    int button = 0;
    if (argc >= 3) JS_ToInt32(ctx, &button, argv[2]);

    const uint64_t wid = argWindowId(ctx, argc, argv, 3);
    float fx = static_cast<float>(x), fy = toWindowY(engine, y, wid);
    int sdlBtn = domToSdlButton(button);
    if (wid) {
        engine->hostMouseDown(wid, fx, fy, sdlBtn);
        engine->hostMouseUp(wid, fx, fy, sdlBtn);
    } else {
        engine->handleMouseDown(fx, fy, sdlBtn);
        engine->handleMouseUp(fx, fy, sdlBtn);
    }
    engine->flush();
    return JS_UNDEFINED;
}

static JSValue js_wheel(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "wheel(x, y, deltaY [, deltaX, windowId]) requires x, y, deltaY");
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
    const uint64_t wid = argWindowId(ctx, argc, argv, 4);
    const float fy = toWindowY(engine, y, wid);
    if (wid) engine->hostWheel(wid, static_cast<float>(x), fy,
                               static_cast<float>(-dx), static_cast<float>(-dy));
    else engine->handleWheel(static_cast<float>(x), fy,
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

// A scancode for a keycode the caller did not pair one with.
//
// `KeyboardEvent.code` is derived from the physical scancode, so a headless
// keyDown(SDLK_a) with no scancode produced code "Unknown0" — and anything
// reading `code` (a game's WASD, a library's key table, the legacy keyCode
// derived from it) saw nothing it recognised. SDL knows the physical key that
// produces a given keysym on the current layout; ask it.
static int scancodeForKeycode(int keycode) {
    if (keycode == 0) return 0;
    return static_cast<int>(SDL_GetScancodeFromKey(
        static_cast<SDL_Keycode>(keycode), nullptr));
}

static JSValue js_keyDown(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "keyDown(keycode [, scancode, mod, repeat, windowId])");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    int keycode = 0, scancode = 0, mod = 0;
    bool repeat = false;
    JS_ToInt32(ctx, &keycode, argv[0]);
    if (argc >= 2) JS_ToInt32(ctx, &scancode, argv[1]);
    if (argc >= 3) JS_ToInt32(ctx, &mod, argv[2]);
    if (argc >= 4) repeat = JS_ToBool(ctx, argv[3]);
    if (scancode == 0) scancode = scancodeForKeycode(keycode);

    const uint64_t wid = argWindowId(ctx, argc, argv, 4);
    if (wid) engine->hostKeyDown(wid, keycode, scancode, mod, repeat);
    else engine->handleKeyDown(keycode, scancode, mod, repeat);
    engine->flush();
    return JS_UNDEFINED;
}

static JSValue js_keyUp(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "keyUp(keycode [, scancode, mod, windowId])");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    int keycode = 0, scancode = 0, mod = 0;
    JS_ToInt32(ctx, &keycode, argv[0]);
    if (argc >= 2) JS_ToInt32(ctx, &scancode, argv[1]);
    if (argc >= 3) JS_ToInt32(ctx, &mod, argv[2]);
    if (scancode == 0) scancode = scancodeForKeycode(keycode);

    const uint64_t wid = argWindowId(ctx, argc, argv, 3);
    if (wid) engine->hostKeyUp(wid, keycode, scancode, mod, false);
    else engine->handleKeyUp(keycode, scancode, mod, false);
    engine->flush();
    return JS_UNDEFINED;
}

static JSValue js_textInput(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "textInput(text [, windowId]) requires text");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    const char* text = JS_ToCString(ctx, argv[0]);
    if (!text) return JS_EXCEPTION;

    const uint64_t wid = argWindowId(ctx, argc, argv, 1);
    if (wid) engine->hostTextInput(wid, text);
    else engine->handleTextInput(text);
    JS_FreeCString(ctx, text);
    engine->flush();
    return JS_UNDEFINED;
}

// --- IME composition simulation ---
// Injects through the same engine path as SDL_EVENT_TEXT_EDITING /
// SDL_EVENT_TEXT_INPUT, so composition events, preedit-in-value rendering,
// and undo behavior all run the real pipeline.

static JSValue js_imeCompose(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "imeCompose(text [, cursorPos, windowId]) requires text");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    const char* text = JS_ToCString(ctx, argv[0]);
    if (!text) return JS_EXCEPTION;
    int cursor = -1;  // < 0 → composition cursor at the end of the preedit
    if (argc >= 2) JS_ToInt32(ctx, &cursor, argv[1]);

    const uint64_t wid = argWindowId(ctx, argc, argv, 2);
    if (wid) engine->hostTextEditing(wid, text, cursor, 0);
    else engine->handleTextEditing(text, cursor, 0);
    JS_FreeCString(ctx, text);
    engine->flush();
    return JS_UNDEFINED;
}

static JSValue js_imeCommit(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "imeCommit(text [, windowId]) requires text");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    const char* text = JS_ToCString(ctx, argv[0]);
    if (!text) return JS_EXCEPTION;

    // A commit is a TEXT_INPUT — the same event a real IME sends.
    const uint64_t wid = argWindowId(ctx, argc, argv, 1);
    if (wid) engine->hostTextInput(wid, text);
    else engine->handleTextInput(text);
    JS_FreeCString(ctx, text);
    engine->flush();
    return JS_UNDEFINED;
}

static JSValue js_imeCancel(JSContext* ctx, JSValueConst, int argc,
                            JSValueConst* argv) {
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    // A cancel is an empty TEXT_EDITING event.
    const uint64_t wid = argWindowId(ctx, argc, argv, 0);
    if (wid) engine->hostTextEditing(wid, "", 0, 0);
    else engine->handleTextEditing("", 0, 0);
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

// lastDownload() — absolute path of the file the most recent <a download>
// click wrote, or null if nothing has been downloaded. Lets a test assert on
// an app's export path without knowing where the user's Downloads folder is.
static JSValue js_lastDownload(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    const std::string& p = lastDownloadPath();
    return p.empty() ? JS_NULL : JS_NewString(ctx, p.c_str());
}

// setPickedFiles(paths) — what the next <input type=file> picker returns.
// There is no native picker to open with no user present, so a script queues
// the choice and then clicks the input exactly as a user would.
static JSValue js_setPickedFiles(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::vector<std::string> paths;
    if (argc >= 1 && JS_IsString(argv[0])) {
        const char* s = JS_ToCString(ctx, argv[0]);
        if (s) { paths.emplace_back(s); JS_FreeCString(ctx, s); }
    } else if (argc >= 1 && JS_IsArray(argv[0])) {
        uint32_t len = 0;
        JSValue lenVal = JS_GetPropertyStr(ctx, argv[0], "length");
        JS_ToUint32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        for (uint32_t i = 0; i < len; i++) {
            JSValue v = JS_GetPropertyUint32(ctx, argv[0], i);
            const char* s = JS_ToCString(ctx, v);
            if (s) { paths.emplace_back(s); JS_FreeCString(ctx, s); }
            JS_FreeValue(ctx, v);
        }
    }
    DialogBindings::setPickedFiles(std::move(paths));
    return JS_UNDEFINED;
}

// setDialogAnswer(accept) — what alert/confirm/prompt do with no user to ask.
// `true` (the default) means confirm() returns true and prompt() returns its
// default value, so a script walks through an app's confirmations instead of
// stopping at the first one; `false` takes the cancel branch.
static JSValue js_setDialogAnswer(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DialogBindings::setAutoDialogAnswer(argc >= 1 ? JS_ToBool(ctx, argv[0]) != 0 : true);
    return JS_UNDEFINED;
}

static JSValue js_dropFiles(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "dropFiles(x, y, paths [, windowId]) requires x, y, and an array of paths");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    double x, y;
    if (JS_ToFloat64(ctx, &x, argv[0])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &y, argv[1])) return JS_EXCEPTION;

    // Move mouse to drop position first
    const uint64_t wid = argWindowId(ctx, argc, argv, 3);
    float fx = static_cast<float>(x), fy = toWindowY(engine, y, wid);
    if (wid) {
        auto* h = engine->windowHostById(wid);
        float lx = h ? h->lastMouseX : fx, ly = h ? h->lastMouseY : fy;
        engine->hostMouseMove(wid, fx, fy, fx - lx, fy - ly);
    } else {
        engine->handleMouseMove(fx, fy, fx - engine->getLastMouseX(),
                                fy - engine->getLastMouseY());
    }

    // Collect every path, then drop them as ONE gesture — matching what the
    // platform layer now does for a real multi-file drop (a single `drop`
    // event whose dataTransfer.files holds them all), rather than synthesizing
    // N separate single-file drops.
    std::vector<std::string> paths;
    if (JS_IsArray(argv[2])) {
        JSValue lenVal = JS_GetPropertyStr(ctx, argv[2], "length");
        int32_t len = 0;
        JS_ToInt32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        for (int32_t i = 0; i < len; i++) {
            JSValue item = JS_GetPropertyUint32(ctx, argv[2], static_cast<uint32_t>(i));
            const char* path = JS_ToCString(ctx, item);
            if (path) {
                paths.emplace_back(path);
                JS_FreeCString(ctx, path);
            }
            JS_FreeValue(ctx, item);
        }
    } else {
        // Single string path
        const char* path = JS_ToCString(ctx, argv[2]);
        if (path) {
            paths.emplace_back(path);
            JS_FreeCString(ctx, path);
        }
    }

    if (!paths.empty()) {
        if (wid) engine->hostDropFile(wid, paths, fx, fy);
        else engine->handleDropFile(paths, fx, fy);
    }

    engine->flush();
    return JS_UNDEFINED;
}

static JSValue js_dropText(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "dropText(x, y, text [, windowId]) requires x, y, and text");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    double x, y;
    if (JS_ToFloat64(ctx, &x, argv[0])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &y, argv[1])) return JS_EXCEPTION;

    const char* text = JS_ToCString(ctx, argv[2]);
    if (!text) return JS_EXCEPTION;

    // Move mouse to drop position first
    const uint64_t wid = argWindowId(ctx, argc, argv, 3);
    float fx = static_cast<float>(x), fy = toWindowY(engine, y, wid);
    if (wid) {
        auto* h = engine->windowHostById(wid);
        float lx = h ? h->lastMouseX : fx, ly = h ? h->lastMouseY : fy;
        engine->hostMouseMove(wid, fx, fy, fx - lx, fy - ly);
        engine->hostDropText(wid, text, fx, fy);
    } else {
        engine->handleMouseMove(fx, fy, fx - engine->getLastMouseX(),
                                fy - engine->getLastMouseY());
        engine->handleDropText(text);
    }
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
//
// (x, y) are *document* coordinates, i.e. the space getBoundingClientRect()
// reports in, so a test can probe exactly where it measured. capturePixels()
// hands back the whole frame, and the app document is drawn inset into that
// frame by contentInsets() — the menu bar on top, a docked inspector on the
// right/left. Probing the raw frame buffer with a DOM coordinate therefore
// reads the wrong pixel whenever any inset is non-zero: with the menu bar
// visible every y was off by its height, which silently shifted every pixel
// assertion in every headless test rather than failing them outright.
static JSValue js_getPixel(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "getPixel(x, y) requires x and y");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    int x, y;
    if (JS_ToInt32(ctx, &x, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &y, argv[1])) return JS_EXCEPTION;

    auto pixels = engine->capturePixels();
    // Bounds are the document box; the buffer stride is the full frame width.
    int w = engine->contentWidth();
    int h = engine->contentHeight();
    int stride = engine->viewportWidth();

    if (pixels.empty() || x < 0 || y < 0 || x >= w || y >= h) {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "r", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, obj, "g", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, obj, "b", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, obj, "a", JS_NewInt32(ctx, 0));
        return obj;
    }

    size_t offset = (static_cast<size_t>(y + engine->contentTop()) * stride
                     + (x + engine->contentLeft())) * 4;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "r", JS_NewInt32(ctx, pixels[offset]));
    JS_SetPropertyStr(ctx, obj, "g", JS_NewInt32(ctx, pixels[offset + 1]));
    JS_SetPropertyStr(ctx, obj, "b", JS_NewInt32(ctx, pixels[offset + 2]));
    JS_SetPropertyStr(ctx, obj, "a", JS_NewInt32(ctx, pixels[offset + 3]));
    return obj;
}

// getFramePixel(x, y) — like getPixel, but in *frame* coordinates: the whole
// composited window including engine chrome (menu bar, docked inspector).
//
// Only tests that are asserting something about the chrome itself want this —
// "the menu bar physically occupies the top 28px". App content is far easier
// to probe with getPixel(), which shares getBoundingClientRect()'s space.
static JSValue js_getFramePixel(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "getFramePixel(x, y) requires x and y");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    int x, y;
    if (JS_ToInt32(ctx, &x, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &y, argv[1])) return JS_EXCEPTION;

    auto pixels = engine->capturePixels();
    int w = engine->viewportWidth();
    int h = engine->viewportHeight();

    JSValue obj = JS_NewObject(ctx);
    if (pixels.empty() || x < 0 || y < 0 || x >= w || y >= h) {
        JS_SetPropertyStr(ctx, obj, "r", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, obj, "g", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, obj, "b", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, obj, "a", JS_NewInt32(ctx, 0));
        return obj;
    }

    size_t offset = (static_cast<size_t>(y) * w + x) * 4;
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

// Real GPU milliseconds for the last flush()'s 3D scene render, from a native
// GL_TIME_ELAPSED query. Blocking: forces that frame's GPU work to complete, so
// the returned number is an isolated per-frame GPU cost (not wall-clock, which
// misses async GPU work). Returns -1 with no GPU scene (2D page / --no-gpu / no
// flush yet). Typical use: `for(...){ scene.setCamera(c); flush(); s+=perf.gpuFrameMs(); }`.
static JSValue js_perf_gpu(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");
    return JS_NewFloat64(ctx, engine->gpuFrameMs());
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
        snum("shadowTilesTotal", s.shadowTilesTotal);
        snum("shadowTilesRendered", s.shadowTilesRendered);
        snum("shadowTilesCached", s.shadowTilesCached);
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

void installScriptArgs(JSContext* ctx, const std::vector<std::string>& args) {
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < args.size(); ++i) {
        JS_SetPropertyUint32(ctx, arr, i,
                             JS_NewStringLen(ctx, args[i].data(), args[i].size()));
    }
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "scriptArgs", arr);
    JS_FreeValue(ctx, global);
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
    JS_SetPropertyStr(ctx, perf, "gpuFrameMs", JS_NewCFunction(ctx, js_perf_gpu, "gpuFrameMs", 0));
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "perf", perf);
    JS_FreeValue(ctx, global);

    qjsbind::Global(ctx)
        // Core
        .function("screenshot", js_screenshot, 1)
        .function("writeFile", js_writeFile, 2)
        .function("advanceTime", js_advanceTime, 1)
        .function("flush", js_flush, 0)
        .function("sleep", js_advanceTime, 1)
        .function("wallSleep", js_wallSleep, 1)
        .function("assert", js_assert, 2)
        // Mouse input simulation
        .function("mouseDown", js_mouseDown, 4)
        .function("mouseUp", js_mouseUp, 4)
        .function("mouseMove", js_mouseMove, 3)
        .function("currentCursor", js_currentCursor, 1)
        .function("click", js_click, 4)
        .function("wheel", js_wheel, 5)
        // Touch input simulation
        .function("touchDown", js_touchDown, 4)
        .function("touchMove", js_touchMove, 4)
        .function("touchUp", js_touchUp, 3)
        .function("touchCancel", js_touchCancel, 3)
        // Keyboard input simulation
        .function("keyDown", js_keyDown, 5)
        .function("keyUp", js_keyUp, 4)
        .function("textInput", js_textInput, 2)
        // IME composition simulation
        .function("imeCompose", js_imeCompose, 3)
        .function("imeCommit", js_imeCommit, 2)
        .function("imeCancel", js_imeCancel, 1)
        // Clipboard simulation
        .function("paste", js_paste, 1)
        .function("copy", js_copy, 0)
        .function("cut", js_cut, 0)
        // Drag & drop simulation
        .function("dropFiles", js_dropFiles, 4)
        // Modal dialogs (alert/confirm/prompt) answer themselves in headless
        .function("setDialogAnswer", js_setDialogAnswer, 1)
        // What the next <input type=file> picker returns
        .function("setPickedFiles", js_setPickedFiles, 1)
        // Where the last <a download> click saved its file
        .function("lastDownload", js_lastDownload, 0)
        .function("dropText", js_dropText, 4)
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
        .function("getPixel", js_getPixel, 2)
        .function("getFramePixel", js_getFramePixel, 2);
}

} // namespace bro::js
