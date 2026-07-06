#include "js/canvas_bindings.h"
#include "js/image_bindings.h"
#include "js/imagebitmap_bindings.h"
#include "js/dom_bindings_internal.h"
#include "canvas/canvas_scene.h"
#include "canvas/canvas2d.h"

#include <qjsbind/qjsbind.h>

#include <include/core/SkColor.h>
#include <include/core/SkPoint.h>
#include <include/core/SkShader.h>
#include <include/core/SkTileMode.h>
#include <include/effects/SkGradient.h>

#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace bro::js {

// Non-owning wrapper so qjsbind's delete-destructor doesn't free the scene
struct Ctx2DWrapper {
    canvas::CanvasScene* scene = nullptr;
};

using CW = Ctx2DWrapper;

// ---------------------------------------------------------------------------
// CanvasGradient — returned by createLinearGradient / createRadialGradient.
// Stores the parameters as plain data; the SkShader is built lazily by
// buildShader() each time fillStyle/strokeStyle is assigned. That matches the
// Canvas 2D spec: assigning the gradient snapshots its current stops onto
// the paint — later addColorStop() calls on the same object only affect
// subsequent assignments.
// ---------------------------------------------------------------------------

struct CanvasGradient {
    enum Kind : uint8_t { kLinear = 0, kRadial = 1 };
    Kind kind = kLinear;
    // Linear: pts[0] = (p[0], p[1]), pts[1] = (p[2], p[3])
    // Radial: center0 = (p[0], p[1]) r0 = p[2], center1 = (p[3], p[4]) r1 = p[5]
    float p[6] = {};
    std::vector<std::pair<float, uint32_t>> stops;  // (offset, ARGB packed)

    sk_sp<SkShader> buildShader() const {
        if (stops.empty()) return nullptr;

        // Canvas 2D spec: if one stop, render as solid color across the range.
        // SkShaders requires at least two stops — duplicate when needed.
        std::vector<SkColor4f> colors;
        std::vector<float> positions;
        colors.reserve(stops.size() + 1);
        positions.reserve(stops.size() + 1);
        for (const auto& [off, argb] : stops) {
            colors.push_back(SkColor4f::FromColor(static_cast<SkColor>(argb)));
            positions.push_back(off);
        }
        if (colors.size() == 1) {
            colors.push_back(colors[0]);
            positions.push_back(positions[0]);
        }

        SkGradient::Colors c(
            SkSpan<const SkColor4f>(colors.data(),    colors.size()),
            SkSpan<const float>    (positions.data(), positions.size()),
            SkTileMode::kClamp);
        SkGradient grad(c, SkGradient::Interpolation{});

        if (kind == kLinear) {
            SkPoint pts[2] = { {p[0], p[1]}, {p[2], p[3]} };
            return SkShaders::LinearGradient(pts, grad);
        }
        // Radial: two-point conical per Canvas 2D semantics.
        return SkShaders::TwoPointConicalGradient(
            { p[0], p[1] }, p[2],
            { p[3], p[4] }, p[5],
            grad);
    }
};

static JSValue js_createLinearGradient(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv) {
    if (argc < 4) return JS_UNDEFINED;
    auto F = [&](int i) { double v = 0; JS_ToFloat64(ctx, &v, argv[i]); return (float)v; };
    auto* g = new CanvasGradient();
    g->kind = CanvasGradient::kLinear;
    g->p[0] = F(0); g->p[1] = F(1);
    g->p[2] = F(2); g->p[3] = F(3);
    return qjsbind::wrap<CanvasGradient>(ctx, g);
}

static JSValue js_createRadialGradient(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv) {
    if (argc < 6) return JS_UNDEFINED;
    auto F = [&](int i) { double v = 0; JS_ToFloat64(ctx, &v, argv[i]); return (float)v; };
    auto* g = new CanvasGradient();
    g->kind = CanvasGradient::kRadial;
    g->p[0] = F(0); g->p[1] = F(1); g->p[2] = F(2);
    g->p[3] = F(3); g->p[4] = F(4); g->p[5] = F(5);
    return qjsbind::wrap<CanvasGradient>(ctx, g);
}

// ---------------------------------------------------------------------------
// Helper
// ---------------------------------------------------------------------------

static std::string colorToRGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "rgba(%d,%d,%d,%.2f)", r, g, b, a / 255.0f);
    return buf;
}

