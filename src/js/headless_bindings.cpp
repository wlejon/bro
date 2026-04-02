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

    JS_FreeValue(ctx, global);
}

} // namespace bro::js
