// JS bindings for TripoSplat — single-image -> 3D Gaussian Splat (bro.triposplat).
// See triposplat_bindings.h for the composition rationale.
//
//   const ts = bro.triposplat.load({ dinov3, vae, flow, decoder, device });
//   const cloud = ts.generate(image, { seed, steps, guidanceScale, shift, numGaussians });
//   // cloud = { positions, scales, rotations, opacities, sh, shDegree, count } (typed arrays)
//   scene.createGaussianSplat({ cloud, scale: 1 });
//
// image is an ImageBitmap or an ImageData-shaped { data, width, height } (RGBA).
// Heavy (multi-second) — run inside a Worker for a responsive UI; the binding is
// installed in the worker context too.

#include "js/triposplat_bindings.h"
#include "js/imagebitmap_bindings.h"

#include <qjsbind/qjsbind.h>

#include <brovisionml/dinov3.h>
#include <brovisionml/birefnet.h>

#include <brodiffusion/triposplat/vae_encoder.h>
#include <brodiffusion/triposplat/flow_model.h>
#include <brodiffusion/triposplat/octree_decoder.h>
#include <brodiffusion/triposplat/sampler.h>

#include <brotensor/runtime.h>
#include <brotensor/safetensors.h>
#include <brotensor/tensor.h>

#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace bro::js {

namespace bt  = ::brotensor;
namespace st  = ::brotensor::safetensors;
namespace tsp = ::brodiffusion::triposplat;
namespace dv3 = ::brovisionml::dinov3;
namespace brn = ::brovisionml::birefnet;

// ─── TU-local helpers (static: avoids the cross-TU ODR clash that bites helper
//     functions with external linkage in bro::js) ──────────────────────────────

namespace {

bool tsGetStr(JSContext* ctx, JSValueConst obj, const char* key, std::string& out) {
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

void tsGetInt(JSContext* ctx, JSValueConst obj, const char* key, int& dst) {
    if (!JS_IsObject(obj)) return;
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsNumber(v)) { int32_t t = dst; JS_ToInt32(ctx, &t, v); dst = t; }
    JS_FreeValue(ctx, v);
}

void tsGetFloat(JSContext* ctx, JSValueConst obj, const char* key, float& dst) {
    if (!JS_IsObject(obj)) return;
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsNumber(v)) { double t = dst; JS_ToFloat64(ctx, &t, v); dst = (float)t; }
    JS_FreeValue(ctx, v);
}

// Process-global cooperative-cancel flag. generate() runs as one synchronous
// native call inside a Worker, so the worker's JS thread is blocked and can't
// receive a "cancel" postMessage mid-run. Instead the MAIN thread calls
// bro.triposplat.cancel(), which flips this atomic; the in-flight generate()
// (on the worker thread) polls it at every stage boundary and between sampler
// steps. Both contexts share this one process-global, so no message-passing —
// and no mutex — is involved.
std::atomic<bool> g_cancelRequested{false};

bt::Device tsAutoDevice() {
    if (bt::is_available(bt::Device::CUDA))  return bt::Device::CUDA;
    if (bt::is_available(bt::Device::Metal)) return bt::Device::Metal;
    return bt::Device::CPU;
}

// Upload host FP32 as a tensor at the pipeline compute dtype on the default
// device (FP16 on a GPU backend, FP32 on CPU).
bt::Tensor tsUploadCompute(const float* src, int rows, int cols) {
    if (bt::compute_dtype() == bt::Dtype::FP16) {
        std::vector<std::uint16_t> bits(static_cast<std::size_t>(rows) * cols);
        for (std::size_t i = 0; i < bits.size(); ++i) bits[i] = bt::fp32_to_fp16_bits(src[i]);
        return bt::Tensor::from_host_fp16(bits.data(), rows, cols);
    }
    return bt::Tensor::from_host(src, rows, cols);
}