// ---------------------------------------------------------------------------
// Raw functions for complex methods needing raw argc/argv
// ---------------------------------------------------------------------------

static JSValue js_drawImage(JSContext* ctx, JSValueConst this_val,
                            int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<CW>(ctx, this_val);
    auto* sc = w ? w->scene : nullptr;
    if (!sc || argc < 3) return JS_UNDEFINED;

    auto F = [&](int i) { double v = 0; JS_ToFloat64(ctx, &v, argv[i]); return (float)v; };

    // Fast path: source is an ImageBitmap. It is an immutable raster SkImage —
    // draw it straight through; Ganesh uploads + caches the texture once, so
    // redrawing the same bitmap (e.g. a scrubbed frame) costs no re-upload.
    if (sk_sp<SkImage> bmp = ImageBitmapBindings::getImage(argv[0])) {
        int sw = bmp->width(), sh = bmp->height();
        if (argc >= 9) {
            sc->drawImage(std::move(bmp),
                          F(1), F(2), F(3), F(4), F(5), F(6), F(7), F(8));
        } else if (argc >= 5) {
            sc->drawImage(std::move(bmp),
                          0, 0, (float)sw, (float)sh, F(1), F(2), F(3), F(4));
        } else {
            sc->drawImage(std::move(bmp),
                          0, 0, (float)sw, (float)sh,
                          F(1), F(2), (float)sw, (float)sh);
        }
        return JS_UNDEFINED;
    }

    // Fast path: source is an HTMLCanvasElement. Snapshot its surface as an
    // SkImage (cached on the source scene, invalidated on mutation) and draw
    // straight from it — Ganesh sees one texture across many blits, instead
    // of N copies of the same RGBA buffer turning into N unique GPU uploads.
    // This is what makes sprite-atlas-style rendering practical: a tilemap
    // that issues 1000 drawImage(atlas, ...) calls per frame works the way
    // you'd expect, not at one-readback-per-blit speed.
    if (auto* el = bro::js::getElement(argv[0])) {
        auto* srcScene = static_cast<bro::canvas::CanvasScene*>(el->canvasScene());
        if (srcScene) {
            sk_sp<SkImage> img = srcScene->snapshotImage();
            if (img) {
                int sw = img->width(), sh = img->height();
                if (argc >= 9) {
                    sc->drawImage(std::move(img),
                                  F(1), F(2), F(3), F(4), F(5), F(6), F(7), F(8));
                } else if (argc >= 5) {
                    sc->drawImage(std::move(img),
                                  0, 0, (float)sw, (float)sh,
                                  F(1), F(2), F(3), F(4));
                } else {
                    sc->drawImage(std::move(img),
                                  0, 0, (float)sw, (float)sh,
                                  F(1), F(2), (float)sw, (float)sh);
                }
                return JS_UNDEFINED;
            }
        }
    }

    // Slow path: Image (or any other CanvasImageSource that exposes pixels).
    // Falls through to the rgba-copy path so PNG/JPG-decoded sources still
    // work the way they always have.
    ImagePixels pix;
    if (!ImageBindings::getImagePixels(argv[0], pix)) return JS_UNDEFINED;

    if (argc >= 9) {
        sc->drawImage(pix.data, pix.width, pix.height,
                      F(1), F(2), F(3), F(4), F(5), F(6), F(7), F(8));
    } else if (argc >= 5) {
        sc->drawImage(pix.data, pix.width, pix.height,
                      0, 0, (float)pix.width, (float)pix.height,
                      F(1), F(2), F(3), F(4));
    } else {
        sc->drawImage(pix.data, pix.width, pix.height,
                      0, 0, (float)pix.width, (float)pix.height,
                      F(1), F(2), (float)pix.width, (float)pix.height);
    }
    return JS_UNDEFINED;
}

static JSValue js_polyline(JSContext* ctx, JSValueConst this_val,
                           int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<CW>(ctx, this_val);
    auto* sc = w ? w->scene : nullptr;
    if (!sc || argc < 1) return JS_UNDEFINED;

    size_t offset = 0, byteLen = 0, bytesPerElement = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, argv[0], &offset, &byteLen, &bytesPerElement);
    if (JS_IsException(abuf)) {
        JS_FreeValue(ctx, abuf);
        return JS_UNDEFINED;
    }
    size_t abufLen = 0;
    uint8_t* rawBuf = JS_GetArrayBuffer(ctx, &abufLen, abuf);
    JS_FreeValue(ctx, abuf);
    if (!rawBuf) return JS_UNDEFINED;

    const float* coords = reinterpret_cast<const float*>(rawBuf + offset);
    int numFloats = static_cast<int>(byteLen / sizeof(float));
    int numPoints = numFloats / 2;
    if (numPoints < 1) return JS_UNDEFINED;

    sc->polyline(coords, numPoints);
    return JS_UNDEFINED;
}

