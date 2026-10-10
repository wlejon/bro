#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_class.h"
#include "bronze_host/host_element.h"
#include "bronze_host/host_image.h"
#include "bronze_host/host_values.h"
#include "canvas/canvas_scene.h"
#include "dom/element.h"
#include "engine/engine.h"
#include "render/gpu_image_upload.h"
#include "render/image_store.h"
#include "render/shared_pixels_image.h"
#include "render/skia_gpu.h"
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

static Value makeBitmapValue(std::shared_ptr<const render::DecodedImage> px, sk_sp<SkImage> image = nullptr,
                             std::shared_ptr<render::GpuImageUpload> upload = nullptr);

// The bitmap's pixels as a crop of `rgba` (srcW x srcH), copied: the source
// is mutable (an ImageData, a canvas) or is about to go (a decode buffer).
// When `shared` holds `rgba` already (another bitmap, a decoded <img>) and the
// crop is the whole of it, the bitmap shares those pixels instead: they are as
// immutable as the bitmap.
static std::shared_ptr<const render::DecodedImage> buildBitmap(
        const uint8_t* rgba, int srcW, int srcH,
        bool crop, int sx, int sy, int sw, int sh, std::string& err,
        const std::shared_ptr<const render::DecodedImage>& shared = nullptr) {
    if (!rgba || srcW <= 0 || srcH <= 0) { err = "empty source"; return nullptr; }
    if (!crop) { sx = 0; sy = 0; sw = srcW; sh = srcH; }
    // The crop rectangle is a script's four numbers: clipped in 64 bits so an
    // edge near INT_MAX cannot overflow before it lands inside the source.
    int64_t cx = sx, cy = sy, cw = sw, ch = sh;
    if (cx < 0) { cw += cx; cx = 0; }
    if (cy < 0) { ch += cy; cy = 0; }
    if (cx >= srcW || cy >= srcH || cw <= 0 || ch <= 0) {
        err = "crop rect outside source"; return nullptr;
    }
    if (cx + cw > srcW) cw = srcW - cx;
    if (cy + ch > srcH) ch = srcH - cy;
    sx = static_cast<int>(cx); sy = static_cast<int>(cy);
    sw = static_cast<int>(cw); sh = static_cast<int>(ch);

    const bool whole = sx == 0 && sy == 0 && sw == srcW && sh == srcH;
    if (whole && shared && shared->rgba.data() == rgba && shared->width == srcW && shared->height == srcH)
        return shared;

    auto out = std::make_shared<render::DecodedImage>();
    out->id = render::newPixelsId();
    out->width = sw;
    out->height = sh;
    out->rgba.resize(static_cast<size_t>(sw) * sh * 4);
    if (whole) {
        std::memcpy(out->rgba.data(), rgba, out->rgba.size());
    } else {
        for (int row = 0; row < sh; ++row) {
            std::memcpy(out->rgba.data() + static_cast<size_t>(row) * sw * 4,
                        rgba + (static_cast<size_t>(sy + row) * srcW + sx) * 4,
                        static_cast<size_t>(sw) * 4);
        }
    }
    return out;
}

// The one place a bitmap is made. The raster image is a view of the pixels
// (no copy). In the page realm a big bitmap starts its texture upload now —
// staging written and copy submitted on the uploader thread — so the frame
// that first draws it does not pay it (render/gpu_image_upload.h). A worker's
// bitmaps do not: they are on their way to the page.
static bool wantsEagerUpload(const render::DecodedImage& px) {
    return int64_t(px.width) * px.height >= render::kEagerUploadMinPixels;
}

static Value makeBitmapValue(std::shared_ptr<const render::DecodedImage> px, sk_sp<SkImage> image,
                             std::shared_ptr<render::GpuImageUpload> upload) {
    auto* bmp = new HostImageBitmap();
    if (px && !px->rgba.empty() && px->width > 0 && px->height > 0) {
        const render::SharedPixels shared = render::sharedPixelsOf(px);
        bmp->width = px->width;
        bmp->height = px->height;
        bmp->image = image ? std::move(image) : render::makeSharedPixelsImage(shared);
        if (onHostImageMainThread() && wantsEagerUpload(*px)) {
            if (upload && upload->pixelsId() == px->id) bmp->upload = std::move(upload);
            else bmp->upload = render::SkiaGpu::uploadToTarget(shared);
        }
        bmp->pixels = std::move(px);
    }
    return g_imageBitmapClass.make(bmp, hostImageBitmapDtor);
}

} // namespace

const uint8_t* HostImageBitmap::rgba() const {
    return !closed && pixels && !pixels->rgba.empty() ? pixels->rgba.data() : nullptr;
}

