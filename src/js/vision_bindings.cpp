// JS bindings for brovisionml — vision-model inference (bro.vision.*).
//
// Installed onto bro.vision.* by installVisionBindings(). Each model from
// brovisionml lives behind an opaque qjsbind handle (DepthEstimator, Sam, …)
// created by a `bro.vision.loadXxx(modelDir, opts)` loader. Models run on GPU
// by default — the loader places the model on CUDA when a backend is available
// (opts.device 'cpu' to force CPU).
//
// Heavy work — load, SAM's image encode, every inference call — runs on a
// background thread via the async-job runner when the caller passes an
// onReady/onDone callback, so the JS thread stays responsive (the same
// convention bro.stt / bro.tts / bro.lm use). With no callback the op runs
// inline and returns its result directly.
//
// Image inputs accept an `ImageBitmap` or an ImageData-shaped
// `{ data, width, height }` (RGBA Uint8/Uint8Clamped). Dense-map results come
// back as a drawable `ImageBitmap` (colorized / grayscale) plus the raw
// typed-array data, so they pipe straight into canvas drawImage, WebGL
// texImage2D, or a bro.diffusion conditioning input.

#include "js/vision_bindings.h"
#include "js/async_job.h"
#include "js/imagebitmap_bindings.h"

#include <qjsbind/qjsbind.h>

#include <brovisionml/depth_anything.h>
#include <brovisionml/sam.h>
#include <brovisionml/sam_amg.h>

#include <brotensor/runtime.h>
#include <brotensor/tensor.h>

#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bro::js {

// ═══════════════════════════════════════════════════════════════════════════
// Scalar / option helpers (TU-local — same shape as the stt/lm/diffusion
// bindings). All static to avoid the cross-TU ODR clash that bites helper
// structs/functions with external linkage in bro::js.
// ═══════════════════════════════════════════════════════════════════════════

static bool argStr(JSContext* ctx, JSValueConst v, std::string& out) {
    if (!JS_IsString(v)) return false;
    const char* s = JS_ToCString(ctx, v);
    if (!s) return false;
    out = s;
    JS_FreeCString(ctx, s);
    return true;
}

static bool getStr(JSContext* ctx, JSValueConst obj, const char* key,
                   std::string& out) {
    if (!JS_IsObject(obj)) return false;
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    bool ok = false;
    if (JS_IsString(v)) {
        const char* s = JS_ToCString(ctx, v);
        if (s) { out = s; JS_FreeCString(ctx, s); ok = true; }
    }
    JS_FreeValue(ctx, v);
    return ok;
}

static void getInt(JSContext* ctx, JSValueConst obj, const char* key, int& dst) {
    if (!JS_IsObject(obj)) return;
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsNumber(v)) { int32_t t = dst; JS_ToInt32(ctx, &t, v); dst = t; }
    JS_FreeValue(ctx, v);
}

static void getFloat(JSContext* ctx, JSValueConst obj, const char* key, float& dst) {
    if (!JS_IsObject(obj)) return;
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsNumber(v)) { double t = dst; JS_ToFloat64(ctx, &t, v); dst = (float)t; }
    JS_FreeValue(ctx, v);
}

static bool getBool(JSContext* ctx, JSValueConst obj, const char* key, bool def) {
    if (!JS_IsObject(obj)) return def;
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    bool out = def;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) out = JS_ToBool(ctx, v) == 1;
    JS_FreeValue(ctx, v);
    return out;
}

static brotensor::Device autoDevice() {
    return brotensor::is_available(brotensor::Device::CUDA)
        ? brotensor::Device::CUDA
        : brotensor::Device::CPU;
}

static const char* deviceName(brotensor::Device d) {
    switch (d) {
        case brotensor::Device::CUDA:  return "CUDA";
        case brotensor::Device::Metal: return "Metal";
        case brotensor::Device::CPU:   return "CPU";
    }
    return "?";
}

// Parse opts.device. Missing key → leave `out`, return true. Unknown value →
// set `err`, return false.
static bool parseDeviceOpt(JSContext* ctx, JSValueConst opts,
                           brotensor::Device& out, std::string& err) {
    if (!JS_IsObject(opts)) return true;
    JSValue v = JS_GetPropertyStr(ctx, opts, "device");
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); return true; }
    if (!JS_IsString(v)) {
        JS_FreeValue(ctx, v);
        err = "opts.device must be a string ('cpu' or 'cuda')";
        return false;
    }
    const char* s = JS_ToCString(ctx, v);
    std::string sv = s ? s : "";
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);
    if (sv == "cpu")   { out = brotensor::Device::CPU;   return true; }
    if (sv == "cuda")  { out = brotensor::Device::CUDA;  return true; }
    if (sv == "metal") { out = brotensor::Device::Metal; return true; }
    err = "opts.device must be 'cpu' or 'cuda' (got '" + sv + "')";
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════
// Image IO — read a JS image argument into an RGBA8 buffer, and build a
// drawable ImageBitmap from pixels / a scalar map.
// ═══════════════════════════════════════════════════════════════════════════

