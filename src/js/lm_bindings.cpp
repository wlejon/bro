// JS bindings for brolm — language-model inference.
//
// Installed onto bro.lm.* by installLmBindings(). The decoder models
// (multi-hundred-MB weights, KV-cache) and their BPE tokenizers live behind
// opaque qjsbind handles — JS never holds weight bytes or vocab maps, only
// the handles.
//
// Three model families:
//   - Qwen3 (loadQwen): GGUF, Qwen BPE tokenizer (vocab.json + merges.txt
//     convention), ChatML chat template.
//   - Mistral 3.1 text (loadMistral): GGUF, the native "tekken" tiktoken-style
//     tokenizer (tekken.json), [INST] chat template.
//   - Qwen3.5 (loadQwen35): safetensors checkpoint dir via brolm's VLM driver
//     (hybrid full/linear-attention decoder, M-RoPE). Text-in/text-out here;
//     the driver owns tokenization, so generate takes a STRING prompt.
//
// Qwen3 and Mistral share the duck-typed dense-decoder interface
// (allocate_cache / forward / config dims) without a common C++ base, so the
// binding erases the type behind LMDecoder and one LMModel JS class fronts
// both. Qwen3.5's forward needs M-RoPE position streams and a per-layer
// hybrid cache — it stays behind brolm's qwen35::VLM driver instead.
//
// The native API surface is in brolm/qwen*.h, mistral3_*.h, qwen35_vl.h.
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
#include <brolm/mistral3_config.h>
#include <brolm/mistral3_text.h>
#include <brolm/mistral_tokenizer.h>
#include <brolm/qwen35_vl.h>

#include <brotensor/gguf.h>
#include <brotensor/runtime.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace bro::js {

// ═══════════════════════════════════════════════════════════════════════════
// Wrapper structs (opaque handles)
// ═══════════════════════════════════════════════════════════════════════════

// Type-erased dense decoder. Qwen3 and Mistral 3.1 expose the same decode
// interface by convention (brolm/detail/generate.h's duck type) but no shared
// base class; this is the binding-side base that lets one LMModel JS class
// front both families.
struct LMDecoder {
    virtual ~LMDecoder() = default;
    virtual const char* family()    const = 0;   // "qwen3" | "mistral3"
    virtual int  vocabSize()  const = 0;
    virtual int  hiddenSize() const = 0;
    virtual int  numLayers()  const = 0;
    virtual int  maxSeqLen()  const = 0;
    virtual int  cacheLen()   const = 0;
    virtual void allocateCache(int n) = 0;
    virtual void resetCache() = 0;
    // Append L tokens, advance the KV cache, write (L, vocab) logits.
    virtual void forward(const int32_t* ids, int L, brotensor::Tensor& out) = 0;
};

struct QwenDecoder final : LMDecoder {
    brolm::qwen::Qwen3Model m;
    explicit QwenDecoder(const brolm::qwen::Qwen3Config& cfg) : m(cfg) {}
    const char* family()    const override { return "qwen3"; }
    int  vocabSize()  const override { return m.config().vocab_size; }
    int  hiddenSize() const override { return m.config().hidden_size; }
    int  numLayers()  const override { return m.config().num_hidden_layers; }
    int  maxSeqLen()  const override { return m.config().max_position_embeddings; }
    int  cacheLen()   const override { return m.cache_len(); }
    void allocateCache(int n) override { m.allocate_cache(n); }
    void resetCache() override { m.reset_cache(); }
    void forward(const int32_t* ids, int L, brotensor::Tensor& out) override {
        m.forward(ids, L, out);
    }
};

struct MistralDecoder final : LMDecoder {
    brolm::mistral3::TextModel m;
    explicit MistralDecoder(const brolm::mistral3::Mistral3Config::Text& cfg)
        : m(cfg) {}
    const char* family()    const override { return "mistral3"; }
    int  vocabSize()  const override { return m.config().vocab_size; }
    int  hiddenSize() const override { return m.config().hidden_size; }
    int  numLayers()  const override { return m.config().num_hidden_layers; }
    int  maxSeqLen()  const override { return m.config().max_position_embeddings; }
    int  cacheLen()   const override { return m.cache_len(); }
    void allocateCache(int n) override { m.allocate_cache(n); }
    void resetCache() override { m.reset_cache(); }
    void forward(const int32_t* ids, int L, brotensor::Tensor& out) override {
        m.forward(ids, L, out);
    }
};

