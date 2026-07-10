#if BRO_WITH_DIFFUSION
// JS bindings for brodiffusion — diffusion-model text-to-image inference.
//
// Installed onto bro.diffusion.* by installDiffusionBindings(). The native
// Pipeline (which owns the multi-GB model weights) lives behind an opaque
// qjsbind handle — JS never holds or moves weight bytes, only the handle.
// The same binding is installed in the main context and in each
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
#include "util/interrupt.h"
#include "util/asset_mounts.h"

#include <qjsbind/qjsbind.h>

#include <brodiffusion/pipeline.h>
#include <brodiffusion/controlnet.h>
#include <brotensor/safetensors.h>
#include <brodiffusion/scheduler.h>
#include <brodiffusion/lcm_scheduler.h>
#include <brodiffusion/flow_match_scheduler.h>
#include <brodiffusion/scm_scheduler.h>
#include <brodiffusion/model_config.h>
#include <brolm/tokenizer.h>
#include <brodiffusion/unet.h>
#include <brodiffusion/version.h>

#include <brotensor/ops/elementwise.h>
#include <brotensor/runtime.h>
#include <brotensor/tensor.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace bdp   = brodiffusion::pipeline;
namespace bds   = brotensor::safetensors;
namespace bdc   = brolm::clip;
namespace bdsch = brodiffusion::scheduler;

namespace bro::js {

// ═══════════════════════════════════════════════════════════════════════════
// Wrapper structs (opaque handles)
// ═══════════════════════════════════════════════════════════════════════════

struct PipelineWrapper {
    std::unique_ptr<bdp::Pipeline> pipeline;
    bool weights_loaded = false;   // guards generate()/prime() before loadWeights
};

// Denoiser-generic count of traceable / steerable cross-attention blocks for
// the loaded model. Delegates to Pipeline::num_xattn_blocks(), which routes to
// the active denoiser: 16 for the SD1.5 UNet, 57 for the Flux DiT (19
// double-stream + 38 single-stream joint-attention blocks), 0 for a denoiser
// with no trace support. The count is only meaningful once weights are loaded
// — it reflects the per-block attention vectors populated by load_weights() —
// so always query it live.
static int xattnBlocks(const PipelineWrapper* w) {
    return w->pipeline->num_xattn_blocks();
}

// A mid-generation state: the working latent + scheduler progress. The
// GenerateOptions captured at prime() are stored so stepOnce()/decode() need
// no re-passing. The owning Pipeline is kept alive by a non-enumerable
// __pipeline JS property on the wrapper object (set by prime()/clone()).
struct PipelineStateWrapper {
    bdp::PipelineState  state;
    bdp::GenerateOptions opts;
};

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

// ── app-relative path resolution ───────────────────────────────────────────
// Set per app load by the engine (and inherited by workers). Lets apps point at
// model dirs, weight files, LoRAs and control dictionaries with `/app/...` and
// app-relative paths instead of brittle absolute machine paths. Mirrors the
// rules in image_bindings / scene_bindings: Windows drive paths and absolute
// paths pass through; leading-slash paths consult engine mounts (`/app`, `/lib`,
// …); everything else is relative to the app directory.
// thread_local because the same binding is installed on the main thread and on
// each worker thread (a worker owns its own Pipeline and resolves its own
// paths). Per-thread state keeps the contexts independent and avoids any
// cross-thread race on these globals — no lock needed.
static thread_local std::string s_basePath;
static thread_local const util::AssetMounts* s_mounts = nullptr;

static std::string resolveAppPath(const std::string& src) {
    if (src.size() >= 2 && src[1] == ':') return src;          // C:\... drive
    if (!src.empty() && (src[0] == '/' || src[0] == '\\')) {
        if (s_mounts) {
            std::string m = s_mounts->resolve(src);
            if (!m.empty()) return m;
        }
        return src;
    }
    if (s_basePath.empty()) return src;
    std::string path = s_basePath;
    if (path.back() != '/' && path.back() != '\\') path += '/';
    return path + src;
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


// Download a brotensor::Tensor to host FP32, converting FP16 bits as needed.
// brodiffusion tensors carry the compute dtype — FP16 on a GPU backend.
static std::vector<float> downloadTensorFloats(const brotensor::Tensor& t) {
    if (t.dtype == brotensor::Dtype::FP16) {
        std::vector<std::uint16_t> bits = t.to_host_vector_fp16();
        std::vector<float> out(bits.size());
        for (std::size_t i = 0; i < bits.size(); ++i)
            out[i] = brotensor::fp16_bits_to_fp32(bits[i]);
        return out;
    }
    if (t.dtype == brotensor::Dtype::BF16) {
        std::vector<std::uint16_t> bits = t.to_host_vector_bf16();
        std::vector<float> out(bits.size());
        for (std::size_t i = 0; i < bits.size(); ++i)
            out[i] = brotensor::bf16_bits_to_fp32(bits[i]);
        return out;
    }
    return t.to_host_vector();
}

// Map a JS opts object onto brodiffusion's GenerateOptions (defaults preserved
// for absent keys). Shared by generate() and prime().
//
// The `controls` array maps one-to-one onto registered ControlNets — its
// length must equal pipeline.numControlNets() at prime time. brodiffusion
// validates this and throws clearly, so we don't pre-check here.
static bdp::GenerateOptions parseGenerateOptions(JSContext* ctx, JSValueConst v) {
    bdp::GenerateOptions o;
    if (!JS_IsObject(v)) return o;
    getInt(ctx, v, "width",  o.width);
    getInt(ctx, v, "height", o.height);
    getInt(ctx, v, "steps",  o.num_inference_steps);
    getNum(ctx, v, "guidanceScale", o.guidance_scale);
    getStr(ctx, v, "negativePrompt", o.negative_prompt);
    getSeed(ctx, v, "seed", o.seed);

    // ── img2img / inpaint ────────────────────────────────────────────────
    getStr(ctx, v, "initImagePath", o.init_image_path);
    getNum(ctx, v, "strength",      o.strength);
    o.vae_encode_sample = getBool(ctx, v, "vaeEncodeSample", o.vae_encode_sample);
    getStr(ctx, v, "maskImagePath", o.mask_image_path);

    // ── noise source ─────────────────────────────────────────────────────
    // 'internal' (default) or 'torch'. Ignored when initNoise / initImagePath
    // is set; brodiffusion documents the precedence.
    {
        std::string ns;
        if (getStr(ctx, v, "noiseSource", ns)) {
            if (ns == "torch")    o.noise_source = bdp::NoiseSource::Torch;
            else if (ns == "internal") o.noise_source = bdp::NoiseSource::Internal;
        }
    }
    // initNoise: Float32Array of raw N(0,1) values, NCHW flat. Copied out of
    // the typed array into the vector brodiffusion will consume.
    {
        JSValue nv = JS_GetPropertyStr(ctx, v, "initNoise");
        if (!JS_IsUndefined(nv) && !JS_IsNull(nv)) {
            std::size_t cnt = 0;
            const float* fp = qjsbind::read_float32_view(ctx, nv, cnt);
            if (fp && cnt > 0) o.init_noise.assign(fp, fp + cnt);
        }
        JS_FreeValue(ctx, nv);
    }

    // ── ControlNet inputs ────────────────────────────────────────────────
    // [{ imagePath, scale?, startStep?, endStep? }, ...]; one entry per
    // registered net. Defaults from brodiffusion's ControlNetInput are kept
    // when fields are absent (scale=1.0, startStep=0.0, endStep=1.0).
    {
        JSValue cv = JS_GetPropertyStr(ctx, v, "controls");
        if (JS_IsArray(cv)) {
            std::uint32_t n = 0;
            JSValue lenV = JS_GetPropertyStr(ctx, cv, "length");
            JS_ToUint32(ctx, &n, lenV);
            JS_FreeValue(ctx, lenV);
            o.controls.reserve(n);
            for (std::uint32_t i = 0; i < n; ++i) {
                JSValue e = JS_GetPropertyUint32(ctx, cv, i);
                bdp::ControlNetInput ci;
                if (JS_IsObject(e)) {
                    getStr(ctx, e, "imagePath", ci.image_path);
                    getNum(ctx, e, "scale",     ci.scale);
                    getNum(ctx, e, "startStep", ci.start_step);
                    getNum(ctx, e, "endStep",   ci.end_step);
                }
                o.controls.push_back(std::move(ci));
                JS_FreeValue(ctx, e);
            }
        }
        JS_FreeValue(ctx, cv);
    }
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

        bdp::PipelineConfig cfg;  // model defaults
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

// bro.diffusion.loadModel(modelDir, opts?) -> Pipeline
//   Load a complete diffusers model directory — `model_index.json` plus one
//   component subdir each (text_encoder/, unet/ or transformer/, vae/,
//   tokenizer/, scheduler/, ...). The model family is auto-detected from
//   `_class_name`: a StableDiffusion directory builds the CLIP + UNet stack, a
//   Flux directory builds CLIP (pooled) + the T5-XXL encoder + the Flux DiT.
//   Every weight and tokenizer is loaded here — the returned Pipeline is fully
//   ready, so call generate()/prime() directly (no loadWeights()/createPipeline).
//   opts.quantizeWeights — INT8 weight-only (W8A16) for every quantizable
//   component the loaded family has (Flux transformer + T5-XXL; Krea 2's DiT
//   *and* its Qwen3-VL-4B text/vision encoder — both, so a checkpoint whose
//   FP16 total doesn't fit a single GPU's VRAM still loads whole). GPU-only,
//   ignored (with a warning) on the CPU backend. See
//   brodiffusion::Pipeline::ModelDirOptions.
static JSValue js_loadModel(JSContext* ctx, JSValueConst, int argc,
                            JSValueConst* argv) {
    std::string dir;
    if (argc < 1 || !argStr(ctx, argv[0], dir))
        return JS_ThrowTypeError(ctx, "loadModel(modelDir, opts?): path string required");
    JSValueConst optsv = (argc >= 2) ? argv[1] : JS_UNDEFINED;
    bdp::Pipeline::ModelDirOptions dirOpts;
    if (JS_IsObject(optsv)) dirOpts.quantize = getBool(ctx, optsv, "quantizeWeights");
    // Cooperative cancellation: a Krea 2 load reads ~26GB on the worker thread —
    // one long synchronous native call the QuickJS interrupt can't break. Poll
    // the process-wide interrupt (set by Ctrl+C / window close / engine teardown)
    // so closing the app mid-load abandons the read promptly instead of blocking
    // the worker-join until every byte finishes. from_model_dir throws
    // LoadCancelled; report it as a plain cancel signal (no pixels/pipeline).
    dirOpts.should_cancel = bro::util::interrupted;
    try {
        brotensor::init();
        auto w = std::make_unique<PipelineWrapper>();
        // from_model_dir() returns by value; Pipeline is move-only, so the
        // temporary moves into the heap-allocated wrapper slot.
        w->pipeline = std::make_unique<bdp::Pipeline>(
            bdp::Pipeline::from_model_dir(resolveAppPath(dir), dirOpts));
        w->weights_loaded = true;
        return qjsbind::wrap<PipelineWrapper>(ctx, w.release());
    } catch (const brodiffusion::LoadCancelled&) {
        JSValue out = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, out, "cancelled", JS_TRUE);
        return out;
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "loadModel: %s", e.what());
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Pipeline class methods
// ═══════════════════════════════════════════════════════════════════════════

static PipelineWrapper* pipelineSelf(JSContext* ctx, JSValueConst this_val) {
    return qjsbind::unwrap<PipelineWrapper>(ctx, this_val);
}

// loadWeights(path)                              — single-file checkpoint
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
            bds::File tf = bds::File::open(resolveAppPath(p0));
            bds::File uf = bds::File::open(resolveAppPath(p1));
            bds::File vf = bds::File::open(resolveAppPath(p2));
            w->pipeline->load_weights(tf, uf, vf);
        } else if (argc == 2 && JS_IsObject(argv[1])) {
            std::string tp, up, vp;
            getStr(ctx, argv[1], "textPrefix", tp);
            getStr(ctx, argv[1], "unetPrefix", up);
            getStr(ctx, argv[1], "vaePrefix",  vp);
            bds::File f = bds::File::open(resolveAppPath(p0));
            w->pipeline->load_weights(f, tp, up, vp);
        } else {
            bds::File f = bds::File::open(resolveAppPath(p0));
            w->pipeline->load_weights(f);
        }
        w->weights_loaded = true;
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "loadWeights: %s", e.what());
    }
    return JS_UNDEFINED;
}

