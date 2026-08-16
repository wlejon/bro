#include "js/imagebitmap_bindings.h"
#include "js/image_bindings.h"
#include "svg/svg_renderer.h"

#include <qjsbind/qjsbind.h>

#include <api/api.h>  // brokit::api::blobBytes
#include "broimage/decode.h"
#if BRO_WITH_WEBP
#include "render/webp_image.h"
#endif

#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

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

    // 3) Blob / File — encoded bytes, decoded here.
    //
    // This is the spec's canonical source and the one every fetch-based image
    // path produces: `fetch(url).then(r => r.blob()).then(createImageBitmap)`
    // is how three.js's ImageBitmapLoader loads a texture, which makes it how
    // GLTFLoader loads every texture in a model. Without it, a Blob fell
    // through to the ImageData branch and came back "unsupported source".
    {
        const uint8_t* bytes = nullptr;
        size_t len = 0;
        if (brokit::api::blobBytes(ctx, src, &bytes, &len) && bytes && len > 0) {
            broimage::Image decoded;
            std::string decodeErr;
            bool ok = broimage::decode_memory(bytes, len, decoded, &decodeErr);
#if BRO_WITH_WEBP
            // stb has no WebP; same fallback the other decode paths take.
            if (!ok) {
                int w = 0, h = 0;
                std::vector<uint8_t> rgba;
                if (render::decodeWebP(bytes, len, w, h, rgba)) {
                    decoded.width = w;
                    decoded.height = h;
                    decoded.channels = 4;
                    decoded.pixels = std::move(rgba);
                    ok = true;
                }
            }
#endif
            if (!ok && svg::looksLikeSvg(reinterpret_cast<const char*>(bytes), len)) {
                int w = 0, h = 0;
                std::vector<uint8_t> rgba;
                if (svg::rasterizeSvgMarkup(reinterpret_cast<const char*>(bytes),
                                            len, 0, 0, w, h, rgba)) {
                    decoded.width = w;
                    decoded.height = h;
                    decoded.channels = 4;
                    decoded.pixels = std::move(rgba);
                    ok = true;
                }
            }
            if (!ok) {
                err = "createImageBitmap: could not decode the blob (" +
                      decodeErr + ")";
                return nullptr;
            }
            return buildBitmap(decoded.pixels.data(), decoded.width, decoded.height,
                               crop, sx, sy, sw, sh, err);
        }
    }

    // 4) ImageData-shaped plain object { width, height, data:TypedArray }.
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
// new ImageData(width, height)  |  new ImageData(Uint8ClampedArray data, width [, height])
//
// The standard Web constructor. bro's canvas (putImageData) and createImageBitmap
// consume the duck-typed { width, height, data } shape, but the global ImageData
// constructor itself was missing — so `new ImageData(pixels, w, h)`, the
// idiomatic way to wrap raw RGBA (e.g. bro.steam.getAvatar pixels), threw
// ReferenceError. This produces that same shape; with a typed array it shares
// the buffer (web semantics), and with dimensions only it allocates zeroed RGBA.
// ---------------------------------------------------------------------------
static JSValue js_imageData_ctor(JSContext* ctx, JSValueConst /*new_target*/,
                                 int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "ImageData requires arguments");

    // Overload select: is arg0 a typed array (data-first) or a number (w,h)?
    size_t off = 0, blen = 0;
    JSValue ab0 = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &blen, nullptr);
    const bool dataFirst = !JS_IsException(ab0);
    if (!dataFirst) JS_FreeValue(ctx, JS_GetException(ctx)); // clear: arg0 not a TypedArray
    else            JS_FreeValue(ctx, ab0);

    JSValue dataArr = JS_UNDEFINED;
    int width = 0, height = 0;

    if (dataFirst) {
        if (argc < 2) return JS_ThrowTypeError(ctx, "ImageData(data, width[, height]) requires a width");
        int32_t w = 0; JS_ToInt32(ctx, &w, argv[1]);
        if (w <= 0) return JS_ThrowRangeError(ctx, "ImageData width must be positive");
        if (blen % 4 != 0) return JS_ThrowRangeError(ctx, "ImageData data length must be a multiple of 4");
        size_t pixels = blen / 4;
        if (pixels % static_cast<size_t>(w) != 0)
            return JS_ThrowRangeError(ctx, "ImageData data length is not a multiple of 4*width");
        int h = static_cast<int>(pixels / static_cast<size_t>(w));
        if (argc >= 3) {
            int32_t hh = 0; JS_ToInt32(ctx, &hh, argv[2]);
            if (hh > 0) {
                if (static_cast<size_t>(w) * hh * 4 != blen)
                    return JS_ThrowRangeError(ctx, "ImageData data length does not match width*height*4");
                h = hh;
            }
        }
        width = w; height = h;
        dataArr = JS_DupValue(ctx, argv[0]); // share the same Uint8ClampedArray
    } else {
        int32_t w = 0, h = 0;
        JS_ToInt32(ctx, &w, argv[0]);
        if (argc >= 2) JS_ToInt32(ctx, &h, argv[1]);
        if (w <= 0 || h <= 0)
            return JS_ThrowRangeError(ctx, "ImageData(width, height) requires positive dimensions");
        width = w; height = h;
        size_t sz = static_cast<size_t>(w) * h * 4;
        std::vector<uint8_t> zeros(sz, 0);
        JSValue zbuf = JS_NewArrayBufferCopy(ctx, zeros.data(), sz);
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue u8cCtor = JS_GetPropertyStr(ctx, global, "Uint8ClampedArray");
        dataArr = JS_CallConstructor(ctx, u8cCtor, 1, &zbuf);
        JS_FreeValue(ctx, u8cCtor);
        JS_FreeValue(ctx, global);
        JS_FreeValue(ctx, zbuf);
    }

    return ImageBitmapBindings::makeImageData(ctx, width, height, dataArr);
}

