#include "js/canvas_bindings.h"
#include "canvas/canvas_scene.h"
#include "canvas/canvas2d.h"

#include <string>
#include <cstring>

namespace bro::js {

static JSClassID js_ctx2d_class_id = 0;

static JSClassDef js_ctx2d_class = {
    "CanvasRenderingContext2D",
    nullptr, nullptr, nullptr, nullptr
};

static inline canvas::CanvasScene* getScene(JSValueConst val) {
    return static_cast<canvas::CanvasScene*>(JS_GetOpaque(val, js_ctx2d_class_id));
}

static std::string jsStr(JSContext* ctx, JSValueConst val) {
    const char* s = JS_ToCString(ctx, val);
    std::string result = s ? s : "";
    if (s) JS_FreeCString(ctx, s);
    return result;
}

// --- Property getters/setters ---

static JSValue js_get_fillStyle(JSContext* ctx, JSValueConst this_val) {
    auto* sc = getScene(this_val);
    if (!sc) return JS_UNDEFINED;
    auto& st = sc->canvas().state();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "rgba(%d,%d,%d,%.2f)",
                  st.fillR, st.fillG, st.fillB, st.fillA / 255.0f);
    return JS_NewString(ctx, buf);
}

static JSValue js_set_fillStyle(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* sc = getScene(this_val);
    if (!sc) return JS_UNDEFINED;
    std::string color = jsStr(ctx, val);
    uint8_t r, g, b, a;
    if (canvas::parseCSSColor(color, r, g, b, a))
        sc->canvas().setFillStyle(r, g, b, a);
    return JS_UNDEFINED;
}

static JSValue js_get_strokeStyle(JSContext* ctx, JSValueConst this_val) {
    auto* sc = getScene(this_val);
    if (!sc) return JS_UNDEFINED;
    auto& st = sc->canvas().state();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "rgba(%d,%d,%d,%.2f)",
                  st.strokeR, st.strokeG, st.strokeB, st.strokeA / 255.0f);
    return JS_NewString(ctx, buf);
}

static JSValue js_set_strokeStyle(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* sc = getScene(this_val);
    if (!sc) return JS_UNDEFINED;
    std::string color = jsStr(ctx, val);
    uint8_t r, g, b, a;
    if (canvas::parseCSSColor(color, r, g, b, a))
        sc->canvas().setStrokeStyle(r, g, b, a);
    return JS_UNDEFINED;
}

static JSValue js_get_lineWidth(JSContext* ctx, JSValueConst this_val) {
    auto* sc = getScene(this_val);
    if (!sc) return JS_UNDEFINED;
    return JS_NewFloat64(ctx, sc->canvas().state().lineWidth);
}

static JSValue js_set_lineWidth(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* sc = getScene(this_val);
    if (!sc) return JS_UNDEFINED;
    double v; JS_ToFloat64(ctx, &v, val);
    sc->canvas().setLineWidth((float)v);
    return JS_UNDEFINED;
}

static JSValue js_get_globalAlpha(JSContext* ctx, JSValueConst this_val) {
    auto* sc = getScene(this_val);
    if (!sc) return JS_UNDEFINED;
    return JS_NewFloat64(ctx, sc->canvas().state().globalAlpha);
}

static JSValue js_set_globalAlpha(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* sc = getScene(this_val);
    if (!sc) return JS_UNDEFINED;
    double v; JS_ToFloat64(ctx, &v, val);
    sc->canvas().setGlobalAlpha((float)v);
    return JS_UNDEFINED;
}

static JSValue js_get_font(JSContext* ctx, JSValueConst this_val) {
    auto* sc = getScene(this_val);
    if (!sc) return JS_UNDEFINED;
    return JS_NewString(ctx, sc->canvas().state().font.c_str());
}

static JSValue js_set_font(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* sc = getScene(this_val);
    if (!sc) return JS_UNDEFINED;
    sc->canvas().setFont(jsStr(ctx, val));
    return JS_UNDEFINED;
}

static JSValue js_get_canvas_width(JSContext* ctx, JSValueConst this_val) {
    auto* sc = getScene(this_val);
    return sc ? JS_NewInt32(ctx, sc->width()) : JS_UNDEFINED;
}

static JSValue js_get_canvas_height(JSContext* ctx, JSValueConst this_val) {
    auto* sc = getScene(this_val);
    return sc ? JS_NewInt32(ctx, sc->height()) : JS_UNDEFINED;
}

// --- Methods ---

static JSValue js_fillRect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* sc = getScene(this_val);
    if (!sc || argc < 4) return JS_UNDEFINED;
    double x, y, w, h;
    JS_ToFloat64(ctx, &x, argv[0]); JS_ToFloat64(ctx, &y, argv[1]);
    JS_ToFloat64(ctx, &w, argv[2]); JS_ToFloat64(ctx, &h, argv[3]);
    sc->canvas().fillRect((float)x, (float)y, (float)w, (float)h);
    return JS_UNDEFINED;
}

static JSValue js_strokeRect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* sc = getScene(this_val);
    if (!sc || argc < 4) return JS_UNDEFINED;
    double x, y, w, h;
    JS_ToFloat64(ctx, &x, argv[0]); JS_ToFloat64(ctx, &y, argv[1]);
    JS_ToFloat64(ctx, &w, argv[2]); JS_ToFloat64(ctx, &h, argv[3]);
    sc->canvas().strokeRect((float)x, (float)y, (float)w, (float)h);
    return JS_UNDEFINED;
}