// applyLora(path, scale=1.0) — apply a LoRA to the loaded weights. SD1.5
// merges the deltas; Krea 2 attaches the file as a runtime-adapter group
// (rescalable via setLoraScale, removable via clearLoras). Must be called
// after loadWeights() / loadModel(); stackable.
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
        bds::File f = bds::File::open(resolveAppPath(path));
        w->pipeline->apply_lora(f, (float)scale);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "applyLora: %s", e.what());
    }
    return JS_UNDEFINED;
}

// setLoraScale(index, scale) — change a runtime LoRA group's user multiplier
// (0 disables it). `index` is the applyLora() call order, 0-based. Krea 2
// only: SD1.5 LoRAs are merged irreversibly.
static JSValue js_pipeline_setLoraScale(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "setLoraScale: not a Pipeline");
    int32_t index = 0;
    double scale = 1.0;
    if (argc < 2 || JS_ToInt32(ctx, &index, argv[0]) ||
        JS_ToFloat64(ctx, &scale, argv[1]))
        return JS_ThrowTypeError(ctx, "setLoraScale(index, scale) required");
    try {
        w->pipeline->set_lora_scale(index, (float)scale);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "setLoraScale: %s", e.what());
    }
    return JS_UNDEFINED;
}

// clearLoras() — drop every runtime LoRA group. Krea 2 only.
static JSValue js_pipeline_clearLoras(JSContext* ctx, JSValueConst this_val,
                                      int /*argc*/, JSValueConst* /*argv*/) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "clearLoras: not a Pipeline");
    try {
        w->pipeline->clear_loras();
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "clearLoras: %s", e.what());
    }
    return JS_UNDEFINED;
}

// numLoras() -> number — count of attached runtime LoRA groups. Krea 2 only.
static JSValue js_pipeline_numLoras(JSContext* ctx, JSValueConst this_val,
                                    int /*argc*/, JSValueConst* /*argv*/) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "numLoras: not a Pipeline");
    try {
        return JS_NewInt32(ctx, w->pipeline->num_loras());
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "numLoras: %s", e.what());
    }
}

// addControlNet(path) -> number
// addControlNet(path, cfg) -> number
//   Register a ControlNet safetensors file. Returns the index used in
//   GenerateOptions.controls. `cfg` is the optional ControlNetConfig — only
//   needed for non-default checkpoint shapes; today we only forward a
//   couple of fields that matter for SD1.5 zoo variants.
// SD1.5 only — brodiffusion throws on Flux.
static JSValue js_pipeline_addControlNet(JSContext* ctx, JSValueConst this_val,
                                         int argc, JSValueConst* argv) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "addControlNet: not a Pipeline");
    if (!w->weights_loaded)
        return JS_ThrowInternalError(ctx,
            "addControlNet: call loadWeights() / loadModel() first");

    std::string path;
    if (argc < 1 || !argStr(ctx, argv[0], path))
        return JS_ThrowTypeError(ctx,
            "addControlNet(path, cfg?): path string required");

    try {
        bds::File f = bds::File::open(resolveAppPath(path));
        int idx;
        if (argc >= 2 && JS_IsObject(argv[1])) {
            brodiffusion::controlnet::ControlNetConfig cfg;
            getInt(ctx, argv[1], "inChannels",          cfg.in_channels);
            getInt(ctx, argv[1], "controlChannels",     cfg.control_channels);
            getInt(ctx, argv[1], "layersPerBlock",      cfg.layers_per_block);
            getInt(ctx, argv[1], "crossAttentionDim",   cfg.cross_attention_dim);
            getInt(ctx, argv[1], "transformerNumHeads", cfg.transformer_num_heads);
            idx = w->pipeline->add_controlnet(f, cfg);
        } else {
            idx = w->pipeline->add_controlnet(f);
        }
        return JS_NewInt32(ctx, idx);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "addControlNet: %s", e.what());
    }
}

