#include "bronze_host/host_globals_internal.h"
#include "bronze_host/gl_internal.h"
#include "canvas/canvas_scene.h"
#include "dom/element.h"
#include "engine/engine.h"
#include "broimage/decode.h"
#include <api/api.h>
#include <include/core/SkData.h>
#include <include/core/SkImageInfo.h>
#include <cstring>
#include <memory>
#include <string>

namespace bro::bronze_host {

namespace {

static thread_local HostClass g_imageBitmapClass;
static thread_local HostClass g_imageDataClass;

static Value makeTypeError(const std::string& msg) {
    Value ctor = ev::globalValue("TypeError").value;
    if (ev::isFunction(ctor)) {
        Value msgVal = ev::fromUtf8(msg);
        auto res = ev::construct(ctor, std::span<const Value>(&msgVal, 1));
        if (!res.thrown) return res.value;
    }
    return ev::fromUtf8(msg);
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
    ObjectBuilder b;
    b.set("width", ev::fromDouble(width));
    b.set("height", ev::fromDouble(height));
    b.set("data", dataArr);
    Value obj = b.get();
    Value proto = g_imageDataClass.prototype();
    if (!ev::isUndefined(proto)) {
        ev::GlobalValue objCtor = ev::globalValue("Object");
        if (objCtor.found) {
            Value setProto = ev::getProperty(objCtor.value, "setPrototypeOf");
            if (ev::isFunction(setProto)) {
                const Value args[2] = { obj, proto };
                ev::call(setProto, ev::undefined(), std::span<const Value>(args, 2));
            }
        }
    }
    return obj;
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

static Value js_createImageBitmap(Value, std::span<const Value> a) {
    Value promise = ev::createPromise();
    if (a.empty() || ev::isNull(a[0]) || ev::isUndefined(a[0])) {
        ev::rejectPromise(promise, makeTypeError("createImageBitmap requires a source"));
        return promise;
    }

    Value src = a[0];
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

    if (auto* bmp = hostImageBitmapOfMut(src)) {
        if (bmp->closed) {
            ev::rejectPromise(promise, makeTypeError("ImageBitmap is closed"));
            return promise;
        }
        resultImg = buildBitmap(bmp->pixels.data(), bmp->width, bmp->height,
                                crop, sx, sy, sw, sh, outPixels, err);
    } else if (const HostImage* img = hostImageOf(src)) {
        if (!img->complete || !img->ok || img->rgba.empty()) {
            ev::rejectPromise(promise, makeTypeError("Image has no valid pixels"));
            return promise;
        }
        resultImg = buildBitmap(img->rgba.data(), img->width, img->height,
                                crop, sx, sy, sw, sh, outPixels, err);
    } else if (dom::Element* el = hostElementOf(src)) {
        if (el->tagName() == "canvas" || el->tagName() == "CANVAS") {
            if (!el->canvasScene()) {
                if (auto* eng = hostEngine()) {
                    eng->createCanvasContext(el);
                }
            }
            if (auto* scene = static_cast<canvas::CanvasScene*>(el->canvasScene())) {
                int w = std::atoi(el->getAttribute("width").c_str());
                int h = std::atoi(el->getAttribute("height").c_str());
                if (w <= 0) w = 300;
                if (h <= 0) h = 150;
                const uint8_t* px = scene->snapshotPixels(w, h);
                if (px) {
                    resultImg = buildBitmap(px, w, h, crop, sx, sy, sw, sh, outPixels, err);
                } else {
                    err = "Canvas snapshot failed";
                }
            } else {
                err = "Canvas snapshot failed";
            }
        } else {
            err = "Element is not a canvas";
        }
    } else {
        const uint8_t* bytes = nullptr;
        size_t len = 0;
        if (brokit::api::blobBytes(src, &bytes, &len) && bytes && len > 0) {
            broimage::Image decoded;
            std::string decErr;
            if (broimage::decode_memory(bytes, len, decoded, &decErr)) {
                resultImg = buildBitmap(decoded.pixels.data(), decoded.width, decoded.height,
                                        crop, sx, sy, sw, sh, outPixels, err);
            } else {
                err = "Blob image decode failed: " + decErr;
            }
        } else if (ev::isObject(src)) {
            Value wV = ev::getProperty(src, "width");
            Value hV = ev::getProperty(src, "height");
            Value dV = ev::getProperty(src, "data");
            int w = static_cast<int>(ev::toDouble(wV));
            int h = static_cast<int>(ev::toDouble(hV));
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
        Value bmpVal = g_imageBitmapClass.make(newBmp, hostImageBitmapDtor);
        ev::resolvePromise(promise, bmpVal);
    } else {
        ev::rejectPromise(promise, makeTypeError(err.empty() ? "createImageBitmap failed" : err));
    }
    return promise;
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
