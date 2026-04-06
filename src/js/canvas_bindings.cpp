#include "js/canvas_bindings.h"
#include "js/image_bindings.h"
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

static double jsFloat(JSContext* ctx, JSValueConst val) {
    double v = 0;
    JS_ToFloat64(ctx, &v, val);
    return v;
}

// =========================================================================
// Property getters/setters
// =========================================================================

static JSValue js_get_fillStyle(JSContext* ctx, JSValueConst this_val) {
    auto* sc = getScene(this_val);
    if (!sc) return JS_UNDEFINED;
    uint8_t r, g, b, a;
    sc->getFillColor(r, g, b, a);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "rgba(%d,%d,%d,%.2f)", r, g, b, a / 255.0f);
    return JS_NewString(ctx, buf);
}

static JSValue js_set_fillStyle(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* sc = getScene(this_val);
    if (!sc) return JS_UNDEFINED;
    uint8_t r, g, b, a;
    if (canvas::parseCSSColor(jsStr(ctx, val), r, g, b, a))
        sc->setFillColor(r, g, b, a);
    return JS_UNDEFINED;
}

static JSValue js_get_strokeStyle(JSContext* ctx, JSValueConst this_val) {
    auto* sc = getScene(this_val);
    if (!sc) return JS_UNDEFINED;
    uint8_t r, g, b, a;
    sc->getStrokeColor(r, g, b, a);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "rgba(%d,%d,%d,%.2f)", r, g, b, a / 255.0f);
    return JS_NewString(ctx, buf);
}

static JSValue js_set_strokeStyle(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* sc = getScene(this_val);
    if (!sc) return JS_UNDEFINED;
    uint8_t r, g, b, a;
    if (canvas::parseCSSColor(jsStr(ctx, val), r, g, b, a))
        sc->setStrokeColor(r, g, b, a);
    return JS_UNDEFINED;
}

static JSValue js_get_lineWidth(JSContext* ctx, JSValueConst this_val) {
    auto* sc = getScene(this_val);
    return sc ? JS_NewFloat64(ctx, sc->lineWidth()) : JS_UNDEFINED;
}

static JSValue js_set_lineWidth(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* sc = getScene(this_val);
    if (sc) sc->setLineWidth((float)jsFloat(ctx, val));
    return JS_UNDEFINED;
}

static JSValue js_get_globalAlpha(JSContext* ctx, JSValueConst this_val) {
    auto* sc = getScene(this_val);
    return sc ? JS_NewFloat64(ctx, sc->globalAlpha()) : JS_UNDEFINED;
}

static JSValue js_set_globalAlpha(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* sc = getScene(this_val);
    if (sc) sc->setGlobalAlpha((float)jsFloat(ctx, val));
    return JS_UNDEFINED;
}

static JSValue js_get_font(JSContext* ctx, JSValueConst this_val) {
    auto* sc = getScene(this_val);
    return sc ? JS_NewString(ctx, sc->fontString().c_str()) : JS_UNDEFINED;
}

