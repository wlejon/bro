// JS bindings for brolm — language-model inference.
//
// Installed onto bro.lm.* by installLmBindings(). The Qwen3 decoder model
// (multi-hundred-MB weights, KV-cache) and the byte-level BPE tokenizer live
// behind opaque qjsbind handles — JS never holds weight bytes or vocab maps,
// only the handles.
//
// The native API surface is in brolm/qwen.h, qwen_generate.h, qwen_tokenizer.h.
// brolm's CPU backend is always built, so the binding is always real (never a
// stub). Quantised GGUF weights (Q4_K / Q6_K / Q8_0) still load on the CPU but
// dispatch through GPU-only fused-dequant matmuls and throw at first forward
// when no GPU backend is enabled — that's a brolm/brotensor constraint, not
// something this binding can paper over.

#include "js/lm_bindings.h"
#include "js/async_job.h"

#include <qjsbind/qjsbind.h>

#include <brolm/qwen.h>
#include <brolm/qwen_generate.h>
#include <brolm/qwen_tokenizer.h>

#include <brotensor/gguf.h>
#include <brotensor/runtime.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace bro::js {

// ═══════════════════════════════════════════════════════════════════════════
// Wrapper structs (opaque handles)
// ═══════════════════════════════════════════════════════════════════════════

struct LMModelWrapper {
    std::unique_ptr<brolm::qwen::Qwen3Model> model;
    bool weights_loaded = false;
    int  cache_allocated_for = 0;
    brotensor::Device device = brotensor::Device::CPU;
    // Set while an async bro.lm.generate() runs on this model's background
    // thread; rejects a second concurrent generation (the model + KV cache are
    // single-owner). Cleared on the JS thread when the job's done() fires.
    std::atomic<bool> generating{false};
};

struct LMTokenizerWrapper {
    std::unique_ptr<brolm::qwen::Tokenizer> tok;
};

// ═══════════════════════════════════════════════════════════════════════════
// Small JS-value helpers (TU-local — same shape as diffusion_bindings)
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

// Pick the default device — CUDA when available, else CPU. brotensor::init()
// must have been called beforehand so the CUDA backend probe has run.
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

static bool parseDeviceOpt(JSContext* ctx, JSValueConst opts,
                           brotensor::Device& out, std::string& err) {
    if (!JS_IsObject(opts)) return true;
    JSValue v = JS_GetPropertyStr(ctx, opts, "device");
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        return true;
    }
    if (!JS_IsString(v)) {
        JS_FreeValue(ctx, v);
        err = "opts.device must be a string ('cpu' or 'cuda')";
        return false;
    }
    const char* s = JS_ToCString(ctx, v);
    std::string sv = s ? s : "";
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);
    if (sv == "cpu")  { out = brotensor::Device::CPU;  return true; }
    if (sv == "cuda") { out = brotensor::Device::CUDA; return true; }
    if (sv == "metal"){ out = brotensor::Device::Metal; return true; }
    err = "opts.device must be 'cpu' or 'cuda' (got '" + sv + "')";
    return false;
}

static const int32_t* getInt32Array(JSContext* ctx, JSValueConst v,
                                    size_t& count) {
    count = 0;
    if (!JS_IsObject(v)) return nullptr;
    size_t byteOff = 0, viewLen = 0, bpe = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, v, &byteOff, &viewLen, &bpe);
    if (JS_IsException(abuf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return nullptr;
    }
    size_t abufLen = 0;
    std::uint8_t* p = JS_GetArrayBuffer(ctx, &abufLen, abuf);
    JS_FreeValue(ctx, abuf);
    if (!p || bpe != sizeof(std::int32_t)) return nullptr;
    count = viewLen / sizeof(std::int32_t);
    return reinterpret_cast<const std::int32_t*>(p + byteOff);
}

// Accept either an Int32Array or a plain number[] of token ids. Returns an
// empty vector for anything else.
static std::vector<int32_t> readIdArray(JSContext* ctx, JSValueConst v) {
    std::vector<int32_t> out;
    size_t cnt = 0;
    if (const int32_t* p = getInt32Array(ctx, v, cnt)) {
        out.assign(p, p + cnt);
        return out;
    }
    if (JS_IsArray(v)) {
        std::uint32_t n = 0;
        JSValue lv = JS_GetPropertyStr(ctx, v, "length");
        JS_ToUint32(ctx, &n, lv);
        JS_FreeValue(ctx, lv);
        out.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            JSValue e = JS_GetPropertyUint32(ctx, v, i);
            int32_t t = 0;
            JS_ToInt32(ctx, &t, e);
            out.push_back(t);
            JS_FreeValue(ctx, e);
        }
    }
    return out;
}

