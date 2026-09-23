#include "bronze_host/host_globals_internal.h"
#include "bronze_host/gl_internal.h"
#include "canvas/canvas_scene.h"
#include "dom/element.h"
#include "engine/engine.h"
#include "webgl/webgl2_context.h"
#include "broimage/decode.h"
#include <api/api.h>
#include <include/core/SkData.h>
#include <include/core/SkImageInfo.h>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace bro::bronze_host {

namespace {

static thread_local HostClass g_imageBitmapClass;
static thread_local HostClass g_imageDataClass;

static Value makeTypeError(const std::string& msg) {
    // The message string is made first: made after the constructor was read,
    // its allocation would leave `ctor` naming a pre-collection address.
    ev::Persistent msgVal(ev::fromUtf8(msg));
    Value ctor = ev::globalValue("TypeError").value;
    if (ev::isFunction(ctor)) {
        Value arg = msgVal.get();
        auto res = ev::construct(ctor, std::span<const Value>(&arg, 1));
        if (!res.thrown) return res.value;
    }
    return msgVal.get();
}

static void hostImageBitmapDtor(void* p) {
    delete static_cast<HostImageBitmap*>(p);
}

static sk_sp<SkImage> buildBitmap(const uint8_t* rgba, int srcW, int srcH,
                                  bool crop, int sx, int sy, int sw, int sh,
                                  std::vector<uint8_t>& outPixels,
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

    outPixels.resize(static_cast<size_t>(sw) * sh * 4);
    if (sx == 0 && sy == 0 && sw == srcW && sh == srcH) {
        std::memcpy(outPixels.data(), rgba, outPixels.size());
    } else {
        for (int row = 0; row < sh; ++row) {
            std::memcpy(outPixels.data() + static_cast<size_t>(row) * sw * 4,
                        rgba + (static_cast<size_t>(sy + row) * srcW + sx) * 4,
                        static_cast<size_t>(sw) * 4);
        }
    }

    SkImageInfo info = SkImageInfo::Make(sw, sh, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    sk_sp<SkData> data = SkData::MakeWithCopy(outPixels.data(), outPixels.size());
    return SkImages::RasterFromData(info, data, static_cast<size_t>(sw) * 4);
}

} // namespace

const HostImageBitmap* hostImageBitmapOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    void* d = ev::handleData(v);
    if (!d) return nullptr;
    auto* bmp = static_cast<const HostImageBitmap*>(d);
    return bmp->tag == 0x4849424D ? bmp : nullptr;
}

HostImageBitmap* hostImageBitmapOfMut(Value v) {
    if (!ev::isObject(v)) return nullptr;
    void* d = ev::handleData(v);
    if (!d) return nullptr;
    auto* bmp = static_cast<HostImageBitmap*>(d);
    return bmp->tag == 0x4849424D ? bmp : nullptr;
}