// Read `val` (an ImageBitmap or an ImageData-shaped { data, width, height })
// into a contiguous RGBA8 buffer. Always 4 channels — brovisionml's detect()
// accepts channels=4. Returns false + sets `err` on a bad source. Must run on
// the JS thread (touches the context).
static bool readImageArg(JSContext* ctx, JSValueConst val,
                         std::vector<std::uint8_t>& rgba, int& w, int& h,
                         std::string& err) {
    // 1) ImageBitmap.
    if (sk_sp<SkImage> img = ImageBitmapBindings::getImage(val)) {
        w = img->width();
        h = img->height();
        if (w <= 0 || h <= 0) { err = "ImageBitmap has zero size"; return false; }
        rgba.assign(static_cast<size_t>(w) * h * 4, 0);
        SkImageInfo info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType,
                                             kUnpremul_SkAlphaType);
        if (!img->readPixels(info, rgba.data(), static_cast<size_t>(w) * 4, 0, 0)) {
            err = "ImageBitmap readPixels failed";
            return false;
        }
        return true;
    }
    // 2) { data: Uint8Array/Uint8ClampedArray (RGBA), width, height }.
    if (!JS_IsObject(val)) {
        err = "image must be an ImageBitmap or { data, width, height }";
        return false;
    }
    w = 0; h = 0;
    getInt(ctx, val, "width", w);
    getInt(ctx, val, "height", h);
    if (w <= 0 || h <= 0) {
        err = "image { width, height } must be positive";
        return false;
    }
    JSValue dataV = JS_GetPropertyStr(ctx, val, "data");
    size_t byteOff = 0, viewLen = 0, bpe = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, dataV, &byteOff, &viewLen, &bpe);
    if (JS_IsException(abuf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, dataV);
        err = "image.data must be a Uint8Array/Uint8ClampedArray (RGBA)";
        return false;
    }
    size_t abufLen = 0;
    std::uint8_t* p = JS_GetArrayBuffer(ctx, &abufLen, abuf);
    JS_FreeValue(ctx, abuf);
    JS_FreeValue(ctx, dataV);
    const size_t need = static_cast<size_t>(w) * h * 4;
    if (!p || viewLen < need) {
        err = "image.data too small for width*height*4 (RGBA expected)";
        return false;
    }
    rgba.assign(p + byteOff, p + byteOff + need);
    return true;
}

// Wrap an RGBA8 buffer into a JS ImageBitmap (raster SkImage). Copies pixels.
static JSValue makeBitmap(JSContext* ctx, const std::uint8_t* rgba, int w, int h) {
    SkImageInfo info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType,
                                         kUnpremul_SkAlphaType);
    sk_sp<SkData> data =
        SkData::MakeWithCopy(rgba, static_cast<size_t>(w) * h * 4);
    sk_sp<SkImage> img =
        SkImages::RasterFromData(info, data, static_cast<size_t>(w) * 4);
    if (!img) return JS_NULL;
    return ImageBitmapBindings::wrap(ctx, std::move(img));
}

static JSValue makeUint8Array(JSContext* ctx, const std::uint8_t* d, size_t n) {
    JSValue ab = JS_NewArrayBufferCopy(ctx, d, n);
    JSValue a[3] = { ab, JS_UNDEFINED, JS_UNDEFINED };
    JSValue ta = JS_NewTypedArray(ctx, 1, a, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return ta;
}

// Min-max normalize a scalar map to a grayscale ImageBitmap. `invert` flips so
// the darker end maps to white. Writes the observed range to lo/hi.
static JSValue makeGrayBitmap(JSContext* ctx, const std::vector<float>& map,
                              int w, int h, bool invert, float& lo, float& hi) {
    lo = 1e30f; hi = -1e30f;
    for (float v : map) { lo = std::min(lo, v); hi = std::max(hi, v); }
    const float range = (hi > lo) ? (hi - lo) : 1.0f;
    std::vector<std::uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < map.size(); ++i) {
        float t = (map[i] - lo) / range;     // [0,1]
        if (invert) t = 1.0f - t;
        int g = (int)std::lround(t * 255.0f);
        g = std::clamp(g, 0, 255);
        rgba[4 * i + 0] = (std::uint8_t)g;
        rgba[4 * i + 1] = (std::uint8_t)g;
        rgba[4 * i + 2] = (std::uint8_t)g;
        rgba[4 * i + 3] = 255;
    }
    return makeBitmap(ctx, rgba.data(), w, h);
}

// Binary mask (0/1, h*w) → translucent colored overlay ImageBitmap: foreground
// painted (r,g,b,255), background fully transparent.
static JSValue makeMaskBitmap(JSContext* ctx, const std::uint8_t* mask,
                              int w, int h, std::uint8_t r, std::uint8_t g,
                              std::uint8_t b) {
    std::vector<std::uint8_t> rgba(static_cast<size_t>(w) * h * 4, 0);
    const size_t n = static_cast<size_t>(w) * h;
    for (size_t i = 0; i < n; ++i) {
        if (mask[i]) {
            rgba[4 * i + 0] = r;
            rgba[4 * i + 1] = g;
            rgba[4 * i + 2] = b;
            rgba[4 * i + 3] = 255;
        }
    }
    return makeBitmap(ctx, rgba.data(), w, h);
}

