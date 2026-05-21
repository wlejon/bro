#include "js/imagebitmap_bindings.h"
#include "js/image_bindings.h"

#include <qjsbind/qjsbind.h>

#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>

#include <cstring>
#include <string>
#include <utility>

namespace bro::js {

// ---------------------------------------------------------------------------
// Backing store. An ImageBitmap is just an immutable raster SkImage; once
// closed (or transferred away) `image` is null and the bitmap draws as a
// no-op. The qjsbind default finalizer deletes this struct, dropping the ref.
// ---------------------------------------------------------------------------
struct ImageBitmapData {
    sk_sp<SkImage> image;
    int width = 0;
    int height = 0;
};

using IB = ImageBitmapData;

// ---------------------------------------------------------------------------
// Build a raster RGBA SkImage from a contiguous pixel buffer, honouring an
// optional crop rect. The crop is clamped to the source bounds (no transparent
// padding for out-of-bounds rects — a deliberate v1 simplification).
// ---------------------------------------------------------------------------
static sk_sp<SkImage> buildBitmap(const uint8_t* rgba, int srcW, int srcH,
                                  bool crop, int sx, int sy, int sw, int sh,
                                  std::string& err) {
    if (!rgba || srcW <= 0 || srcH <= 0) { err = "empty source"; return nullptr; }
    if (!crop) { sx = 0; sy = 0; sw = srcW; sh = srcH; }
    if (sx < 0) { sw += sx; sx = 0; }
    if (sy < 0) { sh += sy; sy = 0; }
    if (sx >= srcW || sy >= srcH || sw <= 0 || sh <= 0) {
        err = "crop rect outside source"; return nullptr;
    }
    if (sx + sw > srcW) sw = srcW - sx;
    if (sy + sh > srcH) sh = srcH - sy;

    SkImageInfo info = SkImageInfo::Make(sw, sh, kRGBA_8888_SkColorType,
                                         kUnpremul_SkAlphaType);
    sk_sp<SkData> data;
    if (sx == 0 && sy == 0 && sw == srcW && sh == srcH) {
        data = SkData::MakeWithCopy(rgba, static_cast<size_t>(srcW) * srcH * 4);
    } else {
        data = SkData::MakeUninitialized(static_cast<size_t>(sw) * sh * 4);
        auto* dst = static_cast<uint8_t*>(data->writable_data());
        for (int row = 0; row < sh; ++row) {
            std::memcpy(dst + static_cast<size_t>(row) * sw * 4,
                        rgba + (static_cast<size_t>(sy + row) * srcW + sx) * 4,
                        static_cast<size_t>(sw) * 4);
        }
    }
    return SkImages::RasterFromData(info, data, static_cast<size_t>(sw) * 4);
}

// ---------------------------------------------------------------------------
// Resolve a createImageBitmap source argument to a raster SkImage. Sources:
// another ImageBitmap, an Image / HTMLCanvasElement (via the shared pixel
// extractor), or an ImageData-shaped plain object { width, height, data }.
// ---------------------------------------------------------------------------
static sk_sp<SkImage> imageFromSource(JSContext* ctx, JSValueConst src,
                                      bool crop, int sx, int sy, int sw, int sh,
                                      std::string& err) {
    // 1) ImageBitmap — immutable, so an uncropped copy can share the SkImage.
    if (sk_sp<SkImage> bm = ImageBitmapBindings::getImage(src)) {
        if (!crop) return bm;
        int bw = bm->width(), bh = bm->height();
        if (sx < 0) { sw += sx; sx = 0; }
        if (sy < 0) { sh += sy; sy = 0; }
        if (sx >= bw || sy >= bh || sw <= 0 || sh <= 0) {
            err = "crop rect outside source"; return nullptr;
        }
        if (sx + sw > bw) sw = bw - sx;
        if (sy + sh > bh) sh = bh - sy;
        SkImageInfo info = SkImageInfo::Make(sw, sh, kRGBA_8888_SkColorType,
                                             kUnpremul_SkAlphaType);
        sk_sp<SkData> data = SkData::MakeUninitialized(
            static_cast<size_t>(sw) * sh * 4);
        if (!bm->readPixels(nullptr, info, data->writable_data(),
                            static_cast<size_t>(sw) * 4, sx, sy)) {
            err = "ImageBitmap readPixels failed"; return nullptr;
        }
        return SkImages::RasterFromData(info, data, static_cast<size_t>(sw) * 4);
    }

    // 2) Image or HTMLCanvasElement — reuse the existing pixel extractor.
    {
        ImagePixels pix;
        if (ImageBindings::getImagePixels(src, pix)) {
            return buildBitmap(pix.data, pix.width, pix.height,
                               crop, sx, sy, sw, sh, err);
        }
    }

    // 3) ImageData-shaped plain object { width, height, data:TypedArray }.
    if (JS_IsObject(src)) {
        JSValue wv = JS_GetPropertyStr(ctx, src, "width");
        JSValue hv = JS_GetPropertyStr(ctx, src, "height");
        JSValue dv = JS_GetPropertyStr(ctx, src, "data");
        int w = 0, h = 0;
        JS_ToInt32(ctx, &w, wv);
        JS_ToInt32(ctx, &h, hv);

        uint8_t* buf = nullptr;
        size_t len = 0;
        size_t off = 0, blen = 0;
        JSValue ab = JS_GetTypedArrayBuffer(ctx, dv, &off, &blen, nullptr);
        if (JS_IsException(ab)) {
            JS_FreeValue(ctx, JS_GetException(ctx));  // clear: dv not a TypedArray
        } else {
            buf = JS_GetArrayBuffer(ctx, &len, ab);
            if (buf) { buf += off; len = blen; }
        }
        JS_FreeValue(ctx, ab);

        sk_sp<SkImage> result;
        if (buf && w > 0 && h > 0 &&
            len >= static_cast<size_t>(w) * h * 4) {
            result = buildBitmap(buf, w, h, crop, sx, sy, sw, sh, err);
        } else {
            err = "createImageBitmap: unsupported or malformed source";
        }
        JS_FreeValue(ctx, wv);
        JS_FreeValue(ctx, hv);
        JS_FreeValue(ctx, dv);
        return result;
    }

    err = "createImageBitmap: unsupported source type";
    return nullptr;
}

// ---------------------------------------------------------------------------
// createImageBitmap(source)  /  createImageBitmap(source, sx, sy, sw, sh)
// Returns a Promise<ImageBitmap>. The RGBA→SkImage work is synchronous; the
// promise is resolved (or rejected) immediately so callers stay portable with
// the web standard.
// ---------------------------------------------------------------------------
static JSValue js_createImageBitmap(JSContext* ctx, JSValueConst /*this_val*/,
                                    int argc, JSValueConst* argv) {
    std::string err;
    sk_sp<SkImage> img;

    if (argc < 1) {
        err = "createImageBitmap requires a source argument";
    } else {
        bool crop = false;
        int sx = 0, sy = 0, sw = 0, sh = 0;
        if (argc >= 5) {
            crop = true;
            JS_ToInt32(ctx, &sx, argv[1]);
            JS_ToInt32(ctx, &sy, argv[2]);
            JS_ToInt32(ctx, &sw, argv[3]);
            JS_ToInt32(ctx, &sh, argv[4]);
        }
        img = imageFromSource(ctx, argv[0], crop, sx, sy, sw, sh, err);
    }

    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise)) return promise;

    if (img) {
        JSValue bmp = ImageBitmapBindings::wrap(ctx, std::move(img));
        JSValue r = JS_Call(ctx, resolving[0], JS_UNDEFINED, 1, &bmp);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, bmp);
    } else {
        JS_ThrowTypeError(ctx, "%s",
                          err.empty() ? "createImageBitmap failed" : err.c_str());
        JSValue e = JS_GetException(ctx);
        JSValue r = JS_Call(ctx, resolving[1], JS_UNDEFINED, 1, &e);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, resolving[0]);
    JS_FreeValue(ctx, resolving[1]);
    return promise;
}