static JSValue js_clearRect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* sc = getScene(this_val);
    if (!sc || argc < 4) return JS_UNDEFINED;
    double x, y, w, h;
    JS_ToFloat64(ctx, &x, argv[0]); JS_ToFloat64(ctx, &y, argv[1]);
    JS_ToFloat64(ctx, &w, argv[2]); JS_ToFloat64(ctx, &h, argv[3]);
    sc->canvas().clearRect((float)x, (float)y, (float)w, (float)h, sc->width(), sc->height());
    return JS_UNDEFINED;
}

static JSValue js_fillText(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* sc = getScene(this_val);
    if (!sc || argc < 3) return JS_UNDEFINED;
    std::string text = jsStr(ctx, argv[0]);
    double x, y;
    JS_ToFloat64(ctx, &x, argv[1]); JS_ToFloat64(ctx, &y, argv[2]);
    sc->canvas().fillText(text, (float)x, (float)y);
    return JS_UNDEFINED;
}

static JSValue js_measureText(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* sc = getScene(this_val);
    if (!sc || argc < 1) return JS_UNDEFINED;
    std::string text = jsStr(ctx, argv[0]);
    auto& fontStr = sc->canvas().state().font;
    // Get or create font handle (lazy via scene)
    // For now, approximate
    auto metrics = sc->renderer()->measureText(text, 0);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "width", JS_NewFloat64(ctx, metrics.width));
    return obj;
}

static JSValue js_save(JSContext*, JSValueConst this_val, int, JSValueConst*) {
    auto* sc = getScene(this_val); if (sc) sc->canvas().save(); return JS_UNDEFINED;
}

static JSValue js_restore(JSContext*, JSValueConst this_val, int, JSValueConst*) {
    auto* sc = getScene(this_val); if (sc) sc->canvas().restore(); return JS_UNDEFINED;
}

static JSValue js_translate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* sc = getScene(this_val);
    if (!sc || argc < 2) return JS_UNDEFINED;
    double x, y;
    JS_ToFloat64(ctx, &x, argv[0]); JS_ToFloat64(ctx, &y, argv[1]);
    sc->canvas().translate((float)x, (float)y);
    return JS_UNDEFINED;
}

static JSValue js_rotate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* sc = getScene(this_val);
    if (!sc || argc < 1) return JS_UNDEFINED;
    double a; JS_ToFloat64(ctx, &a, argv[0]);
    sc->canvas().rotate((float)a);
    return JS_UNDEFINED;
}

static JSValue js_scale(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* sc = getScene(this_val);
    if (!sc || argc < 2) return JS_UNDEFINED;
    double x, y;
    JS_ToFloat64(ctx, &x, argv[0]); JS_ToFloat64(ctx, &y, argv[1]);
    sc->canvas().scale((float)x, (float)y);
    return JS_UNDEFINED;
}

// --- Prototype ---

static const JSCFunctionListEntry js_ctx2d_proto_funcs[] = {
    JS_CGETSET_DEF("fillStyle",   js_get_fillStyle,   js_set_fillStyle),
    JS_CGETSET_DEF("strokeStyle", js_get_strokeStyle,  js_set_strokeStyle),
    JS_CGETSET_DEF("lineWidth",   js_get_lineWidth,    js_set_lineWidth),
    JS_CGETSET_DEF("globalAlpha", js_get_globalAlpha,   js_set_globalAlpha),
    JS_CGETSET_DEF("font",        js_get_font,          js_set_font),
    JS_CGETSET_DEF("canvasWidth",  js_get_canvas_width, nullptr),
    JS_CGETSET_DEF("canvasHeight", js_get_canvas_height, nullptr),
    JS_CFUNC_DEF("fillRect",    4, js_fillRect),
    JS_CFUNC_DEF("strokeRect",  4, js_strokeRect),
    JS_CFUNC_DEF("clearRect",   4, js_clearRect),
    JS_CFUNC_DEF("fillText",    3, js_fillText),
    JS_CFUNC_DEF("measureText", 1, js_measureText),
    JS_CFUNC_DEF("save",        0, js_save),
    JS_CFUNC_DEF("restore",     0, js_restore),
    JS_CFUNC_DEF("translate",   2, js_translate),
    JS_CFUNC_DEF("rotate",      1, js_rotate),
    JS_CFUNC_DEF("scale",       2, js_scale),
};

static JSValue js_ctx2d_proto = JS_UNDEFINED;

void CanvasBindings::install(JSContext* ctx) {
    JS_NewClassID(JS_GetRuntime(ctx), &js_ctx2d_class_id);
    JS_NewClass(JS_GetRuntime(ctx), js_ctx2d_class_id, &js_ctx2d_class);

    js_ctx2d_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, js_ctx2d_proto, js_ctx2d_proto_funcs,
                               sizeof(js_ctx2d_proto_funcs) / sizeof(js_ctx2d_proto_funcs[0]));
    JS_SetClassProto(ctx, js_ctx2d_class_id, js_ctx2d_proto);
}

void CanvasBindings::cleanup(JSContext*) {
    // Proto is freed by the runtime when the class is destroyed
}

JSValue CanvasBindings::wrapContext2D(JSContext* ctx, canvas::CanvasScene* scene) {
    JSValue obj = JS_NewObjectClass(ctx, (int)js_ctx2d_class_id);
    JS_SetOpaque(obj, scene);
    return obj;
}

} // namespace bro::js