std::vector<float> tsDownloadF32(const bt::Tensor& t) {
    if (t.dtype == bt::Dtype::FP16) {
        std::vector<std::uint16_t> bits = t.to_host_vector_fp16();
        std::vector<float> out(bits.size());
        for (std::size_t i = 0; i < bits.size(); ++i) out[i] = bt::fp16_bits_to_fp32(bits[i]);
        return out;
    }
    return t.to_host_vector();
}

// Read an ImageBitmap / { data, width, height } into an RGBA8 buffer (JS thread).
bool tsReadImage(JSContext* ctx, JSValueConst val, std::vector<std::uint8_t>& rgba,
                 int& w, int& h, std::string& err) {
    if (sk_sp<SkImage> img = ImageBitmapBindings::getImage(val)) {
        w = img->width(); h = img->height();
        if (w <= 0 || h <= 0) { err = "ImageBitmap has zero size"; return false; }
        rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);
        SkImageInfo info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
        if (!img->readPixels(info, rgba.data(), static_cast<std::size_t>(w) * 4, 0, 0)) {
            err = "ImageBitmap readPixels failed"; return false;
        }
        return true;
    }
    if (!JS_IsObject(val)) { err = "image must be an ImageBitmap or { data, width, height }"; return false; }
    w = 0; h = 0;
    tsGetInt(ctx, val, "width", w);
    tsGetInt(ctx, val, "height", h);
    if (w <= 0 || h <= 0) { err = "image { width, height } must be positive"; return false; }
    JSValue dataV = JS_GetPropertyStr(ctx, val, "data");
    std::size_t byteOff = 0, viewLen = 0, bpe = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, dataV, &byteOff, &viewLen, &bpe);
    if (JS_IsException(abuf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, dataV);
        err = "image.data must be a Uint8Array/Uint8ClampedArray (RGBA)";
        return false;
    }
    std::size_t abufLen = 0;
    std::uint8_t* p = JS_GetArrayBuffer(ctx, &abufLen, abuf);
    JS_FreeValue(ctx, abuf);
    JS_FreeValue(ctx, dataV);
    const std::size_t need = static_cast<std::size_t>(w) * h * 4;
    if (!p || viewLen < need) { err = "image.data too small for width*height*4 (RGBA)"; return false; }
    rgba.assign(p + byteOff, p + byteOff + need);
    return true;
}

// "Cover" bilinear resample of an RGBA8 image to 1024x1024, compositing alpha
// over black (the encoders saw an RGB-on-black 1024 canvas). This is the
// lightweight stand-in for the reference preprocess_image; the BiRefNet
// background-removal stage is deferred, so a pre-masked / already-foreground
// image gives the best result.
constexpr int kCanvas = 1024;

void tsPreprocess(const std::vector<std::uint8_t>& rgba, int w, int h,
                  std::vector<float>& rgb01 /* 3 * kCanvas * kCanvas, NCHW-friendly HWC */) {
    rgb01.assign(static_cast<std::size_t>(kCanvas) * kCanvas * 3, 0.0f);
    const float s = static_cast<float>(kCanvas) / static_cast<float>(std::min(w, h));
    const float sw = w * s, sh = h * s;
    const float offx = (sw - kCanvas) * 0.5f, offy = (sh - kCanvas) * 0.5f;
    auto px = [&](int x, int y, int c) -> float {
        x = std::min(std::max(x, 0), w - 1);
        y = std::min(std::max(y, 0), h - 1);
        return static_cast<float>(rgba[(static_cast<std::size_t>(y) * w + x) * 4 + c]);
    };
    for (int ty = 0; ty < kCanvas; ++ty) {
        for (int tx = 0; tx < kCanvas; ++tx) {
            const float fx = (static_cast<float>(tx) + offx) / s;
            const float fy = (static_cast<float>(ty) + offy) / s;
            const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
            const float ax = fx - x0, ay = fy - y0;
            float rgbV[4];
            for (int c = 0; c < 4; ++c) {
                const float c00 = px(x0, y0, c), c10 = px(x0 + 1, y0, c);
                const float c01 = px(x0, y0 + 1, c), c11 = px(x0 + 1, y0 + 1, c);
                rgbV[c] = (c00 * (1 - ax) + c10 * ax) * (1 - ay) +
                          (c01 * (1 - ax) + c11 * ax) * ay;
            }
            const float alpha = rgbV[3] / 255.0f;   // composite over black
            float* o = &rgb01[(static_cast<std::size_t>(ty) * kCanvas + tx) * 3];
            o[0] = rgbV[0] / 255.0f * alpha;
            o[1] = rgbV[1] / 255.0f * alpha;
            o[2] = rgbV[2] / 255.0f * alpha;
        }
    }
}