static JSValue js_set_font(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* sc = getScene(this_val);
    if (sc) sc->setFont(jsStr(ctx, val));
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

// --- lineCap ---
static JSValue js_get_lineCap(JSContext* ctx, JSValueConst this_val) {
    auto* sc = getScene(this_val);
    if (!sc) return JS_UNDEFINED;
    static const char* names[] = {"butt", "round", "square"};
    int v = sc->lineCap();
    return JS_NewString(ctx, (v >= 0 && v <= 2) ? names[v] : "butt");
}

static JSValue js_set_lineCap(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* sc = getScene(this_val);
    if (!sc) return JS_UNDEFINED;
    std::string s = jsStr(ctx, val);
    if (s == "butt") sc->setLineCap(0);
    else if (s == "round") sc->setLineCap(1);
    else if (s == "square") sc->setLineCap(2);
    return JS_UNDEFINED;
}

// --- lineJoin ---
static JSValue js_get_lineJoin(JSContext* ctx, JSValueConst this_val) {
    auto* sc = getScene(this_val);
    if (!sc) return JS_UNDEFINED;
    static const char* names[] = {"miter", "round", "bevel"};
    int v = sc->lineJoin();
    return JS_NewString(ctx, (v >= 0 && v <= 2) ? names[v] : "miter");
}

static JSValue js_set_lineJoin(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* sc = getScene(this_val);
    if (!sc) return JS_UNDEFINED;
    std::string s = jsStr(ctx, val);
    if (s == "miter") sc->setLineJoin(0);
    else if (s == "round") sc->setLineJoin(1);
    else if (s == "bevel") sc->setLineJoin(2);
    return JS_UNDEFINED;
}

// --- miterLimit ---
static JSValue js_get_miterLimit(JSContext* ctx, JSValueConst this_val) {
    auto* sc = getScene(this_val);
    return sc ? JS_NewFloat64(ctx, sc->miterLimit()) : JS_UNDEFINED;
}

static JSValue js_set_miterLimit(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* sc = getScene(this_val);
    if (sc) sc->setMiterLimit((float)jsFloat(ctx, val));
    return JS_UNDEFINED;
}

// --- globalCompositeOperation ---
static JSValue js_get_globalCompositeOperation(JSContext* ctx, JSValueConst this_val) {
    auto* sc = getScene(this_val);
    if (!sc) return JS_UNDEFINED;
    static const char* names[] = {
        "source-over", "source-in", "source-out", "source-atop",
        "destination-over", "destination-in", "destination-out", "destination-atop",
        "lighter", "darken", "xor", "lighter",
        "multiply", "screen", "overlay",
        "color-dodge", "color-burn", "hard-light", "soft-light",
        "difference", "exclusion"
    };
    int v = sc->globalCompositeOperation();
    if (v >= 0 && v < 21) return JS_NewString(ctx, names[v]);
    return JS_NewString(ctx, "source-over");
}

static JSValue js_set_globalCompositeOperation(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* sc = getScene(this_val);
    if (!sc) return JS_UNDEFINED;
    std::string s = jsStr(ctx, val);
    static const char* names[] = {
        "source-over", "source-in", "source-out", "source-atop",
        "destination-over", "destination-in", "destination-out", "destination-atop",
        "lighten", "darken", "xor", "lighter",
        "multiply", "screen", "overlay",
        "color-dodge", "color-burn", "hard-light", "soft-light",
        "difference", "exclusion"
    };
    for (int i = 0; i < 21; i++) {
        if (s == names[i]) { sc->setGlobalCompositeOperation(i); return JS_UNDEFINED; }
    }
    return JS_UNDEFINED;
}

// --- textAlign ---
static JSValue js_get_textAlign(JSContext* ctx, JSValueConst this_val) {
    auto* sc = getScene(this_val);
    if (!sc) return JS_UNDEFINED;
    static const char* names[] = {"start", "center", "right", "end"};
    int v = sc->textAlign();
    return JS_NewString(ctx, (v >= 0 && v <= 3) ? names[v] : "start");
}

static JSValue js_set_textAlign(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* sc = getScene(this_val);
    if (!sc) return JS_UNDEFINED;
    std::string s = jsStr(ctx, val);
    if (s == "left" || s == "start") sc->setTextAlign(0);
    else if (s == "center") sc->setTextAlign(1);
    else if (s == "right") sc->setTextAlign(2);
    else if (s == "end") sc->setTextAlign(3);
    return JS_UNDEFINED;
}

// --- textBaseline ---
static JSValue js_get_textBaseline(JSContext* ctx, JSValueConst this_val) {
    auto* sc = getScene(this_val);
    if (!sc) return JS_UNDEFINED;
    static const char* names[] = {"alphabetic", "top", "middle", "bottom", "hanging", "ideographic"};
    int v = sc->textBaseline();
    return JS_NewString(ctx, (v >= 0 && v <= 5) ? names[v] : "alphabetic");
}

static JSValue js_set_textBaseline(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* sc = getScene(this_val);
    if (!sc) return JS_UNDEFINED;
    std::string s = jsStr(ctx, val);
    if (s == "alphabetic") sc->setTextBaseline(0);
    else if (s == "top") sc->setTextBaseline(1);
    else if (s == "middle") sc->setTextBaseline(2);
    else if (s == "bottom") sc->setTextBaseline(3);
    else if (s == "hanging") sc->setTextBaseline(4);
    else if (s == "ideographic") sc->setTextBaseline(5);
    return JS_UNDEFINED;
}

// --- shadowBlur / shadowColor / shadowOffsetX / shadowOffsetY ---
static JSValue js_get_shadowBlur(JSContext* ctx, JSValueConst this_val) {
    auto* sc = getScene(this_val);
    return sc ? JS_NewFloat64(ctx, sc->shadowBlur()) : JS_UNDEFINED;
}

static JSValue js_set_shadowBlur(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* sc = getScene(this_val);
    if (sc) sc->setShadowBlur((float)jsFloat(ctx, val));
    return JS_UNDEFINED;
}

static JSValue js_get_shadowColor(JSContext* ctx, JSValueConst this_val) {
    auto* sc = getScene(this_val);
    if (!sc) return JS_UNDEFINED;
    uint8_t r, g, b, a;
    sc->getShadowColor(r, g, b, a);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "rgba(%d,%d,%d,%.2f)", r, g, b, a / 255.0f);
    return JS_NewString(ctx, buf);
}