// Map a JS opts object onto brolm::qwen::GenerateOptions (defaults preserved
// for absent keys). Reads `maxNewTokens`, `stopOnEos`, and a `sampling`
// sub-object with `temperature`, `topK`, `topP`, `seed`.
static brolm::qwen::GenerateOptions parseGenerateOptions(JSContext* ctx,
                                                        JSValueConst v) {
    brolm::qwen::GenerateOptions o;
    if (!JS_IsObject(v)) return o;
    getInt(ctx, v, "maxNewTokens", o.max_new_tokens);
    o.stop_on_eos = getBool(ctx, v, "stopOnEos", o.stop_on_eos);

    JSValue s = JS_GetPropertyStr(ctx, v, "sampling");
    if (JS_IsObject(s)) {
        getNum(ctx, s, "temperature", o.sampling.temperature);
        getInt(ctx, s, "topK", o.sampling.top_k);
        getNum(ctx, s, "topP", o.sampling.top_p);
        // seed: lossless via BigInt; JS number fits up to 2^53.
        JSValue sd = JS_GetPropertyStr(ctx, s, "seed");
        if (JS_IsBigInt(sd)) {
            std::uint64_t u = o.sampling.seed;
            if (JS_ToBigUint64(ctx, &u, sd) == 0) o.sampling.seed = u;
        } else if (JS_IsNumber(sd)) {
            int64_t t = (int64_t)o.sampling.seed;
            if (JS_ToInt64(ctx, &t, sd) == 0) o.sampling.seed = (std::uint64_t)t;
        }
        JS_FreeValue(ctx, sd);
    }
    JS_FreeValue(ctx, s);
    return o;
}

// ═══════════════════════════════════════════════════════════════════════════
// LMTokenizer methods
// ═══════════════════════════════════════════════════════════════════════════

static LMTokenizerWrapper* tokSelf(JSContext* ctx, JSValueConst this_val) {
    return qjsbind::unwrap<LMTokenizerWrapper>(ctx, this_val);
}

// encode(text, addSpecial=false) -> Int32Array
static JSValue js_tok_encode(JSContext* ctx, JSValueConst this_val,
                             int argc, JSValueConst* argv) {
    auto* w = tokSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "encode: not an LMTokenizer");
    std::string text;
    if (argc < 1 || !argStr(ctx, argv[0], text))
        return JS_ThrowTypeError(ctx, "encode(text, addSpecial?): text required");
    const bool addSpecial = (argc >= 2) && (JS_ToBool(ctx, argv[1]) == 1);
    try {
        auto ids = w->tok->encode(text, addSpecial);
        return qjsbind::make_int32_array(ctx, ids);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "encode: %s", e.what());
    }
}

// decode(ids) -> string. Accepts Int32Array or number[].
static JSValue js_tok_decode(JSContext* ctx, JSValueConst this_val,
                             int argc, JSValueConst* argv) {
    auto* w = tokSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "decode: not an LMTokenizer");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "decode(ids): ids required");
    auto ids = readIdArray(ctx, argv[0]);
    try {
        return JS_NewString(ctx, w->tok->decode(ids).c_str());
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "decode: %s", e.what());
    }
}

// applyChatTemplate([{role, content}, ...], addGenerationPrompt=true) -> string
static JSValue js_tok_applyChatTemplate(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv) {
    auto* w = tokSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "applyChatTemplate: not an LMTokenizer");
    if (argc < 1 || !JS_IsArray(argv[0]))
        return JS_ThrowTypeError(ctx,
            "applyChatTemplate(messages, addGenerationPrompt?): messages array required");

    std::vector<std::pair<std::string, std::string>> msgs;
    std::uint32_t n = 0;
    JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
    JS_ToUint32(ctx, &n, lv);
    JS_FreeValue(ctx, lv);
    msgs.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[0], i);
        std::string role, content;
        if (JS_IsObject(e)) {
            getStr(ctx, e, "role", role);
            getStr(ctx, e, "content", content);
        }
        msgs.emplace_back(std::move(role), std::move(content));
        JS_FreeValue(ctx, e);
    }
    const bool addGen = (argc < 2) ? true : (JS_ToBool(ctx, argv[1]) == 1);
    try {
        return JS_NewString(ctx,
            w->tok->apply_chat_template(msgs, addGen).c_str());
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "applyChatTemplate: %s", e.what());
    }
}

