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

#include <qjsbind/qjsbind.h>

#include <brolm/qwen.h>
#include <brolm/qwen_generate.h>
#include <brolm/qwen_tokenizer.h>

#include <brotensor/gguf.h>
#include <brotensor/runtime.h>

#include <cstdint>
#include <exception>
#include <memory>
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
        auto ids = brolm::qwen::generate(*w->model, prompt, eos_id, opts);
        return qjsbind::make_int32_array(ctx, ids);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "generate: %s", e.what());
    }
}

static void registerModelClass(JSContext* ctx) {
    qjsbind::Class<LMModelWrapper>(ctx, "LMModel", qjsbind::NoGlobal)
        .get("vocabSize",  [](LMModelWrapper* w) { return w->model->config().vocab_size; })
        .get("hiddenSize", [](LMModelWrapper* w) { return w->model->config().hidden_size; })
        .get("numLayers",  [](LMModelWrapper* w) { return w->model->config().num_hidden_layers; })
        .get("maxSeqLen",  [](LMModelWrapper* w) { return w->model->config().max_position_embeddings; })
        .get("cacheLen",   [](LMModelWrapper* w) { return w->model->cache_len(); })
        .method_raw("allocateCache", js_model_allocateCache, 1)
        .method_raw("resetCache",    js_model_resetCache,    0)
        .method_raw("generate",      js_model_generate,      2);
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

// bro.lm.loadQwen(ggufPath) -> { model, tokenizer }
//   Single-file Qwen3 loader. Opens a Qwen3 GGUF, builds the Qwen3Config from
//   its metadata, constructs the decoder, loads the tensor weights, and reads
//   the tokenizer's vocab + merges out of the same file. Both handles are
//   bundled into one object so a caller can destructure them.
static JSValue js_loadQwen(JSContext* ctx, JSValueConst,
                           int argc, JSValueConst* argv) {
    std::string path;
    if (argc < 1 || !argStr(ctx, argv[0], path))
        return JS_ThrowTypeError(ctx, "loadQwen(ggufPath): path string required");
    try {
        brotensor::init();
        brotensor::gguf::File f = brotensor::gguf::File::open(path);
        auto cfg = brolm::qwen::Qwen3Config::from_gguf(f);

        auto mw = std::make_unique<LMModelWrapper>();
        mw->model = std::make_unique<brolm::qwen::Qwen3Model>(cfg);
        mw->model->load_weights(f);
        mw->weights_loaded = true;

        auto tw = std::make_unique<LMTokenizerWrapper>();
        tw->tok = std::make_unique<brolm::qwen::Tokenizer>(
            brolm::qwen::Tokenizer::from_gguf(f));

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
        JS_NewCFunction(ctx, js_loadQwen, "loadQwen", 1));
    JS_SetPropertyStr(ctx, lm, "loadTokenizer",
        JS_NewCFunction(ctx, js_loadTokenizer, "loadTokenizer", 1));
    JS_SetPropertyStr(ctx, broObj, "lm", lm);

    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

void cleanupLmBindings(JSContext* /*ctx*/) {
    // No-op: qjsbind owns the class finalizers; bro.lm is reached from
    // globalThis and swept by runtime teardown.
}

}  // namespace bro::js