void HostImageBitmap::detach() {
    closed = true;
    width = 0;
    height = 0;
    image = nullptr;
    // Let the pixels and the texture go now, not when the wrapper is
    // collected: a page that closes its bitmaps allocates too little JS to
    // bring a collection round for a long time (an image viewer: a full-size
    // picture held per picture shown). Pixels a clone still shares stay
    // with it.
    pixels.reset();
    upload.reset();
}

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
    const int w = img ? img->width() : 0;
    const int h = img ? img->height() : 0;
    if (w <= 0 || h <= 0) return makeBitmapValue(nullptr);
    auto px = std::make_shared<render::DecodedImage>();
    px->id = render::newPixelsId();
    px->width = w;
    px->height = h;
    px->rgba.resize(static_cast<size_t>(w) * h * 4);
    SkImageInfo info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    img->readPixels(nullptr, info, px->rgba.data(), static_cast<size_t>(w) * 4, 0, 0);
    return makeBitmapValue(std::move(px), std::move(img));
}

Value wrapHostImageBitmap(const uint8_t* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return makeBitmapValue(nullptr);
    std::string err;
    return makeBitmapValue(buildBitmap(rgba, w, h, false, 0, 0, 0, 0, err));
}

Value wrapHostImageBitmap(std::shared_ptr<const render::DecodedImage> pixels,
                          std::shared_ptr<render::GpuImageUpload> upload) {
    return makeBitmapValue(std::move(pixels), nullptr, std::move(upload));
}