static JSValue js_getImageData(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<CW>(ctx, this_val);
    auto* sc = w ? w->scene : nullptr;
    if (!sc || argc < 4) return JS_UNDEFINED;

    auto F = [&](int i) { double v = 0; JS_ToFloat64(ctx, &v, argv[i]); return (int)v; };
    int x = F(0), y = F(1), wi = F(2), h = F(3);

    auto pixels = sc->getImageData(x, y, wi, h);

    JSValue abuf = JS_NewArrayBufferCopy(ctx, pixels.data(), pixels.size());
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue u8cCtor = JS_GetPropertyStr(ctx, global, "Uint8ClampedArray");
    JSValue dataArr = JS_CallConstructor(ctx, u8cCtor, 1, &abuf);
    JS_FreeValue(ctx, u8cCtor);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, abuf);

    // Real ImageData instance (instanceof-true), not a duck-typed object.
    return ImageBitmapBindings::makeImageData(ctx, wi, h, dataArr);
}

static JSValue js_putImageData(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<CW>(ctx, this_val);
    auto* sc = w ? w->scene : nullptr;
    if (!sc || argc < 3) return JS_UNDEFINED;

    JSValue wVal = JS_GetPropertyStr(ctx, argv[0], "width");
    JSValue hVal = JS_GetPropertyStr(ctx, argv[0], "height");
    JSValue dataVal = JS_GetPropertyStr(ctx, argv[0], "data");

    int wi = 0, h = 0;
    JS_ToInt32(ctx, &wi, wVal);
    JS_ToInt32(ctx, &h, hVal);

    size_t len = 0;
    uint8_t* buf = JS_GetArrayBuffer(ctx, &len, dataVal);
    if (!buf) {
        size_t offset, blen;
        JSValue abuf = JS_GetTypedArrayBuffer(ctx, dataVal, &offset, &blen, nullptr);
        buf = JS_GetArrayBuffer(ctx, &len, abuf);
        if (buf) buf += offset;
        JS_FreeValue(ctx, abuf);
    }

    double dx_d = 0, dy_d = 0;
    JS_ToFloat64(ctx, &dx_d, argv[1]);
    JS_ToFloat64(ctx, &dy_d, argv[2]);
    int dx = (int)dx_d, dy = (int)dy_d;

    if (buf && wi > 0 && h > 0) {
        sc->putImageData(buf, wi, h, dx, dy);
    }

    JS_FreeValue(ctx, wVal);
    JS_FreeValue(ctx, hVal);
    JS_FreeValue(ctx, dataVal);
    return JS_UNDEFINED;
}

static JSValue js_createImageData(JSContext* ctx, JSValueConst this_val,
                                  int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    double wd = 0, hd = 0;
    JS_ToFloat64(ctx, &wd, argv[0]);
    JS_ToFloat64(ctx, &hd, argv[1]);
    int w = (int)wd, h = (int)hd;
    if (w <= 0 || h <= 0) return JS_UNDEFINED;

    size_t sz = (size_t)w * h * 4;
    std::vector<uint8_t> zeros(sz, 0);
    JSValue abuf = JS_NewArrayBufferCopy(ctx, zeros.data(), sz);
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue u8cCtor = JS_GetPropertyStr(ctx, global, "Uint8ClampedArray");
    JSValue dataArr = JS_CallConstructor(ctx, u8cCtor, 1, &abuf);
    JS_FreeValue(ctx, u8cCtor);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, abuf);

    // Real ImageData instance (instanceof-true), not a duck-typed object.
    return ImageBitmapBindings::makeImageData(ctx, w, h, dataArr);
}