static void registerTokenizerClass(JSContext* ctx) {
    qjsbind::Class<LMTokenizerWrapper>(ctx, "LMTokenizer", qjsbind::NoGlobal)
        .get("eosId",       [](LMTokenizerWrapper* w) { return w->tok->eos_id(); })
        .get("imStartId",   [](LMTokenizerWrapper* w) { return w->tok->im_start_id(); })
        .get("imEndId",     [](LMTokenizerWrapper* w) { return w->tok->im_end_id(); })
        .get("endoftextId", [](LMTokenizerWrapper* w) { return w->tok->endoftext_id(); })
        .get("vocabCount",  [](LMTokenizerWrapper* w) { return (int)w->tok->vocab_count(); })
        .get("mergeCount",  [](LMTokenizerWrapper* w) { return (int)w->tok->merge_count(); })
        .method_raw("encode",            js_tok_encode,            2)
        .method_raw("decode",            js_tok_decode,            1)
        .method_raw("applyChatTemplate", js_tok_applyChatTemplate, 2);
}

// ═══════════════════════════════════════════════════════════════════════════
// LMModel methods
// ═══════════════════════════════════════════════════════════════════════════

static LMModelWrapper* modelSelf(JSContext* ctx, JSValueConst this_val) {
    return qjsbind::unwrap<LMModelWrapper>(ctx, this_val);
}

// allocateCache(maxSeqLen) — size the KV-cache once, before generate().
static JSValue js_model_allocateCache(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv) {
    auto* w = modelSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "allocateCache: not an LMModel");
    if (argc < 1 || !JS_IsNumber(argv[0]))
        return JS_ThrowTypeError(ctx, "allocateCache(maxSeqLen): number required");
    int32_t n = 0;
    JS_ToInt32(ctx, &n, argv[0]);
    try {
        brotensor::DeviceScope scope(w->device);
        w->model->allocate_cache(n);
        w->cache_allocated_for = n;
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "allocateCache: %s", e.what());
    }
    return JS_UNDEFINED;
}

// resetCache() — keep the allocation, zero the length.
static JSValue js_model_resetCache(JSContext* ctx, JSValueConst this_val,
                                   int, JSValueConst*) {
    auto* w = modelSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "resetCache: not an LMModel");
    w->model->reset_cache();
    return JS_UNDEFINED;
}

// generate(promptIds, opts?) -> Int32Array  (newly generated ids only)
//   opts.maxNewTokens
//   opts.stopOnEos
//   opts.eosId            (default -1 = no EOS stop; brolm::qwen::generate
//                          uses this verbatim, so callers pass tokenizer.eosId
//                          or tokenizer.endoftextId to wire stopping up)
//   opts.sampling.{temperature, topK, topP, seed}
static JSValue js_model_generate(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    auto* w = modelSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "generate: not an LMModel");
    if (!w->weights_loaded)
        return JS_ThrowInternalError(ctx, "generate: weights not loaded");
    if (argc < 1)
        return JS_ThrowTypeError(ctx,
            "generate(promptIds, opts?): promptIds required");

    std::vector<int32_t> prompt = readIdArray(ctx, argv[0]);
    if (prompt.empty())
        return JS_ThrowTypeError(ctx,
            "generate: promptIds must be a non-empty Int32Array or number[]");

    brolm::qwen::GenerateOptions opts;
    int eos_id = -1;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        opts = parseGenerateOptions(ctx, argv[1]);
        int tmp = -1;
        getInt(ctx, argv[1], "eosId", tmp);
        eos_id = tmp;
    }

    try {
        brotensor::DeviceScope scope(w->device);
        auto ids = brolm::qwen::generate(*w->model, prompt, eos_id, opts);
        return qjsbind::make_int32_array(ctx, ids);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "generate: %s", e.what());
    }
}