static JSValue js_set_shadowColor(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* sc = getScene(this_val);
    if (!sc) return JS_UNDEFINED;
    uint8_t r, g, b, a;
    if (canvas::parseCSSColor(jsStr(ctx, val), r, g, b, a))
        sc->setShadowColor(r, g, b, a);
    return JS_UNDEFINED;
}

static JSValue js_get_shadowOffsetX(JSContext* ctx, JSValueConst this_val) {
    auto* sc = getScene(this_val);
    return sc ? JS_NewFloat64(ctx, sc->shadowOffsetX()) : JS_UNDEFINED;
}

static JSValue js_set_shadowOffsetX(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* sc = getScene(this_val);
    if (sc) sc->setShadowOffsetX((float)jsFloat(ctx, val));
    return JS_UNDEFINED;
}

static JSValue js_get_shadowOffsetY(JSContext* ctx, JSValueConst this_val) {
    auto* sc = getScene(this_val);
    return sc ? JS_NewFloat64(ctx, sc->shadowOffsetY()) : JS_UNDEFINED;
}

static JSValue js_set_shadowOffsetY(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* sc = getScene(this_val);
    if (sc) sc->setShadowOffsetY((float)jsFloat(ctx, val));
    return JS_UNDEFINED;
}

// --- imageSmoothingEnabled ---
static JSValue js_get_imageSmoothingEnabled(JSContext* ctx, JSValueConst this_val) {
    auto* sc = getScene(this_val);
    return sc ? JS_NewBool(ctx, sc->imageSmoothingEnabled()) : JS_UNDEFINED;
}

static JSValue js_set_imageSmoothingEnabled(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* sc = getScene(this_val);
    if (sc) sc->setImageSmoothingEnabled(JS_ToBool(ctx, val) != 0);
    return JS_UNDEFINED;
}

// =========================================================================
// Methods
// =========================================================================

static JSValue js_fillRect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* sc = getScene(this_val);
    if (!sc || argc < 4) return JS_UNDEFINED;
    sc->fillRect((float)jsFloat(ctx, argv[0]), (float)jsFloat(ctx, argv[1]),
                 (float)jsFloat(ctx, argv[2]), (float)jsFloat(ctx, argv[3]));
    return JS_UNDEFINED;
}