// ═══════════════════════════════════════════════════════════════════════════
// Async-op runner. Wraps the launchAsyncJob boilerplate the bindings repeat:
// run `compute` (heavy, background thread, throws on error) then `build` (JS
// thread → result JSValue) and `release` (JS thread → clear busy / free dups).
// If `onDoneVal` is a function the op is async (returns an AsyncHandle, fires
// onDone(result, info)); otherwise it runs inline and returns build()'s value.
// `compute`, `build`, `release` all capture one shared_ptr state struct.
// ═══════════════════════════════════════════════════════════════════════════

using ComputeFn = std::function<void(const std::atomic<bool>&)>;
using BuildFn   = std::function<JSValue(JSContext*)>;
using ReleaseFn = std::function<void()>;

static JSValue runVisionOp(JSContext* ctx, JSValueConst onDoneVal,
                           ComputeFn compute, BuildFn build, ReleaseFn release) {
    const bool async = JS_IsFunction(ctx, onDoneVal);

    if (!async) {
        std::atomic<bool> noCancel{false};
        try {
            compute(noCancel);
        } catch (const std::exception& e) {
            release();
            return JS_ThrowInternalError(ctx, "%s", e.what());
        }
        JSValue r = build(ctx);
        release();
        return r;
    }

    JSValue onDone = JS_DupValue(ctx, onDoneVal);
    auto work = std::move(compute);
    auto done = [onDone, build, release](JSContext* c, bool cancelled,
                                         const std::string& error) {
        JSValue info = JS_NewObject(c);
        JS_SetPropertyStr(c, info, "cancelled", JS_NewBool(c, cancelled));
        JSValue result;
        if (cancelled || !error.empty()) {
            if (!error.empty())
                JS_SetPropertyStr(c, info, "error", JS_NewString(c, error.c_str()));
            result = JS_NULL;
        } else {
            result = build(c);
        }
        JSValue args[2] = { result, info };
        JSValue rr = JS_Call(c, onDone, JS_UNDEFINED, 2, args);
        if (JS_IsException(rr)) JS_FreeValue(c, JS_GetException(c));
        JS_FreeValue(c, rr);
        JS_FreeValue(c, result);
        JS_FreeValue(c, info);
        release();
        JS_FreeValue(c, onDone);
    };
    return launchAsyncJob(ctx, std::move(work), nullptr, std::move(done));
}

// Pull `onDone` off an opts object (returns JS_UNDEFINED if absent / not an
// object). Caller frees the returned value.
static JSValue getOnDone(JSContext* ctx, JSValueConst opts) {
    if (!JS_IsObject(opts)) return JS_UNDEFINED;
    return JS_GetPropertyStr(ctx, opts, "onDone");
}

// ═══════════════════════════════════════════════════════════════════════════
// Depth-Anything-V2 — bro.vision.loadDepth / DepthEstimator.estimate
// ═══════════════════════════════════════════════════════════════════════════

struct VisionDepthWrapper {
    std::unique_ptr<brovisionml::depth::DepthEstimator> est;
    brotensor::Device device = brotensor::Device::CPU;
    std::atomic<bool> busy{false};
};

static brovisionml::depth::DepthAnythingConfig depthConfigForVariant(
        const std::string& v) {
    if (v == "base")  return brovisionml::depth::DepthAnythingConfig::v2_base();
    if (v == "large") return brovisionml::depth::DepthAnythingConfig::v2_large();
    return brovisionml::depth::DepthAnythingConfig::v2_small();
}

static void buildDepth(const std::string& dir, const std::string& variant,
                       brotensor::Device dev,
                       std::unique_ptr<VisionDepthWrapper>& out) {
    auto w = std::make_unique<VisionDepthWrapper>();
    w->device = dev;
    w->est = std::make_unique<brovisionml::depth::DepthEstimator>(
        depthConfigForVariant(variant));
    {
        brotensor::DeviceScope scope(dev);
        w->est->load(dir);
        w->est->to(dev);
    }
    std::fprintf(stderr, "[INFO] [vision] Depth-Anything (%s) loaded on %s\n",
                 variant.c_str(), deviceName(dev));
    out = std::move(w);
}

// State shared by an async estimate(): JS-thread reads pixels in, background
// thread fills the DepthMap, JS-thread done() builds the result.
struct DepthJob {
    VisionDepthWrapper*       w = nullptr;
    std::vector<std::uint8_t> rgba;
    int  in_w = 0, in_h = 0;
    bool invert = false;
    brovisionml::depth::DepthMap dm;
    JSValue selfRef = JS_UNDEFINED;
};

