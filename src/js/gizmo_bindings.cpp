#if BRO_WITH_3D

#include "js/gizmo_bindings.h"
#include "engine/engine.h"
#include "engine/gizmo.h"
#include "scene/scene_graph.h"
#include "scene/scene_node.h"
#include <cstring>

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

namespace {
static int slotFromName(const char* name) {
    if (!name) return -1;
    if (!strcmp(name, "position"))     return engine::GizmoManager::CB_Position;
    if (!strcmp(name, "orientation"))  return engine::GizmoManager::CB_Orientation;
    if (!strcmp(name, "beginDrag"))    return engine::GizmoManager::CB_BeginDrag;
    if (!strcmp(name, "translate"))    return engine::GizmoManager::CB_Translate;
    if (!strcmp(name, "rotate"))       return engine::GizmoManager::CB_Rotate;
    if (!strcmp(name, "scale"))        return engine::GizmoManager::CB_Scale;
    if (!strcmp(name, "endDrag"))      return engine::GizmoManager::CB_EndDrag;
    if (!strcmp(name, "hoverChange"))  return engine::GizmoManager::CB_HoverChange;
    return -1;
}
} // namespace

// ---------------------------------------------------------------------------
// Engine pointer stash (no pinned JSValues, no finalizer-order hazard).
// ---------------------------------------------------------------------------

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
// Accessors
// ---------------------------------------------------------------------------

static JSValue js_gizmo_get_visible(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* eng = getEngine(ctx);
    auto* e = getEngine(ctx);
    return JS_NewBool(ctx, e ? e->gizmo().visible() : 0);
}

static JSValue js_gizmo_get_dragging(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* eng = getEngine(ctx);
    auto* e = getEngine(ctx);
    return JS_NewBool(ctx, e ? e->gizmo().isDragging() : 0);
}

static JSValue js_gizmo_get_hovered(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* eng = getEngine(ctx);
    auto* e = getEngine(ctx);
    if (!e) return JS_NULL;
    const char* s = nullptr;
    switch (e->gizmo().hovered()) {
    case engine::GizmoAxis::X: s = "x"; break;
    case engine::GizmoAxis::Y: s = "y"; break;
    case engine::GizmoAxis::Z: s = "z"; break;
    case engine::GizmoAxis::XY: s = "xy"; break;
    case engine::GizmoAxis::YZ: s = "yz"; break;
    case engine::GizmoAxis::XZ: s = "xz"; break;
    case engine::GizmoAxis::View: s = "view"; break;
    case engine::GizmoAxis::Center: s = "center"; break;
    default: return JS_NULL;
    }
    return JS_NewString(ctx, s);
}

static JSValue js_gizmo_show(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (auto* e = getEngine(ctx)) e->gizmo().show();
    return JS_UNDEFINED;
}

static JSValue js_gizmo_hide(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (auto* e = getEngine(ctx)) e->gizmo().hide();
    return JS_UNDEFINED;
}

static JSValue js_gizmo_set_mode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
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