struct LMModelWrapper {
    std::unique_ptr<LMDecoder> model;
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

struct MistralTokenizerWrapper {
    std::unique_ptr<brolm::mistral::Tokenizer> tok;
};

// Qwen3.5 stays behind brolm's VLM driver (it owns the tokenizer, the M-RoPE
// position streams, and the hybrid per-layer cache).
struct Qwen35Wrapper {
    std::unique_ptr<brolm::qwen35::VLM> vlm;
    int maxSeqLen = 4096;
    brotensor::Device device = brotensor::Device::CPU;
    std::atomic<bool> generating{false};
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

// Pick the default device — CUDA, then Metal, then CPU. brotensor::init()
// must have been called beforehand so the GPU backend probes have run.
static brotensor::Device autoDevice() {
    if (brotensor::is_available(brotensor::Device::CUDA))  return brotensor::Device::CUDA;
    if (brotensor::is_available(brotensor::Device::Metal)) return brotensor::Device::Metal;
    return brotensor::Device::CPU;
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
        err = "opts.device must be a string ('cpu', 'cuda', or 'metal')";
        return false;
    }
    const char* s = JS_ToCString(ctx, v);
    std::string sv = s ? s : "";
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);
    if (sv == "cpu")  { out = brotensor::Device::CPU;  return true; }
    if (sv == "cuda") { out = brotensor::Device::CUDA; return true; }
    if (sv == "metal"){ out = brotensor::Device::Metal; return true; }
    err = "opts.device must be 'cpu', 'cuda', or 'metal' (got '" + sv + "')";
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
// MistralTokenizer methods — the native "tekken" tiktoken-style tokenizer
// ═══════════════════════════════════════════════════════════════════════════

static MistralTokenizerWrapper* mtokSelf(JSContext* ctx, JSValueConst this_val) {
    return qjsbind::unwrap<MistralTokenizerWrapper>(ctx, this_val);
}

// encode(text, addSpecial=false) -> Int32Array
//   addSpecial prepends BOS (<s>). A bare prompt wants it (Mistral prefixes
//   BOS at the tokenizer level); the output of applyChatTemplate does NOT
//   (the template already emits a leading <s>).
static JSValue js_mtok_encode(JSContext* ctx, JSValueConst this_val,
                              int argc, JSValueConst* argv) {
    auto* w = mtokSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "encode: not a MistralTokenizer");
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
static JSValue js_mtok_decode(JSContext* ctx, JSValueConst this_val,
                              int argc, JSValueConst* argv) {
    auto* w = mtokSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "decode: not a MistralTokenizer");
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
//   Mistral's [INST] template. Encode the result with addSpecial=false — the
//   template emits its own leading <s>.
static JSValue js_mtok_applyChatTemplate(JSContext* ctx, JSValueConst this_val,
                                         int argc, JSValueConst* argv) {
    auto* w = mtokSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "applyChatTemplate: not a MistralTokenizer");
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

static void registerMistralTokenizerClass(JSContext* ctx) {
    qjsbind::Class<MistralTokenizerWrapper>(ctx, "MistralTokenizer",
                                            qjsbind::NoGlobal)
        .get("eosId",      [](MistralTokenizerWrapper* w) { return w->tok->eos_id(); })
        .get("bosId",      [](MistralTokenizerWrapper* w) { return w->tok->bos_id(); })
        .get("vocabCount", [](MistralTokenizerWrapper* w) { return (int)w->tok->vocab_count(); })
        .method_raw("encode",            js_mtok_encode,            2)
        .method_raw("decode",            js_mtok_decode,            1)
        .method_raw("applyChatTemplate", js_mtok_applyChatTemplate, 2);
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
        w->model->allocateCache(n);
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
    w->model->resetCache();
    return JS_UNDEFINED;
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

// The prefill + decode loop, shared by the blocking generate paths (the
// streaming/async paths inline the same loop with their callback plumbing).
// Mirrors brolm::detail::generate: prefill in one forward, then one token a
// step; EOS is excluded from the result. Runs under the caller's DeviceScope.
static std::vector<int32_t> runDecode(LMDecoder& model,
                                      const std::vector<int32_t>& prompt,
                                      int eos_id,
                                      const brolm::qwen::GenerateOptions& opts) {
    std::vector<int32_t> generated;
    if (prompt.empty() || opts.max_new_tokens <= 0) return generated;

    const int vocab = model.vocabSize();
    model.allocateCache(static_cast<int>(prompt.size()) + opts.max_new_tokens);
    std::mt19937_64 rng(opts.sampling.seed);
    const bool stop = opts.stop_on_eos && eos_id >= 0;

    brotensor::Tensor logits;
    model.forward(prompt.data(), static_cast<int>(prompt.size()), logits);
    std::vector<float> row = lastRowFp32(logits);
    int next = brolm::qwen::sample_token(row.data(), vocab, opts.sampling, rng);

    while (true) {
        if (stop && next == eos_id) break;
        generated.push_back(static_cast<int32_t>(next));
        if (static_cast<int>(generated.size()) >= opts.max_new_tokens) break;
        int32_t cur = generated.back();
        model.forward(&cur, 1, logits);
        row = lastRowFp32(logits);
        next = brolm::qwen::sample_token(row.data(), vocab, opts.sampling, rng);
    }
    return generated;
}

// generate(promptIds, opts?) -> Int32Array  (newly generated ids only)
//   opts.maxNewTokens
//   opts.stopOnEos
//   opts.eosId            (default -1 = no EOS stop; callers pass
//                          tokenizer.eosId or tokenizer.endoftextId to wire
//                          stopping up)
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
        auto ids = runDecode(*w->model, prompt, eos_id, opts);
        return qjsbind::make_int32_array(ctx, ids);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "generate: %s", e.what());
    }
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
        const int vocab = w->model->vocabSize();
        w->model->allocateCache(static_cast<int>(prompt.size()) +
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
        .get("family",     [](LMModelWrapper* w) { return std::string(w->model->family()); })
        .get("vocabSize",  [](LMModelWrapper* w) { return w->model->vocabSize(); })
        .get("hiddenSize", [](LMModelWrapper* w) { return w->model->hiddenSize(); })
        .get("numLayers",  [](LMModelWrapper* w) { return w->model->numLayers(); })
        .get("maxSeqLen",  [](LMModelWrapper* w) { return w->model->maxSeqLen(); })
        .get("cacheLen",   [](LMModelWrapper* w) { return w->model->cacheLen(); })
        .method_raw("allocateCache",  js_model_allocateCache,  1)
        .method_raw("resetCache",     js_model_resetCache,     0)
        .method_raw("generate",       js_model_generate,       2)
        .method_raw("generateStream", js_model_generateStream, 3);
}