static JSValue js_setLineDash(JSContext* ctx, JSValueConst this_val,
                              int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<CW>(ctx, this_val);
    auto* sc = w ? w->scene : nullptr;
    if (!sc || argc < 1) return JS_UNDEFINED;
    std::vector<float> segs;
    if (JS_IsArray(argv[0])) {
        JSValue lenVal = JS_GetPropertyStr(ctx, argv[0], "length");
        uint32_t len = 0;
        JS_ToUint32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        segs.reserve(len);
        for (uint32_t i = 0; i < len; ++i) {
            JSValue v = JS_GetPropertyUint32(ctx, argv[0], i);
            double d = 0;
            JS_ToFloat64(ctx, &d, v);
            JS_FreeValue(ctx, v);
            segs.push_back(static_cast<float>(d));
        }
    }
    sc->setLineDash(segs);
    return JS_UNDEFINED;
}

static JSValue js_getLineDash(JSContext* ctx, JSValueConst this_val,
                              int, JSValueConst*) {
    auto* w = qjsbind::unwrap<CW>(ctx, this_val);
    auto* sc = w ? w->scene : nullptr;
    if (!sc) return JS_NewArray(ctx);
    const auto& d = sc->lineDash();
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < d.size(); ++i) {
        JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), JS_NewFloat64(ctx, d[i]));
    }
    return arr;
}

static JSValue js_measureText(JSContext* ctx, JSValueConst this_val,
                              int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<CW>(ctx, this_val);
    auto* sc = w ? w->scene : nullptr;
    if (!sc || argc < 1) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, argv[0]);
    std::string str = s ? s : "";
    if (s) JS_FreeCString(ctx, s);
    auto metrics = sc->measureText(str);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "width", JS_NewFloat64(ctx, metrics.width));
    JS_SetPropertyStr(ctx, obj, "actualBoundingBoxAscent", JS_NewFloat64(ctx, metrics.ascent));
    JS_SetPropertyStr(ctx, obj, "actualBoundingBoxDescent", JS_NewFloat64(ctx, metrics.descent));
    return obj;
}

static JSValue js_arc(JSContext* ctx, JSValueConst this_val,
                      int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<CW>(ctx, this_val);
    auto* sc = w ? w->scene : nullptr;
    if (!sc || argc < 5) return JS_UNDEFINED;
    auto F = [&](int i) { double v = 0; JS_ToFloat64(ctx, &v, argv[i]); return (float)v; };
    bool acw = (argc >= 6) ? JS_ToBool(ctx, argv[5]) != 0 : false;
    sc->arc(F(0), F(1), F(2), F(3), F(4), acw);
    return JS_UNDEFINED;
}