// DepthEstimator.estimate(image, opts?) → result | AsyncHandle
//   result: { width, height, depth: Float32Array(h*w), image: ImageBitmap,
//             min, max }  — depth is relative inverse-depth (nearer = larger).
//   opts.invert: flip the grayscale (default false → brighter = nearer).
//   opts.onDone(result, info): run on a background thread.
static JSValue js_depth_estimate(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<VisionDepthWrapper>(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "estimate: not a DepthEstimator");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "estimate(image, opts?): image required");

    auto job = std::make_shared<DepthJob>();
    job->w = w;
    std::string err;
    if (!readImageArg(ctx, argv[0], job->rgba, job->in_w, job->in_h, err))
        return JS_ThrowTypeError(ctx, "estimate: %s", err.c_str());

    JSValue opts = (argc >= 2) ? argv[1] : JS_UNDEFINED;
    job->invert = getBool(ctx, opts, "invert", false);
    JSValue onDone = getOnDone(ctx, opts);

    bool expected = false;
    if (!w->busy.compare_exchange_strong(expected, true)) {
        JS_FreeValue(ctx, onDone);
        return JS_ThrowInternalError(ctx,
            "estimate: an operation is already in flight on this model");
    }
    job->selfRef = JS_DupValue(ctx, this_val);

    auto compute = [job](const std::atomic<bool>&) {
        brotensor::DeviceScope scope(job->w->device);
        job->dm = job->w->est->estimate(job->rgba.data(), job->in_w, job->in_h, 4);
    };
    auto build = [job](JSContext* c) -> JSValue {
        const auto& dm = job->dm;
        float lo = 0, hi = 0;
        JSValue bmp = makeGrayBitmap(c, dm.depth, dm.width, dm.height,
                                     job->invert, lo, hi);
        JSValue res = JS_NewObject(c);
        JS_SetPropertyStr(c, res, "width",  JS_NewInt32(c, dm.width));
        JS_SetPropertyStr(c, res, "height", JS_NewInt32(c, dm.height));
        JS_SetPropertyStr(c, res, "depth",  qjsbind::make_float32_array(c, dm.depth));
        JS_SetPropertyStr(c, res, "image",  bmp);
        JS_SetPropertyStr(c, res, "min",    JS_NewFloat64(c, lo));
        JS_SetPropertyStr(c, res, "max",    JS_NewFloat64(c, hi));
        return res;
    };
    auto release = [job, ctx]() {
        job->w->busy.store(false, std::memory_order_release);
        JS_FreeValue(ctx, job->selfRef);
    };
    JSValue r = runVisionOp(ctx, onDone, std::move(compute), std::move(build),
                            std::move(release));
    JS_FreeValue(ctx, onDone);
    return r;
}

static void registerDepthClass(JSContext* ctx) {
    qjsbind::Class<VisionDepthWrapper>(ctx, "DepthEstimator", qjsbind::NoGlobal)
        .get("device", [](VisionDepthWrapper* w) {
            return std::string(deviceName(w->device));
        })
        .method_raw("estimate", js_depth_estimate, 2);
}

// State for an async loadDepth.
struct DepthLoadState {
    std::string dir, variant;
    brotensor::Device dev = brotensor::Device::CPU;
    std::unique_ptr<VisionDepthWrapper> w;
    JSValue onReady = JS_UNDEFINED, onError = JS_UNDEFINED;
    bool hasReady = false, hasError = false;
};

// bro.vision.loadDepth(modelDir, opts?) → DepthEstimator | AsyncHandle
//   modelDir holds model.safetensors (HF DepthAnythingForDepthEstimation).
//   opts.variant: 'small' (default) | 'base' | 'large'
//   opts.device:  'cuda' | 'cpu'  (default: CUDA when available)
//   opts.onReady(est) / opts.onError(msg): load on a background thread.
static JSValue js_loadDepth(JSContext* ctx, JSValueConst, int argc,
                            JSValueConst* argv) {
    std::string dir;
    if (argc < 1 || !argStr(ctx, argv[0], dir))
        return JS_ThrowTypeError(ctx, "loadDepth(modelDir, opts?): path required");

    brotensor::init();
    brotensor::Device dev = autoDevice();
    std::string variant = "small";
    if (argc >= 2) {
        std::string err;
        if (!parseDeviceOpt(ctx, argv[1], dev, err))
            return JS_ThrowTypeError(ctx, "loadDepth: %s", err.c_str());
        getStr(ctx, argv[1], "variant", variant);
    }

    const bool haveOpts = (argc >= 2) && JS_IsObject(argv[1]);
    JSValue onReady = haveOpts ? JS_GetPropertyStr(ctx, argv[1], "onReady")
                               : JS_UNDEFINED;
    JSValue onError = haveOpts ? JS_GetPropertyStr(ctx, argv[1], "onError")
                               : JS_UNDEFINED;
    const bool async = JS_IsFunction(ctx, onReady);

    if (!async) {
        JS_FreeValue(ctx, onReady);
        JS_FreeValue(ctx, onError);
        try {
            std::unique_ptr<VisionDepthWrapper> w;
            buildDepth(dir, variant, dev, w);
            return qjsbind::wrap<VisionDepthWrapper>(ctx, w.release());
        } catch (const std::exception& e) {
            return JS_ThrowInternalError(ctx, "loadDepth: %s", e.what());
        }
    }

    auto ls = std::make_shared<DepthLoadState>();
    ls->dir = dir; ls->variant = variant; ls->dev = dev;
    ls->hasReady = true;
    ls->onReady = JS_DupValue(ctx, onReady);
    ls->hasError = JS_IsFunction(ctx, onError);
    ls->onError = ls->hasError ? JS_DupValue(ctx, onError) : JS_UNDEFINED;
    JS_FreeValue(ctx, onReady);
    JS_FreeValue(ctx, onError);

    auto work = [ls](const std::atomic<bool>&) {
        buildDepth(ls->dir, ls->variant, ls->dev, ls->w);
    };
    auto done = [ls](JSContext* c, bool, const std::string& error) {
        if (!error.empty() || !ls->w) {
            if (ls->hasError) {
                JSValue e = JS_NewString(c, error.empty() ? "loadDepth failed"
                                                          : error.c_str());
                JSValue r = JS_Call(c, ls->onError, JS_UNDEFINED, 1, &e);
                if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
                JS_FreeValue(c, r);
                JS_FreeValue(c, e);
            }
        } else {
            JSValue out = qjsbind::wrap<VisionDepthWrapper>(c, ls->w.release());
            JSValue r = JS_Call(c, ls->onReady, JS_UNDEFINED, 1, &out);
            if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
            JS_FreeValue(c, r);
            JS_FreeValue(c, out);
        }
        if (ls->hasReady) JS_FreeValue(c, ls->onReady);
        if (ls->hasError) JS_FreeValue(c, ls->onError);
    };
    return launchAsyncJob(ctx, std::move(work), nullptr, std::move(done));
}

