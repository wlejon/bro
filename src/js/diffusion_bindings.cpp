// JS bindings for brodiffusion — Stable Diffusion 1.5 text-to-image inference.
//
// Installed onto bro.diffusion.* by installDiffusionBindings(). The native
// Pipeline (which owns the multi-GB CLIP/U-Net/VAE weights) lives behind an
// opaque qjsbind handle — JS never holds or moves weight bytes, only the
// handle. The same binding is installed in the main context and in each
// worker context; a worker owns its own Pipeline and only plain cloneable
// data (prompt/opts in, {width,height,data} image out) crosses postMessage.
//
// brodiffusion's CPU backend is always built, so this binding is always real
// — it is NOT gated on BROTENSOR_HAS_GPU and does not touch the GPU-only
// tensor_bindings_internal.h. Intermediate tensors (latents, attention maps)
// are small and download to JS Float32Arrays on demand.
//
// This TU holds the Pipeline class (one-shot generation). The step-wise
// PipelineState class is added alongside it.

#include "js/diffusion_bindings.h"

#include <qjsbind/qjsbind.h>

#include <brodiffusion/pipeline.h>
#include <brodiffusion/safetensors.h>
#include <brodiffusion/scheduler.h>
#include <brodiffusion/lcm_scheduler.h>
#include <brodiffusion/tokenizer.h>
#include <brodiffusion/unet.h>
#include <brodiffusion/version.h>

#include <brotensor/runtime.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace bdp   = brodiffusion::pipeline;
namespace bds   = brodiffusion::safetensors;
namespace bdc   = brodiffusion::clip;
namespace bdsch = brodiffusion::scheduler;

namespace bro::js {

// ═══════════════════════════════════════════════════════════════════════════
// Wrapper structs (opaque handles)
// ═══════════════════════════════════════════════════════════════════════════

struct PipelineWrapper {
    std::unique_ptr<bdp::Pipeline> pipeline;
    bool weights_loaded = false;   // guards generate()/prime() before loadWeights
};

// Cross-attention block count is only meaningful once weights are loaded —
// num_xattn_blocks() counts the per-block Transformer2D vectors, which
// load_weights() populates. Always query it live.
static int xattnBlocks(const PipelineWrapper* w) {
    return w->pipeline->unet().num_xattn_blocks();
}

// ═══════════════════════════════════════════════════════════════════════════
// Small JS-value helpers
// ═══════════════════════════════════════════════════════════════════════════

static bool getStr(JSContext* ctx, JSValueConst obj, const char* key,
                   std::string& out) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    bool ok = false;
    if (JS_IsString(v)) {
        const char* s = JS_ToCString(ctx, v);
        if (s) { out = s; JS_FreeCString(ctx, s); ok = true; }
    }
    JS_FreeValue(ctx, v);
    return ok;
}

static bool argStr(JSContext* ctx, JSValueConst v, std::string& out) {
    if (!JS_IsString(v)) return false;
    const char* s = JS_ToCString(ctx, v);
    if (!s) return false;
    out = s;
    JS_FreeCString(ctx, s);
    return true;
}

static void getInt(JSContext* ctx, JSValueConst obj, const char* key, int& dst) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsNumber(v)) { int32_t t = dst; JS_ToInt32(ctx, &t, v); dst = t; }
    JS_FreeValue(ctx, v);
}

static void getNum(JSContext* ctx, JSValueConst obj, const char* key, float& dst) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsNumber(v)) { double t = dst; JS_ToFloat64(ctx, &t, v); dst = (float)t; }
    JS_FreeValue(ctx, v);
}

static bool getBool(JSContext* ctx, JSValueConst obj, const char* key,
                    bool def = false) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    bool out = def;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) out = JS_ToBool(ctx, v) == 1;
    JS_FreeValue(ctx, v);
    return out;
}

// uint64 seed: a plain JS number (lossless to 2^53) or a BigInt (full range).
static void getSeed(JSContext* ctx, JSValueConst obj, const char* key,
                    std::uint64_t& dst) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsBigInt(v)) {
        uint64_t u = dst;
        if (JS_ToBigUint64(ctx, &u, v) == 0) dst = u;
    } else if (JS_IsNumber(v)) {
        int64_t t = (int64_t)dst;
        if (JS_ToInt64(ctx, &t, v) == 0) dst = (std::uint64_t)t;
    }
    JS_FreeValue(ctx, v);
}

