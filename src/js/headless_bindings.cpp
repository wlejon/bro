#include "js/headless_bindings.h"
#include "engine/engine.h"
#include "dom/element.h"
#include "dom/node.h"
#include "util/log.h"

#include <string>

extern "C" {
#include "quickjs.h"
}

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

        // Compute absolute position by walking up the parent chain.
        // Layout positions are parent-relative; drawTraversal accumulates offsets.
        auto& box = el->layoutBox();
        float ax = box.contentRect.x - box.padding.left - box.border.left;
        float ay = box.contentRect.y - box.padding.top - box.border.top;
        for (auto* p = el->parentNode(); p; p = p->parentNode()) {
            if (p->nodeType() != bro::dom::NodeType::Element) continue;
            auto& pb = static_cast<bro::dom::Element*>(p)->layoutBox();
            ax += pb.contentRect.x;
            ay += pb.contentRect.y;
        }
        int w = static_cast<int>(box.fullWidth());
        int h = static_cast<int>(box.fullHeight());
        ok = engine->screenshot(path, static_cast<int>(ax), static_cast<int>(ay), w, h);
    } else {
        ok = engine->screenshot(path);
    }

    JS_FreeCString(ctx, path);
    if (ok) return JS_TRUE;
    return JS_ThrowInternalError(ctx, "screenshot failed");
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

static JSValue js_mouseDown(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "mouseDown(x, y [, button]) requires x and y");
    auto* engine = getEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "No engine");

    double x, y;
    if (JS_ToFloat64(ctx, &x, argv[0])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &y, argv[1])) return JS_EXCEPTION;
    int button = 0;
    if (argc >= 3) JS_ToInt32(ctx, &button, argv[2]);

    engine->handleMouseDown(static_cast<float>(x), static_cast<float>(y), button);
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

    engine->handleMouseUp(static_cast<float>(x), static_cast<float>(y), button);
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

    engine->handleMouseMove(static_cast<float>(x), static_cast<float>(y));
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

    float fx = static_cast<float>(x), fy = static_cast<float>(y);
    engine->handleMouseDown(fx, fy, button);
    engine->handleMouseUp(fx, fy, button);
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

    engine->handleWheel(static_cast<float>(x), static_cast<float>(y),
                        static_cast<float>(dx), static_cast<float>(dy));
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
// Install
// ---------------------------------------------------------------------------

void installHeadlessBindings(JSContext* ctx, engine::Engine* engine) {
    JSValue global = JS_GetGlobalObject(ctx);

    // Stash engine pointer
    JS_SetPropertyStr(ctx, global, kEngineKey,
                      JS_NewInt64(ctx, static_cast<int64_t>(
                          reinterpret_cast<intptr_t>(engine))));

    JS_SetPropertyStr(ctx, global, "screenshot",
                      JS_NewCFunction(ctx, js_screenshot, "screenshot", 1));
    JS_SetPropertyStr(ctx, global, "advanceTime",
                      JS_NewCFunction(ctx, js_advanceTime, "advanceTime", 1));
    JS_SetPropertyStr(ctx, global, "flush",
                      JS_NewCFunction(ctx, js_flush, "flush", 0));
    // sleep is an alias for advanceTime
    JS_SetPropertyStr(ctx, global, "sleep",
                      JS_NewCFunction(ctx, js_advanceTime, "sleep", 1));
    JS_SetPropertyStr(ctx, global, "assert",
                      JS_NewCFunction(ctx, js_assert, "assert", 2));

    // Mouse input simulation
    JS_SetPropertyStr(ctx, global, "mouseDown",
                      JS_NewCFunction(ctx, js_mouseDown, "mouseDown", 3));
    JS_SetPropertyStr(ctx, global, "mouseUp",
                      JS_NewCFunction(ctx, js_mouseUp, "mouseUp", 3));
    JS_SetPropertyStr(ctx, global, "mouseMove",
                      JS_NewCFunction(ctx, js_mouseMove, "mouseMove", 2));
    JS_SetPropertyStr(ctx, global, "click",
                      JS_NewCFunction(ctx, js_click, "click", 3));
    JS_SetPropertyStr(ctx, global, "wheel",
                      JS_NewCFunction(ctx, js_wheel, "wheel", 4));

    // Keyboard input simulation
    JS_SetPropertyStr(ctx, global, "keyDown",
                      JS_NewCFunction(ctx, js_keyDown, "keyDown", 4));
    JS_SetPropertyStr(ctx, global, "keyUp",
                      JS_NewCFunction(ctx, js_keyUp, "keyUp", 3));
    JS_SetPropertyStr(ctx, global, "textInput",
                      JS_NewCFunction(ctx, js_textInput, "textInput", 1));

    // Viewport
    JS_SetPropertyStr(ctx, global, "resize",
                      JS_NewCFunction(ctx, js_resize, "resize", 2));

    JS_FreeValue(ctx, global);
}

} // namespace bro::js