// ═══════════════════════════════════════════════════════════════════════════
// SAM — bro.vision.loadSam / Sam.setImage / Sam.segment / Sam.segmentEverything
// ═══════════════════════════════════════════════════════════════════════════

struct VisionSamWrapper {
    std::unique_ptr<brovisionml::sam::Sam> sam;
    brotensor::Device device = brotensor::Device::CPU;
    std::atomic<bool> busy{false};   // guards the heavy encode / AMG passes
};

static brovisionml::sam::SamConfig samConfigForVariant(const std::string& v) {
    if (v == "vit_l" || v == "large") return brovisionml::sam::SamConfig::vit_l();
    if (v == "vit_b" || v == "base")  return brovisionml::sam::SamConfig::vit_b();
    return brovisionml::sam::SamConfig::vit_h();
}

static void buildSam(const std::string& dir, const std::string& variant,
                     brotensor::Device dev,
                     std::unique_ptr<VisionSamWrapper>& out) {
    auto w = std::make_unique<VisionSamWrapper>();
    w->device = dev;
    w->sam = std::make_unique<brovisionml::sam::Sam>(samConfigForVariant(variant));
    {
        brotensor::DeviceScope scope(dev);
        w->sam->load(dir);
        w->sam->to(dev);
    }
    std::fprintf(stderr, "[INFO] [vision] SAM (%s) loaded on %s\n",
                 variant.c_str(), deviceName(dev));
    out = std::move(w);
}

// Read an array of [x,y] pairs into std::array<float,2>.
static std::vector<std::array<float, 2>> readPoints(JSContext* ctx,
                                                    JSValueConst v) {
    std::vector<std::array<float, 2>> out;
    if (!JS_IsArray(v)) return out;
    std::uint32_t n = 0;
    JSValue lv = JS_GetPropertyStr(ctx, v, "length");
    JS_ToUint32(ctx, &n, lv);
    JS_FreeValue(ctx, lv);
    out.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        JSValue e = JS_GetPropertyUint32(ctx, v, i);
        std::array<float, 2> p{0, 0};
        JSValue a = JS_GetPropertyUint32(ctx, e, 0);
        JSValue b = JS_GetPropertyUint32(ctx, e, 1);
        double x = 0, y = 0;
        JS_ToFloat64(ctx, &x, a); JS_ToFloat64(ctx, &y, b);
        p[0] = (float)x; p[1] = (float)y;
        JS_FreeValue(ctx, a); JS_FreeValue(ctx, b); JS_FreeValue(ctx, e);
        out.push_back(p);
    }
    return out;
}

static std::vector<int> readInts(JSContext* ctx, JSValueConst v) {
    std::vector<int> out;
    if (!JS_IsArray(v)) return out;
    std::uint32_t n = 0;
    JSValue lv = JS_GetPropertyStr(ctx, v, "length");
    JS_ToUint32(ctx, &n, lv);
    JS_FreeValue(ctx, lv);
    out.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        JSValue e = JS_GetPropertyUint32(ctx, v, i);
        int32_t t = 0; JS_ToInt32(ctx, &t, e);
        out.push_back(t);
        JS_FreeValue(ctx, e);
    }
    return out;
}

static std::vector<std::array<float, 4>> readBoxes(JSContext* ctx,
                                                   JSValueConst v) {
    std::vector<std::array<float, 4>> out;
    if (!JS_IsArray(v)) return out;
    std::uint32_t n = 0;
    JSValue lv = JS_GetPropertyStr(ctx, v, "length");
    JS_ToUint32(ctx, &n, lv);
    JS_FreeValue(ctx, lv);
    out.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        JSValue e = JS_GetPropertyUint32(ctx, v, i);
        std::array<float, 4> b{0, 0, 0, 0};
        for (int k = 0; k < 4; ++k) {
            JSValue c = JS_GetPropertyUint32(ctx, e, k);
            double t = 0; JS_ToFloat64(ctx, &t, c);
            b[k] = (float)t;
            JS_FreeValue(ctx, c);
        }
        JS_FreeValue(ctx, e);
        out.push_back(b);
    }
    return out;
}