// ---------------------------------------------------------------------------
// ImageBitmap.prototype.close() — release the backing image eagerly.
// ---------------------------------------------------------------------------
static JSValue js_imagebitmap_close(JSContext* ctx, JSValueConst this_val,
                                    int /*argc*/, JSValueConst* /*argv*/) {
    if (auto* d = qjsbind::unwrap<IB>(ctx, this_val)) {
        d->image = nullptr;
        d->width = 0;
        d->height = 0;
    }
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Install + public API
// ---------------------------------------------------------------------------
void ImageBitmapBindings::install(JSContext* ctx) {
    qjsbind::Class<IB>(ctx, "ImageBitmap", qjsbind::NoGlobal)
        .get("width",  [](IB* d) -> int { return d->width; })
        .get("height", [](IB* d) -> int { return d->height; })
        .method_raw("close", js_imagebitmap_close, 0);

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "createImageBitmap",
        JS_NewCFunction(ctx, js_createImageBitmap, "createImageBitmap", 1));
    JS_FreeValue(ctx, global);
}

JSClassID ImageBitmapBindings::classId() {
    return qjsbind::class_id<IB>();
}

JSValue ImageBitmapBindings::wrap(JSContext* ctx, sk_sp<SkImage> img) {
    auto* d = new IB();
    d->width  = img ? img->width()  : 0;
    d->height = img ? img->height() : 0;
    d->image  = std::move(img);
    return qjsbind::wrap<IB>(ctx, d);
}

sk_sp<SkImage> ImageBitmapBindings::getImage(JSValueConst val) {
    auto* d = qjsbind::unwrap<IB>(nullptr, val);
    return d ? d->image : nullptr;
}

sk_sp<SkImage> ImageBitmapBindings::takeImage(JSValueConst val) {
    auto* d = qjsbind::unwrap<IB>(nullptr, val);
    if (!d || !d->image) return nullptr;
    sk_sp<SkImage> img = std::move(d->image);
    d->image = nullptr;
    d->width = 0;
    d->height = 0;
    return img;
}

} // namespace bro::js