JSValue ImageBitmapBindings::makeImageData(JSContext* ctx, int width, int height,
                                           JSValue dataArr) {
    // Resolve the shared prototype off the global constructor so every
    // producer yields true `instanceof ImageData` objects. Falls back to a
    // plain object if the constructor isn't installed (defensive — install()
    // runs before any producer in both main and worker contexts).
    JSValue obj = JS_UNDEFINED;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, "ImageData");
    if (JS_IsObject(ctor)) {
        JSValue proto = JS_GetPropertyStr(ctx, ctor, "prototype");
        if (JS_IsObject(proto)) obj = JS_NewObjectProto(ctx, proto);
        JS_FreeValue(ctx, proto);
    }
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, global);
    if (!JS_IsObject(obj)) obj = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, obj, "width",  JS_NewInt32(ctx, width));
    JS_SetPropertyStr(ctx, obj, "height", JS_NewInt32(ctx, height));
    JS_SetPropertyStr(ctx, obj, "data",   dataArr);
    return obj;
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

    // The interface object. ImageBitmap has no usable constructor — only
    // createImageBitmap makes one — but it is still a global, and code branches
    // on `x instanceof ImageBitmap` to tell a decoded bitmap from an <img> or a
    // raw {data,width,height}. With the name absent, three.js's texture
    // serializer took every ImageBitmap for an unknown source and refused to
    // save it, so a scene that imported fine came back textureless.
    {
        JSValue proto = JS_GetClassProto(ctx, qjsbind::class_id<IB>());
        JSValue ibCtor = JS_NewCFunction2(ctx,
            [](JSContext* c, JSValueConst, int, JSValueConst*) -> JSValue {
                return JS_ThrowTypeError(c, "Illegal constructor: use createImageBitmap()");
            }, "ImageBitmap", 0, JS_CFUNC_constructor, 0);
        JS_SetConstructor(ctx, ibCtor, proto);
        JS_FreeValue(ctx, proto);
        JS_SetPropertyStr(ctx, global, "ImageBitmap", ibCtor);
    }
    // ImageData needs a real prototype object paired with the constructor
    // (JS_SetConstructor wires ctor.prototype ↔ proto.constructor). A bare
    // C-function constructor has no "prototype" property at all, which makes
    // `x instanceof ImageData` throw TypeError instead of answering — that
    // took out any app using the standard CanvasImageSource type switch.
    // makeImageData() stamps this prototype on every produced instance.
    {
        JSValue idCtor = JS_NewCFunction2(ctx, js_imageData_ctor, "ImageData", 2,
                                          JS_CFUNC_constructor, 0);
        JSValue idProto = JS_NewObject(ctx);
        JS_SetConstructor(ctx, idCtor, idProto);
        JS_FreeValue(ctx, idProto);
        JS_SetPropertyStr(ctx, global, "ImageData", idCtor);
    }
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