static JSValue js_ellipse(JSContext* ctx, JSValueConst this_val,
                          int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<CW>(ctx, this_val);
    auto* sc = w ? w->scene : nullptr;
    if (!sc || argc < 7) return JS_UNDEFINED;
    auto F = [&](int i) { double v = 0; JS_ToFloat64(ctx, &v, argv[i]); return (float)v; };
    bool acw = (argc >= 8) ? JS_ToBool(ctx, argv[7]) != 0 : false;
    sc->ellipse(F(0), F(1), F(2), F(3), F(4), F(5), F(6), acw);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Raw getter/setter functions for string-enum properties
// These need raw JSValue handling (color parsing, enum string lookup, etc.)
// They use the JSCFunction signature for use with JS_NewCFunction.
// ---------------------------------------------------------------------------

static JSValue raw_get_fillStyle(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* w = qjsbind::unwrap<CW>(ctx, this_val);
    auto* sc = w ? w->scene : nullptr;
    if (!sc) return JS_UNDEFINED;
    uint8_t r, g, b, a;
    sc->getFillColor(r, g, b, a);
    return JS_NewString(ctx, colorToRGBA(r, g, b, a).c_str());
}

static JSValue raw_set_fillStyle(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<CW>(ctx, this_val);
    auto* sc = w ? w->scene : nullptr;
    if (!sc || argc < 1) return JS_UNDEFINED;
    // Gradient / pattern? Snapshot the shader and install it on the paint.
    if (JS_IsObject(argv[0])) {
        if (auto* grad = qjsbind::unwrap<CanvasGradient>(ctx, argv[0])) {
            sc->setFillShader(grad->buildShader());
            return JS_UNDEFINED;
        }
    }
    const char* s = JS_ToCString(ctx, argv[0]);
    std::string str = s ? s : "";
    if (s) JS_FreeCString(ctx, s);
    uint8_t r, g, b, a;
    if (canvas::parseCSSColor(str, r, g, b, a))
        sc->setFillColor(r, g, b, a);
    return JS_UNDEFINED;
}

static JSValue raw_get_strokeStyle(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* w = qjsbind::unwrap<CW>(ctx, this_val);
    auto* sc = w ? w->scene : nullptr;
    if (!sc) return JS_UNDEFINED;
    uint8_t r, g, b, a;
    sc->getStrokeColor(r, g, b, a);
    return JS_NewString(ctx, colorToRGBA(r, g, b, a).c_str());
}

static JSValue raw_set_strokeStyle(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<CW>(ctx, this_val);
    auto* sc = w ? w->scene : nullptr;
    if (!sc || argc < 1) return JS_UNDEFINED;
    if (JS_IsObject(argv[0])) {
        if (auto* grad = qjsbind::unwrap<CanvasGradient>(ctx, argv[0])) {
            sc->setStrokeShader(grad->buildShader());
            return JS_UNDEFINED;
        }
    }
    const char* s = JS_ToCString(ctx, argv[0]);
    std::string str = s ? s : "";
    if (s) JS_FreeCString(ctx, s);
    uint8_t r, g, b, a;
    if (canvas::parseCSSColor(str, r, g, b, a))
        sc->setStrokeColor(r, g, b, a);
    return JS_UNDEFINED;
}

static JSValue raw_get_lineCap(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* w = qjsbind::unwrap<CW>(ctx, this_val);
    auto* sc = w ? w->scene : nullptr;
    if (!sc) return JS_UNDEFINED;
    static const char* names[] = {"butt", "round", "square"};
    int v = sc->lineCap();
    return JS_NewString(ctx, (v >= 0 && v <= 2) ? names[v] : "butt");
}

static JSValue raw_set_lineCap(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<CW>(ctx, this_val);
    auto* sc = w ? w->scene : nullptr;
    if (!sc || argc < 1) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, argv[0]);
    std::string str = s ? s : "";
    if (s) JS_FreeCString(ctx, s);
    if (str == "butt") sc->setLineCap(0);
    else if (str == "round") sc->setLineCap(1);
    else if (str == "square") sc->setLineCap(2);
    return JS_UNDEFINED;
}

static JSValue raw_get_lineJoin(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* w = qjsbind::unwrap<CW>(ctx, this_val);
    auto* sc = w ? w->scene : nullptr;
    if (!sc) return JS_UNDEFINED;
    static const char* names[] = {"miter", "round", "bevel"};
    int v = sc->lineJoin();
    return JS_NewString(ctx, (v >= 0 && v <= 2) ? names[v] : "miter");
}

static JSValue raw_set_lineJoin(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<CW>(ctx, this_val);
    auto* sc = w ? w->scene : nullptr;
    if (!sc || argc < 1) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, argv[0]);
    std::string str = s ? s : "";
    if (s) JS_FreeCString(ctx, s);
    if (str == "miter") sc->setLineJoin(0);
    else if (str == "round") sc->setLineJoin(1);
    else if (str == "bevel") sc->setLineJoin(2);
    return JS_UNDEFINED;
}

static JSValue raw_get_globalCompositeOp(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* w = qjsbind::unwrap<CW>(ctx, this_val);
    auto* sc = w ? w->scene : nullptr;
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

static JSValue raw_set_globalCompositeOp(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<CW>(ctx, this_val);
    auto* sc = w ? w->scene : nullptr;
    if (!sc || argc < 1) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, argv[0]);
    std::string str = s ? s : "";
    if (s) JS_FreeCString(ctx, s);
    static const char* names[] = {
        "source-over", "source-in", "source-out", "source-atop",
        "destination-over", "destination-in", "destination-out", "destination-atop",
        "lighten", "darken", "xor", "lighter",
        "multiply", "screen", "overlay",
        "color-dodge", "color-burn", "hard-light", "soft-light",
        "difference", "exclusion"
    };
    for (int i = 0; i < 21; i++) {
        if (str == names[i]) { sc->setGlobalCompositeOperation(i); return JS_UNDEFINED; }
    }
    return JS_UNDEFINED;
}

static JSValue raw_get_textAlign(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* w = qjsbind::unwrap<CW>(ctx, this_val);
    auto* sc = w ? w->scene : nullptr;
    if (!sc) return JS_UNDEFINED;
    static const char* names[] = {"start", "center", "right", "end"};
    int v = sc->textAlign();
    return JS_NewString(ctx, (v >= 0 && v <= 3) ? names[v] : "start");
}