// removeControlNet(index) — drop one registered ControlNet; subsequent
// indices shift down.
static JSValue js_pipeline_removeControlNet(JSContext* ctx, JSValueConst this_val,
                                            int argc, JSValueConst* argv) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "removeControlNet: not a Pipeline");
    int32_t idx = -1;
    if (argc < 1 || JS_ToInt32(ctx, &idx, argv[0]) != 0)
        return JS_ThrowTypeError(ctx,
            "removeControlNet(index): integer index required");
    try {
        w->pipeline->remove_controlnet(idx);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "removeControlNet: %s", e.what());
    }
    return JS_UNDEFINED;
}

// clearControlNets() — drop every registered ControlNet.
static JSValue js_pipeline_clearControlNets(JSContext* ctx, JSValueConst this_val,
                                            int, JSValueConst*) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "clearControlNets: not a Pipeline");
    try {
        w->pipeline->clear_controlnets();
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "clearControlNets: %s", e.what());
    }
    return JS_UNDEFINED;
}

// loadControlDictionary(path) — load a conditioning-space control dictionary
// (a BCD1 file of named direction axes built offline). Replaces any loaded
// axes and resets weights. The axes steer generate()/prime() once weighted via
// setControl(). The dictionary's dim must match the model's text encoder (Gemma
// 2304 for Sana); a mismatch throws at the next generate()/prime().
static JSValue js_pipeline_loadControlDictionary(JSContext* ctx, JSValueConst this_val,
                                                 int argc, JSValueConst* argv) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "loadControlDictionary: not a Pipeline");
    std::string path;
    if (argc < 1 || !argStr(ctx, argv[0], path))
        return JS_ThrowTypeError(ctx, "loadControlDictionary(path): path string required");
    try {
        w->pipeline->cond_control().load(resolveAppPath(path));
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "loadControlDictionary: %s", e.what());
    }
    return JS_UNDEFINED;
}

// setControl(name, alpha) or setControl({name: alpha, ...}) — set per-axis
// control weights (natural units; the applied vector is alpha * scale * dir).
// Unknown axis names throw. Weights persist until changed or clearControl().
static JSValue js_pipeline_setControl(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "setControl: not a Pipeline");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "setControl(name, alpha) or setControl(map) required");

    // Form 1: (string name, number alpha).
    std::string name;
    if (argStr(ctx, argv[0], name)) {
        double alpha = 0.0;
        if (argc < 2 || JS_ToFloat64(ctx, &alpha, argv[1]) != 0)
            return JS_ThrowTypeError(ctx, "setControl(name, alpha): numeric alpha required");
        try {
            w->pipeline->cond_control().set(name, (float)alpha);
        } catch (const std::exception& e) {
            return JS_ThrowTypeError(ctx, "setControl: %s", e.what());
        }
        return JS_UNDEFINED;
    }

    // Form 2: ({name: alpha, ...}) object map.
    if (!JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "setControl: expected a name string or an object map");
    JSPropertyEnum* tab = nullptr;
    uint32_t len = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, argv[0],
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0)
        return JS_ThrowInternalError(ctx, "setControl: cannot enumerate map");
    std::string err;
    for (uint32_t i = 0; i < len; ++i) {
        JSValue key = JS_AtomToString(ctx, tab[i].atom);
        const char* kc = JS_ToCString(ctx, key);
        JSValue val = JS_GetProperty(ctx, argv[0], tab[i].atom);
        double alpha = 0.0;
        if (kc && JS_ToFloat64(ctx, &alpha, val) == 0) {
            try {
                w->pipeline->cond_control().set(kc, (float)alpha);
            } catch (const std::exception& e) {
                if (err.empty()) err = e.what();
            }
        }
        if (kc) JS_FreeCString(ctx, kc);
        JS_FreeValue(ctx, val);
        JS_FreeValue(ctx, key);
        JS_FreeAtom(ctx, tab[i].atom);
    }
    js_free(ctx, tab);
    if (!err.empty()) return JS_ThrowTypeError(ctx, "setControl: %s", err.c_str());
    return JS_UNDEFINED;
}

// clearControl() — reset every control-axis weight to zero (no injection). The
// loaded dictionary is kept; only the weights are cleared.
static JSValue js_pipeline_clearControl(JSContext* ctx, JSValueConst this_val,
                                        int, JSValueConst*) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "clearControl: not a Pipeline");
    w->pipeline->cond_control().clear();
    return JS_UNDEFINED;
}

// dispose() — deterministically free the underlying pipeline (and its GPU
// weights) NOW, instead of waiting for the JS wrapper to be garbage-collected.
// A caller that reloads a large model into a memory-tight device (e.g. swapping
// Krea 2 checkpoints on a 24GB card) must release the old model's VRAM before
// building the new one, or the two coexist and OOM. Idempotent; after this the
// wrapper is inert — the caller is expected to drop its reference.
static JSValue js_pipeline_dispose(JSContext* ctx, JSValueConst this_val,
                                   int, JSValueConst*) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "dispose: not a Pipeline");
    w->pipeline.reset();
    w->weights_loaded = false;
    return JS_UNDEFINED;
}

// controlAxes() -> [name, ...] — the names of the loaded dictionary's axes
// (empty array if none loaded). For introspection / building UI.
static JSValue js_pipeline_controlAxes(JSContext* ctx, JSValueConst this_val,
                                       int, JSValueConst*) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "controlAxes: not a Pipeline");
    const auto& names = w->pipeline->cond_control().names();
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < names.size(); ++i)
        JS_SetPropertyUint32(ctx, arr, i, JS_NewString(ctx, names[i].c_str()));
    return arr;
}

// controlVector(name) -> { dir: Float32Array, scale } — the stored direction
// and baked scale of a loaded-dictionary or runtime axis. Introspection for
// explaining axes to the user (e.g. cosine-decompose a freshly minted axis
// against the dictionary's named directions). Throws on unknown name.
static JSValue js_pipeline_controlVector(JSContext* ctx, JSValueConst this_val,
                                         int argc, JSValueConst* argv) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "controlVector: not a Pipeline");
    std::string name;
    if (argc < 1 || !argStr(ctx, argv[0], name))
        return JS_ThrowTypeError(ctx, "controlVector(name): name string required");
    try {
        std::vector<float> dir = w->pipeline->cond_control().direction(name);
        float scale = w->pipeline->cond_control().axis_scale(name);
        JSValue out = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, out, "dir", qjsbind::make_float32_array(ctx, dir));
        JS_SetPropertyStr(ctx, out, "scale", JS_NewFloat64(ctx, scale));
        return out;
    } catch (const std::exception& e) {
        return JS_ThrowTypeError(ctx, "controlVector: %s", e.what());
    }
}

// encodeConditioning(prompt) -> { rows, cols, data: Float32Array }
//   Encode `prompt` into the model's text-conditioning sequence — the (L, hidden)
//   embeddings the denoiser cross-attends to (row 0 = BOS). Downloaded to host
//   FP32. The primitive for building control directions in the encoder's own
//   space, e.g. a diff-of-means axis from two phrase sets: mean the content rows
//   (skip row 0) of each phrase, difference the set means, normalize.
static JSValue js_pipeline_encodeConditioning(JSContext* ctx, JSValueConst this_val,
                                              int argc, JSValueConst* argv) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "encodeConditioning: not a Pipeline");
    if (!w->weights_loaded)
        return JS_ThrowInternalError(ctx, "encodeConditioning: call loadWeights() first");
    std::string prompt;
    if (argc < 1 || !argStr(ctx, argv[0], prompt))
        return JS_ThrowTypeError(ctx, "encodeConditioning(prompt): prompt string required");
    try {
        brotensor::Tensor emb = w->pipeline->encode_conditioning(prompt);
        std::vector<float> host = downloadTensorFloats(emb);
        JSValue out = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, out, "rows", JS_NewInt32(ctx, emb.rows));
        JS_SetPropertyStr(ctx, out, "cols", JS_NewInt32(ctx, emb.cols));
        JS_SetPropertyStr(ctx, out, "data", qjsbind::make_float32_array(ctx, host));
        return out;
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "encodeConditioning: %s", e.what());
    }
}

