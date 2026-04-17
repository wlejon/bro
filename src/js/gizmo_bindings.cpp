#include "js/gizmo_bindings.h"
#include "engine/engine.h"
#include "engine/gizmo.h"

extern "C" {
#include "quickjs.h"
}

#include <cstring>

namespace bro::js {

// Phase-1 binding surface for bro.gizmo.*. Attaches by pivot position only;
// full target-object attach (scene node / group / duck-typed object) plus
// mouse-driven interaction arrive in phase 2.

static const char* kGizmoEngineKey = "__bro_gizmo_engine_ptr";

static engine::Engine* getEngine(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue val = JS_GetPropertyStr(ctx, global, kGizmoEngineKey);
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
// Methods
// ---------------------------------------------------------------------------

static JSValue js_gizmo_show(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (auto* e = getEngine(ctx)) e->gizmo().show();
    return JS_UNDEFINED;
}

static JSValue js_gizmo_hide(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (auto* e = getEngine(ctx)) e->gizmo().hide();
    return JS_UNDEFINED;
}

static JSValue js_gizmo_setMode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* e = getEngine(ctx);
    if (!e || argc < 1 || !JS_IsString(argv[0])) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, argv[0]);
    if (s) {
        if (strcmp(s, "translate") == 0)   e->gizmo().setMode(engine::GizmoMode::Translate);
        else if (strcmp(s, "rotate") == 0) e->gizmo().setMode(engine::GizmoMode::Rotate);
        else if (strcmp(s, "scale") == 0)  e->gizmo().setMode(engine::GizmoMode::Scale);
        JS_FreeCString(ctx, s);
    }
    return JS_UNDEFINED;
}

static JSValue js_gizmo_setSpace(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* e = getEngine(ctx);
    if (!e || argc < 1 || !JS_IsString(argv[0])) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, argv[0]);
    if (s) {
        if (strcmp(s, "local") == 0) e->gizmo().setSpace(engine::GizmoSpace::Local);
        else                         e->gizmo().setSpace(engine::GizmoSpace::World);
        JS_FreeCString(ctx, s);
    }
    return JS_UNDEFINED;
}

static JSValue js_gizmo_setPosition(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* e = getEngine(ctx);
    if (!e || argc < 3) return JS_UNDEFINED;
    double x = 0, y = 0, z = 0;
    JS_ToFloat64(ctx, &x, argv[0]);
    JS_ToFloat64(ctx, &y, argv[1]);
    JS_ToFloat64(ctx, &z, argv[2]);
    e->gizmo().setPosition(static_cast<float>(x),
                           static_cast<float>(y),
                           static_cast<float>(z));
    return JS_UNDEFINED;
}

static JSValue js_gizmo_configure(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* e = getEngine(ctx);
    if (!e) return JS_UNDEFINED;
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "gizmo.configure() requires an options object");

    auto& cfg = e->gizmo().config();

    auto readFloat = [&](const char* name, float& out) {
        JSValue v = JS_GetPropertyStr(ctx, argv[0], name);
        if (JS_IsNumber(v)) {
            double d;
            JS_ToFloat64(ctx, &d, v);
            out = static_cast<float>(d);
        }
        JS_FreeValue(ctx, v);
    };

    readFloat("size", cfg.targetPixelSize);
    readFloat("emissive", cfg.emissive);
    readFloat("emissiveHover", cfg.emissiveHover);

    JSValue alwaysOnTop = JS_GetPropertyStr(ctx, argv[0], "alwaysOnTop");
    if (JS_IsBool(alwaysOnTop)) cfg.alwaysOnTop = JS_ToBool(ctx, alwaysOnTop);
    JS_FreeValue(ctx, alwaysOnTop);

    return JS_UNDEFINED;
}

static JSValue js_gizmo_get_visible(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* e = getEngine(ctx);
    return JS_NewBool(ctx, e ? e->gizmo().visible() : 0);
}

static const JSCFunctionListEntry js_gizmo_funcs[] = {
    JS_CFUNC_DEF("show", 0, js_gizmo_show),
    JS_CFUNC_DEF("hide", 0, js_gizmo_hide),
    JS_CFUNC_DEF("setMode", 1, js_gizmo_setMode),
    JS_CFUNC_DEF("setSpace", 1, js_gizmo_setSpace),
    JS_CFUNC_DEF("setPosition", 3, js_gizmo_setPosition),
    JS_CFUNC_DEF("configure", 1, js_gizmo_configure),
};

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void GizmoBindings::install(JSContext* ctx, engine::Engine* engine) {
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, kGizmoEngineKey,
                      JS_NewInt64(ctx, static_cast<int64_t>(
                          reinterpret_cast<intptr_t>(engine))));

    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue gzObj = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, gzObj, js_gizmo_funcs,
                               sizeof(js_gizmo_funcs) / sizeof(js_gizmo_funcs[0]));

    // visible getter
    JSAtom visibleAtom = JS_NewAtom(ctx, "visible");
    JS_DefinePropertyGetSet(ctx, gzObj, visibleAtom,
        JS_NewCFunction(ctx, js_gizmo_get_visible, "get visible", 0),
        JS_UNDEFINED,
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, visibleAtom);

    JS_SetPropertyStr(ctx, broObj, "gizmo", gzObj);
    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

} // namespace bro::js