std::shared_ptr<render::GpuImageUpload> startTransferUpload(const HostImageBitmap& bmp) {
    if (bmp.upload) return bmp.upload;
    if (onHostImageMainThread() || !bmp.pixels || !wantsEagerUpload(*bmp.pixels)) return nullptr;
    return render::SkiaGpu::uploadToTarget(render::sharedPixelsOf(bmp.pixels));
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
    // Every caller has bounded width*height*4 to one buffer (rgbaBufferFits);
    // an engine-sized source (a frame capture) stays far below it.
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
        int32_t w = satCast<int32_t>(ev::toDouble(a[1]));
        if (w <= 0) return ev::throwRangeError("ImageData width must be positive");
        if (info0.byteLength % 4 != 0) return ev::throwRangeError("ImageData data length must be a multiple of 4");
        size_t pixelCount = info0.byteLength / 4;
        if (pixelCount % static_cast<size_t>(w) != 0)
            return ev::throwRangeError("ImageData data length is not a multiple of 4*width");
        int h = static_cast<int>(pixelCount / static_cast<size_t>(w));
        if (a.size() >= 3) {
            int32_t hh = satCast<int32_t>(ev::toDouble(a[2]));
            if (hh > 0) {
                if (static_cast<size_t>(w) * hh * 4 != info0.byteLength)
                    return ev::throwRangeError("ImageData data length does not match width*height*4");
                h = hh;
            }
        }
        width = w; height = h;
        dataArr = a[0];
    } else {
        int32_t w = satCast<int32_t>(ev::toDouble(a[0]));
        int32_t h = a.size() >= 2 ? satCast<int32_t>(ev::toDouble(a[1])) : 0;
        if (w <= 0 || h <= 0)
            return ev::throwRangeError("ImageData(width, height) requires positive dimensions");
        // Checked before the allocation, in 64 bits: width*height*4 past one
        // buffer's cap would otherwise wrap in the uint32 length below and
        // hand back an ImageData whose data is shorter than it says.
        if (!rgbaBufferFits(w, h))
            return ev::throwRangeError("ImageData(width, height): " + std::to_string(w) + "x" +
                                       std::to_string(h) + " is larger than one ImageData can hold");
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
        sx = satCast<int>(ev::toDouble(a[1]));
        sy = satCast<int>(ev::toDouble(a[2]));
        sw = satCast<int>(ev::toDouble(a[3]));
        sh = satCast<int>(ev::toDouble(a[4]));
    }

    std::string err;
    std::shared_ptr<const render::DecodedImage> result;

    if (auto* bmp = hostImageBitmapOfMut(a[0])) {
        if (bmp->closed || !bmp->rgba()) return reject("ImageBitmap is closed");
        // Uncropped, the new bitmap shares the source's pixels.
        result = buildBitmap(bmp->rgba(), bmp->width, bmp->height,
                             crop, sx, sy, sw, sh, err, bmp->pixels);
    } else if (const HostImage* img = hostImageOf(a[0])) {
        if (!img->complete || !img->ok || !img->rgba()) return reject("Image has no valid pixels");
        result = buildBitmap(img->rgba(), img->width, img->height,
                             crop, sx, sy, sw, sh, err, img->pixels);
    } else if (dom::Element* el = hostElementOf(a[0])) {
        if (el->tagName() == "canvas" || el->tagName() == "CANVAS") {
            std::vector<uint8_t> px;
            int w = 0, h = 0;
            bool invalidState = false;
            if (canvasBitmapPixels(el, px, w, h, invalidState, err)) {
                result = buildBitmap(px.data(), w, h, crop, sx, sy, sw, sh, err);
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
            // The same ladder an <img> src goes through (render/image_store):
            // bitmap codecs, WebP, then SVG — a fetched .webp or .svg blob
            // is as much an image here as a PNG one. EXIF orientation applies
            // unless the options say imageOrientation: 'none'.
            bool orient = true;
            {
                const size_t optAt = crop ? 5 : 1;
                if (a.size() > optAt && ev::isObject(a[optAt])) {
                    Value io = ev::getProperty(a[optAt], "imageOrientation");
                    if (ev::isString(io) && ev::toUtf8(io) == "none") orient = false;
                }
            }
            // The decode and the crop run on a decoder thread; the page thread
            // only copies the bytes (the Blob may be collected before the
            // decode runs) and settles the promise. Uncropped, the decoded
            // buffer itself becomes the bitmap's pixels.
            struct Result {
                std::shared_ptr<const render::DecodedImage> pixels;
                std::string err;
            };
            auto shared = std::make_shared<Result>();
            auto work = [data = std::vector<uint8_t>(bytes, bytes + len), orient, crop, sx, sy, sw, sh,
                         shared](render::DecodedImage& out, std::string& werr) -> bool {
                auto img = std::make_shared<render::DecodedImage>();
                std::string decErr;
                if (!render::decodeImageData(data.data(), data.size(), orient, *img, decErr) ||
                    img->rgba.empty()) {
                    werr = "Blob image decode failed: " +
                           (decErr.empty() ? std::string("SVG markup did not rasterize (no intrinsic size?)") : decErr);
                    shared->err = werr;
                    return false;
                }
                img->isSvg = false;
                img->svgMarkup.clear();
                std::string cropErr;
                shared->pixels = buildBitmap(img->rgba.data(), img->width, img->height,
                                             crop, sx, sy, sw, sh, cropErr, img);
                if (!shared->pixels) {
                    werr = cropErr.empty() ? std::string("createImageBitmap failed") : cropErr;
                    shared->err = werr;
                    return false;
                }
                out.width = shared->pixels->width;
                out.height = shared->pixels->height;
                return true;
            };
            if (!onHostImageMainThread()) {
                // A worker realm is already off the page thread: decode here.
                render::DecodedImage unused;
                std::string werr;
                if (work(unused, werr)) {
                    result = std::move(shared->pixels);
                } else {
                    err = werr;
                }
            } else {
                auto req = render::ImageStore::instance().submit(std::move(work));
                auto held = std::make_shared<ev::Persistent>(promise.get());
                whenHostDecodeSettles(req, [held, shared]() {
                    if (shared->pixels) {
                        ev::Persistent bmpVal(makeBitmapValue(std::move(shared->pixels)));
                        ev::resolvePromise(held->get(), bmpVal.get());
                    } else {
                        ev::Persistent errVal(makeTypeError(
                            shared->err.empty() ? std::string("createImageBitmap failed") : shared->err));
                        ev::rejectPromise(held->get(), errVal.get());
                    }
                });
                return promise.get();
            }
        } else if (ev::isObject(a[0])) {
            // Each read reduced to a number before the next getProperty
            // (which may allocate) runs; the data pointer is consumed by
            // buildBitmap before anything else can.
            int w = satCast<int>(ev::toDouble(ev::getProperty(a[0], "width")));
            int h = satCast<int>(ev::toDouble(ev::getProperty(a[0], "height")));
            Value dV = ev::getProperty(a[0], "data");
            auto info = ev::typedArrayInfo(dV);
            if (info.data && w > 0 && h > 0 && info.byteLength >= static_cast<size_t>(w) * h * 4) {
                result = buildBitmap(info.data, w, h, crop, sx, sy, sw, sh, err);
            } else {
                err = "Malformed image data source";
            }
        } else {
            err = "Unsupported source type";
        }
    }

    if (result) {
        ev::Persistent bmpVal(makeBitmapValue(std::move(result)));
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
                // Frees the pixels (and the texture) now, not when the
                // wrapper is collected (HostImageBitmap::detach).
                if (auto* bmp = hostImageBitmapOfMut(thisVal)) bmp->detach();
                return ev::undefined();
            });
        });

    g_imageDataClass.install("ImageData", 2, js_imageData_ctor, [](ObjectBuilder&) {});

    ev::setGlobalFunction("createImageBitmap", 1, js_createImageBitmap);
}

} // namespace bro::bronze_host