// setControlVector(name, dir, alpha, scale=1) — register/replace a runtime axis
//   from an explicit direction (a Float32Array of width = encoder hidden dim)
//   and set its weight. `dir` is taken as-is (caller normalizes / MASSIVE-zeros
//   per the recipe). Coexists with loaded dictionary axes; the injected vector
//   is alpha * scale * dir. Unknown-length dir throws at the next generate().
static JSValue js_pipeline_setControlVector(JSContext* ctx, JSValueConst this_val,
                                            int argc, JSValueConst* argv) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "setControlVector: not a Pipeline");
    std::string name;
    if (argc < 1 || !argStr(ctx, argv[0], name))
        return JS_ThrowTypeError(ctx, "setControlVector(name, dir, alpha, scale?): name required");
    std::size_t count = 0;
    const float* dir = qjsbind::read_float32_view(ctx, (argc >= 2) ? argv[1] : JS_UNDEFINED, count);
    if (!dir || count == 0)
        return JS_ThrowTypeError(ctx, "setControlVector: dir must be a non-empty Float32Array");
    double alpha = 0.0;
    if (argc < 3 || JS_ToFloat64(ctx, &alpha, argv[2]) != 0)
        return JS_ThrowTypeError(ctx, "setControlVector: numeric alpha required");
    double scale = 1.0;
    if (argc >= 4 && JS_IsNumber(argv[3])) JS_ToFloat64(ctx, &scale, argv[3]);
    try {
        std::vector<float> v(dir, dir + count);
        w->pipeline->cond_control().set_vector(name, (float)alpha, v, (float)scale);
    } catch (const std::exception& e) {
        return JS_ThrowTypeError(ctx, "setControlVector: %s", e.what());
    }
    return JS_UNDEFINED;
}

// removeControl(name) — remove a single axis (runtime or dictionary). No-op if
// unknown. Lets the lab drop a built search axis without reloading.
static JSValue js_pipeline_removeControl(JSContext* ctx, JSValueConst this_val,
                                         int argc, JSValueConst* argv) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "removeControl: not a Pipeline");
    std::string name;
    if (argc < 1 || !argStr(ctx, argv[0], name))
        return JS_ThrowTypeError(ctx, "removeControl(name): name string required");
    w->pipeline->cond_control().remove(name);
    return JS_UNDEFINED;
}

// generate(prompt, opts) -> { width, height, data } — one-shot txt2img.
// Blocking; intended for a worker thread. The main thread should drive the
// step-wise prime()/stepOnce()/decode() API instead.
// Returns { cancelled: true } (no pixels) when the process is shutting down
// (Ctrl+C / window close / engine teardown) — the denoise loop polls the
// interrupt once per step so teardown never blocks on a full generation.
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
    opts.should_cancel = bro::util::interrupted;

    try {
        std::vector<float> img = w->pipeline->generate(prompt, opts);
        return makeImageResult(ctx, img, opts.height, opts.width, includeFp32);
    } catch (const bdp::GenerateCancelled&) {
        JSValue out = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, out, "cancelled", JS_TRUE);
        return out;
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "generate: %s", e.what());
    }
}

// numXAttnBlocks() -> number — cross-attention block count for the loaded
// model. Meaningful only after loadWeights(); returns 1 before.
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
    const bool lcm  = std::holds_alternative<bdsch::LCMConfig>(cfg.scheduler);
    const bool flow = std::holds_alternative<bdsch::FlowMatchConfig>(cfg.scheduler);
    const bool scm  = std::holds_alternative<bdsch::SCMConfig>(cfg.scheduler);
    const char* schedName = scm ? "scm" : lcm ? "lcm"
                                              : (flow ? "flowmatch" : "ddim");
    const char* modelClassName =
        cfg.model_class == brodiffusion::ModelClass::Flux   ? "Flux" :
        cfg.model_class == brodiffusion::ModelClass::Sana   ? "Sana" :
        cfg.model_class == brodiffusion::ModelClass::PixArt ? "PixArt" :
        cfg.model_class == brodiffusion::ModelClass::Krea2  ? "Krea2" :
                                                              "StableDiffusion";

    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "modelClass", JS_NewString(ctx, modelClassName));
    JS_SetPropertyStr(ctx, o, "scheduler", JS_NewString(ctx, schedName));
    JS_SetPropertyStr(ctx, o, "timeCondProjDim",
                      JS_NewInt32(ctx, cfg.unet.time_cond_proj_dim));
    JS_SetPropertyStr(ctx, o, "quantizeWeights",
                      JS_NewBool(ctx, cfg.unet.quantize_weights));
    JS_SetPropertyStr(ctx, o, "numXAttnBlocks",
                      JS_NewInt32(ctx, xattnBlocks(w)));
    JS_SetPropertyStr(ctx, o, "weightsLoaded", JS_NewBool(ctx, w->weights_loaded));
    JS_SetPropertyStr(ctx, o, "numControlNets",
                      JS_NewInt32(ctx, w->pipeline->num_controlnets()));
    JS_SetPropertyStr(ctx, o, "hasControlNet",
                      JS_NewBool(ctx, w->pipeline->has_controlnet()));
    return o;
}

// prime(prompt, opts) -> PipelineState — begin a step-wise generation. The
// opts are captured on the returned state so stepOnce()/decode() need none.
static JSValue js_pipeline_prime(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "prime: not a Pipeline");
    if (!w->weights_loaded)
        return JS_ThrowInternalError(ctx, "prime: call loadWeights() first");

    std::string prompt;
    if (argc < 1 || !argStr(ctx, argv[0], prompt))
        return JS_ThrowTypeError(ctx, "prime(prompt, opts?): prompt string required");
    bdp::GenerateOptions opts =
        parseGenerateOptions(ctx, (argc >= 2) ? argv[1] : JS_UNDEFINED);

    try {
        auto sw = std::make_unique<PipelineStateWrapper>();
        sw->state = w->pipeline->prime(prompt, opts);
        sw->opts  = opts;
        JSValue js = qjsbind::wrap<PipelineStateWrapper>(ctx, sw.release());
        // Retain the owning Pipeline so it cannot be GC'd while this state
        // (or any clone) is alive. Non-enumerable: flags = 0.
        JS_DefinePropertyValueStr(ctx, js, "__pipeline",
                                  JS_DupValue(ctx, this_val), 0);
        return js;
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "prime: %s", e.what());
    }
}

// setIdentityAnchor(prompt, opts?) -> image — capture a reference identity from
// one full Sana generation, then arm the reference-attention seam. The returned
// image IS the anchor (e.g. a neutral portrait). Subsequent generate()/prime()
// calls inject the anchor's appearance (scaled by setIdentityWeight) while each
// prompt still sets pose / expression. Match opts (steps, size, seed) to the
// later generations for tight alignment. Sana only (throws otherwise).
static JSValue js_pipeline_setIdentityAnchor(JSContext* ctx, JSValueConst this_val,
                                             int argc, JSValueConst* argv) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "setIdentityAnchor: not a Pipeline");
    if (!w->weights_loaded)
        return JS_ThrowInternalError(ctx, "setIdentityAnchor: call loadWeights() first");
    std::string prompt;
    if (argc < 1 || !argStr(ctx, argv[0], prompt))
        return JS_ThrowTypeError(ctx, "setIdentityAnchor(prompt, opts?): prompt string required");
    JSValueConst optsv = (argc >= 2) ? argv[1] : JS_UNDEFINED;
    bdp::GenerateOptions opts = parseGenerateOptions(ctx, optsv);
    const bool includeFp32 = JS_IsObject(optsv) && getBool(ctx, optsv, "includeFp32");
    opts.should_cancel = bro::util::interrupted;
    try {
        std::vector<float> img = w->pipeline->capture_identity_anchor(prompt, opts);
        return makeImageResult(ctx, img, opts.height, opts.width, includeFp32);
    } catch (const bdp::GenerateCancelled&) {
        JSValue out = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, out, "cancelled", JS_TRUE);
        return out;
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "setIdentityAnchor: %s", e.what());
    }
}