static JSValue js_gizmo_set_space(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
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

static JSValue js_gizmo_set_position(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
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

static JSValue js_gizmo_set_orientation(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* e = getEngine(ctx);
    if (!e || argc < 4) return JS_UNDEFINED;
    double x = 0, y = 0, z = 0, w = 1;
    JS_ToFloat64(ctx, &x, argv[0]);
    JS_ToFloat64(ctx, &y, argv[1]);
    JS_ToFloat64(ctx, &z, argv[2]);
    JS_ToFloat64(ctx, &w, argv[3]);
    e->gizmo().setOrientation(bromath::Quat(static_cast<float>(x),
                                          static_cast<float>(y),
                                          static_cast<float>(z),
                                          static_cast<float>(w)));
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
        if (JS_IsNumber(v)) { double d; JS_ToFloat64(ctx, &d, v); out = static_cast<float>(d); }
        JS_FreeValue(ctx, v);
    };
    readFloat("size", cfg.targetPixelSize);
    readFloat("emissive", cfg.emissive);
    readFloat("emissiveHover", cfg.emissiveHover);
    
    JSValue aot = JS_GetPropertyStr(ctx, argv[0], "alwaysOnTop");
    if (JS_IsBool(aot)) cfg.alwaysOnTop = JS_ToBool(ctx, aot);
    JS_FreeValue(ctx, aot);
    
    JSValue colors = JS_GetPropertyStr(ctx, argv[0], "colors");
    if (JS_IsObject(colors)) {
        JSValue xv = JS_GetPropertyStr(ctx, colors, "x");
        JSValue yv = JS_GetPropertyStr(ctx, colors, "y");
        JSValue zv = JS_GetPropertyStr(ctx, colors, "z");
        JSValue hv = JS_GetPropertyStr(ctx, colors, "hover");
        JSValue av = JS_GetPropertyStr(ctx, colors, "active");
        auto apply = [&](JSValue v, float (&out)[4]) {
            if (JS_IsString(v)) {
                const char* s = JS_ToCString(ctx, v);
                if (s && s[0] == '#') {
                    size_t len = strlen(s);
                    unsigned long val = strtoul(s + 1, nullptr, 16);
                    if (len == 7) {
                        out[0] = ((val >> 16) & 0xFF) / 255.0f;
                        out[1] = ((val >>  8) & 0xFF) / 255.0f;
                        out[2] = (val & 0xFF) / 255.0f;
                        out[3] = 1.0f;
                    } else if (len == 9) {
                        out[0] = ((val >> 24) & 0xFF) / 255.0f;
                        out[1] = ((val >> 16) & 0xFF) / 255.0f;
                        out[2] = ((val >>  8) & 0xFF) / 255.0f;
                        out[3] = (val & 0xFF) / 255.0f;
                    }
                }
                if (s) JS_FreeCString(ctx, s);
            }
        };
        apply(xv, cfg.colorX);
        apply(yv, cfg.colorY);
        apply(zv, cfg.colorZ);
        apply(hv, cfg.colorHover);
        apply(av, cfg.colorActive);
        JS_FreeValue(ctx, xv); JS_FreeValue(ctx, yv); JS_FreeValue(ctx, zv);
        JS_FreeValue(ctx, hv); JS_FreeValue(ctx, av);
    }
    JS_FreeValue(ctx, colors);
    return JS_UNDEFINED;
}

static JSValue js_gizmo_attach(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* e = getEngine(ctx);
    if (!e) return JS_UNDEFINED;
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "gizmo.attach() requires an object");
    
    e->gizmo().setJSContext(ctx);
    const char* keys[] = {
        "position", "orientation", "beginDrag", "translate",
        "rotate", "scale", "endDrag", "hoverChange"
    };
    for (const char* k : keys) {
        JSValue v = JS_GetPropertyStr(ctx, argv[0], k);
        int slot = slotFromName(k);
        if (slot >= 0) {
            if (JS_IsFunction(ctx, v)) {
                e->gizmo().setCallback(slot, v);
            } else if (!JS_IsUndefined(v)) {
                e->gizmo().setCallback(slot, JS_UNDEFINED);
            }
        }
        JS_FreeValue(ctx, v);
    }
    e->gizmo().show();
    return JS_UNDEFINED;
}

static JSValue js_gizmo_detach(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* e = getEngine(ctx);
    if (!e) return JS_UNDEFINED;
    for (int i = 0; i < engine::GizmoManager::CB_COUNT; ++i) {
        e->gizmo().setCallback(i, JS_UNDEFINED);
    }
    e->gizmo().hide();
    return JS_UNDEFINED;
}

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

    JSValue gizmoObj = JS_NewObject(ctx);

    auto defineGetSet = [&](const char* name, JSCFunction* getter,
                            JSCFunction* setter) {
        JSAtom atom = JS_NewAtom(ctx, name);
        JS_DefinePropertyGetSet(ctx, gizmoObj, atom,
            JS_NewCFunction(ctx, getter, name, 0),
            setter ? JS_NewCFunction(ctx, setter, name, 1) : JS_UNDEFINED,
            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, atom);
    };
    defineGetSet("visible",  js_gizmo_get_visible,  nullptr);
    defineGetSet("dragging",  js_gizmo_get_dragging,  nullptr);
    defineGetSet("hovered",  js_gizmo_get_hovered,  nullptr);
    JS_SetPropertyStr(ctx, gizmoObj, "show",
        JS_NewCFunction(ctx, js_gizmo_show, "show", 0));
    JS_SetPropertyStr(ctx, gizmoObj, "hide",
        JS_NewCFunction(ctx, js_gizmo_hide, "hide", 0));
    JS_SetPropertyStr(ctx, gizmoObj, "setMode",
        JS_NewCFunction(ctx, js_gizmo_set_mode, "setMode", 1));
    JS_SetPropertyStr(ctx, gizmoObj, "setSpace",
        JS_NewCFunction(ctx, js_gizmo_set_space, "setSpace", 1));
    JS_SetPropertyStr(ctx, gizmoObj, "setPosition",
        JS_NewCFunction(ctx, js_gizmo_set_position, "setPosition", 3));
    JS_SetPropertyStr(ctx, gizmoObj, "setOrientation",
        JS_NewCFunction(ctx, js_gizmo_set_orientation, "setOrientation", 4));
    JS_SetPropertyStr(ctx, gizmoObj, "configure",
        JS_NewCFunction(ctx, js_gizmo_configure, "configure", 1));
    JS_SetPropertyStr(ctx, gizmoObj, "attach",
        JS_NewCFunction(ctx, js_gizmo_attach, "attach", 1));
    JS_SetPropertyStr(ctx, gizmoObj, "detach",
        JS_NewCFunction(ctx, js_gizmo_detach, "detach", 0));

    JS_SetPropertyStr(ctx, broObj, "gizmo", gizmoObj);
    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}


} // namespace bro::js

#endif // BRO_WITH_3D