// ═══════════════════════════════════════════════════════════════════════════
// Qwen35Model methods — text generation through brolm's qwen35::VLM driver
// ═══════════════════════════════════════════════════════════════════════════

static Qwen35Wrapper* q35Self(JSContext* ctx, JSValueConst this_val) {
    return qjsbind::unwrap<Qwen35Wrapper>(ctx, this_val);
}

// encode(text, addSpecial=false) -> Int32Array  (driver-owned Qwen BPE)
static JSValue js_q35_encode(JSContext* ctx, JSValueConst this_val,
                             int argc, JSValueConst* argv) {
    auto* w = q35Self(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "encode: not a Qwen35Model");
    std::string text;
    if (argc < 1 || !argStr(ctx, argv[0], text))
        return JS_ThrowTypeError(ctx, "encode(text, addSpecial?): text required");
    const bool addSpecial = (argc >= 2) && (JS_ToBool(ctx, argv[1]) == 1);
    try {
        auto ids = w->vlm->tokenizer().encode(text, addSpecial);
        return qjsbind::make_int32_array(ctx, ids);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "encode: %s", e.what());
    }
}

// decode(ids) -> string
static JSValue js_q35_decode(JSContext* ctx, JSValueConst this_val,
                             int argc, JSValueConst* argv) {
    auto* w = q35Self(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "decode: not a Qwen35Model");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "decode(ids): ids required");
    auto ids = readIdArray(ctx, argv[0]);
    try {
        return JS_NewString(ctx, w->vlm->tokenizer().decode(ids).c_str());
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "decode: %s", e.what());
    }
}