JSValue tsFloat32Array(JSContext* ctx, const float* d, std::size_t n) {
    JSValue ab = JS_NewArrayBufferCopy(ctx, reinterpret_cast<const std::uint8_t*>(d), n * sizeof(float));
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, "Float32Array");
    JSValue arr = JS_CallConstructor(ctx, ctor, 1, &ab);
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, ab);
    return arr;
}

}  // namespace

// ─── pipeline wrapper ─────────────────────────────────────────────────────────

struct TripoSplatWrapper {
    std::unique_ptr<dv3::Backbone>              dino;
    std::unique_ptr<tsp::Flux2VaeEncoder>       vae;
    std::unique_ptr<tsp::FlowDiT>               flow;
    std::unique_ptr<tsp::OctreeGaussianDecoder> decoder;
    std::unique_ptr<brn::BiRefNet>              rmbg;   // optional bg-removal
    bt::Device device = bt::Device::CPU;
};

namespace {

// The pipeline: preprocess -> DINOv3 + VAE encoders -> seeded noise -> Euler CFG
// sampler -> octree decode -> Gaussian cloud (typed arrays).
JSValue tsGenerate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<TripoSplatWrapper>(ctx, this_val);
    if (!w || !w->dino || !w->flow || !w->decoder || !w->vae)
        return JS_ThrowTypeError(ctx, "triposplat: pipeline not loaded");
    if (argc < 1) return JS_ThrowTypeError(ctx, "generate(image, opts): image required");

    // Start a fresh run: drop any cancel request left over from a prior call.
    g_cancelRequested.store(false, std::memory_order_relaxed);
    auto cancelled = [] { return g_cancelRequested.load(std::memory_order_relaxed); };

    // options
    int   seed = 42, steps = 20, numGaussians = 131072;
    float guidance = 3.0f, shift = 3.0f;
    // BiRefNet bg-removal runs whenever the matte model is loaded; opts can turn
    // it off per call (e.g. for an already-masked input) without a reload.
    bool  removeBackground = (w->rmbg != nullptr);
    if (argc > 1 && JS_IsObject(argv[1])) {
        tsGetInt(ctx, argv[1], "seed", seed);
        tsGetInt(ctx, argv[1], "steps", steps);
        tsGetInt(ctx, argv[1], "numGaussians", numGaussians);
        tsGetFloat(ctx, argv[1], "guidanceScale", guidance);
        tsGetFloat(ctx, argv[1], "shift", shift);
        JSValue rb = JS_GetPropertyStr(ctx, argv[1], "removeBackground");
        if (!JS_IsUndefined(rb)) removeBackground = JS_ToBool(ctx, rb);
        JS_FreeValue(ctx, rb);
    }