// setIdentityWeight(weight) — injection strength for the armed identity anchor.
// 0 disables (even with an anchor set); ~1 holds identity faithfully; higher
// over-anchors. Takes effect on the next generate()/prime(). Sana only.
static JSValue js_pipeline_setIdentityWeight(JSContext* ctx, JSValueConst this_val,
                                             int argc, JSValueConst* argv) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "setIdentityWeight: not a Pipeline");
    double wt = 0.0;
    if (argc < 1 || JS_ToFloat64(ctx, &wt, argv[0]) != 0)
        return JS_ThrowTypeError(ctx, "setIdentityWeight(weight): numeric weight required");
    w->pipeline->set_identity_weight((float)wt);
    return JS_UNDEFINED;
}

// hasIdentityAnchor() -> bool — whether an anchor has been captured + armed.
static JSValue js_pipeline_hasIdentityAnchor(JSContext* ctx, JSValueConst this_val,
                                             int, JSValueConst*) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "hasIdentityAnchor: not a Pipeline");
    return JS_NewBool(ctx, w->pipeline->has_identity_anchor());
}

// clearIdentityAnchor() — drop the cached anchor and zero the weight.
static JSValue js_pipeline_clearIdentityAnchor(JSContext* ctx, JSValueConst this_val,
                                               int, JSValueConst*) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "clearIdentityAnchor: not a Pipeline");
    w->pipeline->clear_identity_anchor();
    return JS_UNDEFINED;
}

// ═══════════════════════════════════════════════════════════════════════════
// Krea 2 research-hook bindings — AdaLN/gate dials, raw-taps conditioning,
// image-as-prompt. Every one below is Krea2-only; the native Pipeline methods
// they wrap already throw a clear error for any other model_class, so no
// separate guard is added here (mirrors how encodeConditioning etc. rely on
// the native side's own checks).
// ═══════════════════════════════════════════════════════════════════════════

// { rows, cols, data: Float32Array } -> brotensor::Tensor (host FP32 upload;
// Krea2's native setters/encoders cast to their own compute dtype on ingest).
static bool tensorFromJs(JSContext* ctx, JSValueConst v, brotensor::Tensor& out) {
    if (!JS_IsObject(v)) return false;
    int rows = 0, cols = 0;
    getInt(ctx, v, "rows", rows);
    getInt(ctx, v, "cols", cols);
    JSValue dv = JS_GetPropertyStr(ctx, v, "data");
    std::size_t cnt = 0;
    const float* fp = qjsbind::read_float32_view(ctx, dv, cnt);
    JS_FreeValue(ctx, dv);
    if (!fp || rows <= 0 || cols <= 0 || cnt != (std::size_t)rows * (std::size_t)cols)
        return false;
    out = brotensor::Tensor::from_host(fp, rows, cols);
    return true;
}

// brotensor::Tensor -> { rows, cols, data: Float32Array } (host download).
static JSValue tensorToJs(JSContext* ctx, const brotensor::Tensor& t) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "rows", JS_NewInt32(ctx, t.rows));
    JS_SetPropertyStr(ctx, o, "cols", JS_NewInt32(ctx, t.cols));
    JS_SetPropertyStr(ctx, o, "data", qjsbind::make_float32_array(ctx, downloadTensorFloats(t)));
    return o;
}

// krea2::TextConditioning -> { embeds: {rows,cols,data}, mask: {rows,cols,data} }
static JSValue taxConditioningToJs(JSContext* ctx,
                                   const brodiffusion::krea2::TextConditioning& tc) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "embeds", tensorToJs(ctx, tc.prompt_embeds));
    JS_SetPropertyStr(ctx, o, "mask",   tensorToJs(ctx, tc.prompt_embeds_mask));
    return o;
}

// krea2SetModDelta(delta: {rows,cols,data} | null, blockLo, blockHi) — delta
// is (1, 6*krea2HiddenSize()); null/undefined clears.
static JSValue js_pipeline_krea2SetModDelta(JSContext* ctx, JSValueConst this_val,
                                            int argc, JSValueConst* argv) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "krea2SetModDelta: not a Pipeline");
    brotensor::Tensor delta;
    if (argc >= 1 && JS_IsObject(argv[0]) && !JS_IsNull(argv[0])) {
        if (!tensorFromJs(ctx, argv[0], delta))
            return JS_ThrowTypeError(ctx, "krea2SetModDelta: delta must be {rows,cols,data}");
    }
    int32_t lo = 0, hi = 0;
    if (argc < 3 || JS_ToInt32(ctx, &lo, argv[1]) != 0 || JS_ToInt32(ctx, &hi, argv[2]) != 0)
        return JS_ThrowTypeError(ctx, "krea2SetModDelta(delta, blockLo, blockHi): integer range required");
    try {
        w->pipeline->krea_set_mod_delta(delta, lo, hi);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "krea2SetModDelta: %s", e.what());
    }
    return JS_UNDEFINED;
}

// krea2TimeMod(timestep) -> { temb: {rows,cols,data}, mod: {rows,cols,data} }
static JSValue js_pipeline_krea2TimeMod(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "krea2TimeMod: not a Pipeline");
    double t = 0.0;
    if (argc < 1 || JS_ToFloat64(ctx, &t, argv[0]) != 0)
        return JS_ThrowTypeError(ctx, "krea2TimeMod(timestep): numeric timestep required");
    try {
        brotensor::Tensor temb, mod;
        w->pipeline->krea_time_mod((float)t, temb, mod);
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "temb", tensorToJs(ctx, temb));
        JS_SetPropertyStr(ctx, o, "mod",  tensorToJs(ctx, mod));
        return o;
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "krea2TimeMod: %s", e.what());
    }
}

// krea2SetGateScale(txtScale, imgScale, blockLo, blockHi)
static JSValue js_pipeline_krea2SetGateScale(JSContext* ctx, JSValueConst this_val,
                                             int argc, JSValueConst* argv) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "krea2SetGateScale: not a Pipeline");
    double txt = 1.0, img = 1.0;
    int32_t lo = 0, hi = 0;
    if (argc < 4 || JS_ToFloat64(ctx, &txt, argv[0]) != 0 ||
        JS_ToFloat64(ctx, &img, argv[1]) != 0 ||
        JS_ToInt32(ctx, &lo, argv[2]) != 0 || JS_ToInt32(ctx, &hi, argv[3]) != 0) {
        return JS_ThrowTypeError(ctx,
            "krea2SetGateScale(txtScale, imgScale, blockLo, blockHi): numeric args required");
    }
    try {
        w->pipeline->krea_set_gate_scale((float)txt, (float)img, lo, hi);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "krea2SetGateScale: %s", e.what());
    }
    return JS_UNDEFINED;
}

// krea2SetGateMask(mask: {rows,cols,data} | null, blockLo, blockHi) — mask
// holds (text_seq + img_len) values; null/undefined clears.
static JSValue js_pipeline_krea2SetGateMask(JSContext* ctx, JSValueConst this_val,
                                            int argc, JSValueConst* argv) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "krea2SetGateMask: not a Pipeline");
    brotensor::Tensor mask;
    if (argc >= 1 && JS_IsObject(argv[0]) && !JS_IsNull(argv[0])) {
        if (!tensorFromJs(ctx, argv[0], mask))
            return JS_ThrowTypeError(ctx, "krea2SetGateMask: mask must be {rows,cols,data}");
    }
    int32_t lo = 0, hi = 0;
    if (argc < 3 || JS_ToInt32(ctx, &lo, argv[1]) != 0 || JS_ToInt32(ctx, &hi, argv[2]) != 0)
        return JS_ThrowTypeError(ctx, "krea2SetGateMask(mask, blockLo, blockHi): integer range required");
    try {
        w->pipeline->krea_set_gate_mask(mask, lo, hi);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "krea2SetGateMask: %s", e.what());
    }
    return JS_UNDEFINED;
}