// generate(prompt, opts?) -> Int32Array  (newly generated ids only)
//   prompt: STRING — the VLM driver owns tokenization (and, later, image
//   splicing). Wrap chat turns in ChatML yourself:
//     <|im_start|>user\n{...}<|im_end|>\n<|im_start|>assistant\n
//   Generation stops on <|im_end|> / <|endoftext|> or the budget.
//   opts.maxNewTokens / opts.sampling.{temperature, topK, topP, seed}
//   opts.onToken(id): per generated token, synchronous on this thread;
//   return false to stop early.
static JSValue js_q35_generate(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    auto* w = q35Self(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "generate: not a Qwen35Model");
    std::string prompt;
    if (argc < 1 || !argStr(ctx, argv[0], prompt))
        return JS_ThrowTypeError(ctx, "generate(prompt, opts?): prompt string required");

    brolm::qwen::GenerateOptions opts;
    JSValue onToken = JS_UNDEFINED;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        opts = parseGenerateOptions(ctx, argv[1]);
        onToken = JS_GetPropertyStr(ctx, argv[1], "onToken");
    }
    const bool hasCb = JS_IsFunction(ctx, onToken);

    brolm::qwen35::VLM::TokenCallback hook;
    bool cbThrew = false;
    if (hasCb) {
        hook = [ctx, onToken, &cbThrew](int id) -> bool {
            JSValue a = JS_NewInt32(ctx, id);
            JSValue r = JS_Call(ctx, onToken, JS_UNDEFINED, 1, &a);
            JS_FreeValue(ctx, a);
            if (JS_IsException(r)) { cbThrew = true; return false; }
            const bool keepGoing = JS_ToBool(ctx, r) != 0 || JS_IsUndefined(r);
            JS_FreeValue(ctx, r);
            return keepGoing;
        };
    }

    JSValue result;
    try {
        brotensor::DeviceScope scope(w->device);
        w->vlm->set_generation(opts.max_new_tokens, opts.sampling.temperature,
                               opts.sampling.top_k, opts.sampling.top_p,
                               opts.sampling.seed);
        auto ids = w->vlm->generate_tokens(prompt, {}, hook);
        result = cbThrew ? JS_EXCEPTION
                         : qjsbind::make_int32_array(
                               ctx, std::vector<int32_t>(ids.begin(), ids.end()));
    } catch (const std::exception& e) {
        result = JS_ThrowInternalError(ctx, "generate: %s", e.what());
    }
    JS_FreeValue(ctx, onToken);
    return result;
}