    // 1) read + preprocess the image (JS thread).
    std::vector<std::uint8_t> rgba;
    int iw = 0, ih = 0;
    std::string err;
    if (!tsReadImage(ctx, argv[0], rgba, iw, ih, err))
        return JS_ThrowTypeError(ctx, "triposplat: %s", err.c_str());
    // Optional BiRefNet bg-removal: replace the image alpha with the predicted
    // matte so the composite-over-black isolates the subject.
    if (w->rmbg && removeBackground) {
        try {
            std::vector<float> rgb(static_cast<std::size_t>(iw) * ih * 3);
            for (std::size_t i = 0; i < static_cast<std::size_t>(iw) * ih; ++i) {
                rgb[i * 3 + 0] = rgba[i * 4 + 0];
                rgb[i * 3 + 1] = rgba[i * 4 + 1];
                rgb[i * 3 + 2] = rgba[i * 4 + 2];
            }
            brn::Matte m = w->rmbg->removeBackground(rgb.data(), iw, ih, /*rgbIs255=*/true);
            for (std::size_t i = 0; i < m.alpha.size(); ++i)
                rgba[i * 4 + 3] = static_cast<std::uint8_t>(
                    std::min(std::max(m.alpha[i], 0.0f), 1.0f) * 255.0f + 0.5f);
        } catch (const std::exception& e) {
            return JS_ThrowTypeError(ctx, "triposplat: birefnet: %s", e.what());
        }
    }
    std::vector<float> rgb01;
    tsPreprocess(rgba, iw, ih, rgb01);