// krea2CaptureGates(enable)
static JSValue js_pipeline_krea2CaptureGates(JSContext* ctx, JSValueConst this_val,
                                             int argc, JSValueConst* argv) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "krea2CaptureGates: not a Pipeline");
    const bool enable = argc >= 1 && JS_ToBool(ctx, argv[0]);
    try {
        w->pipeline->krea_capture_gates(enable);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "krea2CaptureGates: %s", e.what());
    }
    return JS_UNDEFINED;
}

// krea2Gates() -> { rows, cols, data } — rows = krea2NumLayers(), cols =
// text_seq + img_len (inferred from the flat buffer length).
static JSValue js_pipeline_krea2Gates(JSContext* ctx, JSValueConst this_val,
                                      int, JSValueConst*) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "krea2Gates: not a Pipeline");
    try {
        std::vector<float> flat = w->pipeline->krea_gates();
        const int rows = w->pipeline->krea_num_layers();
        const int cols = rows > 0 ? (int)(flat.size() / (std::size_t)rows) : 0;
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "rows", JS_NewInt32(ctx, rows));
        JS_SetPropertyStr(ctx, o, "cols", JS_NewInt32(ctx, cols));
        JS_SetPropertyStr(ctx, o, "data", qjsbind::make_float32_array(ctx, flat));
        return o;
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "krea2Gates: %s", e.what());
    }
}

// krea2HiddenSize() -> number (6144)
static JSValue js_pipeline_krea2HiddenSize(JSContext* ctx, JSValueConst this_val,
                                           int, JSValueConst*) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "krea2HiddenSize: not a Pipeline");
    try {
        return JS_NewInt32(ctx, w->pipeline->krea_hidden_size());
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "krea2HiddenSize: %s", e.what());
    }
}

// krea2NumLayers() -> number (28)
static JSValue js_pipeline_krea2NumLayers(JSContext* ctx, JSValueConst this_val,
                                          int, JSValueConst*) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "krea2NumLayers: not a Pipeline");
    try {
        return JS_NewInt32(ctx, w->pipeline->krea_num_layers());
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "krea2NumLayers: %s", e.what());
    }
}

// krea2EncodePromptTaps(prompt) -> { embeds, mask } — raw per-layer Qwen3-VL
// taps, pre-fusion (token-major/layer-minor). Edit rows, then feed to
// krea2EncodeText()/krea2PrimeFromTaps().
static JSValue js_pipeline_krea2EncodePromptTaps(JSContext* ctx, JSValueConst this_val,
                                                 int argc, JSValueConst* argv) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "krea2EncodePromptTaps: not a Pipeline");
    if (!w->weights_loaded)
        return JS_ThrowInternalError(ctx, "krea2EncodePromptTaps: call loadWeights() first");
    std::string prompt;
    if (argc < 1 || !argStr(ctx, argv[0], prompt))
        return JS_ThrowTypeError(ctx, "krea2EncodePromptTaps(prompt): prompt string required");
    try {
        return taxConditioningToJs(ctx, w->pipeline->krea_encode_prompt_taps(prompt));
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "krea2EncodePromptTaps: %s", e.what());
    }
}

// krea2EncodeText(embeds, mask) -> {rows,cols,data} — fuse raw taps into the
// (n_valid, krea2HiddenSize()) conditioning the DiT cross-attends to. This is
// the same space cond_control axes (setControl/setControlVector) apply in.
static JSValue js_pipeline_krea2EncodeText(JSContext* ctx, JSValueConst this_val,
                                           int argc, JSValueConst* argv) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "krea2EncodeText: not a Pipeline");
    brotensor::Tensor embeds, mask;
    if (argc < 2 || !tensorFromJs(ctx, argv[0], embeds) || !tensorFromJs(ctx, argv[1], mask))
        return JS_ThrowTypeError(ctx, "krea2EncodeText(embeds, mask): {rows,cols,data} tensors required");
    try {
        return tensorToJs(ctx, w->pipeline->krea_encode_text(embeds, mask));
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "krea2EncodeText: %s", e.what());
    }
}

// krea2EncodeImagePrompt(pixels: Float32Array, H, W) -> { embeds, mask } — the
// same raw-taps shape krea2EncodePromptTaps() produces for text, from an image
// through Krea 2's own Qwen3-VL vision tower. `pixels` is FP32 CHW, [0,1]
// range, length 3*H*W.
static JSValue js_pipeline_krea2EncodeImagePrompt(JSContext* ctx, JSValueConst this_val,
                                                  int argc, JSValueConst* argv) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "krea2EncodeImagePrompt: not a Pipeline");
    if (!w->weights_loaded)
        return JS_ThrowInternalError(ctx, "krea2EncodeImagePrompt: call loadWeights() first");
    int32_t H = 0, W = 0;
    std::size_t cnt = 0;
    const float* px = (argc >= 1) ? qjsbind::read_float32_view(ctx, argv[0], cnt) : nullptr;
    if (!px || argc < 3 || JS_ToInt32(ctx, &H, argv[1]) != 0 || JS_ToInt32(ctx, &W, argv[2]) != 0 ||
        H <= 0 || W <= 0 || cnt != (std::size_t)3 * (std::size_t)H * (std::size_t)W) {
        return JS_ThrowTypeError(ctx,
            "krea2EncodeImagePrompt(pixels, H, W): pixels must be a Float32Array of length 3*H*W");
    }
    try {
        return taxConditioningToJs(ctx, w->pipeline->krea_encode_image_prompt(px, H, W));
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "krea2EncodeImagePrompt: %s", e.what());
    }
}

// krea2PrimeFromTaps(embeds, mask, opts?, uncondEmbeds?, uncondMask?) -> PipelineState
// Prime a step-wise generation from caller-supplied raw taps (as returned /
// edited from krea2EncodePromptTaps()/krea2EncodeImagePrompt()) instead of a
// plain prompt string. uncondEmbeds/uncondMask are optional {rows,cols,data}
// pairs; omit both to fall back to encoding opts.negativePrompt normally.
static JSValue js_pipeline_krea2PrimeFromTaps(JSContext* ctx, JSValueConst this_val,
                                              int argc, JSValueConst* argv) {
    auto* w = pipelineSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "krea2PrimeFromTaps: not a Pipeline");
    if (!w->weights_loaded)
        return JS_ThrowInternalError(ctx, "krea2PrimeFromTaps: call loadWeights() first");

    brotensor::Tensor embeds, mask;
    if (argc < 2 || !tensorFromJs(ctx, argv[0], embeds) || !tensorFromJs(ctx, argv[1], mask))
        return JS_ThrowTypeError(ctx, "krea2PrimeFromTaps(embeds, mask, opts?, uncondEmbeds?, uncondMask?): "
                                       "{rows,cols,data} tensors required");
    JSValueConst optsv = (argc >= 3) ? argv[2] : JS_UNDEFINED;
    bdp::GenerateOptions opts = parseGenerateOptions(ctx, optsv);

    brotensor::Tensor uembeds, umask;
    bool haveUncond = false;
    if (argc >= 5 && JS_IsObject(argv[3]) && JS_IsObject(argv[4])) {
        if (!tensorFromJs(ctx, argv[3], uembeds) || !tensorFromJs(ctx, argv[4], umask))
            return JS_ThrowTypeError(ctx, "krea2PrimeFromTaps: uncondEmbeds/uncondMask must be {rows,cols,data}");
        haveUncond = true;
    }

    try {
        auto sw = std::make_unique<PipelineStateWrapper>();
        sw->state = w->pipeline->krea_prime_from_taps(
            embeds, mask, haveUncond ? &uembeds : nullptr, haveUncond ? &umask : nullptr, opts);
        sw->opts  = opts;
        JSValue js = qjsbind::wrap<PipelineStateWrapper>(ctx, sw.release());
        JS_DefinePropertyValueStr(ctx, js, "__pipeline",
                                  JS_DupValue(ctx, this_val), 0);
        return js;
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "krea2PrimeFromTaps: %s", e.what());
    }
}