static JSValue raw_set_textAlign(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<CW>(ctx, this_val);
    auto* sc = w ? w->scene : nullptr;
    if (!sc || argc < 1) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, argv[0]);
    std::string str = s ? s : "";
    if (s) JS_FreeCString(ctx, s);
    if (str == "left" || str == "start") sc->setTextAlign(0);
    else if (str == "center") sc->setTextAlign(1);
    else if (str == "right") sc->setTextAlign(2);
    else if (str == "end") sc->setTextAlign(3);
    return JS_UNDEFINED;
}

static JSValue raw_get_textBaseline(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* w = qjsbind::unwrap<CW>(ctx, this_val);
    auto* sc = w ? w->scene : nullptr;
    if (!sc) return JS_UNDEFINED;
    static const char* names[] = {"alphabetic", "top", "middle", "bottom", "hanging", "ideographic"};
    int v = sc->textBaseline();
    return JS_NewString(ctx, (v >= 0 && v <= 5) ? names[v] : "alphabetic");
}

static JSValue raw_set_textBaseline(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<CW>(ctx, this_val);
    auto* sc = w ? w->scene : nullptr;
    if (!sc || argc < 1) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, argv[0]);
    std::string str = s ? s : "";
    if (s) JS_FreeCString(ctx, s);
    if (str == "alphabetic") sc->setTextBaseline(0);
    else if (str == "top") sc->setTextBaseline(1);
    else if (str == "middle") sc->setTextBaseline(2);
    else if (str == "bottom") sc->setTextBaseline(3);
    else if (str == "hanging") sc->setTextBaseline(4);
    else if (str == "ideographic") sc->setTextBaseline(5);
    return JS_UNDEFINED;
}

static JSValue raw_get_shadowColor(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* w = qjsbind::unwrap<CW>(ctx, this_val);
    auto* sc = w ? w->scene : nullptr;
    if (!sc) return JS_UNDEFINED;
    uint8_t r, g, b, a;
    sc->getShadowColor(r, g, b, a);
    return JS_NewString(ctx, colorToRGBA(r, g, b, a).c_str());
}

static JSValue raw_set_shadowColor(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<CW>(ctx, this_val);
    auto* sc = w ? w->scene : nullptr;
    if (!sc || argc < 1) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, argv[0]);
    std::string str = s ? s : "";
    if (s) JS_FreeCString(ctx, s);
    uint8_t r, g, b, a;
    if (canvas::parseCSSColor(str, r, g, b, a))
        sc->setShadowColor(r, g, b, a);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Helper to define a raw getter/setter property on a prototype
// ---------------------------------------------------------------------------

static void defineRawProp(JSContext* ctx, JSValue proto, const char* name,
                          JSCFunction* getter, JSCFunction* setter) {
    JSAtom atom = JS_NewAtom(ctx, name);
    JSValue getterFn = JS_NewCFunction(ctx, getter, name, 0);
    JSValue setterFn = setter ? JS_NewCFunction(ctx, setter, name, 1) : JS_UNDEFINED;
    JS_DefinePropertyGetSet(ctx, proto, atom, getterFn, setterFn,
                            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, atom);
}

// =========================================================================
// install / cleanup / wrapContext2D
// =========================================================================