    try {
        const int HW = kCanvas * kCanvas;

        // 2a) DINOv3 input: normalized NCHW FP32 on the backbone's device.
        static const float kMean[3] = {0.485f, 0.456f, 0.406f};
        static const float kStd[3]  = {0.229f, 0.224f, 0.225f};
        std::vector<float> dino_in(static_cast<std::size_t>(3) * HW);
        for (int c = 0; c < 3; ++c)
            for (int i = 0; i < HW; ++i)
                dino_in[static_cast<std::size_t>(c) * HW + i] =
                    (rgb01[static_cast<std::size_t>(i) * 3 + c] - kMean[c]) / kStd[c];
        bt::Tensor dino_px = bt::Tensor::from_host_on(w->dino->device(), dino_in.data(), 1, 3 * HW);
        dv3::BackboneOutput dout = w->dino->encode(dino_px, kCanvas, kCanvas);
        bt::sync_all();
        if (cancelled()) throw tsp::SampleCancelled();

        // feature1 = affine-free LayerNorm over the 1280 channels (reference does
        // F.layer_norm(dinov3_feat, [-1])), at the compute dtype.
        std::vector<float> f1 = tsDownloadF32(dout.last_hidden_state);
        const int K  = dout.last_hidden_state.rows;
        const int D1 = dout.last_hidden_state.cols;   // 1280
        for (int r = 0; r < K; ++r) {
            float* row = &f1[static_cast<std::size_t>(r) * D1];
            float mean = 0.0f;
            for (int j = 0; j < D1; ++j) mean += row[j];
            mean /= D1;
            float var = 0.0f;
            for (int j = 0; j < D1; ++j) { const float d = row[j] - mean; var += d * d; }
            var /= D1;
            const float inv = 1.0f / std::sqrt(var + 1e-5f);
            for (int j = 0; j < D1; ++j) row[j] = (row[j] - mean) * inv;
        }
        bt::Tensor feature1 = tsUploadCompute(f1.data(), K, D1);

        // 2b) VAE input: (1, 3*HW) NCHW in [-1,1] at the compute dtype.
        std::vector<float> vae_in(static_cast<std::size_t>(3) * HW);
        for (int c = 0; c < 3; ++c)
            for (int i = 0; i < HW; ++i)
                vae_in[static_cast<std::size_t>(c) * HW + i] =
                    rgb01[static_cast<std::size_t>(i) * 3 + c] * 2.0f - 1.0f;
        bt::Tensor vae_px = tsUploadCompute(vae_in.data(), 1, 3 * HW);
        bt::Tensor vae_tok;
        w->vae->encode(vae_px, kCanvas, kCanvas, vae_tok);
        bt::sync_all();
        if (cancelled()) throw tsp::SampleCancelled();

        // feature2 = [5 zero tokens ; vae tokens] to align with feature1's
        // cls+4-register prefix; (K, 128) at the compute dtype.
        std::vector<float> vt = tsDownloadF32(vae_tok);
        const int Tvae = vae_tok.rows;       // (HW/256) = 4096
        const int D2   = vae_tok.cols;       // 128
        const int prefix = K - Tvae;         // 5 (cls + 4 registers)
        if (prefix < 0) throw std::runtime_error("feature token-count mismatch (dino < vae)");
        std::vector<float> f2(static_cast<std::size_t>(K) * D2, 0.0f);
        std::copy(vt.begin(), vt.end(), f2.begin() + static_cast<std::size_t>(prefix) * D2);
        bt::Tensor feature2 = tsUploadCompute(f2.data(), K, D2);

        // 3) seeded noise (our own RNG — deterministic; the reference's torch
        // randn is not reproducible in C++).
        const auto& fc = w->flow->config();
        std::mt19937_64 rng(static_cast<std::uint64_t>(static_cast<std::uint32_t>(seed)));
        std::normal_distribution<float> norm(0.0f, 1.0f);
        std::vector<float> nlat(static_cast<std::size_t>(fc.q_token_length) * fc.in_channels);
        for (float& v : nlat) v = norm(rng);
        std::vector<float> ncam(static_cast<std::size_t>(fc.cam_channels));
        for (float& v : ncam) v = norm(rng);
        bt::Tensor noise_lat = tsUploadCompute(nlat.data(), fc.q_token_length, fc.in_channels);
        bt::Tensor noise_cam = tsUploadCompute(ncam.data(), 1, fc.cam_channels);

        // 4) flow Euler CFG sampler -> clean latent.
        tsp::FlowSampleOptions sopts;
        sopts.steps = steps;
        sopts.guidance_scale = guidance;
        sopts.shift = shift;
        sopts.should_cancel = cancelled;   // abort between Euler steps on request
        bt::Tensor latent;
        tsp::sample_latent(*w->flow, feature1, feature2, noise_lat, noise_cam, sopts, latent);
        bt::sync_all();
        if (cancelled()) throw tsp::SampleCancelled();

        // 5) octree decode -> Gaussian cloud.
        tsp::GaussianSplats splats =
            w->decoder->decode(latent, numGaussians, static_cast<std::uint64_t>(static_cast<std::uint32_t>(seed)));

        // 6) return typed arrays (the GaussianSplats SoA already matches the
        // scene GaussianSplatNode / bromesh::GaussianSplatCloud convention).
        JSValue out = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, out, "positions", tsFloat32Array(ctx, splats.positions.data(), splats.positions.size()));
        JS_SetPropertyStr(ctx, out, "scales",    tsFloat32Array(ctx, splats.scales.data(), splats.scales.size()));
        JS_SetPropertyStr(ctx, out, "rotations", tsFloat32Array(ctx, splats.rotations.data(), splats.rotations.size()));
        JS_SetPropertyStr(ctx, out, "opacities", tsFloat32Array(ctx, splats.opacities.data(), splats.opacities.size()));
        JS_SetPropertyStr(ctx, out, "sh",        tsFloat32Array(ctx, splats.sh.data(), splats.sh.size()));
        JS_SetPropertyStr(ctx, out, "shDegree",  JS_NewInt32(ctx, splats.shDegree));
        JS_SetPropertyStr(ctx, out, "count",     JS_NewInt64(ctx, static_cast<int64_t>(splats.count())));
        return out;
    } catch (const tsp::SampleCancelled&) {
        // User-requested abort — not an error. Hand back a small marker the
        // caller can distinguish from a real cloud.
        JSValue out = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, out, "cancelled", JS_TRUE);
        return out;
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "triposplat.generate failed: %s", e.what());
    }
}

// bro.triposplat.cancel() — request that an in-flight generate() abort. Safe to
// call from any context/thread (e.g. the main thread while a Worker runs
// generate). No-op if nothing is running.
JSValue tsCancel(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    (void)ctx;
    g_cancelRequested.store(true, std::memory_order_relaxed);
    return JS_UNDEFINED;
}

