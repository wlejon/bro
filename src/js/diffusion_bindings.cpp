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

// Read a Float32Array's element pointer + count. Returns nullptr if `v` is
// not a Float32Array. The pointer is valid only for the current call.
static const float* getFloatArray(JSContext* ctx, JSValueConst v, size_t& count) {
    count = 0;
    if (!JS_IsObject(v)) return nullptr;
    size_t byteOff = 0, viewLen = 0, bytesPerEl = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, v, &byteOff, &viewLen, &bytesPerEl);
    if (JS_IsException(abuf)) { JS_FreeValue(ctx, JS_GetException(ctx)); return nullptr; }
    size_t abufLen = 0;
    std::uint8_t* p = JS_GetArrayBuffer(ctx, &abufLen, abuf);
    JS_FreeValue(ctx, abuf);
    if (!p || bytesPerEl != sizeof(float)) return nullptr;
    count = viewLen / sizeof(float);
    return reinterpret_cast<const float*>(p + byteOff);
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
            const float* fp = getFloatArray(ctx, nv, cnt);
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

// bro.diffusion.loadModel(modelDir) -> Pipeline
//   Load a complete diffusers model directory — `model_index.json` plus one
//   component subdir each (text_encoder/, unet/ or transformer/, vae/,
//   tokenizer/, scheduler/, ...). The model family is auto-detected from
//   `_class_name`: a StableDiffusion directory builds the CLIP + UNet stack, a
//   Flux directory builds CLIP (pooled) + the T5-XXL encoder + the Flux DiT.
//   Every weight and tokenizer is loaded here — the returned Pipeline is fully
//   ready, so call generate()/prime() directly (no loadWeights()/createPipeline).
static JSValue js_loadModel(JSContext* ctx, JSValueConst, int argc,
                            JSValueConst* argv) {
    std::string dir;
    if (argc < 1 || !argStr(ctx, argv[0], dir))
        return JS_ThrowTypeError(ctx, "loadModel(modelDir): path string required");
    try {
        brotensor::init();
        auto w = std::make_unique<PipelineWrapper>();
        // from_model_dir() returns by value; Pipeline is move-only, so the
        // temporary moves into the heap-allocated wrapper slot.
        w->pipeline = std::make_unique<bdp::Pipeline>(
            bdp::Pipeline::from_model_dir(dir));
        w->weights_loaded = true;
        return qjsbind::wrap<PipelineWrapper>(ctx, w.release());
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
        bds::File f = bds::File::open(path);
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
        cfg.model_class == brodiffusion::ModelClass::Flux ? "Flux" :
        cfg.model_class == brodiffusion::ModelClass::Sana ? "Sana" :
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
        .method_raw("config",             js_pipeline_config,             0);
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
                const float* fp = getFloatArray(ctx, dv, cnt);
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
                      JS_NewCFunction(ctx, js_loadModel, "loadModel", 1));
    JS_SetPropertyStr(ctx, broObj, "diffusion", diff);

    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

void cleanupDiffusionBindings(JSContext* /*ctx*/) {
    // No-op: qjsbind owns the class finalizers; bro.diffusion is reached from
    // globalThis and swept by runtime teardown.
}

} // namespace bro::js