static void registerQwen35Class(JSContext* ctx) {
    qjsbind::Class<Qwen35Wrapper>(ctx, "Qwen35Model", qjsbind::NoGlobal)
        .get("family",      [](Qwen35Wrapper*)    { return std::string("qwen35"); })
        .get("vocabSize",   [](Qwen35Wrapper* w)  { return w->vlm->config().text.vocab_size; })
        .get("hiddenSize",  [](Qwen35Wrapper* w)  { return w->vlm->config().text.hidden_size; })
        .get("numLayers",   [](Qwen35Wrapper* w)  { return w->vlm->config().text.num_hidden_layers; })
        .get("maxSeqLen",   [](Qwen35Wrapper* w)  { return w->maxSeqLen; })
        .get("eosId",       [](Qwen35Wrapper* w)  { return w->vlm->tokenizer().eos_id(); })
        .get("imEndId",     [](Qwen35Wrapper* w)  { return w->vlm->tokenizer().im_end_id(); })
        .get("endoftextId", [](Qwen35Wrapper* w)  { return w->vlm->tokenizer().endoftext_id(); })
        .method_raw("encode",   js_q35_encode,   2)
        .method_raw("decode",   js_q35_decode,   1)
        .method_raw("generate", js_q35_generate, 2);
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
        auto dec = std::make_unique<QwenDecoder>(cfg);
        dec->m.load_weights(f);
        mw->model = std::move(dec);
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

// Build the Mistral 3.1 text model + tekken tokenizer. Heavy + blocking (file
// IO + GPU upload); shared by the sync and async loadMistral paths. Throws on
// error.
static void buildMistral(const std::string& ggufPath, const std::string& tokPath,
                         brotensor::Device dev,
                         std::unique_ptr<LMModelWrapper>& mw_out,
                         std::unique_ptr<MistralTokenizerWrapper>& tw_out) {
    brotensor::gguf::File f = brotensor::gguf::File::open(ggufPath);
    auto cfg = brolm::mistral3::Mistral3Config::from_gguf(f);

    auto mw = std::make_unique<LMModelWrapper>();
    mw->device = dev;
    {
        brotensor::DeviceScope scope(dev);
        auto dec = std::make_unique<MistralDecoder>(cfg.text);
        dec->m.load_weights(f);
        mw->model = std::move(dec);
    }
    mw->weights_loaded = true;

    auto tw = std::make_unique<MistralTokenizerWrapper>();
    tw->tok = std::make_unique<brolm::mistral::Tokenizer>(
        brolm::mistral::Tokenizer::load(tokPath));

    std::fprintf(stderr, "[INFO] [lm] Mistral 3.1 loaded on %s\n", deviceName(dev));
    mw_out = std::move(mw);
    tw_out = std::move(tw);
}

struct MistralLoadState {
    std::string                              ggufPath, tokPath;
    brotensor::Device                        dev = brotensor::Device::CPU;
    std::unique_ptr<LMModelWrapper>          mw;
    std::unique_ptr<MistralTokenizerWrapper> tw;
    JSValue onReady = JS_UNDEFINED;
    JSValue onError = JS_UNDEFINED;
    bool    hasReady = false, hasError = false;
};

// bro.lm.loadMistral(ggufPath, opts) -> { model, tokenizer }  (sync)
//                                    -> AsyncHandle           (async, if opts.onReady)
//   ggufPath: the quantized Mistral 3.1 text GGUF (Q4_K / Q6_K / Q8_0). The
//   quant matmul path is GPU-only — loading works on CPU but the first
//   forward throws without a GPU backend.
//   opts.tokenizerPath (REQUIRED): the native tekken.json beside the
//   safetensors release (Mistral does not ship vocab.json + merges.txt).
//   opts.device / opts.onReady / opts.onError: as loadQwen.
static JSValue js_loadMistral(JSContext* ctx, JSValueConst,
                              int argc, JSValueConst* argv) {
    std::string path;
    if (argc < 1 || !argStr(ctx, argv[0], path))
        return JS_ThrowTypeError(ctx,
            "loadMistral(ggufPath, opts): path string required");
    if (argc < 2 || !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx,
            "loadMistral: opts object with tokenizerPath required");
    std::string tokPath;
    if (!getStr(ctx, argv[1], "tokenizerPath", tokPath))
        return JS_ThrowTypeError(ctx,
            "loadMistral: opts.tokenizerPath (tekken.json) required");

    brotensor::init();
    brotensor::Device dev = autoDevice();
    {
        std::string err;
        if (!parseDeviceOpt(ctx, argv[1], dev, err))
            return JS_ThrowTypeError(ctx, "loadMistral: %s", err.c_str());
    }

    JSValue onReady = JS_GetPropertyStr(ctx, argv[1], "onReady");
    JSValue onError = JS_GetPropertyStr(ctx, argv[1], "onError");
    const bool async = JS_IsFunction(ctx, onReady);

    // ── Sync path ──
    if (!async) {
        JS_FreeValue(ctx, onReady);
        JS_FreeValue(ctx, onError);
        try {
            std::unique_ptr<LMModelWrapper>          mw;
            std::unique_ptr<MistralTokenizerWrapper> tw;
            buildMistral(path, tokPath, dev, mw, tw);
            JSValue out = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, out, "model",
                qjsbind::wrap<LMModelWrapper>(ctx, mw.release()));
            JS_SetPropertyStr(ctx, out, "tokenizer",
                qjsbind::wrap<MistralTokenizerWrapper>(ctx, tw.release()));
            return out;
        } catch (const std::exception& e) {
            return JS_ThrowInternalError(ctx, "loadMistral: %s", e.what());
        }
    }

    // ── Async path ──
    auto ls = std::make_shared<MistralLoadState>();
    ls->ggufPath = path;
    ls->tokPath  = tokPath;
    ls->dev      = dev;
    ls->hasReady = true;
    ls->onReady  = JS_DupValue(ctx, onReady);
    ls->hasError = JS_IsFunction(ctx, onError);
    ls->onError  = ls->hasError ? JS_DupValue(ctx, onError) : JS_UNDEFINED;
    JS_FreeValue(ctx, onReady);
    JS_FreeValue(ctx, onError);

    auto work = [ls](const std::atomic<bool>&) {
        buildMistral(ls->ggufPath, ls->tokPath, ls->dev, ls->mw, ls->tw);
    };
    auto done = [ls](JSContext* c, bool /*cancelled*/, const std::string& error) {
        if (!error.empty() || !ls->mw) {
            if (ls->hasError) {
                JSValue e = JS_NewString(c, error.empty() ? "loadMistral failed"
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
                qjsbind::wrap<MistralTokenizerWrapper>(c, ls->tw.release()));
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

// Build the Qwen3.5 driver from a safetensors checkpoint directory. Heavy +
// blocking; shared by the sync and async loadQwen35 paths. Throws on error.
static void buildQwen35(const std::string& dir, int maxSeqLen,
                        brotensor::Device dev,
                        std::unique_ptr<Qwen35Wrapper>& w_out) {
    auto w = std::make_unique<Qwen35Wrapper>();
    w->device    = dev;
    w->maxSeqLen = maxSeqLen;
    brolm::qwen35::VLMConfig vcfg;
    vcfg.max_seq_len = maxSeqLen;
    {
        brotensor::DeviceScope scope(dev);
        w->vlm = std::make_unique<brolm::qwen35::VLM>(vcfg);
        w->vlm->load_from_directory(dir);
    }
    std::fprintf(stderr, "[INFO] [lm] Qwen3.5 loaded on %s\n", deviceName(dev));
    w_out = std::move(w);
}

struct Qwen35LoadState {
    std::string                    dir;
    int                            maxSeqLen = 4096;
    brotensor::Device              dev = brotensor::Device::CPU;
    std::unique_ptr<Qwen35Wrapper> w;
    JSValue onReady = JS_UNDEFINED;
    JSValue onError = JS_UNDEFINED;
    bool    hasReady = false, hasError = false;
};

// bro.lm.loadQwen35(checkpointDir, opts?) -> Qwen35Model  (sync)
//                                         -> AsyncHandle  (async, if opts.onReady)
//   checkpointDir: HF Qwen3.5 layout — config.json, vocab.json + merges.txt,
//   model.safetensors shard(s) (e.g. Qwen3.5-0.8B). The driver owns the
//   tokenizer, so no separate tokenizer handle: the model exposes
//   encode()/decode() and generate() takes a string prompt.
//   opts.maxSeqLen: KV/state capacity per generate call (default 4096).
//   opts.device / opts.onReady / opts.onError: as loadQwen.
static JSValue js_loadQwen35(JSContext* ctx, JSValueConst,
                             int argc, JSValueConst* argv) {
    std::string dir;
    if (argc < 1 || !argStr(ctx, argv[0], dir))
        return JS_ThrowTypeError(ctx,
            "loadQwen35(checkpointDir, opts?): path string required");

    brotensor::init();
    brotensor::Device dev = autoDevice();
    int maxSeqLen = 4096;
    if (argc >= 2) {
        std::string err;
        if (!parseDeviceOpt(ctx, argv[1], dev, err))
            return JS_ThrowTypeError(ctx, "loadQwen35: %s", err.c_str());
        if (JS_IsObject(argv[1])) getInt(ctx, argv[1], "maxSeqLen", maxSeqLen);
    }
    if (maxSeqLen <= 0)
        return JS_ThrowTypeError(ctx, "loadQwen35: opts.maxSeqLen must be > 0");

    const bool haveOpts = (argc >= 2) && JS_IsObject(argv[1]);
    JSValue onReady = haveOpts ? JS_GetPropertyStr(ctx, argv[1], "onReady")
                               : JS_UNDEFINED;
    JSValue onError = haveOpts ? JS_GetPropertyStr(ctx, argv[1], "onError")
                               : JS_UNDEFINED;
    const bool async = JS_IsFunction(ctx, onReady);

    // ── Sync path ──
    if (!async) {
        JS_FreeValue(ctx, onReady);
        JS_FreeValue(ctx, onError);
        try {
            std::unique_ptr<Qwen35Wrapper> w;
            buildQwen35(dir, maxSeqLen, dev, w);
            return qjsbind::wrap<Qwen35Wrapper>(ctx, w.release());
        } catch (const std::exception& e) {
            return JS_ThrowInternalError(ctx, "loadQwen35: %s", e.what());
        }
    }

    // ── Async path ──
    auto ls = std::make_shared<Qwen35LoadState>();
    ls->dir       = dir;
    ls->maxSeqLen = maxSeqLen;
    ls->dev       = dev;
    ls->hasReady  = true;
    ls->onReady   = JS_DupValue(ctx, onReady);
    ls->hasError  = JS_IsFunction(ctx, onError);
    ls->onError   = ls->hasError ? JS_DupValue(ctx, onError) : JS_UNDEFINED;
    JS_FreeValue(ctx, onReady);
    JS_FreeValue(ctx, onError);

    auto work = [ls](const std::atomic<bool>&) {
        buildQwen35(ls->dir, ls->maxSeqLen, ls->dev, ls->w);
    };
    auto done = [ls](JSContext* c, bool /*cancelled*/, const std::string& error) {
        if (!error.empty() || !ls->w) {
            if (ls->hasError) {
                JSValue e = JS_NewString(c, error.empty() ? "loadQwen35 failed"
                                                          : error.c_str());
                JSValue r = JS_Call(c, ls->onError, JS_UNDEFINED, 1, &e);
                if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
                JS_FreeValue(c, r);
                JS_FreeValue(c, e);
            }
        } else {
            JSValue out = qjsbind::wrap<Qwen35Wrapper>(c, ls->w.release());
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
// Async streaming generation — bro.lm.generate(model, promptIds, opts)
//                              bro.lm.generate(qwen35, promptText, opts)
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

// bro.lm.generate(qwen35, promptText, opts) — async Qwen3.5 generation on a
// background thread. Same contract as the LMModel form below (onToken / onDone
// / cancel via the AsyncHandle), but the prompt is a STRING (the VLM driver
// owns tokenization) and cancellation rides the brolm per-token hook.
static JSValue js_lm_generate_qwen35(JSContext* ctx, Qwen35Wrapper* w,
                                     int argc, JSValueConst* argv) {
    std::string prompt;
    if (!argStr(ctx, argv[1], prompt))
        return JS_ThrowTypeError(ctx,
            "generate(qwen35, prompt, opts): prompt must be a string for a "
            "Qwen35Model (the driver owns tokenization)");

    brolm::qwen::GenerateOptions opts;
    JSValue onToken = JS_UNDEFINED, onDone = JS_UNDEFINED;
    if (argc >= 3 && JS_IsObject(argv[2])) {
        opts = parseGenerateOptions(ctx, argv[2]);
        onToken = JS_GetPropertyStr(ctx, argv[2], "onToken");
        onDone  = JS_GetPropertyStr(ctx, argv[2], "onDone");
    }
    if (opts.max_new_tokens <= 0) {
        JS_FreeValue(ctx, onToken);
        JS_FreeValue(ctx, onDone);
        return JS_ThrowTypeError(ctx, "generate: opts.maxNewTokens must be > 0");
    }

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

    Qwen35Wrapper* mw = w;

    // Background thread: drive the VLM's decode loop, publishing each token
    // through the brolm per-token hook; returning false on a flipped cancel
    // flag stops the decode within one token.
    auto work = [st, mw, prompt = std::move(prompt), opts]
                (const std::atomic<bool>& cancel) {
        brotensor::DeviceScope scope(mw->device);
        mw->vlm->set_generation(opts.max_new_tokens, opts.sampling.temperature,
                                opts.sampling.top_k, opts.sampling.top_p,
                                opts.sampling.seed);
        mw->vlm->generate_tokens(prompt, {}, [st, &cancel](int id) -> bool {
            st->ids.push_back(static_cast<int32_t>(id));
            st->produced.store(st->ids.size(), std::memory_order_release);
            return !cancel.load(std::memory_order_acquire);
        });
    };

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

static JSValue js_lm_generate(JSContext* ctx, JSValueConst,
                              int argc, JSValueConst* argv) {
    if (argc < 2)
        return JS_ThrowTypeError(ctx,
            "generate(model, promptIds, opts): model and promptIds required");

    // Qwen3.5 path — string prompt through the VLM driver.
    if (auto* q = qjsbind::unwrap<Qwen35Wrapper>(ctx, argv[0]))
        return js_lm_generate_qwen35(ctx, q, argc, argv);

    auto* w = qjsbind::unwrap<LMModelWrapper>(ctx, argv[0]);
    if (!w) return JS_ThrowTypeError(ctx,
        "generate: arg 0 must be an LMModel or Qwen35Model");
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
        const int vocab = mw->model->vocabSize();
        mw->model->allocateCache(static_cast<int>(prompt.size()) +
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
    registerMistralTokenizerClass(ctx);
    registerModelClass(ctx);
    registerQwen35Class(ctx);

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
    JS_SetPropertyStr(ctx, lm, "loadMistral",
        JS_NewCFunction(ctx, js_loadMistral, "loadMistral", 2));
    JS_SetPropertyStr(ctx, lm, "loadQwen35",
        JS_NewCFunction(ctx, js_loadQwen35, "loadQwen35", 2));
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