static JSValue js_strokeRect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* sc = getScene(this_val);
    if (!sc || argc < 4) return JS_UNDEFINED;
    sc->strokeRect((float)jsFloat(ctx, argv[0]), (float)jsFloat(ctx, argv[1]),
                   (float)jsFloat(ctx, argv[2]), (float)jsFloat(ctx, argv[3]));
    return JS_UNDEFINED;
}

static JSValue js_clearRect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* sc = getScene(this_val);
    if (!sc || argc < 4) return JS_UNDEFINED;
    sc->clearRect((float)jsFloat(ctx, argv[0]), (float)jsFloat(ctx, argv[1]),
                  (float)jsFloat(ctx, argv[2]), (float)jsFloat(ctx, argv[3]));
    return JS_UNDEFINED;
}

static JSValue js_reset(JSContext*, JSValueConst this_val, int, JSValueConst*) {
    auto* sc = getScene(this_val);
    if (sc) sc->reset();
    return JS_UNDEFINED;
}

static JSValue js_fillText(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* sc = getScene(this_val);
    if (!sc || argc < 3) return JS_UNDEFINED;
    sc->fillText(jsStr(ctx, argv[0]), (float)jsFloat(ctx, argv[1]), (float)jsFloat(ctx, argv[2]));
    return JS_UNDEFINED;
}

static JSValue js_strokeText(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* sc = getScene(this_val);
    if (!sc || argc < 3) return JS_UNDEFINED;
    sc->strokeText(jsStr(ctx, argv[0]), (float)jsFloat(ctx, argv[1]), (float)jsFloat(ctx, argv[2]));
    return JS_UNDEFINED;
}

static JSValue js_measureText(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* sc = getScene(this_val);
    if (!sc || argc < 1) return JS_UNDEFINED;
    auto metrics = sc->measureText(jsStr(ctx, argv[0]));
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "width", JS_NewFloat64(ctx, metrics.width));
    JS_SetPropertyStr(ctx, obj, "actualBoundingBoxAscent", JS_NewFloat64(ctx, metrics.ascent));
    JS_SetPropertyStr(ctx, obj, "actualBoundingBoxDescent", JS_NewFloat64(ctx, metrics.descent));
    return obj;
}

static JSValue js_save(JSContext*, JSValueConst this_val, int, JSValueConst*) {
    auto* sc = getScene(this_val); if (sc) sc->save(); return JS_UNDEFINED;
}

static JSValue js_restore(JSContext*, JSValueConst this_val, int, JSValueConst*) {
    auto* sc = getScene(this_val); if (sc) sc->restore(); return JS_UNDEFINED;
}