void CanvasBindings::install(JSContext* ctx) {
    // Register CanvasGradient first — returned by context createLinear/Radial.
    {
        qjsbind::Class<CanvasGradient>(ctx, "CanvasGradient")
            .method("addColorStop", [](CanvasGradient* g, double offset, std::string color) {
                if (!g) return;
                if (!std::isfinite(offset)) return;
                offset = offset < 0.0 ? 0.0 : (offset > 1.0 ? 1.0 : offset);
                uint8_t r, gc, b, a;
                if (!canvas::parseCSSColor(color, r, gc, b, a)) return;
                uint32_t argb = (static_cast<uint32_t>(a) << 24) |
                                (static_cast<uint32_t>(r)  << 16) |
                                (static_cast<uint32_t>(gc) << 8)  |
                                 static_cast<uint32_t>(b);
                g->stops.emplace_back(static_cast<float>(offset), argb);
            });
    }

    // Register the class with qjsbind — destructor finalizes at end of block
    {
        qjsbind::Class<CW>(ctx, "CanvasRenderingContext2D")
            // --- Simple numeric/bool properties ---
            .prop("lineWidth",
                [](CW* w) -> double { return w->scene ? w->scene->lineWidth() : 0; },
                [](CW* w, double v) { if (w->scene) w->scene->setLineWidth((float)v); })
            .prop("globalAlpha",
                [](CW* w) -> double { return w->scene ? w->scene->globalAlpha() : 0; },
                [](CW* w, double v) { if (w->scene) w->scene->setGlobalAlpha((float)v); })
            .prop("font",
                [](CW* w, JSContext* c) -> JSValue {
                    return w->scene ? JS_NewString(c, w->scene->fontString().c_str()) : JS_UNDEFINED;
                },
                [](CW* w, std::string v) { if (w->scene) w->scene->setFont(v); })
            .prop("miterLimit",
                [](CW* w) -> double { return w->scene ? w->scene->miterLimit() : 0; },
                [](CW* w, double v) { if (w->scene) w->scene->setMiterLimit((float)v); })
            .prop("shadowBlur",
                [](CW* w) -> double { return w->scene ? w->scene->shadowBlur() : 0; },
                [](CW* w, double v) { if (w->scene) w->scene->setShadowBlur((float)v); })
            .prop("shadowOffsetX",
                [](CW* w) -> double { return w->scene ? w->scene->shadowOffsetX() : 0; },
                [](CW* w, double v) { if (w->scene) w->scene->setShadowOffsetX((float)v); })
            .prop("shadowOffsetY",
                [](CW* w) -> double { return w->scene ? w->scene->shadowOffsetY() : 0; },
                [](CW* w, double v) { if (w->scene) w->scene->setShadowOffsetY((float)v); })
            .prop("imageSmoothingEnabled",
                [](CW* w) -> bool { return w->scene ? w->scene->imageSmoothingEnabled() : false; },
                [](CW* w, bool v) { if (w->scene) w->scene->setImageSmoothingEnabled(v); })

            // --- Read-only properties ---
            .get("canvasWidth",
                [](CW* w) -> int { return w->scene ? w->scene->width() : 0; })
            .get("canvasHeight",
                [](CW* w) -> int { return w->scene ? w->scene->height() : 0; })

            // --- Simple void methods ---
            .method("save", [](CW* w) { if (w->scene) w->scene->save(); })
            .method("restore", [](CW* w) { if (w->scene) w->scene->restore(); })
            .method("reset", [](CW* w) { if (w->scene) w->scene->reset(); })
            .method("beginPath", [](CW* w) { if (w->scene) w->scene->beginPath(); })
            .method("closePath", [](CW* w) { if (w->scene) w->scene->closePath(); })
            .method("stroke", [](CW* w) { if (w->scene) w->scene->stroke(); })
            .method("fill", [](CW* w) { if (w->scene) w->scene->fill(); })
            .method("clip", [](CW* w) { if (w->scene) w->scene->clip(); })
            .method("resetTransform", [](CW* w) { if (w->scene) w->scene->resetTransform(); })

            // --- Methods with typed args ---
            .method("fillRect", [](CW* w, double x, double y, double wi, double h) {
                if (w->scene) w->scene->fillRect((float)x, (float)y, (float)wi, (float)h);
            })
            .method("strokeRect", [](CW* w, double x, double y, double wi, double h) {
                if (w->scene) w->scene->strokeRect((float)x, (float)y, (float)wi, (float)h);
            })
            .method("clearRect", [](CW* w, double x, double y, double wi, double h) {
                if (w->scene) w->scene->clearRect((float)x, (float)y, (float)wi, (float)h);
            })
            .method("fillText", [](CW* w, std::string text, double x, double y) {
                if (w->scene) w->scene->fillText(text, (float)x, (float)y);
            })
            .method("strokeText", [](CW* w, std::string text, double x, double y) {
                if (w->scene) w->scene->strokeText(text, (float)x, (float)y);
            })
            .method("translate", [](CW* w, double x, double y) {
                if (w->scene) w->scene->translate((float)x, (float)y);
            })
            .method("rotate", [](CW* w, double angle) {
                if (w->scene) w->scene->rotate((float)angle);
            })
            .method("scale", [](CW* w, double x, double y) {
                if (w->scene) w->scene->scale((float)x, (float)y);
            })
            .method("setTransform", [](CW* w, double a, double b, double c, double d, double e, double f) {
                if (w->scene) w->scene->setTransform((float)a, (float)b, (float)c, (float)d, (float)e, (float)f);
            })
            .method("transform", [](CW* w, double a, double b, double c, double d, double e, double f) {
                if (w->scene) w->scene->transform((float)a, (float)b, (float)c, (float)d, (float)e, (float)f);
            })
            .method("moveTo", [](CW* w, double x, double y) {
                if (w->scene) w->scene->moveTo((float)x, (float)y);
            })
            .method("lineTo", [](CW* w, double x, double y) {
                if (w->scene) w->scene->lineTo((float)x, (float)y);
            })
            .method("arcTo", [](CW* w, double x1, double y1, double x2, double y2, double r) {
                if (w->scene) w->scene->arcTo((float)x1, (float)y1, (float)x2, (float)y2, (float)r);
            })
            .method("bezierCurveTo", [](CW* w, double cp1x, double cp1y, double cp2x, double cp2y, double x, double y) {
                if (w->scene) w->scene->bezierCurveTo((float)cp1x, (float)cp1y, (float)cp2x, (float)cp2y, (float)x, (float)y);
            })
            .method("quadraticCurveTo", [](CW* w, double cpx, double cpy, double x, double y) {
                if (w->scene) w->scene->quadraticCurveTo((float)cpx, (float)cpy, (float)x, (float)y);
            })
            .method("rect", [](CW* w, double x, double y, double wi, double h) {
                if (w->scene) w->scene->rect((float)x, (float)y, (float)wi, (float)h);
            })
            .method("isPointInPath", [](CW* w, double x, double y) -> bool {
                return w->scene ? w->scene->isPointInPath((float)x, (float)y) : false;
            })

            // --- Complex methods needing raw argc/argv ---
            .method_raw("arc", js_arc, 6)
            .method_raw("ellipse", js_ellipse, 8)
            .method_raw("polyline", js_polyline, 1)
            .method_raw("drawImage", js_drawImage, 9)
            .method_raw("getImageData", js_getImageData, 4)
            .method_raw("putImageData", js_putImageData, 3)
            .method_raw("createImageData", js_createImageData, 2)
            .method_raw("createLinearGradient", js_createLinearGradient, 4)
            .method_raw("createRadialGradient", js_createRadialGradient, 6)
            .method_raw("measureText", js_measureText, 1)
            .method_raw("setLineDash", js_setLineDash, 1)
            .method_raw("getLineDash", js_getLineDash, 0)
            .prop("lineDashOffset",
                [](CW* w) -> double { return w->scene ? w->scene->lineDashOffset() : 0; },
                [](CW* w, double v) { if (w->scene) w->scene->setLineDashOffset((float)v); });
    }
    // Class destructor has run — proto is now registered. Add raw string-enum properties.
    JSValue proto = JS_GetClassProto(ctx, qjsbind::class_id<CW>());

    defineRawProp(ctx, proto, "fillStyle",               raw_get_fillStyle,       raw_set_fillStyle);
    defineRawProp(ctx, proto, "strokeStyle",             raw_get_strokeStyle,     raw_set_strokeStyle);
    defineRawProp(ctx, proto, "lineCap",                 raw_get_lineCap,         raw_set_lineCap);
    defineRawProp(ctx, proto, "lineJoin",                raw_get_lineJoin,        raw_set_lineJoin);
    defineRawProp(ctx, proto, "globalCompositeOperation",raw_get_globalCompositeOp, raw_set_globalCompositeOp);
    defineRawProp(ctx, proto, "textAlign",               raw_get_textAlign,       raw_set_textAlign);
    defineRawProp(ctx, proto, "textBaseline",            raw_get_textBaseline,    raw_set_textBaseline);
    defineRawProp(ctx, proto, "shadowColor",             raw_get_shadowColor,     raw_set_shadowColor);

    JS_FreeValue(ctx, proto);
}

void CanvasBindings::cleanup(JSContext*) {
    // No persistent JSValue/atom storage in this binding — qjsbind finalizers
    // handle Ctx2DWrapper lifecycle.
}

JSValue CanvasBindings::wrapContext2D(JSContext* ctx, canvas::CanvasScene* scene) {
    auto* w = new Ctx2DWrapper{scene};
    return qjsbind::wrap<CW>(ctx, w);
}

} // namespace bro::js