// Pull the last (vocab,) logits row from a (L, vocab) tensor to host FP32.
// Mirrors brolm::qwen's internal download_fp32/last_row_fp32 (which are
// TU-local in qwen_generate.cpp) so streaming here matches generate() exactly.
static std::vector<float> lastRowFp32(const brotensor::Tensor& logits) {
    const std::size_t vocab = static_cast<std::size_t>(logits.cols);
    const std::size_t rows  = static_cast<std::size_t>(logits.rows);
    const std::size_t n     = static_cast<std::size_t>(logits.size());
    std::vector<float> all;
    if (logits.dtype == brotensor::Dtype::FP16) {
        std::vector<std::uint16_t> bits(n);
        logits.copy_to_host_fp16(bits.data());
        all.resize(n);
        for (std::size_t i = 0; i < n; ++i)
            all[i] = brotensor::fp16_bits_to_fp32(bits[i]);
    } else {
        all = logits.to_host_vector();
    }
    const std::size_t base = (rows - 1) * vocab;
    return std::vector<float>(all.begin() + static_cast<std::ptrdiff_t>(base),
                             all.begin() + static_cast<std::ptrdiff_t>(base + vocab));
}

// generateStream(promptIds, opts?, onToken) -> Int32Array (all new ids)
//   Same prefill + decode loop as generate(), but invokes onToken(id) after
//   each sampled token so callers can stream text as it's produced. The
//   model's KV cache persists across the per-token forwards (forward() with
//   L==1 appends one position), so this is O(n), not O(n^2). onToken may
//   return false to stop early; the stopping token is still reported. The eos
//   token is NOT passed to onToken nor included in the result.
static JSValue js_model_generateStream(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv) {
    auto* w = modelSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "generateStream: not an LMModel");
    if (!w->weights_loaded)
        return JS_ThrowInternalError(ctx, "generateStream: weights not loaded");
    if (argc < 1)
        return JS_ThrowTypeError(ctx,
            "generateStream(promptIds, opts?, onToken): promptIds required");

    std::vector<int32_t> prompt = readIdArray(ctx, argv[0]);
    if (prompt.empty())
        return JS_ThrowTypeError(ctx,
            "generateStream: promptIds must be a non-empty Int32Array or number[]");

    brolm::qwen::GenerateOptions opts;
    int eos_id = -1;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        opts = parseGenerateOptions(ctx, argv[1]);
        int tmp = -1;
        getInt(ctx, argv[1], "eosId", tmp);
        eos_id = tmp;
    }

    JSValueConst onToken = (argc >= 3) ? argv[2] : JS_UNDEFINED;
    const bool hasCb = JS_IsFunction(ctx, onToken);
    if (argc >= 3 && !hasCb)
        return JS_ThrowTypeError(ctx, "generateStream: onToken must be a function");

    std::vector<int32_t> generated;
    if (opts.max_new_tokens <= 0)
        return qjsbind::make_int32_array(ctx, generated);

    try {
        brotensor::DeviceScope scope(w->device);
        const int vocab = w->model->config().vocab_size;
        w->model->allocate_cache(static_cast<int>(prompt.size()) +
                                 opts.max_new_tokens);
        std::mt19937_64 rng(opts.sampling.seed);
        const bool stop = opts.stop_on_eos && eos_id >= 0;

        brotensor::Tensor logits;
        w->model->forward(prompt.data(), static_cast<int>(prompt.size()), logits);
        std::vector<float> row = lastRowFp32(logits);
        int next = brolm::qwen::sample_token(row.data(), vocab, opts.sampling, rng);

        while (true) {
            if (stop && next == eos_id) break;
            generated.push_back(static_cast<int32_t>(next));

            if (hasCb) {
                JSValue arg = JS_NewInt32(ctx, next);
                JSValue ret = JS_Call(ctx, onToken, JS_UNDEFINED, 1, &arg);
                JS_FreeValue(ctx, arg);
                if (JS_IsException(ret)) return ret;  // propagate JS error
                const bool keepGoing = JS_ToBool(ctx, ret) != 0 || JS_IsUndefined(ret);
                JS_FreeValue(ctx, ret);
                if (!keepGoing) break;
            }

            if (static_cast<int>(generated.size()) >= opts.max_new_tokens) break;

            int32_t cur = generated.back();
            w->model->forward(&cur, 1, logits);
            row = lastRowFp32(logits);
            next = brolm::qwen::sample_token(row.data(), vocab, opts.sampling, rng);
        }
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "generateStream: %s", e.what());
    }

    return qjsbind::make_int32_array(ctx, generated);
}