void tsRegisterClass(JSContext* ctx) {
    qjsbind::Class<TripoSplatWrapper>(ctx, "TripoSplatPipeline", qjsbind::NoGlobal)
        .get("device", [](TripoSplatWrapper* w) {
            switch (w->device) {
                case bt::Device::CUDA:  return std::string("CUDA");
                case bt::Device::Metal: return std::string("Metal");
                default:                return std::string("CPU");
            }
        })
        // True when a BiRefNet matte model was loaded — i.e. generate() can
        // isolate the subject. Lets a UI gate its "remove background" control.
        .get("backgroundRemoval", [](TripoSplatWrapper* w) { return w->rmbg != nullptr; })
        .method_raw("generate", tsGenerate, 2);
}

// bro.triposplat.load({ dinov3, vae, flow, decoder, device })
JSValue tsLoad(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "load({ dinov3, vae, flow, decoder }) requires an options object");

    std::string p_dino, p_vae, p_flow, p_dec, dev;
    if (!tsGetStr(ctx, argv[0], "dinov3", p_dino) || !tsGetStr(ctx, argv[0], "vae", p_vae) ||
        !tsGetStr(ctx, argv[0], "flow", p_flow)   || !tsGetStr(ctx, argv[0], "decoder", p_dec))
        return JS_ThrowTypeError(ctx, "load: dinov3, vae, flow and decoder paths are all required");

    try {
        // init() FIRST — the CUDA/Metal backends only register (and become
        // is_available / default_device candidates) after the driver probe, so
        // selecting the device before init() would always fall back to CPU.
        bt::init();
        bt::Device device = tsAutoDevice();   // best available now that init ran
        if (tsGetStr(ctx, argv[0], "device", dev)) {
            if (dev == "cpu") device = bt::Device::CPU;
            else if (dev == "cuda") device = bt::Device::CUDA;
            else if (dev == "metal") device = bt::Device::Metal;
        }
        bt::set_default_device(device);   // brodiffusion models load at compute dtype here

        auto w = std::make_unique<TripoSplatWrapper>();
        w->device = device;

        w->dino = std::make_unique<dv3::Backbone>(dv3::Config::vit_h());
        w->dino->load_file(p_dino);
        w->dino->to(device);

        w->vae = std::make_unique<tsp::Flux2VaeEncoder>();
        { st::File f = st::File::open(p_vae); w->vae->load_weights(f); }

        w->flow = std::make_unique<tsp::FlowDiT>();
        { st::File f = st::File::open(p_flow); w->flow->load_weights(f); }

        w->decoder = std::make_unique<tsp::OctreeGaussianDecoder>();
        { st::File f = st::File::open(p_dec); w->decoder->load_weights(f); }

        // Optional BiRefNet background removal (preprocessor). When given, the
        // input image's alpha is replaced by BiRefNet's predicted matte before
        // the cover-fit / composite-over-black step.
        std::string p_rmbg;
        if (tsGetStr(ctx, argv[0], "birefnet", p_rmbg) && !p_rmbg.empty()) {
            w->rmbg = std::make_unique<brn::BiRefNet>();
            w->rmbg->load(p_rmbg);
            w->rmbg->to(device);
        }

        return qjsbind::wrap<TripoSplatWrapper>(ctx, w.release());
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "triposplat.load failed: %s", e.what());
    }
}

JSValue tsInit(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    try { bt::init(); } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "triposplat.init failed: %s", e.what());
    }
    return JS_UNDEFINED;
}

}  // namespace

void installTriposplatBindings(JSContext* ctx) {
    tsRegisterClass(ctx);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj)) {
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue ns = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ns, "init", JS_NewCFunction(ctx, tsInit, "init", 0));
    JS_SetPropertyStr(ctx, ns, "load", JS_NewCFunction(ctx, tsLoad, "load", 1));
    JS_SetPropertyStr(ctx, ns, "cancel", JS_NewCFunction(ctx, tsCancel, "cancel", 0));
    JS_SetPropertyStr(ctx, broObj, "triposplat", ns);

    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

}  // namespace bro::js