static void registerPipelineClass(JSContext* ctx) {
    qjsbind::Class<PipelineWrapper>(ctx, "DiffusionPipeline", qjsbind::NoGlobal)
        .method_raw("loadWeights",        js_pipeline_loadWeights,        3)
        .method_raw("applyLora",          js_pipeline_applyLora,          2)
        .method_raw("addControlNet",      js_pipeline_addControlNet,      2)
        .method_raw("removeControlNet",   js_pipeline_removeControlNet,   1)
        .method_raw("clearControlNets",   js_pipeline_clearControlNets,   0)
        .method_raw("generate",           js_pipeline_generate,           2)
        .method_raw("prime",              js_pipeline_prime,              2)
        .method_raw("numXAttnBlocks",     js_pipeline_numXAttnBlocks,     0)
        .method_raw("config",             js_pipeline_config,             0)
        .method_raw("loadControlDictionary", js_pipeline_loadControlDictionary, 1)
        .method_raw("setControl",         js_pipeline_setControl,         2)
        .method_raw("clearControl",       js_pipeline_clearControl,       0)
        .method_raw("dispose",            js_pipeline_dispose,            0)
        .method_raw("controlAxes",        js_pipeline_controlAxes,        0)
        .method_raw("controlVector",      js_pipeline_controlVector,      1)
        .method_raw("encodeConditioning", js_pipeline_encodeConditioning, 1)
        .method_raw("setControlVector",   js_pipeline_setControlVector,   4)
        .method_raw("removeControl",      js_pipeline_removeControl,      1)
        .method_raw("setIdentityAnchor",  js_pipeline_setIdentityAnchor,  2)
        .method_raw("setIdentityWeight",  js_pipeline_setIdentityWeight,  1)
        .method_raw("hasIdentityAnchor",  js_pipeline_hasIdentityAnchor,  0)
        .method_raw("clearIdentityAnchor", js_pipeline_clearIdentityAnchor, 0)
        .method_raw("krea2SetModDelta",       js_pipeline_krea2SetModDelta,       3)
        .method_raw("krea2TimeMod",           js_pipeline_krea2TimeMod,           1)
        .method_raw("krea2SetGateScale",      js_pipeline_krea2SetGateScale,      4)
        .method_raw("krea2SetGateMask",       js_pipeline_krea2SetGateMask,       3)
        .method_raw("krea2CaptureGates",      js_pipeline_krea2CaptureGates,      1)
        .method_raw("krea2Gates",             js_pipeline_krea2Gates,             0)
        .method_raw("krea2HiddenSize",        js_pipeline_krea2HiddenSize,        0)
        .method_raw("krea2NumLayers",         js_pipeline_krea2NumLayers,         0)
        .method_raw("krea2EncodePromptTaps",  js_pipeline_krea2EncodePromptTaps,  1)
        .method_raw("krea2EncodeText",        js_pipeline_krea2EncodeText,        2)
        .method_raw("krea2EncodeImagePrompt", js_pipeline_krea2EncodeImagePrompt, 3)
        .method_raw("krea2PrimeFromTaps",     js_pipeline_krea2PrimeFromTaps,     5)
        .method_raw("setLoraScale",           js_pipeline_setLoraScale,           2)
        .method_raw("clearLoras",             js_pipeline_clearLoras,             0)
        .method_raw("numLoras",               js_pipeline_numLoras,               0);
}

// ═══════════════════════════════════════════════════════════════════════════
// PipelineState class methods (step-wise inspection API)
// ═══════════════════════════════════════════════════════════════════════════

// Resolve the owning Pipeline from a state's retained __pipeline handle.
static bdp::Pipeline* pipelineOfState(JSContext* ctx, JSValueConst stateVal) {
    JSValue p = JS_GetPropertyStr(ctx, stateVal, "__pipeline");
    auto* pw = qjsbind::unwrap<PipelineWrapper>(ctx, p);
    JS_FreeValue(ctx, p);
    return pw ? pw->pipeline.get() : nullptr;
}

// stepOnce(ctrl?) -> { trace? } — advance one denoising step.
//   ctrl.trace    bool — capture per-layer head-averaged attention maps
//   ctrl.attnBias ({data:Float32Array, Lq, Lk} | null)[] — per-layer
//                 pre-softmax logit bias; length must equal numXAttnBlocks().
// Supplying attnBias forces trace mode internally (brodiffusion contract).
static JSValue js_state_stepOnce(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    auto* sw = qjsbind::unwrap<PipelineStateWrapper>(ctx, this_val);
    if (!sw) return JS_ThrowTypeError(ctx, "stepOnce: not a PipelineState");
    bdp::Pipeline* pipe = pipelineOfState(ctx, this_val);
    if (!pipe) return JS_ThrowInternalError(ctx, "stepOnce: pipeline handle lost");

    JSValueConst ctrl = (argc >= 1) ? argv[0] : JS_UNDEFINED;
    const bool wantTrace = JS_IsObject(ctrl) && getBool(ctx, ctrl, "trace");

    // attn_logit_biases — owned tensors kept alive for the step_once call;
    // ptrs is the parallel null-preserving pointer vector brodiffusion takes.
    std::vector<brotensor::Tensor> owned;
    std::vector<const brotensor::Tensor*> ptrs;
    bool haveBias = false;
    if (JS_IsObject(ctrl)) {
        JSValue biasArr = JS_GetPropertyStr(ctx, ctrl, "attnBias");
        if (!JS_IsUndefined(biasArr) && !JS_IsNull(biasArr)) {
            haveBias = true;
            // Denoiser-generic block count: 16 for the SD1.5 UNet, 57 for the
            // Flux DiT. A denoiser with no trace support reports 0, so any
            // supplied attnBias array trips the length check below with a
            // clear message instead of throwing past the binding.
            const int n = pipe->num_xattn_blocks();
            std::uint32_t len = 0;
            JSValue lenV = JS_GetPropertyStr(ctx, biasArr, "length");
            JS_ToUint32(ctx, &len, lenV);
            JS_FreeValue(ctx, lenV);
            if ((int)len != n) {
                JS_FreeValue(ctx, biasArr);
                return JS_ThrowRangeError(ctx,
                    "stepOnce: attnBias length %u must equal numXAttnBlocks() %d",
                    len, n);
            }
            owned.reserve((std::size_t)n);
            ptrs.reserve((std::size_t)n);
            for (int i = 0; i < n; ++i) {
                JSValue e = JS_GetPropertyUint32(ctx, biasArr, (std::uint32_t)i);
                if (JS_IsNull(e) || JS_IsUndefined(e)) {
                    ptrs.push_back(nullptr);
                    JS_FreeValue(ctx, e);
                    continue;
                }
                int Lq = 0, Lk = 0;
                getInt(ctx, e, "Lq", Lq);
                getInt(ctx, e, "Lk", Lk);
                JSValue dv = JS_GetPropertyStr(ctx, e, "data");
                std::size_t cnt = 0;
                const float* fp = qjsbind::read_float32_view(ctx, dv, cnt);
                const bool ok = fp && Lq > 0 && Lk > 0 &&
                                cnt == (std::size_t)Lq * (std::size_t)Lk;
                if (!ok) {
                    JS_FreeValue(ctx, dv);
                    JS_FreeValue(ctx, e);
                    JS_FreeValue(ctx, biasArr);
                    return JS_ThrowTypeError(ctx,
                        "stepOnce: attnBias[%d] must be "
                        "{ data: Float32Array(Lq*Lk), Lq, Lk } or null", i);
                }
                owned.push_back(brotensor::Tensor::from_host(fp, Lq, Lk));
                ptrs.push_back(&owned.back());
                JS_FreeValue(ctx, dv);
                JS_FreeValue(ctx, e);
            }
        }
        JS_FreeValue(ctx, biasArr);
    }

    // Denoiser-generic trace buffer: a vector of (Lq, Lk) attention tensors,
    // one per cross-attention block, regardless of denoiser type (UNet or DiT).
    brodiffusion::AttentionTrace trace;
    brodiffusion::AttentionTrace* tracePtr = wantTrace ? &trace : nullptr;
    const std::vector<const brotensor::Tensor*>* biasPtr = haveBias ? &ptrs : nullptr;

    try {
        pipe->step_once(sw->state, sw->opts, tracePtr, biasPtr);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "stepOnce: %s", e.what());
    }

    JSValue res = JS_NewObject(ctx);
    if (wantTrace) {
        JSValue arr = JS_NewArray(ctx);
        for (std::size_t i = 0; i < trace.size(); ++i) {
            const brotensor::Tensor& t = trace[i];
            JSValue entry = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, entry, "Lq", JS_NewInt32(ctx, t.rows));
            JS_SetPropertyStr(ctx, entry, "Lk", JS_NewInt32(ctx, t.cols));
            JS_SetPropertyStr(ctx, entry, "data",
                qjsbind::make_float32_array(ctx, downloadTensorFloats(t)));
            JS_SetPropertyUint32(ctx, arr, (std::uint32_t)i, entry);
        }
        JS_SetPropertyStr(ctx, res, "trace", arr);
    }
    return res;
}