static void registerModelClass(JSContext* ctx) {
    qjsbind::Class<LMModelWrapper>(ctx, "LMModel", qjsbind::NoGlobal)
        .get("vocabSize",  [](LMModelWrapper* w) { return w->model->config().vocab_size; })
        .get("hiddenSize", [](LMModelWrapper* w) { return w->model->config().hidden_size; })
        .get("numLayers",  [](LMModelWrapper* w) { return w->model->config().num_hidden_layers; })
        .get("maxSeqLen",  [](LMModelWrapper* w) { return w->model->config().max_position_embeddings; })
        .get("cacheLen",   [](LMModelWrapper* w) { return w->model->cache_len(); })
        .method_raw("allocateCache",  js_model_allocateCache,  1)
        .method_raw("resetCache",     js_model_resetCache,     0)
        .method_raw("generate",       js_model_generate,       2)
        .method_raw("generateStream", js_model_generateStream, 3);
}

// ═══════════════════════════════════════════════════════════════════════════
// bro.lm free functions
// ═══════════════════════════════════════════════════════════════════════════

// bro.lm.init() — brotensor::init() (idempotent). Workers may call it to warm
// up explicitly; loadQwen() also calls it.
static JSValue js_init(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    try {
        brotensor::init();
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "bro.lm.init: %s", e.what());
    }
    return JS_UNDEFINED;
}

// bro.lm.loadQwen(ggufPath, opts?) -> { model, tokenizer }
//   Single-file Qwen3 loader. Opens a Qwen3 GGUF, builds the Qwen3Config from
//   its metadata, constructs the decoder, loads the tensor weights, and reads
//   the tokenizer's vocab + merges out of the same file. Both handles are
//   bundled into one object so a caller can destructure them.
//   opts.device: 'cuda' | 'cpu' — defaults to CUDA when available, else CPU.
//   Qwen3Model takes the device implicitly via brotensor::default_device(),
//   so we install a DeviceScope around construction + load_weights + cache
//   allocation paths. Forward dispatches on the device the model's tensors
//   live on, so no scope needed at generate() time.
// Build the Qwen3 model + tokenizer from a GGUF on disk. Heavy + blocking (file
// IO + GPU upload); shared by the sync and async loadQwen paths. Throws on error.
static void buildQwen(const std::string& path, brotensor::Device dev,
                      std::unique_ptr<LMModelWrapper>& mw_out,
                      std::unique_ptr<LMTokenizerWrapper>& tw_out) {
    brotensor::gguf::File f = brotensor::gguf::File::open(path);
    auto cfg = brolm::qwen::Qwen3Config::from_gguf(f);

    auto mw = std::make_unique<LMModelWrapper>();
    mw->device = dev;
    {
        brotensor::DeviceScope scope(dev);
        mw->model = std::make_unique<brolm::qwen::Qwen3Model>(cfg);
        mw->model->load_weights(f);
    }
    mw->weights_loaded = true;

    auto tw = std::make_unique<LMTokenizerWrapper>();
    tw->tok = std::make_unique<brolm::qwen::Tokenizer>(
        brolm::qwen::Tokenizer::from_gguf(f));

    std::fprintf(stderr, "[INFO] [lm] Qwen3 loaded on %s\n", deviceName(dev));
    mw_out = std::move(mw);
    tw_out = std::move(tw);
}

// State for an async load: the work thread fills mw/tw (or error); the JS-thread
// done() wraps them and invokes onReady/onError.
struct QwenLoadState {
    std::string                         path;
    brotensor::Device                   dev = brotensor::Device::CPU;
    std::unique_ptr<LMModelWrapper>     mw;
    std::unique_ptr<LMTokenizerWrapper> tw;
    JSValue onReady = JS_UNDEFINED;
    JSValue onError = JS_UNDEFINED;
    bool    hasReady = false, hasError = false;
};