// Build a { num, width, height, best, masks: [{ iou, data, image }] } result
// from a Segmentation (logits thresholded at 0). Runs on the JS thread.
static JSValue buildSegmentation(JSContext* ctx,
                                 const brovisionml::sam::Segmentation& seg) {
    const int W = seg.width, H = seg.height;
    const size_t plane = static_cast<size_t>(W) * H;
    JSValue masks = JS_NewArray(ctx);
    for (int m = 0; m < seg.num; ++m) {
        std::vector<std::uint8_t> bin(plane);
        const float* lg = seg.logits.data() + static_cast<size_t>(m) * plane;
        for (size_t i = 0; i < plane; ++i) bin[i] = lg[i] > 0.0f ? 1 : 0;
        JSValue mo = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, mo, "iou",
            JS_NewFloat64(ctx, m < (int)seg.iou.size() ? seg.iou[m] : 0.0));
        JS_SetPropertyStr(ctx, mo, "data", makeUint8Array(ctx, bin.data(), plane));
        JS_SetPropertyStr(ctx, mo, "image",
            makeMaskBitmap(ctx, bin.data(), W, H, 30, 144, 255));
        JS_SetPropertyUint32(ctx, masks, (std::uint32_t)m, mo);
    }
    JSValue res = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, res, "num",    JS_NewInt32(ctx, seg.num));
    JS_SetPropertyStr(ctx, res, "width",  JS_NewInt32(ctx, W));
    JS_SetPropertyStr(ctx, res, "height", JS_NewInt32(ctx, H));
    JS_SetPropertyStr(ctx, res, "best",   JS_NewInt32(ctx, seg.best()));
    JS_SetPropertyStr(ctx, res, "masks",  masks);
    return res;
}

static VisionSamWrapper* samSelf(JSContext* ctx, JSValueConst this_val) {
    return qjsbind::unwrap<VisionSamWrapper>(ctx, this_val);
}

// Sam.setImage(image, opts?) → undefined | AsyncHandle
//   Runs the slow ViT encode; caches the embedding for subsequent segment().
//   opts.onDone(_, info): run on a background thread.
struct SamEncodeJob {
    VisionSamWrapper*         w = nullptr;
    std::vector<std::uint8_t> rgba;
    int in_w = 0, in_h = 0;
    JSValue selfRef = JS_UNDEFINED;
};

static JSValue js_sam_setImage(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    auto* w = samSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "setImage: not a Sam");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "setImage(image, opts?): image required");

    auto job = std::make_shared<SamEncodeJob>();
    job->w = w;
    std::string err;
    if (!readImageArg(ctx, argv[0], job->rgba, job->in_w, job->in_h, err))
        return JS_ThrowTypeError(ctx, "setImage: %s", err.c_str());

    JSValue onDone = getOnDone(ctx, (argc >= 2) ? argv[1] : JS_UNDEFINED);

    bool expected = false;
    if (!w->busy.compare_exchange_strong(expected, true)) {
        JS_FreeValue(ctx, onDone);
        return JS_ThrowInternalError(ctx,
            "setImage: an operation is already in flight on this model");
    }
    job->selfRef = JS_DupValue(ctx, this_val);

    auto compute = [job](const std::atomic<bool>&) {
        brotensor::DeviceScope scope(job->w->device);
        job->w->sam->set_image(job->rgba.data(), job->in_w, job->in_h, 4);
    };
    auto build = [](JSContext* c) -> JSValue { return JS_UNDEFINED; };
    auto release = [job, ctx]() {
        job->w->busy.store(false, std::memory_order_release);
        JS_FreeValue(ctx, job->selfRef);
    };
    JSValue r = runVisionOp(ctx, onDone, std::move(compute), std::move(build),
                            std::move(release));
    JS_FreeValue(ctx, onDone);
    return r;
}

// Sam.segment(opts) → result   (cheap per-click decode; synchronous)
//   opts.points:   [[x,y],...] in original-image pixels
//   opts.labels:   [1,0,...]   1 = foreground, 0 = background (default all 1)
//   opts.boxes:    [[x1,y1,x2,y2],...]
//   opts.multimask: bool (default true)
static JSValue js_sam_segment(JSContext* ctx, JSValueConst this_val,
                              int argc, JSValueConst* argv) {
    auto* w = samSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "segment: not a Sam");
    if (w->busy.load(std::memory_order_acquire))
        return JS_ThrowInternalError(ctx,
            "segment: an encode/generate is in flight on this model");
    if (!w->sam->has_image())
        return JS_ThrowInternalError(ctx,
            "segment: call setImage() before segment()");

    JSValue opts = (argc >= 1) ? argv[0] : JS_UNDEFINED;
    std::vector<std::array<float, 2>> points;
    std::vector<int> labels;
    std::vector<std::array<float, 4>> boxes;
    bool multimask = true;
    if (JS_IsObject(opts)) {
        JSValue pv = JS_GetPropertyStr(ctx, opts, "points");
        points = readPoints(ctx, pv); JS_FreeValue(ctx, pv);
        JSValue lv = JS_GetPropertyStr(ctx, opts, "labels");
        labels = readInts(ctx, lv); JS_FreeValue(ctx, lv);
        JSValue bv = JS_GetPropertyStr(ctx, opts, "boxes");
        boxes = readBoxes(ctx, bv); JS_FreeValue(ctx, bv);
        multimask = getBool(ctx, opts, "multimask", true);
    }
    if (labels.empty()) labels.assign(points.size(), 1);   // default foreground
    if (points.empty() && boxes.empty())
        return JS_ThrowTypeError(ctx,
            "segment: opts.points or opts.boxes required");

    try {
        brotensor::DeviceScope scope(w->device);
        auto seg = w->sam->segment(points, labels, boxes, multimask);
        return buildSegmentation(ctx, seg);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "segment: %s", e.what());
    }
}