// Map a JS opts object onto brodiffusion's GenerateOptions (defaults preserved
// for absent keys). Shared by generate() and prime().
static bdp::GenerateOptions parseGenerateOptions(JSContext* ctx, JSValueConst v) {
    bdp::GenerateOptions o;
    if (!JS_IsObject(v)) return o;
    getInt(ctx, v, "width",  o.width);
    getInt(ctx, v, "height", o.height);
    getInt(ctx, v, "steps",  o.num_inference_steps);
    getNum(ctx, v, "guidanceScale", o.guidance_scale);
    getStr(ctx, v, "negativePrompt", o.negative_prompt);
    getSeed(ctx, v, "seed", o.seed);
    return o;
}

// ═══════════════════════════════════════════════════════════════════════════
// Image result: brodiffusion's 3*H*W FP32 NCHW [-1,1] → canvas-ready RGBA
// ═══════════════════════════════════════════════════════════════════════════

// Returns { width, height, data: Uint8ClampedArray(4*H*W, RGBA HWC) }, drop-in
// for createImageData()+putImageData(). With includeFp32, also attaches the raw
// NCHW FP32 buffer as `fp32`. The planar→interleaved + rescale loop runs in C++.
static JSValue makeImageResult(JSContext* ctx, const std::vector<float>& nchw,
                               int H, int W, bool includeFp32) {
    const int plane = H * W;
    std::vector<std::uint8_t> rgba((size_t)4 * plane);
    for (int i = 0; i < plane; ++i) {
        for (int c = 0; c < 3; ++c) {
            float v = (nchw[(size_t)c * plane + i] * 0.5f + 0.5f) * 255.0f;
            if (v < 0.0f) v = 0.0f; else if (v > 255.0f) v = 255.0f;
            rgba[(size_t)4 * i + c] = (std::uint8_t)(v + 0.5f);
        }
        rgba[(size_t)4 * i + 3] = 255;
    }
    JSValue abuf = JS_NewArrayBufferCopy(ctx, rgba.data(), rgba.size());
    // JS_NewTypedArray reads argv[1]/argv[2] (byteOffset/length) even with
    // argc=1, so the array must hold 3 slots padded with JS_UNDEFINED — a
    // 1-slot array causes out-of-bounds reads and a zero-length result.
    JSValue taArgs[3] = { abuf, JS_UNDEFINED, JS_UNDEFINED };
    JSValue data = JS_NewTypedArray(ctx, 1, taArgs, JS_TYPED_ARRAY_UINT8C);
    JS_FreeValue(ctx, abuf);

    JSValue res = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, res, "width",  JS_NewInt32(ctx, W));
    JS_SetPropertyStr(ctx, res, "height", JS_NewInt32(ctx, H));
    JS_SetPropertyStr(ctx, res, "data",   data);
    if (includeFp32)
        JS_SetPropertyStr(ctx, res, "fp32", qjsbind::make_float32_array(ctx, nchw));
    return res;
}

// ═══════════════════════════════════════════════════════════════════════════
// bro.diffusion free functions
// ═══════════════════════════════════════════════════════════════════════════

// bro.diffusion.init() — brotensor::init() (idempotent, thread-safe). Optional;
// createPipeline() also calls it. Exposed so a worker can warm up explicitly.
static JSValue js_init(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    try {
        brotensor::init();
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "bro.diffusion.init: %s", e.what());
    }
    return JS_UNDEFINED;
}