// decode(opts?) -> { width, height, data } — VAE-decode the current latent.
// opts.includeFp32 attaches the raw NCHW FP32 buffer as `fp32`.
static JSValue js_state_decode(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    auto* sw = qjsbind::unwrap<PipelineStateWrapper>(ctx, this_val);
    if (!sw) return JS_ThrowTypeError(ctx, "decode: not a PipelineState");
    bdp::Pipeline* pipe = pipelineOfState(ctx, this_val);
    if (!pipe) return JS_ThrowInternalError(ctx, "decode: pipeline handle lost");
    const bool includeFp32 = argc >= 1 && JS_IsObject(argv[0]) &&
                             getBool(ctx, argv[0], "includeFp32");
    try {
        std::vector<float> img = pipe->decode(sw->state);
        // KL-VAE upsamples 8x (SD / Flux); Sana's DC-AE upsamples 32x.
        const int scale = pipe->vae_scale_factor();
        return makeImageResult(ctx, img, sw->state.H_lat * scale,
                               sw->state.W_lat * scale, includeFp32);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "decode: %s", e.what());
    }
}

// latent() -> Float32Array — download the working latent (small; on demand).
static JSValue js_state_latent(JSContext* ctx, JSValueConst this_val,
                               int, JSValueConst*) {
    auto* sw = qjsbind::unwrap<PipelineStateWrapper>(ctx, this_val);
    if (!sw) return JS_ThrowTypeError(ctx, "latent: not a PipelineState");
    try {
        return qjsbind::make_float32_array(ctx,
                   downloadTensorFloats(sw->state.latent));
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "latent: %s", e.what());
    }
}

// setLatent(Float32Array) — overwrite the working latent in place (host FP32,
// length must equal the current latent's element count; cast to the state's
// working dtype if needed). For spatial paint compositing: blend two states'
// latents host-side each step, then push the blend back into one state before
// its next stepOnce()/decode().
static JSValue js_state_setLatent(JSContext* ctx, JSValueConst this_val,
                                  int argc, JSValueConst* argv) {
    auto* sw = qjsbind::unwrap<PipelineStateWrapper>(ctx, this_val);
    if (!sw) return JS_ThrowTypeError(ctx, "setLatent: not a PipelineState");
    std::size_t cnt = 0;
    const float* fp = (argc >= 1) ? qjsbind::read_float32_view(ctx, argv[0], cnt) : nullptr;
    const std::size_t expect = (std::size_t)sw->state.latent.rows * (std::size_t)sw->state.latent.cols;
    if (!fp || cnt != expect) {
        return JS_ThrowTypeError(ctx,
            "setLatent(data): Float32Array length must equal the current latent's element count (%zu)",
            expect);
    }
    try {
        const brotensor::Dtype dt = sw->state.latent.dtype;
        brotensor::Tensor host = brotensor::Tensor::from_host(
            fp, sw->state.latent.rows, sw->state.latent.cols);
        if (dt == brotensor::Dtype::FP32) {
            sw->state.latent = host;
        } else {
            brotensor::Tensor casted;
            brotensor::cast(host, casted, dt);
            sw->state.latent = casted;
        }
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "setLatent: %s", e.what());
    }
    return JS_UNDEFINED;
}

// krea2StepTimestep() -> number — the active scheduler's timestep for this
// state's current step_index (0..1000 scale), the same value step_once() will
// feed the denoiser next. Krea 2 only. For building a krea2TimeMod() query or
// mod-delta ahead of the upcoming step_once().
static JSValue js_state_krea2StepTimestep(JSContext* ctx, JSValueConst this_val,
                                          int, JSValueConst*) {
    auto* sw = qjsbind::unwrap<PipelineStateWrapper>(ctx, this_val);
    if (!sw) return JS_ThrowTypeError(ctx, "krea2StepTimestep: not a PipelineState");
    bdp::Pipeline* pipe = pipelineOfState(ctx, this_val);
    if (!pipe) return JS_ThrowInternalError(ctx, "krea2StepTimestep: pipeline handle lost");
    try {
        return JS_NewFloat64(ctx, (double)pipe->krea_step_timestep(sw->state));
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "krea2StepTimestep: %s", e.what());
    }
}

// clone() -> PipelineState — deep-copy the state (one latent clone). The
// owning Pipeline handle is carried forward so the clone stays valid.
static JSValue js_state_clone(JSContext* ctx, JSValueConst this_val,
                              int, JSValueConst*) {
    auto* sw = qjsbind::unwrap<PipelineStateWrapper>(ctx, this_val);
    if (!sw) return JS_ThrowTypeError(ctx, "clone: not a PipelineState");
    try {
        auto nw = std::make_unique<PipelineStateWrapper>();
        nw->state = sw->state.clone();
        nw->opts  = sw->opts;
        JSValue js = qjsbind::wrap<PipelineStateWrapper>(ctx, nw.release());
        // JS_GetPropertyStr returns a fresh ref; DefinePropertyValue consumes it.
        JS_DefinePropertyValueStr(ctx, js, "__pipeline",
                                  JS_GetPropertyStr(ctx, this_val, "__pipeline"), 0);
        return js;
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "clone: %s", e.what());
    }
}

static void registerPipelineStateClass(JSContext* ctx) {
    qjsbind::Class<PipelineStateWrapper>(ctx, "DiffusionPipelineState",
                                         qjsbind::NoGlobal)
        .get("stepIndex",    [](PipelineStateWrapper* s) { return s->state.step_index; })
        .get("numSteps",     [](PipelineStateWrapper* s) { return s->state.n_steps; })
        .get("done",         [](PipelineStateWrapper* s) {
            return s->state.step_index >= s->state.n_steps; })
        .get("latentWidth",  [](PipelineStateWrapper* s) { return s->state.W_lat; })
        .get("latentHeight", [](PipelineStateWrapper* s) { return s->state.H_lat; })
        .method_raw("stepOnce", js_state_stepOnce, 1)
        .method_raw("decode",   js_state_decode,   1)
        .method_raw("latent",   js_state_latent,   0)
        .method_raw("setLatent", js_state_setLatent, 1)
        .method_raw("krea2StepTimestep", js_state_krea2StepTimestep, 0)
        .method_raw("clone",    js_state_clone,    0);
}

// ═══════════════════════════════════════════════════════════════════════════
// Install
// ═══════════════════════════════════════════════════════════════════════════

void installDiffusionBindings(JSContext* ctx) {
    registerPipelineClass(ctx);
    registerPipelineStateClass(ctx);

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
    JS_SetPropertyStr(ctx, diff, "loadModel",
                      JS_NewCFunction(ctx, js_loadModel, "loadModel", 2));
    JS_SetPropertyStr(ctx, broObj, "diffusion", diff);

    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

void cleanupDiffusionBindings(JSContext* /*ctx*/) {
    // No-op: qjsbind owns the class finalizers; bro.diffusion is reached from
    // globalThis and swept by runtime teardown.
}

void setDiffusionAppContext(const std::string& basePath,
                            const util::AssetMounts* mounts) {
    s_basePath = basePath;
    s_mounts = mounts;
}

} // namespace bro::js

#endif  // BRO_WITH_DIFFUSION