// Sam.segmentEverything(image, opts?) → result | AsyncHandle
//   AutomaticMaskGenerator ("segment everything"). Heavy — async via opts.onDone.
//   opts.pointsPerSide, pointsPerBatch, predIouThresh, stabilityThresh,
//   boxNmsThresh, cropNLayers, minMaskRegionArea  (defaults mirror upstream).
//   result: { width, height, masks: [{ data, image, bbox, area, predictedIou,
//             stabilityScore, point }] } sorted by descending area.
struct SamAmgJob {
    VisionSamWrapper*         w = nullptr;
    std::vector<std::uint8_t> rgba;
    int in_w = 0, in_h = 0;
    brovisionml::sam::AmgConfig cfg;
    std::vector<brovisionml::sam::GeneratedMask> masks;
    JSValue selfRef = JS_UNDEFINED;
};

static JSValue js_sam_segmentEverything(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv) {
    auto* w = samSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "segmentEverything: not a Sam");
    if (argc < 1)
        return JS_ThrowTypeError(ctx,
            "segmentEverything(image, opts?): image required");

    auto job = std::make_shared<SamAmgJob>();
    job->w = w;
    std::string err;
    if (!readImageArg(ctx, argv[0], job->rgba, job->in_w, job->in_h, err))
        return JS_ThrowTypeError(ctx, "segmentEverything: %s", err.c_str());

    JSValue opts = (argc >= 2) ? argv[1] : JS_UNDEFINED;
    getInt(ctx, opts, "pointsPerSide",      job->cfg.points_per_side);
    getInt(ctx, opts, "pointsPerBatch",     job->cfg.points_per_batch);
    getFloat(ctx, opts, "predIouThresh",    job->cfg.pred_iou_thresh);
    getFloat(ctx, opts, "stabilityThresh",  job->cfg.stability_score_thresh);
    getFloat(ctx, opts, "boxNmsThresh",     job->cfg.box_nms_thresh);
    getInt(ctx, opts, "cropNLayers",        job->cfg.crop_n_layers);
    getInt(ctx, opts, "minMaskRegionArea",  job->cfg.min_mask_region_area);
    JSValue onDone = getOnDone(ctx, opts);

    bool expected = false;
    if (!w->busy.compare_exchange_strong(expected, true)) {
        JS_FreeValue(ctx, onDone);
        return JS_ThrowInternalError(ctx,
            "segmentEverything: an operation is already in flight on this model");
    }
    job->selfRef = JS_DupValue(ctx, this_val);

    auto compute = [job](const std::atomic<bool>&) {
        brotensor::DeviceScope scope(job->w->device);
        brovisionml::sam::AutomaticMaskGenerator gen(*job->w->sam, job->cfg);
        job->masks = gen.generate(job->rgba.data(), job->in_w, job->in_h, 4);
    };
    auto build = [job](JSContext* c) -> JSValue {
        JSValue arr = JS_NewArray(c);
        for (std::uint32_t i = 0; i < job->masks.size(); ++i) {
            const auto& gm = job->masks[i];
            JSValue mo = JS_NewObject(c);
            JS_SetPropertyStr(c, mo, "data",
                makeUint8Array(c, gm.mask.data(), gm.mask.size()));
            JS_SetPropertyStr(c, mo, "image",
                makeMaskBitmap(c, gm.mask.data(), gm.width, gm.height,
                               30, 144, 255));
            JSValue bbox = JS_NewArray(c);
            for (int k = 0; k < 4; ++k)
                JS_SetPropertyUint32(c, bbox, k, JS_NewInt32(c, gm.bbox[k]));
            JS_SetPropertyStr(c, mo, "bbox", bbox);
            JS_SetPropertyStr(c, mo, "area", JS_NewInt64(c, gm.area));
            JS_SetPropertyStr(c, mo, "predictedIou",
                JS_NewFloat64(c, gm.predicted_iou));
            JS_SetPropertyStr(c, mo, "stabilityScore",
                JS_NewFloat64(c, gm.stability_score));
            JSValue pt = JS_NewArray(c);
            JS_SetPropertyUint32(c, pt, 0, JS_NewFloat64(c, gm.point[0]));
            JS_SetPropertyUint32(c, pt, 1, JS_NewFloat64(c, gm.point[1]));
            JS_SetPropertyStr(c, mo, "point", pt);
            JS_SetPropertyUint32(c, arr, i, mo);
        }
        JSValue res = JS_NewObject(c);
        JS_SetPropertyStr(c, res, "width",  JS_NewInt32(c, job->in_w));
        JS_SetPropertyStr(c, res, "height", JS_NewInt32(c, job->in_h));
        JS_SetPropertyStr(c, res, "masks",  arr);
        return res;
    };
    auto release = [job, ctx]() {
        job->w->busy.store(false, std::memory_order_release);
        JS_FreeValue(ctx, job->selfRef);
    };
    JSValue r = runVisionOp(ctx, onDone, std::move(compute), std::move(build),
                            std::move(release));
    JS_FreeValue(ctx, onDone);
    return r;
}