static JSValue js_translate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* sc = getScene(this_val);
    if (!sc || argc < 2) return JS_UNDEFINED;
    sc->translate((float)jsFloat(ctx, argv[0]), (float)jsFloat(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue js_rotate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* sc = getScene(this_val);
    if (!sc || argc < 1) return JS_UNDEFINED;
    sc->rotate((float)jsFloat(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_scale(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* sc = getScene(this_val);
    if (!sc || argc < 2) return JS_UNDEFINED;
    sc->scale((float)jsFloat(ctx, argv[0]), (float)jsFloat(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue js_setTransform(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* sc = getScene(this_val);
    if (!sc || argc < 6) return JS_UNDEFINED;
    sc->setTransform((float)jsFloat(ctx, argv[0]), (float)jsFloat(ctx, argv[1]),
                     (float)jsFloat(ctx, argv[2]), (float)jsFloat(ctx, argv[3]),
                     (float)jsFloat(ctx, argv[4]), (float)jsFloat(ctx, argv[5]));
    return JS_UNDEFINED;
}

static JSValue js_resetTransform(JSContext*, JSValueConst this_val, int, JSValueConst*) {
    auto* sc = getScene(this_val);
    if (sc) sc->resetTransform();
    return JS_UNDEFINED;
}

static JSValue js_transform(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* sc = getScene(this_val);
    if (!sc || argc < 6) return JS_UNDEFINED;
    sc->transform((float)jsFloat(ctx, argv[0]), (float)jsFloat(ctx, argv[1]),
                  (float)jsFloat(ctx, argv[2]), (float)jsFloat(ctx, argv[3]),
                  (float)jsFloat(ctx, argv[4]), (float)jsFloat(ctx, argv[5]));
    return JS_UNDEFINED;
}

// --- Path API ---

static JSValue js_beginPath(JSContext*, JSValueConst this_val, int, JSValueConst*) {
    auto* sc = getScene(this_val); if (sc) sc->beginPath(); return JS_UNDEFINED;
}

static JSValue js_moveTo(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* sc = getScene(this_val);
    if (!sc || argc < 2) return JS_UNDEFINED;
    sc->moveTo((float)jsFloat(ctx, argv[0]), (float)jsFloat(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue js_lineTo(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* sc = getScene(this_val);
    if (!sc || argc < 2) return JS_UNDEFINED;
    sc->lineTo((float)jsFloat(ctx, argv[0]), (float)jsFloat(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue js_closePath(JSContext*, JSValueConst this_val, int, JSValueConst*) {
    auto* sc = getScene(this_val); if (sc) sc->closePath(); return JS_UNDEFINED;
}

static JSValue js_stroke(JSContext*, JSValueConst this_val, int, JSValueConst*) {
    auto* sc = getScene(this_val); if (sc) sc->stroke(); return JS_UNDEFINED;
}

static JSValue js_fill(JSContext*, JSValueConst this_val, int, JSValueConst*) {
    auto* sc = getScene(this_val); if (sc) sc->fill(); return JS_UNDEFINED;
}

static JSValue js_clip(JSContext*, JSValueConst this_val, int, JSValueConst*) {
    auto* sc = getScene(this_val); if (sc) sc->clip(); return JS_UNDEFINED;
}

static JSValue js_arc(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* sc = getScene(this_val);
    if (!sc || argc < 5) return JS_UNDEFINED;
    bool acw = (argc >= 6) ? JS_ToBool(ctx, argv[5]) != 0 : false;
    sc->arc((float)jsFloat(ctx, argv[0]), (float)jsFloat(ctx, argv[1]),
            (float)jsFloat(ctx, argv[2]), (float)jsFloat(ctx, argv[3]),
            (float)jsFloat(ctx, argv[4]), acw);
    return JS_UNDEFINED;
}

static JSValue js_arcTo(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* sc = getScene(this_val);
    if (!sc || argc < 5) return JS_UNDEFINED;
    sc->arcTo((float)jsFloat(ctx, argv[0]), (float)jsFloat(ctx, argv[1]),
              (float)jsFloat(ctx, argv[2]), (float)jsFloat(ctx, argv[3]),
              (float)jsFloat(ctx, argv[4]));
    return JS_UNDEFINED;
}

static JSValue js_bezierCurveTo(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* sc = getScene(this_val);
    if (!sc || argc < 6) return JS_UNDEFINED;
    sc->bezierCurveTo((float)jsFloat(ctx, argv[0]), (float)jsFloat(ctx, argv[1]),
                      (float)jsFloat(ctx, argv[2]), (float)jsFloat(ctx, argv[3]),
                      (float)jsFloat(ctx, argv[4]), (float)jsFloat(ctx, argv[5]));
    return JS_UNDEFINED;
}

static JSValue js_quadraticCurveTo(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* sc = getScene(this_val);
    if (!sc || argc < 4) return JS_UNDEFINED;
    sc->quadraticCurveTo((float)jsFloat(ctx, argv[0]), (float)jsFloat(ctx, argv[1]),
                         (float)jsFloat(ctx, argv[2]), (float)jsFloat(ctx, argv[3]));
    return JS_UNDEFINED;
}

static JSValue js_ellipse(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* sc = getScene(this_val);
    if (!sc || argc < 7) return JS_UNDEFINED;
    bool acw = (argc >= 8) ? JS_ToBool(ctx, argv[7]) != 0 : false;
    sc->ellipse((float)jsFloat(ctx, argv[0]), (float)jsFloat(ctx, argv[1]),
                (float)jsFloat(ctx, argv[2]), (float)jsFloat(ctx, argv[3]),
                (float)jsFloat(ctx, argv[4]), (float)jsFloat(ctx, argv[5]),
                (float)jsFloat(ctx, argv[6]), acw);
    return JS_UNDEFINED;
}

static JSValue js_rect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* sc = getScene(this_val);
    if (!sc || argc < 4) return JS_UNDEFINED;
    sc->rect((float)jsFloat(ctx, argv[0]), (float)jsFloat(ctx, argv[1]),
             (float)jsFloat(ctx, argv[2]), (float)jsFloat(ctx, argv[3]));
    return JS_UNDEFINED;
}

static JSValue js_isPointInPath(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* sc = getScene(this_val);
    if (!sc || argc < 2) return JS_FALSE;
    return JS_NewBool(ctx, sc->isPointInPath((float)jsFloat(ctx, argv[0]),
                                              (float)jsFloat(ctx, argv[1])));
}

// --- drawImage ---

static JSValue js_drawImage(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* sc = getScene(this_val);
    if (!sc || argc < 3) return JS_UNDEFINED;

    ImagePixels pix;
    if (!ImageBindings::getImagePixels(argv[0], pix)) return JS_UNDEFINED;

    if (argc >= 9) {
        // drawImage(img, sx, sy, sw, sh, dx, dy, dw, dh)
        sc->drawImage(pix.data, pix.width, pix.height,
                      (float)jsFloat(ctx, argv[1]), (float)jsFloat(ctx, argv[2]),
                      (float)jsFloat(ctx, argv[3]), (float)jsFloat(ctx, argv[4]),
                      (float)jsFloat(ctx, argv[5]), (float)jsFloat(ctx, argv[6]),
                      (float)jsFloat(ctx, argv[7]), (float)jsFloat(ctx, argv[8]));
    } else if (argc >= 5) {
        // drawImage(img, dx, dy, dw, dh)
        sc->drawImage(pix.data, pix.width, pix.height,
                      0, 0, (float)pix.width, (float)pix.height,
                      (float)jsFloat(ctx, argv[1]), (float)jsFloat(ctx, argv[2]),
                      (float)jsFloat(ctx, argv[3]), (float)jsFloat(ctx, argv[4]));
    } else {
        // drawImage(img, dx, dy)
        sc->drawImage(pix.data, pix.width, pix.height,
                      0, 0, (float)pix.width, (float)pix.height,
                      (float)jsFloat(ctx, argv[1]), (float)jsFloat(ctx, argv[2]),
                      (float)pix.width, (float)pix.height);
    }
    return JS_UNDEFINED;
}

// --- getImageData / putImageData ---

static JSValue js_getImageData(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* sc = getScene(this_val);
    if (!sc || argc < 4) return JS_UNDEFINED;

    int x = (int)jsFloat(ctx, argv[0]), y = (int)jsFloat(ctx, argv[1]);
    int w = (int)jsFloat(ctx, argv[2]), h = (int)jsFloat(ctx, argv[3]);

    auto pixels = sc->getImageData(x, y, w, h);

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "width", JS_NewInt32(ctx, w));
    JS_SetPropertyStr(ctx, obj, "height", JS_NewInt32(ctx, h));

    // Create a Uint8ClampedArray for the data
    JSValue abuf = JS_NewArrayBufferCopy(ctx, pixels.data(), pixels.size());
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue u8cCtor = JS_GetPropertyStr(ctx, global, "Uint8ClampedArray");
    JSValue dataArr = JS_CallConstructor(ctx, u8cCtor, 1, &abuf);
    JS_FreeValue(ctx, u8cCtor);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, abuf);
    JS_SetPropertyStr(ctx, obj, "data", dataArr);

    return obj;
}

static JSValue js_putImageData(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* sc = getScene(this_val);
    if (!sc || argc < 3) return JS_UNDEFINED;

    // argv[0] is an ImageData object with .data, .width, .height
    JSValue wVal = JS_GetPropertyStr(ctx, argv[0], "width");
    JSValue hVal = JS_GetPropertyStr(ctx, argv[0], "height");
    JSValue dataVal = JS_GetPropertyStr(ctx, argv[0], "data");

    int w = 0, h = 0;
    JS_ToInt32(ctx, &w, wVal);
    JS_ToInt32(ctx, &h, hVal);

    // Get the underlying buffer from the typed array
    size_t len = 0;
    uint8_t* buf = JS_GetArrayBuffer(ctx, &len, dataVal);
    if (!buf) {
        // Try as typed array
        size_t offset, blen;
        JSValue abuf = JS_GetTypedArrayBuffer(ctx, dataVal, &offset, &blen, nullptr);
        buf = JS_GetArrayBuffer(ctx, &len, abuf);
        if (buf) buf += offset;
        JS_FreeValue(ctx, abuf);
    }

    int dx = (int)jsFloat(ctx, argv[1]);
    int dy = (int)jsFloat(ctx, argv[2]);

    if (buf && w > 0 && h > 0) {
        sc->putImageData(buf, w, h, dx, dy);
    }

    JS_FreeValue(ctx, wVal);
    JS_FreeValue(ctx, hVal);
    JS_FreeValue(ctx, dataVal);
    return JS_UNDEFINED;
}

// --- createImageData ---

static JSValue js_createImageData(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    int w = (int)jsFloat(ctx, argv[0]), h = (int)jsFloat(ctx, argv[1]);
    if (w <= 0 || h <= 0) return JS_UNDEFINED;

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "width", JS_NewInt32(ctx, w));
    JS_SetPropertyStr(ctx, obj, "height", JS_NewInt32(ctx, h));

    size_t sz = (size_t)w * h * 4;
    std::vector<uint8_t> zeros(sz, 0);
    JSValue abuf = JS_NewArrayBufferCopy(ctx, zeros.data(), sz);
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue u8cCtor = JS_GetPropertyStr(ctx, global, "Uint8ClampedArray");
    JSValue dataArr = JS_CallConstructor(ctx, u8cCtor, 1, &abuf);
    JS_FreeValue(ctx, u8cCtor);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, abuf);
    JS_SetPropertyStr(ctx, obj, "data", dataArr);
    return obj;
}

// =========================================================================
// Prototype
// =========================================================================

static const JSCFunctionListEntry js_ctx2d_proto_funcs[] = {
    // Properties
    JS_CGETSET_DEF("fillStyle",   js_get_fillStyle,   js_set_fillStyle),
    JS_CGETSET_DEF("strokeStyle", js_get_strokeStyle,  js_set_strokeStyle),
    JS_CGETSET_DEF("lineWidth",   js_get_lineWidth,    js_set_lineWidth),
    JS_CGETSET_DEF("globalAlpha", js_get_globalAlpha,   js_set_globalAlpha),
    JS_CGETSET_DEF("font",        js_get_font,          js_set_font),
    JS_CGETSET_DEF("canvasWidth",  js_get_canvas_width, nullptr),
    JS_CGETSET_DEF("canvasHeight", js_get_canvas_height, nullptr),
    JS_CGETSET_DEF("lineCap",     js_get_lineCap,       js_set_lineCap),
    JS_CGETSET_DEF("lineJoin",    js_get_lineJoin,      js_set_lineJoin),
    JS_CGETSET_DEF("miterLimit",  js_get_miterLimit,    js_set_miterLimit),
    JS_CGETSET_DEF("globalCompositeOperation", js_get_globalCompositeOperation, js_set_globalCompositeOperation),
    JS_CGETSET_DEF("textAlign",   js_get_textAlign,     js_set_textAlign),
    JS_CGETSET_DEF("textBaseline", js_get_textBaseline,  js_set_textBaseline),
    JS_CGETSET_DEF("shadowBlur",  js_get_shadowBlur,    js_set_shadowBlur),
    JS_CGETSET_DEF("shadowColor", js_get_shadowColor,   js_set_shadowColor),
    JS_CGETSET_DEF("shadowOffsetX", js_get_shadowOffsetX, js_set_shadowOffsetX),
    JS_CGETSET_DEF("shadowOffsetY", js_get_shadowOffsetY, js_set_shadowOffsetY),
    JS_CGETSET_DEF("imageSmoothingEnabled", js_get_imageSmoothingEnabled, js_set_imageSmoothingEnabled),

    // Methods — drawing
    JS_CFUNC_DEF("fillRect",    4, js_fillRect),
    JS_CFUNC_DEF("strokeRect",  4, js_strokeRect),
    JS_CFUNC_DEF("clearRect",   4, js_clearRect),
    JS_CFUNC_DEF("reset",       0, js_reset),
    JS_CFUNC_DEF("fillText",    3, js_fillText),
    JS_CFUNC_DEF("strokeText",  3, js_strokeText),
    JS_CFUNC_DEF("measureText", 1, js_measureText),

    // Methods — state
    JS_CFUNC_DEF("save",        0, js_save),
    JS_CFUNC_DEF("restore",     0, js_restore),
    JS_CFUNC_DEF("translate",   2, js_translate),
    JS_CFUNC_DEF("rotate",      1, js_rotate),
    JS_CFUNC_DEF("scale",       2, js_scale),
    JS_CFUNC_DEF("setTransform", 6, js_setTransform),
    JS_CFUNC_DEF("resetTransform", 0, js_resetTransform),
    JS_CFUNC_DEF("transform",   6, js_transform),

    // Methods — path
    JS_CFUNC_DEF("beginPath",   0, js_beginPath),
    JS_CFUNC_DEF("moveTo",      2, js_moveTo),
    JS_CFUNC_DEF("lineTo",      2, js_lineTo),
    JS_CFUNC_DEF("closePath",   0, js_closePath),
    JS_CFUNC_DEF("stroke",      0, js_stroke),
    JS_CFUNC_DEF("fill",        0, js_fill),
    JS_CFUNC_DEF("clip",        0, js_clip),
    JS_CFUNC_DEF("arc",         6, js_arc),
    JS_CFUNC_DEF("arcTo",       5, js_arcTo),
    JS_CFUNC_DEF("bezierCurveTo", 6, js_bezierCurveTo),
    JS_CFUNC_DEF("quadraticCurveTo", 4, js_quadraticCurveTo),
    JS_CFUNC_DEF("ellipse",     8, js_ellipse),
    JS_CFUNC_DEF("rect",        4, js_rect),
    JS_CFUNC_DEF("isPointInPath", 2, js_isPointInPath),

    // Methods — image
    JS_CFUNC_DEF("drawImage",   9, js_drawImage),
    JS_CFUNC_DEF("getImageData", 4, js_getImageData),
    JS_CFUNC_DEF("putImageData", 3, js_putImageData),
    JS_CFUNC_DEF("createImageData", 2, js_createImageData),
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
}

JSValue CanvasBindings::wrapContext2D(JSContext* ctx, canvas::CanvasScene* scene) {
    JSValue obj = JS_NewObjectClass(ctx, (int)js_ctx2d_class_id);
    JS_SetOpaque(obj, scene);
    return obj;
}

} // namespace bro::js