// bro.diffusion.createPipeline(opts) -> Pipeline
//   opts.vocabPath        (string, required) — CLIP vocab.json
//   opts.mergesPath       (string, required) — CLIP merges.txt
//   opts.scheduler        "ddim" (default) | "lcm"
//   opts.lcmDistilled     bool — LCM-distilled checkpoint (time_cond_proj_dim=256)
//   opts.quantizeWeights  bool — INT8 U-Net weights (GPU-only; NOP on CPU)
static JSValue js_createPipeline(JSContext* ctx, JSValueConst, int argc,
                                 JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "createPipeline(opts): opts object required");

    std::string vocabPath, mergesPath;
    if (!getStr(ctx, argv[0], "vocabPath", vocabPath))
        return JS_ThrowTypeError(ctx, "createPipeline: opts.vocabPath (string) required");
    if (!getStr(ctx, argv[0], "mergesPath", mergesPath))
        return JS_ThrowTypeError(ctx, "createPipeline: opts.mergesPath (string) required");

    std::string scheduler = "ddim";
    getStr(ctx, argv[0], "scheduler", scheduler);
    const bool lcm          = (scheduler == "lcm");
    const bool lcmDistilled = getBool(ctx, argv[0], "lcmDistilled");
    const bool quantize     = getBool(ctx, argv[0], "quantizeWeights");

    try {
        brotensor::init();
        bdc::Tokenizer tok = bdc::Tokenizer::load(vocabPath, mergesPath);

        bdp::PipelineConfig cfg;  // SD1.5 defaults
        if (lcm) {
            cfg.scheduler = bdsch::LCMConfig{};
            if (lcmDistilled) cfg.unet.time_cond_proj_dim = 256;
        }
        cfg.unet.quantize_weights = quantize;

        auto w = std::make_unique<PipelineWrapper>();
        w->pipeline = std::make_unique<bdp::Pipeline>(cfg, std::move(tok));
        return qjsbind::wrap<PipelineWrapper>(ctx, w.release());
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "createPipeline: %s", e.what());
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Pipeline class methods
// ═══════════════════════════════════════════════════════════════════════════

static PipelineWrapper* pipelineSelf(JSContext* ctx, JSValueConst this_val) {
    return qjsbind::unwrap<PipelineWrapper>(ctx, this_val);
}

// loadWeights(path)                              — single SD1.5 checkpoint
// loadWeights(path, {textPrefix,unetPrefix,vaePrefix}) — single file, custom prefixes
// loadWeights(textPath, unetPath, vaePath)       — diffusers 3-file export
static JSValue js_pipeline_loadWeights(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "loadWeights: not a Pipeline");

    std::string p0;
    if (argc < 1 || !argStr(ctx, argv[0], p0))
        return JS_ThrowTypeError(ctx, "loadWeights: expected path string(s)");

    try {
        if (argc >= 3) {
            std::string p1, p2;
            if (!argStr(ctx, argv[1], p1) || !argStr(ctx, argv[2], p2))
                return JS_ThrowTypeError(ctx,
                    "loadWeights(textPath, unetPath, vaePath): all three must be strings");
            bds::File tf = bds::File::open(p0);
            bds::File uf = bds::File::open(p1);
            bds::File vf = bds::File::open(p2);
            w->pipeline->load_weights(tf, uf, vf);
        } else if (argc == 2 && JS_IsObject(argv[1])) {
            std::string tp, up, vp;
            getStr(ctx, argv[1], "textPrefix", tp);
            getStr(ctx, argv[1], "unetPrefix", up);
            getStr(ctx, argv[1], "vaePrefix",  vp);
            bds::File f = bds::File::open(p0);
            w->pipeline->load_weights(f, tp, up, vp);
        } else {
            bds::File f = bds::File::open(p0);
            w->pipeline->load_weights(f);
        }
        w->weights_loaded = true;
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "loadWeights: %s", e.what());
    }
    return JS_UNDEFINED;
}

// applyLora(path, scale=1.0) — merge a LoRA into the loaded weights. Must be
// called after loadWeights(); stackable.
static JSValue js_pipeline_applyLora(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "applyLora: not a Pipeline");
    if (!w->weights_loaded)
        return JS_ThrowInternalError(ctx, "applyLora: call loadWeights() first");

    std::string path;
    if (argc < 1 || !argStr(ctx, argv[0], path))
        return JS_ThrowTypeError(ctx, "applyLora(path, scale?): path string required");
    double scale = 1.0;
    if (argc >= 2 && JS_IsNumber(argv[1])) JS_ToFloat64(ctx, &scale, argv[1]);

    try {
        bds::File f = bds::File::open(path);
        w->pipeline->apply_lora(f, (float)scale);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "applyLora: %s", e.what());
    }
    return JS_UNDEFINED;
}