// bro.lm.loadQwen(ggufPath, opts?) -> { model, tokenizer }  (sync)
//                                  -> AsyncHandle             (async, if opts.onReady)
//   opts.device: 'cuda' | 'cpu' — defaults to CUDA when available, else CPU.
//   opts.onReady({model,tokenizer}) / opts.onError(message): when onReady is a
//   function the load runs on a background thread (non-blocking, parallelizable
//   with other loads) and these fire on the JS thread.
static JSValue js_loadQwen(JSContext* ctx, JSValueConst,
                           int argc, JSValueConst* argv) {
    std::string path;
    if (argc < 1 || !argStr(ctx, argv[0], path))
        return JS_ThrowTypeError(ctx, "loadQwen(ggufPath, opts?): path string required");

    brotensor::init();
    brotensor::Device dev = autoDevice();
    if (argc >= 2) {
        std::string err;
        if (!parseDeviceOpt(ctx, argv[1], dev, err))
            return JS_ThrowTypeError(ctx, "loadQwen: %s", err.c_str());
    }

    const bool haveOpts = (argc >= 2) && JS_IsObject(argv[1]);
    JSValue onReady = haveOpts ? JS_GetPropertyStr(ctx, argv[1], "onReady")
                               : JS_UNDEFINED;
    JSValue onError = haveOpts ? JS_GetPropertyStr(ctx, argv[1], "onError")
                               : JS_UNDEFINED;
    const bool async = JS_IsFunction(ctx, onReady);

    // ── Sync path (back-compat) ──
    if (!async) {
        JS_FreeValue(ctx, onReady);
        JS_FreeValue(ctx, onError);
        try {
            std::unique_ptr<LMModelWrapper>     mw;
            std::unique_ptr<LMTokenizerWrapper> tw;
            buildQwen(path, dev, mw, tw);
            JSValue out = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, out, "model",
                qjsbind::wrap<LMModelWrapper>(ctx, mw.release()));
            JS_SetPropertyStr(ctx, out, "tokenizer",
                qjsbind::wrap<LMTokenizerWrapper>(ctx, tw.release()));
            return out;
        } catch (const std::exception& e) {
            return JS_ThrowInternalError(ctx, "loadQwen: %s", e.what());
        }
    }

    // ── Async path ──
    auto ls = std::make_shared<QwenLoadState>();
    ls->path     = path;
    ls->dev      = dev;
    ls->hasReady = true;
    ls->onReady  = JS_DupValue(ctx, onReady);
    ls->hasError = JS_IsFunction(ctx, onError);
    ls->onError  = ls->hasError ? JS_DupValue(ctx, onError) : JS_UNDEFINED;
    JS_FreeValue(ctx, onReady);
    JS_FreeValue(ctx, onError);

    auto work = [ls](const std::atomic<bool>&) {
        buildQwen(ls->path, ls->dev, ls->mw, ls->tw);  // throws -> error
    };
    auto done = [ls](JSContext* c, bool /*cancelled*/, const std::string& error) {
        if (!error.empty() || !ls->mw) {
            if (ls->hasError) {
                JSValue e = JS_NewString(c, error.empty() ? "loadQwen failed"
                                                          : error.c_str());
                JSValue r = JS_Call(c, ls->onError, JS_UNDEFINED, 1, &e);
                if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
                JS_FreeValue(c, r);
                JS_FreeValue(c, e);
            }
        } else {
            JSValue out = JS_NewObject(c);
            JS_SetPropertyStr(c, out, "model",
                qjsbind::wrap<LMModelWrapper>(c, ls->mw.release()));
            JS_SetPropertyStr(c, out, "tokenizer",
                qjsbind::wrap<LMTokenizerWrapper>(c, ls->tw.release()));
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

// bro.lm.loadTokenizer({ vocabPath, mergesPath }) -> LMTokenizer
//   For HF-format tokenizers (vocab.json + merges.txt). No model loader for
//   the safetensors path yet — Qwen3Config has no JSON loader in brolm, so
//   safetensors users would have to parse config.json themselves; future
//   work.
static JSValue js_loadTokenizer(JSContext* ctx, JSValueConst,
                                int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx,
            "loadTokenizer(opts): opts object required");
    std::string vocab, merges;
    if (!getStr(ctx, argv[0], "vocabPath", vocab) ||
        !getStr(ctx, argv[0], "mergesPath", merges))
        return JS_ThrowTypeError(ctx,
            "loadTokenizer: opts.vocabPath and opts.mergesPath required");
    try {
        auto tw = std::make_unique<LMTokenizerWrapper>();
        tw->tok = std::make_unique<brolm::qwen::Tokenizer>(
            brolm::qwen::Tokenizer::load(vocab, merges));
        return qjsbind::wrap<LMTokenizerWrapper>(ctx, tw.release());
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "loadTokenizer: %s", e.what());
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Async streaming generation — bro.lm.generate(model, promptIds, opts)
// ═══════════════════════════════════════════════════════════════════════════
//
// Runs the same prefill + decode loop as generateStream(), but on a background
// thread via the async-job runner, so the JS thread (and the app) stays
// responsive and the generation is cancellable mid-loop. Returns an AsyncHandle
// with .cancel(); opts.onToken(id) fires per token, opts.onDone(ids, info) once
// at the end, both on the JS thread (drained by tickAsync). info is
// { cancelled, error? }. This is the engine-owned replacement for a JS Worker +
// blocking generateStream: the app just says "generate, here's how to stop".

// Shared between the work thread (sole writer of the committed prefix) and the
// JS thread (sole reader / caller of the JS callbacks). Held by shared_ptr.
struct LmStream {
    std::vector<int32_t> ids;            // reserved to maxNewTokens up front
    std::atomic<size_t>  produced{0};    // committed prefix length (publish)
    size_t               drained = 0;    // JS-thread cursor into ids
    JSValue              onToken = JS_UNDEFINED;  // dup'd; JS_UNDEFINED if absent
    JSValue              onDone  = JS_UNDEFINED;  // dup'd; JS_UNDEFINED if absent
    JSValue              modelRef = JS_UNDEFINED; // dup of the model JS object
    bool                 hasOnToken = false;
    bool                 hasOnDone  = false;
};

static JSValue js_lm_generate(JSContext* ctx, JSValueConst,
                              int argc, JSValueConst* argv) {
    if (argc < 2)
        return JS_ThrowTypeError(ctx,
            "generate(model, promptIds, opts): model and promptIds required");
    auto* w = qjsbind::unwrap<LMModelWrapper>(ctx, argv[0]);
    if (!w) return JS_ThrowTypeError(ctx, "generate: arg 0 must be an LMModel");
    if (!w->weights_loaded)
        return JS_ThrowInternalError(ctx, "generate: weights not loaded");

    std::vector<int32_t> prompt = readIdArray(ctx, argv[1]);
    if (prompt.empty())
        return JS_ThrowTypeError(ctx,
            "generate: promptIds must be a non-empty Int32Array or number[]");

    brolm::qwen::GenerateOptions opts;
    int eos_id = -1;
    JSValue onToken = JS_UNDEFINED, onDone = JS_UNDEFINED;
    if (argc >= 3 && JS_IsObject(argv[2])) {
        opts = parseGenerateOptions(ctx, argv[2]);
        int tmp = -1;
        getInt(ctx, argv[2], "eosId", tmp);
        eos_id = tmp;
        onToken = JS_GetPropertyStr(ctx, argv[2], "onToken");
        onDone  = JS_GetPropertyStr(ctx, argv[2], "onDone");
    }
    if (opts.max_new_tokens <= 0) {
        JS_FreeValue(ctx, onToken);
        JS_FreeValue(ctx, onDone);
        return JS_ThrowTypeError(ctx, "generate: opts.maxNewTokens must be > 0");
    }

    // Claim the model for this generation (single-owner; one in flight).
    bool expected = false;
    if (!w->generating.compare_exchange_strong(expected, true)) {
        JS_FreeValue(ctx, onToken);
        JS_FreeValue(ctx, onDone);
        return JS_ThrowInternalError(ctx,
            "generate: a generation is already in flight on this model");
    }

    auto st = std::make_shared<LmStream>();
    st->hasOnToken = JS_IsFunction(ctx, onToken);
    st->hasOnDone  = JS_IsFunction(ctx, onDone);
    st->onToken    = st->hasOnToken ? JS_DupValue(ctx, onToken) : JS_UNDEFINED;
    st->onDone     = st->hasOnDone  ? JS_DupValue(ctx, onDone)  : JS_UNDEFINED;
    st->modelRef   = JS_DupValue(ctx, argv[0]);  // keep the model alive
    st->ids.reserve(static_cast<size_t>(opts.max_new_tokens));
    JS_FreeValue(ctx, onToken);
    JS_FreeValue(ctx, onDone);

    LMModelWrapper* mw = w;

    // Background thread: prefill + greedy/sampled decode, publishing each token.
    // Mirrors js_model_generateStream's loop exactly (eos excluded from output).
    auto work = [st, mw, prompt = std::move(prompt), opts, eos_id]
                (const std::atomic<bool>& cancel) {
        brotensor::DeviceScope scope(mw->device);
        const int vocab = mw->model->config().vocab_size;
        mw->model->allocate_cache(static_cast<int>(prompt.size()) +
                                  opts.max_new_tokens);
        std::mt19937_64 rng(opts.sampling.seed);
        const bool stop = opts.stop_on_eos && eos_id >= 0;

        brotensor::Tensor logits;
        mw->model->forward(prompt.data(), static_cast<int>(prompt.size()), logits);
        std::vector<float> row = lastRowFp32(logits);
        int next = brolm::qwen::sample_token(row.data(), vocab, opts.sampling, rng);

        while (true) {
            if (stop && next == eos_id) break;
            st->ids.push_back(next);
            st->produced.store(st->ids.size(), std::memory_order_release);
            if (cancel.load(std::memory_order_acquire)) break;
            if (static_cast<int>(st->ids.size()) >= opts.max_new_tokens) break;
            int32_t cur = next;
            mw->model->forward(&cur, 1, logits);
            row = lastRowFp32(logits);
            next = brolm::qwen::sample_token(row.data(), vocab, opts.sampling, rng);
        }
    };

    // JS thread, per tick: deliver newly committed tokens to onToken.
    auto poll = [st](JSContext* c) {
        if (!st->hasOnToken) return;
        const size_t n = st->produced.load(std::memory_order_acquire);
        while (st->drained < n) {
            JSValue arg = JS_NewInt32(c, st->ids[st->drained++]);
            JSValue r = JS_Call(c, st->onToken, JS_UNDEFINED, 1, &arg);
            JS_FreeValue(c, arg);
            if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
            JS_FreeValue(c, r);
        }
    };

    // JS thread, once: hand the full id array + {cancelled,error} to onDone,
    // free the dup'd values, release the model.
    auto done = [st, mw](JSContext* c, bool cancelled, const std::string& error) {
        const size_t n = st->produced.load(std::memory_order_acquire);
        if (st->hasOnDone) {
            JSValue arr  = qjsbind::make_int32_array(c, st->ids.data(), n);
            JSValue info = JS_NewObject(c);
            JS_SetPropertyStr(c, info, "cancelled", JS_NewBool(c, cancelled));
            if (!error.empty())
                JS_SetPropertyStr(c, info, "error", JS_NewString(c, error.c_str()));
            JSValue args[2] = { arr, info };
            JSValue r = JS_Call(c, st->onDone, JS_UNDEFINED, 2, args);
            if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
            JS_FreeValue(c, r);
            JS_FreeValue(c, arr);
            JS_FreeValue(c, info);
        }
        if (st->hasOnToken) JS_FreeValue(c, st->onToken);
        if (st->hasOnDone)  JS_FreeValue(c, st->onDone);
        JS_FreeValue(c, st->modelRef);
        mw->generating.store(false, std::memory_order_release);
    };

    return launchAsyncJob(ctx, std::move(work), std::move(poll), std::move(done));
}

// ═══════════════════════════════════════════════════════════════════════════
// Install
// ═══════════════════════════════════════════════════════════════════════════

void installLmBindings(JSContext* ctx) {
    registerTokenizerClass(ctx);
    registerModelClass(ctx);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        JS_FreeValue(ctx, broObj);
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue lm = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, lm, "init",
        JS_NewCFunction(ctx, js_init, "init", 0));
    JS_SetPropertyStr(ctx, lm, "loadQwen",
        JS_NewCFunction(ctx, js_loadQwen, "loadQwen", 2));
    JS_SetPropertyStr(ctx, lm, "loadTokenizer",
        JS_NewCFunction(ctx, js_loadTokenizer, "loadTokenizer", 1));
    JS_SetPropertyStr(ctx, lm, "generate",
        JS_NewCFunction(ctx, js_lm_generate, "generate", 3));
    JS_SetPropertyStr(ctx, broObj, "lm", lm);

    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

void cleanupLmBindings(JSContext* /*ctx*/) {
    // No-op: qjsbind owns the class finalizers; bro.lm is reached from
    // globalThis and swept by runtime teardown.
}

}  // namespace bro::js