Value wrapHostImageBitmap(sk_sp<SkImage> img) {
    auto* bmp = new HostImageBitmap();
    bmp->image = std::move(img);
    bmp->width = bmp->image ? bmp->image->width() : 0;
    bmp->height = bmp->image ? bmp->image->height() : 0;
    if (bmp->width > 0 && bmp->height > 0) {
        bmp->pixels.resize(static_cast<size_t>(bmp->width) * bmp->height * 4);
        SkImageInfo info = SkImageInfo::Make(bmp->width, bmp->height, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
        bmp->image->readPixels(nullptr, info, bmp->pixels.data(), static_cast<size_t>(bmp->width) * 4, 0, 0);
    }
    return g_imageBitmapClass.make(bmp, hostImageBitmapDtor);
}

Value wrapHostImageBitmap(const uint8_t* rgba, int w, int h) {
    auto* bmp = new HostImageBitmap();
    bmp->width = w > 0 ? w : 0;
    bmp->height = h > 0 ? h : 0;
    if (rgba && bmp->width > 0 && bmp->height > 0) {
        bmp->pixels.assign(rgba, rgba + static_cast<size_t>(w) * h * 4);
        SkImageInfo info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
        sk_sp<SkData> data = SkData::MakeWithCopy(bmp->pixels.data(), bmp->pixels.size());
        bmp->image = SkImages::RasterFromData(info, data, static_cast<size_t>(w) * 4);
    }
    return g_imageBitmapClass.make(bmp, hostImageBitmapDtor);
}

Value makeImageDataValue(int width, int height, Value dataArr) {
    // `dataArr` is current only at entry, and building the object allocates.
    ev::Persistent data(dataArr);
    ObjectBuilder b;
    b.set("width", ev::fromDouble(width));
    b.set("height", ev::fromDouble(height));
    b.set("data", data.get());
    ev::Persistent proto(g_imageDataClass.prototype());
    if (!ev::isUndefined(proto.get())) {
        ev::GlobalValue objCtor = ev::globalValue("Object");
        if (objCtor.found) {
            Value setProto = ev::getProperty(objCtor.value, "setPrototypeOf");
            if (ev::isFunction(setProto)) {
                const Value args[2] = { b.get(), proto.get() };
                ev::call(setProto, ev::undefined(), std::span<const Value>(args, 2));
            }
        }
    }
    return b.get();
}

Value makeImageDataValue(int width, int height, const uint8_t* pixels) {
    size_t sz = static_cast<size_t>(width) * height * 4;
    Value dataArr = ev::createTypedArray(bronze::embed::elements::Uint8Clamped, static_cast<uint32_t>(sz));
    if (pixels && sz > 0) {
        ev::fillTypedArray(dataArr, std::span<const uint8_t>(pixels, sz));
    }
    return makeImageDataValue(width, height, dataArr);
}

static Value js_imageData_ctor(Value, std::span<const Value> a) {
    if (a.empty()) return ev::throwTypeError("ImageData requires arguments");

    auto info0 = ev::typedArrayInfo(a[0]);
    bool dataFirst = info0.data != nullptr;
    int width = 0, height = 0;
    Value dataArr = ev::undefined();

    if (dataFirst) {
        if (a.size() < 2) return ev::throwTypeError("ImageData(data, width[, height]) requires a width");
        int32_t w = static_cast<int32_t>(ev::toDouble(a[1]));
        if (w <= 0) return ev::throwRangeError("ImageData width must be positive");
        if (info0.byteLength % 4 != 0) return ev::throwRangeError("ImageData data length must be a multiple of 4");
        size_t pixelCount = info0.byteLength / 4;
        if (pixelCount % static_cast<size_t>(w) != 0)
            return ev::throwRangeError("ImageData data length is not a multiple of 4*width");
        int h = static_cast<int>(pixelCount / static_cast<size_t>(w));
        if (a.size() >= 3) {
            int32_t hh = static_cast<int32_t>(ev::toDouble(a[2]));
            if (hh > 0) {
                if (static_cast<size_t>(w) * hh * 4 != info0.byteLength)
                    return ev::throwRangeError("ImageData data length does not match width*height*4");
                h = hh;
            }
        }
        width = w; height = h;
        dataArr = a[0];
    } else {
        int32_t w = static_cast<int32_t>(ev::toDouble(a[0]));
        int32_t h = a.size() >= 2 ? static_cast<int32_t>(ev::toDouble(a[1])) : 0;
        if (w <= 0 || h <= 0)
            return ev::throwRangeError("ImageData(width, height) requires positive dimensions");
        width = w; height = h;
        size_t sz = static_cast<size_t>(w) * h * 4;
        dataArr = ev::createTypedArray(bronze::embed::elements::Uint8Clamped, static_cast<uint32_t>(sz));
    }

    return makeImageDataValue(width, height, dataArr);
}

// createImageBitmap(<canvas>): a copy of the canvas's bitmap as displayed now,
// at the bitmap's own size. For a 2D or bitmaprenderer canvas that is the
// scene's surface size — which for a bitmaprenderer canvas is the size of the
// ImageBitmap last transferred in, not the width/height attributes; a WebGL
// canvas reads back its drawing buffer; a canvas with no context yet has a
// transparent-black bitmap of its attribute size (300x150 by default), and
// asking for it must not create a context.
static bool canvasBitmapPixels(dom::Element* el, std::vector<uint8_t>& out, int& w, int& h,
                               bool& invalidState, std::string& err) {
    // A zero-sized bitmap is the spec's InvalidStateError.
    auto empty = [&]() {
        invalidState = true;
        err = "createImageBitmap: the canvas has a zero width or height";
        return false;
    };
    if (auto* scene = static_cast<canvas::CanvasScene*>(el->canvasScene())) {
        w = scene->width();
        h = scene->height();
        if (w <= 0 || h <= 0) return empty();
        const uint8_t* px = scene->snapshotPixels(w, h);
        if (!px) { err = "Canvas snapshot failed"; return false; }
        out.assign(px, px + static_cast<size_t>(w) * h * 4);
        return true;
    }
    if (auto* gl = static_cast<webgl::WebGL2RenderingContext*>(el->webglContext())) {
        w = gl->canvasWidth();
        h = gl->canvasHeight();
        if (w <= 0 || h <= 0) return empty();
        if (!gl->readCanvasPixels(out) || out.size() < static_cast<size_t>(w) * h * 4) {
            err = "Canvas snapshot failed";
            return false;
        }
        return true;
    }
    if (el->sceneGraph()) { err = "createImageBitmap does not read a scene canvas"; return false; }
    auto attr = [el](const char* name, int fallback) {
        const std::string& v = el->getAttribute(name);
        return v.empty() ? fallback : std::atoi(v.c_str());
    };
    w = attr("width", 300);
    h = attr("height", 150);
    if (w <= 0 || h <= 0) return empty();
    out.assign(static_cast<size_t>(w) * h * 4, 0);
    return true;
}

static Value js_createImageBitmap(Value, std::span<const Value> a) {
    // The promise is rooted for the whole call: nearly everything below
    // allocates, the error objects included. The source is read through a[0],
    // a rooted argument slot, never through a copy.
    ev::Persistent promise(ev::createPromise());
    auto reject = [&promise](const std::string& msg) {
        ev::Persistent err(makeTypeError(msg));
        ev::rejectPromise(promise.get(), err.get());
        return promise.get();
    };
    if (a.empty() || ev::isNull(a[0]) || ev::isUndefined(a[0])) {
        return reject("createImageBitmap requires a source");
    }

    bool crop = false;
    int sx = 0, sy = 0, sw = 0, sh = 0;
    if (a.size() >= 5) {
        crop = true;
        sx = static_cast<int>(ev::toDouble(a[1]));
        sy = static_cast<int>(ev::toDouble(a[2]));
        sw = static_cast<int>(ev::toDouble(a[3]));
        sh = static_cast<int>(ev::toDouble(a[4]));
    }

    std::string err;
    std::vector<uint8_t> outPixels;
    sk_sp<SkImage> resultImg;

    if (auto* bmp = hostImageBitmapOfMut(a[0])) {
        if (bmp->closed) return reject("ImageBitmap is closed");
        resultImg = buildBitmap(bmp->pixels.data(), bmp->width, bmp->height,
                                crop, sx, sy, sw, sh, outPixels, err);
    } else if (const HostImage* img = hostImageOf(a[0])) {
        if (!img->complete || !img->ok || img->rgba.empty()) return reject("Image has no valid pixels");
        resultImg = buildBitmap(img->rgba.data(), img->width, img->height,
                                crop, sx, sy, sw, sh, outPixels, err);
    } else if (dom::Element* el = hostElementOf(a[0])) {
        if (el->tagName() == "canvas" || el->tagName() == "CANVAS") {
            std::vector<uint8_t> px;
            int w = 0, h = 0;
            bool invalidState = false;
            if (canvasBitmapPixels(el, px, w, h, invalidState, err)) {
                resultImg = buildBitmap(px.data(), w, h, crop, sx, sy, sw, sh, outPixels, err);
            } else if (invalidState) {
                ev::Persistent domErr(hostMakeDomError("InvalidStateError", err));
                ev::rejectPromise(promise.get(), domErr.get());
                return promise.get();
            }
        } else {
            err = "Element is not a canvas";
        }
    } else {
        const uint8_t* bytes = nullptr;
        size_t len = 0;
        if (brokit::api::blobBytes(a[0], &bytes, &len) && bytes && len > 0) {
            // The same ladder an <img> src goes through (host_image.cpp):
            // bitmap codecs, WebP, then SVG — a fetched .webp or .svg blob
            // is as much an image here as a PNG one.
            int dw = 0, dh = 0;
            std::vector<uint8_t> decoded;
            std::string decErr;
            if (decodeHostImageBytes(bytes, len, dw, dh, decoded, &decErr)) {
                resultImg = buildBitmap(decoded.data(), dw, dh,
                                        crop, sx, sy, sw, sh, outPixels, err);
            } else {
                err = "Blob image decode failed: " + decErr;
            }
        } else if (ev::isObject(a[0])) {
            // Each read reduced to a number before the next getProperty
            // (which may allocate) runs; the data pointer is consumed by
            // buildBitmap before anything else can.
            int w = static_cast<int>(ev::toDouble(ev::getProperty(a[0], "width")));
            int h = static_cast<int>(ev::toDouble(ev::getProperty(a[0], "height")));
            Value dV = ev::getProperty(a[0], "data");
            auto info = ev::typedArrayInfo(dV);
            if (info.data && w > 0 && h > 0 && info.byteLength >= static_cast<size_t>(w) * h * 4) {
                resultImg = buildBitmap(info.data, w, h, crop, sx, sy, sw, sh, outPixels, err);
            } else {
                err = "Malformed image data source";
            }
        } else {
            err = "Unsupported source type";
        }
    }

    if (resultImg && !outPixels.empty()) {
        auto* newBmp = new HostImageBitmap();
        newBmp->image = resultImg;
        newBmp->pixels = std::move(outPixels);
        newBmp->width = resultImg->width();
        newBmp->height = resultImg->height();
        ev::Persistent bmpVal(g_imageBitmapClass.make(newBmp, hostImageBitmapDtor));
        ev::resolvePromise(promise.get(), bmpVal.get());
        return promise.get();
    }
    return reject(err.empty() ? "createImageBitmap failed" : err);
}

void installImageBitmapGlobals() {
    g_imageBitmapClass.install("ImageBitmap", 0,
        [](Value, std::span<const Value>) -> Value {
            return ev::throwTypeError("Illegal constructor: use createImageBitmap()");
        },
        [](ObjectBuilder& proto) {
            proto.accessor("width",
                [](Value thisVal, std::span<const Value>) -> Value {
                    auto* bmp = hostImageBitmapOfMut(thisVal);
                    return ev::fromDouble((bmp && !bmp->closed) ? bmp->width : 0);
                }, nullptr);
            proto.accessor("height",
                [](Value thisVal, std::span<const Value>) -> Value {
                    auto* bmp = hostImageBitmapOfMut(thisVal);
                    return ev::fromDouble((bmp && !bmp->closed) ? bmp->height : 0);
                }, nullptr);
            proto.def("close", 0, [](Value thisVal, std::span<const Value>) -> Value {
                if (auto* bmp = hostImageBitmapOfMut(thisVal)) {
                    bmp->closed = true;
                    bmp->width = 0;
                    bmp->height = 0;
                    bmp->image = nullptr;
                    bmp->pixels.clear();
                }
                return ev::undefined();
            });
        });

    g_imageDataClass.install("ImageData", 2, js_imageData_ctor, [](ObjectBuilder&) {});

    ev::setGlobalFunction("createImageBitmap", 1, js_createImageBitmap);
}

} // namespace bro::bronze_host