// generate(prompt, opts) -> { width, height, data } — one-shot txt2img.
// Blocking; intended for a worker thread. The main thread should drive the
// step-wise prime()/stepOnce()/decode() API instead.
static JSValue js_pipeline_generate(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "generate: not a Pipeline");
    if (!w->weights_loaded)
        return JS_ThrowInternalError(ctx, "generate: call loadWeights() first");

    std::string prompt;
    if (argc < 1 || !argStr(ctx, argv[0], prompt))
        return JS_ThrowTypeError(ctx, "generate(prompt, opts?): prompt string required");
    JSValueConst optsv = (argc >= 2) ? argv[1] : JS_UNDEFINED;
    bdp::GenerateOptions opts = parseGenerateOptions(ctx, optsv);
    const bool includeFp32 = JS_IsObject(optsv) && getBool(ctx, optsv, "includeFp32");

    try {
        std::vector<float> img = w->pipeline->generate(prompt, opts);
        return makeImageResult(ctx, img, opts.height, opts.width, includeFp32);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "generate: %s", e.what());
    }
}

// numXAttnBlocks() -> number — cross-attention block count (16 for SD1.5).
// Meaningful only after loadWeights(); returns 1 before.
static JSValue js_pipeline_numXAttnBlocks(JSContext* ctx, JSValueConst this_val,
                                          int, JSValueConst*) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "numXAttnBlocks: not a Pipeline");
    return JS_NewInt32(ctx, xattnBlocks(w));
}

// config() -> object — read-only snapshot of the resolved PipelineConfig.
static JSValue js_pipeline_config(JSContext* ctx, JSValueConst this_val,
                                  int, JSValueConst*) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "config: not a Pipeline");
    const bdp::PipelineConfig& cfg = w->pipeline->config();
    const bool lcm = std::holds_alternative<bdsch::LCMConfig>(cfg.scheduler);

    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "scheduler", JS_NewString(ctx, lcm ? "lcm" : "ddim"));
    JS_SetPropertyStr(ctx, o, "timeCondProjDim",
                      JS_NewInt32(ctx, cfg.unet.time_cond_proj_dim));
    JS_SetPropertyStr(ctx, o, "quantizeWeights",
                      JS_NewBool(ctx, cfg.unet.quantize_weights));
    JS_SetPropertyStr(ctx, o, "numXAttnBlocks",
                      JS_NewInt32(ctx, xattnBlocks(w)));
    JS_SetPropertyStr(ctx, o, "weightsLoaded", JS_NewBool(ctx, w->weights_loaded));
    return o;
}

static void registerPipelineClass(JSContext* ctx) {
    qjsbind::Class<PipelineWrapper>(ctx, "DiffusionPipeline", qjsbind::NoGlobal)
        .method_raw("loadWeights",     js_pipeline_loadWeights,     3)
        .method_raw("applyLora",       js_pipeline_applyLora,       2)
        .method_raw("generate",        js_pipeline_generate,        2)
        .method_raw("numXAttnBlocks",  js_pipeline_numXAttnBlocks,  0)
        .method_raw("config",          js_pipeline_config,          0);
}

// ═══════════════════════════════════════════════════════════════════════════
// Install
// ═══════════════════════════════════════════════════════════════════════════

void installDiffusionBindings(JSContext* ctx) {
    registerPipelineClass(ctx);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        JS_FreeValue(ctx, broObj);
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue diff = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, diff, "version",
                      JS_NewString(ctx, brodiffusion::version_string()));
    JS_SetPropertyStr(ctx, diff, "init",
                      JS_NewCFunction(ctx, js_init, "init", 0));
    JS_SetPropertyStr(ctx, diff, "createPipeline",
                      JS_NewCFunction(ctx, js_createPipeline, "createPipeline", 1));
    JS_SetPropertyStr(ctx, broObj, "diffusion", diff);

    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

void cleanupDiffusionBindings(JSContext* /*ctx*/) {
    // No-op: qjsbind owns the class finalizers; bro.diffusion is reached from
    // globalThis and swept by runtime teardown.
}

} // namespace bro::js