static void registerSamClass(JSContext* ctx) {
    qjsbind::Class<VisionSamWrapper>(ctx, "Sam", qjsbind::NoGlobal)
        .get("device", [](VisionSamWrapper* w) {
            return std::string(deviceName(w->device));
        })
        .get("hasImage", [](VisionSamWrapper* w) { return w->sam->has_image(); })
        .method_raw("setImage",          js_sam_setImage,          2)
        .method_raw("segment",           js_sam_segment,           1)
        .method_raw("segmentEverything", js_sam_segmentEverything, 2);
}

struct SamLoadState {
    std::string dir, variant;
    brotensor::Device dev = brotensor::Device::CPU;
    std::unique_ptr<VisionSamWrapper> w;
    JSValue onReady = JS_UNDEFINED, onError = JS_UNDEFINED;
    bool hasReady = false, hasError = false;
};

// bro.vision.loadSam(modelDir, opts?) → Sam | AsyncHandle
//   modelDir holds model.safetensors (HF SamModel checkpoint).
//   opts.variant: 'vit_h' (default) | 'vit_l' | 'vit_b'
//   opts.device / opts.onReady / opts.onError: as loadDepth.
static JSValue js_loadSam(JSContext* ctx, JSValueConst, int argc,
                          JSValueConst* argv) {
    std::string dir;
    if (argc < 1 || !argStr(ctx, argv[0], dir))
        return JS_ThrowTypeError(ctx, "loadSam(modelDir, opts?): path required");

    brotensor::init();
    brotensor::Device dev = autoDevice();
    std::string variant = "vit_h";
    if (argc >= 2) {
        std::string err;
        if (!parseDeviceOpt(ctx, argv[1], dev, err))
            return JS_ThrowTypeError(ctx, "loadSam: %s", err.c_str());
        getStr(ctx, argv[1], "variant", variant);
    }

    const bool haveOpts = (argc >= 2) && JS_IsObject(argv[1]);
    JSValue onReady = haveOpts ? JS_GetPropertyStr(ctx, argv[1], "onReady")
                               : JS_UNDEFINED;
    JSValue onError = haveOpts ? JS_GetPropertyStr(ctx, argv[1], "onError")
                               : JS_UNDEFINED;
    const bool async = JS_IsFunction(ctx, onReady);

    if (!async) {
        JS_FreeValue(ctx, onReady);
        JS_FreeValue(ctx, onError);
        try {
            std::unique_ptr<VisionSamWrapper> w;
            buildSam(dir, variant, dev, w);
            return qjsbind::wrap<VisionSamWrapper>(ctx, w.release());
        } catch (const std::exception& e) {
            return JS_ThrowInternalError(ctx, "loadSam: %s", e.what());
        }
    }

    auto ls = std::make_shared<SamLoadState>();
    ls->dir = dir; ls->variant = variant; ls->dev = dev;
    ls->hasReady = true;
    ls->onReady = JS_DupValue(ctx, onReady);
    ls->hasError = JS_IsFunction(ctx, onError);
    ls->onError = ls->hasError ? JS_DupValue(ctx, onError) : JS_UNDEFINED;
    JS_FreeValue(ctx, onReady);
    JS_FreeValue(ctx, onError);

    auto work = [ls](const std::atomic<bool>&) {
        buildSam(ls->dir, ls->variant, ls->dev, ls->w);
    };
    auto done = [ls](JSContext* c, bool, const std::string& error) {
        if (!error.empty() || !ls->w) {
            if (ls->hasError) {
                JSValue e = JS_NewString(c, error.empty() ? "loadSam failed"
                                                          : error.c_str());
                JSValue r = JS_Call(c, ls->onError, JS_UNDEFINED, 1, &e);
                if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
                JS_FreeValue(c, r);
                JS_FreeValue(c, e);
            }
        } else {
            JSValue out = qjsbind::wrap<VisionSamWrapper>(c, ls->w.release());
            JSValue r = JS_Call(c, ls->onReady, JS_UNDEFINED, 1, &out);
            if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
            JS_FreeValue(c, r);
            JS_FreeValue(c, out);
        }
        if (ls->hasReady) JS_FreeValue(c, ls->onReady);
        if (ls->hasError) JS_FreeValue(c, ls->onError);
    };
    return launchAsyncJob(ctx, std::move(work), nullptr, std::move(done));
}

// ═══════════════════════════════════════════════════════════════════════════
// bro.vision free functions + install
// ═══════════════════════════════════════════════════════════════════════════

static JSValue js_init(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    try {
        brotensor::init();
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "bro.vision.init: %s", e.what());
    }
    return JS_UNDEFINED;
}

void installVisionBindings(JSContext* ctx) {
    registerDepthClass(ctx);
    registerSamClass(ctx);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        JS_FreeValue(ctx, broObj);
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue vision = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, vision, "init",
        JS_NewCFunction(ctx, js_init, "init", 0));
    JS_SetPropertyStr(ctx, vision, "loadDepth",
        JS_NewCFunction(ctx, js_loadDepth, "loadDepth", 2));
    JS_SetPropertyStr(ctx, vision, "loadSam",
        JS_NewCFunction(ctx, js_loadSam, "loadSam", 2));
    JS_SetPropertyStr(ctx, broObj, "vision", vision);

    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

void cleanupVisionBindings(JSContext* /*ctx*/) {}

}  // namespace bro::js
