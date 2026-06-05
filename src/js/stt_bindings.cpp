// JS bindings for brosoundml::Whisper — speech-to-text inference.
//
// Installed onto bro.stt.* by installSttBindings(). The Whisper model
// (encoder + KV-cached decoder, ~150 MB - ~3 GB depending on checkpoint) and
// its tokenizer (brolm::whisper::Tokenizer) live behind opaque qjsbind
// handles.
//
// The model runs on GPU by default — loadWhisper places it on CUDA when a GPU
// backend is available (pass opts.device 'cpu' to force CPU). transcribe()
// takes 16 kHz mono FP32 audio and returns the raw token id stream —
// `transcribeText` adds the build_prompt / decode round-trip and hands back
// plain text.

#include "js/stt_bindings.h"
#include "js/async_job.h"

#include <qjsbind/qjsbind.h>

#include <brosoundml/whisper.h>
#include <brosoundml/audio.h>

#include <brolm/whisper_tokenizer.h>

#include <brotensor/runtime.h>
#include <brotensor/tensor.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace bro::js {

// ═══════════════════════════════════════════════════════════════════════════
// Wrapper structs
// ═══════════════════════════════════════════════════════════════════════════

struct WhisperWrapper {
    std::unique_ptr<brosoundml::Whisper> whisper;
    brotensor::Device device = brotensor::Device::CPU;  // captured at load
    // Set while an async bro.stt.transcribe() runs on this model's background
    // thread; rejects a second concurrent op (the decoder + KV cache are
    // single-owner). Cleared on the JS thread when the job's done() fires.
    std::atomic<bool> busy{false};
};

struct WhisperTokenizerWrapper {
    std::unique_ptr<brolm::whisper::Tokenizer> tok;
};

// ═══════════════════════════════════════════════════════════════════════════
// Helpers (TU-local — same shape as the lm/diffusion bindings)
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

// Parse opts.device. On success, writes the device into `out` and returns
// true. On a missing key, leaves `out` untouched and returns true. On an
// unknown value, sets `err` and returns false.
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

// Read a Float32Array's element pointer + count. Returns nullptr if not a
// Float32Array view.
static const float* getFloatArray(JSContext* ctx, JSValueConst v,
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
    if (!p || bpe != sizeof(float)) return nullptr;
    count = viewLen / sizeof(float);
    return reinterpret_cast<const float*>(p + byteOff);
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
    if (!p || bpe != sizeof(int32_t)) return nullptr;
    count = viewLen / sizeof(int32_t);
    return reinterpret_cast<const int32_t*>(p + byteOff);
}

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

// Read a JS audio object — either { samples: Float32Array, sampleRate }, a
// raw Float32Array (assumed 16 kHz), or { samples: number[], sampleRate } —
// into a brosoundml::AudioBuffer.
static bool readAudioBuffer(JSContext* ctx, JSValueConst v,
                            brosoundml::AudioBuffer& out,
                            std::string& err) {
    out.sample_rate = 16000;
    // Bare Float32Array path.
    size_t cnt = 0;
    if (const float* p = getFloatArray(ctx, v, cnt)) {
        out.samples.assign(p, p + cnt);
        return true;
    }
    if (!JS_IsObject(v)) {
        err = "audio must be a Float32Array or { samples, sampleRate } object";
        return false;
    }
    JSValue sr = JS_GetPropertyStr(ctx, v, "sampleRate");
    if (JS_IsNumber(sr)) {
        int32_t t = out.sample_rate;
        JS_ToInt32(ctx, &t, sr);
        out.sample_rate = t;
    }
    JS_FreeValue(ctx, sr);

    JSValue s = JS_GetPropertyStr(ctx, v, "samples");
    bool ok = false;
    size_t fc = 0;
    if (const float* p = getFloatArray(ctx, s, fc)) {
        out.samples.assign(p, p + fc);
        ok = true;
    } else if (JS_IsArray(s)) {
        std::uint32_t n = 0;
        JSValue lv = JS_GetPropertyStr(ctx, s, "length");
        JS_ToUint32(ctx, &n, lv);
        JS_FreeValue(ctx, lv);
        out.samples.resize(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            JSValue e = JS_GetPropertyUint32(ctx, s, i);
            double t = 0.0;
            JS_ToFloat64(ctx, &t, e);
            out.samples[i] = (float)t;
            JS_FreeValue(ctx, e);
        }
        ok = true;
    } else {
        err = "audio.samples must be a Float32Array or number[]";
    }
    JS_FreeValue(ctx, s);
    return ok;
}

// ═══════════════════════════════════════════════════════════════════════════
// WhisperTokenizer methods
// ═══════════════════════════════════════════════════════════════════════════

static WhisperTokenizerWrapper* tokSelf(JSContext* ctx, JSValueConst this_val) {
    return qjsbind::unwrap<WhisperTokenizerWrapper>(ctx, this_val);
}

static JSValue js_wtok_encode(JSContext* ctx, JSValueConst this_val,
                              int argc, JSValueConst* argv) {
    auto* w = tokSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "encode: not a WhisperTokenizer");
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

// decode(ids, skipSpecial=false) — special-token ids decode to literal
// "<|...|>" by default; skipSpecial=true drops them.
static JSValue js_wtok_decode(JSContext* ctx, JSValueConst this_val,
                              int argc, JSValueConst* argv) {
    auto* w = tokSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "decode: not a WhisperTokenizer");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "decode(ids): ids required");
    auto ids = readIdArray(ctx, argv[0]);
    const bool skipSpecial = (argc >= 2) && (JS_ToBool(ctx, argv[1]) == 1);
    try {
        return JS_NewString(ctx, w->tok->decode(ids, skipSpecial).c_str());
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "decode: %s", e.what());
    }
}

// buildPrompt(language, task='transcribe', withTimestamps=true) -> Int32Array
static JSValue js_wtok_buildPrompt(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    auto* w = tokSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "buildPrompt: not a WhisperTokenizer");
    std::string lang = "en";
    if (argc >= 1) argStr(ctx, argv[0], lang);
    std::string task = "transcribe";
    if (argc >= 2) argStr(ctx, argv[1], task);
    const bool withTs = (argc < 3) ? true : (JS_ToBool(ctx, argv[2]) == 1);
    try {
        auto ids = w->tok->build_prompt(lang, task, withTs);
        return qjsbind::make_int32_array(ctx, ids);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "buildPrompt: %s", e.what());
    }
}

static JSValue js_wtok_isTimestamp(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    auto* w = tokSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "isTimestamp: not a WhisperTokenizer");
    if (argc < 1) return JS_ThrowTypeError(ctx, "isTimestamp(id): id required");
    int32_t id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    return JS_NewBool(ctx, w->tok->is_timestamp(id));
}

static JSValue js_wtok_timestampSeconds(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv) {
    auto* w = tokSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "timestampSeconds: not a WhisperTokenizer");
    if (argc < 1) return JS_ThrowTypeError(ctx, "timestampSeconds(id): id required");
    int32_t id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    return JS_NewFloat64(ctx, w->tok->timestamp_seconds(id));
}

static void registerTokenizerClass(JSContext* ctx) {
    qjsbind::Class<WhisperTokenizerWrapper>(ctx, "WhisperTokenizer",
                                            qjsbind::NoGlobal)
        .get("eosId",            [](WhisperTokenizerWrapper* w) { return w->tok->eos_id(); })
        .get("sotId",             [](WhisperTokenizerWrapper* w) { return w->tok->sot_id(); })
        .get("noSpeechId",        [](WhisperTokenizerWrapper* w) { return w->tok->no_speech_id(); })
        .get("noTimestampsId",    [](WhisperTokenizerWrapper* w) { return w->tok->no_timestamps_id(); })
        .get("transcribeId",      [](WhisperTokenizerWrapper* w) { return w->tok->transcribe_id(); })
        .get("translateId",       [](WhisperTokenizerWrapper* w) { return w->tok->translate_id(); })
        .get("firstTimestampId",  [](WhisperTokenizerWrapper* w) { return w->tok->first_timestamp_id(); })
        .get("lastTimestampId",   [](WhisperTokenizerWrapper* w) { return w->tok->last_timestamp_id(); })
        .get("vocabCount",        [](WhisperTokenizerWrapper* w) { return (int)w->tok->vocab_count(); })
        .get("mergeCount",        [](WhisperTokenizerWrapper* w) { return (int)w->tok->merge_count(); })
        .method_raw("encode",            js_wtok_encode,           2)
        .method_raw("decode",            js_wtok_decode,           2)
        .method_raw("buildPrompt",       js_wtok_buildPrompt,      3)
        .method_raw("isTimestamp",       js_wtok_isTimestamp,      1)
        .method_raw("timestampSeconds",  js_wtok_timestampSeconds, 1);
}

// ═══════════════════════════════════════════════════════════════════════════
// Whisper methods
// ═══════════════════════════════════════════════════════════════════════════

static WhisperWrapper* whisperSelf(JSContext* ctx, JSValueConst this_val) {
    return qjsbind::unwrap<WhisperWrapper>(ctx, this_val);
}

// transcribe(audio, promptIds, opts?) -> Int32Array
//   audio:    Float32Array @ 16 kHz, OR { samples, sampleRate } object.
//   promptIds: Int32Array of decoder prefix (from tokenizer.buildPrompt()).
//   opts.maxNewTokens: cap on autoregressive loop (0 = model.maxTargetPositions).
static JSValue js_whisper_transcribe(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    auto* w = whisperSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "transcribe: not a Whisper");
    if (argc < 2)
        return JS_ThrowTypeError(ctx,
            "transcribe(audio, promptIds, opts?): audio and promptIds required");

    brosoundml::AudioBuffer audio;
    std::string err;
    if (!readAudioBuffer(ctx, argv[0], audio, err))
        return JS_ThrowTypeError(ctx, "transcribe: %s", err.c_str());

    std::vector<int32_t> prompt = readIdArray(ctx, argv[1]);
    if (prompt.empty())
        return JS_ThrowTypeError(ctx,
            "transcribe: promptIds must be a non-empty Int32Array or number[] "
            "(use tokenizer.buildPrompt(lang, task))");

    int maxNew = 0;
    if (argc >= 3 && JS_IsObject(argv[2]))
        getInt(ctx, argv[2], "maxNewTokens", maxNew);

    try {
        auto out = w->whisper->transcribe(audio, prompt, maxNew);
        return qjsbind::make_int32_array(ctx, out.token_ids);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "transcribe: %s", e.what());
    }
}

static void registerWhisperClass(JSContext* ctx) {
    qjsbind::Class<WhisperWrapper>(ctx, "Whisper", qjsbind::NoGlobal)
        .get("loaded",              [](WhisperWrapper* w) { return w->whisper->loaded(); })
        .get("sampleRate",          [](WhisperWrapper* w) { return w->whisper->config().sample_rate; })
        .get("numMelBins",          [](WhisperWrapper* w) { return w->whisper->config().num_mel_bins; })
        .get("dModel",              [](WhisperWrapper* w) { return w->whisper->config().d_model; })
        .get("maxSourcePositions",  [](WhisperWrapper* w) { return w->whisper->config().max_source_positions; })
        .get("maxTargetPositions",  [](WhisperWrapper* w) { return w->whisper->config().max_target_positions; })
        .get("vocabSize",           [](WhisperWrapper* w) { return w->whisper->config().vocab_size; })
        .get("eosTokenId",          [](WhisperWrapper* w) { return w->whisper->config().eos_token_id; })
        .get("decoderStartTokenId", [](WhisperWrapper* w) { return w->whisper->config().decoder_start_token_id; })
        .method_raw("transcribe", js_whisper_transcribe, 3);
}

// ═══════════════════════════════════════════════════════════════════════════
// bro.stt free functions
// ═══════════════════════════════════════════════════════════════════════════

static JSValue js_init(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    try {
        brotensor::init();
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "bro.stt.init: %s", e.what());
    }
    return JS_UNDEFINED;
}

// Build + load the Whisper model from a checkpoint dir. Heavy + blocking (file
// IO + GPU upload); shared by the sync and async loadWhisper paths. Throws on
// error.
static void buildWhisper(const std::string& dir, brotensor::Device dev,
                         std::unique_ptr<WhisperWrapper>& w_out) {
    auto w = std::make_unique<WhisperWrapper>();
    w->device  = dev;
    w->whisper = std::make_unique<brosoundml::Whisper>();
    {
        brotensor::DeviceScope scope(dev);
        w->whisper->load(dir, dev);
    }
    std::fprintf(stderr, "[INFO] [stt] Whisper loaded on %s\n", deviceName(dev));
    w_out = std::move(w);
}

// State for an async loadWhisper: the work thread fills w (or error); the
// JS-thread done() wraps it and invokes onReady/onError.
struct WhisperLoadState {
    std::string                     dir;
    brotensor::Device               dev = brotensor::Device::CPU;
    std::unique_ptr<WhisperWrapper> w;
    JSValue onReady = JS_UNDEFINED;
    JSValue onError = JS_UNDEFINED;
    bool    hasReady = false, hasError = false;
};

// bro.stt.loadWhisper(modelDir, opts?) -> Whisper          (sync)
//                                      -> AsyncHandle       (async, if opts.onReady)
//   modelDir contains config.json + model.safetensors (HF Whisper checkpoint).
//   opts.device: 'cuda' | 'cpu' — defaults to CUDA when available, else CPU.
//   opts.onReady(whisper) / opts.onError(message): when onReady is a function
//   the load runs on a background thread (non-blocking, parallelizable with
//   other loads) and these fire on the JS thread.
static JSValue js_loadWhisper(JSContext* ctx, JSValueConst,
                              int argc, JSValueConst* argv) {
    std::string dir;
    if (argc < 1 || !argStr(ctx, argv[0], dir))
        return JS_ThrowTypeError(ctx, "loadWhisper(modelDir, opts?): path required");

    brotensor::init();
    brotensor::Device dev = autoDevice();
    if (argc >= 2) {
        std::string err;
        if (!parseDeviceOpt(ctx, argv[1], dev, err))
            return JS_ThrowTypeError(ctx, "loadWhisper: %s", err.c_str());
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
            std::unique_ptr<WhisperWrapper> w;
            buildWhisper(dir, dev, w);
            return qjsbind::wrap<WhisperWrapper>(ctx, w.release());
        } catch (const std::exception& e) {
            return JS_ThrowInternalError(ctx, "loadWhisper: %s", e.what());
        }
    }

    // ── Async path ──
    auto ls = std::make_shared<WhisperLoadState>();
    ls->dir      = dir;
    ls->dev      = dev;
    ls->hasReady = true;
    ls->onReady  = JS_DupValue(ctx, onReady);
    ls->hasError = JS_IsFunction(ctx, onError);
    ls->onError  = ls->hasError ? JS_DupValue(ctx, onError) : JS_UNDEFINED;
    JS_FreeValue(ctx, onReady);
    JS_FreeValue(ctx, onError);

    auto work = [ls](const std::atomic<bool>&) {
        buildWhisper(ls->dir, ls->dev, ls->w);  // throws -> error
    };
    auto done = [ls](JSContext* c, bool /*cancelled*/, const std::string& error) {
        if (!error.empty() || !ls->w) {
            if (ls->hasError) {
                JSValue e = JS_NewString(c, error.empty() ? "loadWhisper failed"
                                                          : error.c_str());
                JSValue r = JS_Call(c, ls->onError, JS_UNDEFINED, 1, &e);
                if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
                JS_FreeValue(c, r);
                JS_FreeValue(c, e);
            }
        } else {
            JSValue out = qjsbind::wrap<WhisperWrapper>(c, ls->w.release());
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

// Build the HF-format Whisper tokenizer (vocab.json + merges.txt). Cheap
// relative to the model, but the async path keeps the loader API uniform.
static void buildWhisperTokenizer(const std::string& vocab,
                                  const std::string& merges,
                                  const std::string& addedTokens,
                                  std::unique_ptr<WhisperTokenizerWrapper>& tw_out) {
    auto tw = std::make_unique<WhisperTokenizerWrapper>();
    tw->tok = std::make_unique<brolm::whisper::Tokenizer>(
        brolm::whisper::Tokenizer::load(vocab, merges, addedTokens));
    tw_out = std::move(tw);
}

// State for an async loadTokenizer.
struct WhisperTokLoadState {
    std::string                              vocab, merges, addedTokens;
    std::unique_ptr<WhisperTokenizerWrapper> tw;
    JSValue onReady = JS_UNDEFINED;
    JSValue onError = JS_UNDEFINED;
    bool    hasReady = false, hasError = false;
};

// bro.stt.loadTokenizer({ vocabPath, mergesPath, onReady?, onError? })
//   -> WhisperTokenizer  (sync)
//   -> AsyncHandle       (async, if onReady)
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

    // Optional: an upstream openai/whisper added_tokens.json carrying the
    // "<|...|>" specials. Absent for the converted (merged) layout.
    std::string addedTokens;
    getStr(ctx, argv[0], "addedTokensPath", addedTokens);

    JSValue onReady = JS_GetPropertyStr(ctx, argv[0], "onReady");
    JSValue onError = JS_GetPropertyStr(ctx, argv[0], "onError");
    const bool async = JS_IsFunction(ctx, onReady);

    // ── Sync path (back-compat) ──
    if (!async) {
        JS_FreeValue(ctx, onReady);
        JS_FreeValue(ctx, onError);
        try {
            std::unique_ptr<WhisperTokenizerWrapper> tw;
            buildWhisperTokenizer(vocab, merges, addedTokens, tw);
            return qjsbind::wrap<WhisperTokenizerWrapper>(ctx, tw.release());
        } catch (const std::exception& e) {
            return JS_ThrowInternalError(ctx, "loadTokenizer: %s", e.what());
        }
    }

    // ── Async path ──
    auto ls = std::make_shared<WhisperTokLoadState>();
    ls->vocab    = vocab;
    ls->merges   = merges;
    ls->addedTokens = addedTokens;
    ls->hasReady = true;
    ls->onReady  = JS_DupValue(ctx, onReady);
    ls->hasError = JS_IsFunction(ctx, onError);
    ls->onError  = ls->hasError ? JS_DupValue(ctx, onError) : JS_UNDEFINED;
    JS_FreeValue(ctx, onReady);
    JS_FreeValue(ctx, onError);

    auto work = [ls](const std::atomic<bool>&) {
        buildWhisperTokenizer(ls->vocab, ls->merges, ls->addedTokens, ls->tw);  // throws -> error
    };
    auto done = [ls](JSContext* c, bool /*cancelled*/, const std::string& error) {
        if (!error.empty() || !ls->tw) {
            if (ls->hasError) {
                JSValue e = JS_NewString(c, error.empty() ? "loadTokenizer failed"
                                                          : error.c_str());
                JSValue r = JS_Call(c, ls->onError, JS_UNDEFINED, 1, &e);
                if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
                JS_FreeValue(c, r);
                JS_FreeValue(c, e);
            }
        } else {
            JSValue out = qjsbind::wrap<WhisperTokenizerWrapper>(c, ls->tw.release());
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
// Async transcription — bro.stt.transcribe(whisper, audio, promptIds, opts)
// ═══════════════════════════════════════════════════════════════════════════
//
// Runs Whisper's autoregressive decode on a background thread via the async-job
// runner, so the JS thread stays responsive. There is no per-token streaming
// poll (the ids are delivered once at the end), but cancellation is real:
// brosoundml's greedy loop checks the async-job cancel flag once per token, so
// .cancel() stops the decode within ~1 token and frees the model's busy lock
// rather than running to completion. Returns an AsyncHandle with .cancel();
// opts.onDone(ids, info) fires once on the JS thread with info being
// { cancelled, error? }.

// Shared between the work thread (sole writer of token_ids) and the JS thread
// (sole reader / caller of onDone). Held by shared_ptr.
struct SttJob {
    brosoundml::AudioBuffer audio;
    std::vector<int32_t>    prompt;
    int                     maxNew = 0;
    std::vector<int32_t>    token_ids;          // filled by work()
    JSValue                 onDone     = JS_UNDEFINED;  // dup'd; UNDEFINED if absent
    JSValue                 whisperRef = JS_UNDEFINED;  // dup of the whisper JS object
    bool                    hasOnDone  = false;
};

static JSValue js_stt_transcribe(JSContext* ctx, JSValueConst,
                                 int argc, JSValueConst* argv) {
    if (argc < 3)
        return JS_ThrowTypeError(ctx,
            "transcribe(whisper, audio, promptIds, opts?): whisper, audio and "
            "promptIds required");
    auto* w = qjsbind::unwrap<WhisperWrapper>(ctx, argv[0]);
    if (!w) return JS_ThrowTypeError(ctx, "transcribe: arg 0 must be a Whisper");

    auto job = std::make_shared<SttJob>();
    std::string err;
    if (!readAudioBuffer(ctx, argv[1], job->audio, err))
        return JS_ThrowTypeError(ctx, "transcribe: %s", err.c_str());

    job->prompt = readIdArray(ctx, argv[2]);
    if (job->prompt.empty())
        return JS_ThrowTypeError(ctx,
            "transcribe: promptIds must be a non-empty Int32Array or number[] "
            "(use tokenizer.buildPrompt(lang, task))");

    JSValue onDone = JS_UNDEFINED;
    if (argc >= 4 && JS_IsObject(argv[3])) {
        getInt(ctx, argv[3], "maxNewTokens", job->maxNew);
        onDone = JS_GetPropertyStr(ctx, argv[3], "onDone");
    }

    // Claim the model for this transcription (single-owner; one in flight).
    bool expected = false;
    if (!w->busy.compare_exchange_strong(expected, true)) {
        JS_FreeValue(ctx, onDone);
        return JS_ThrowInternalError(ctx,
            "transcribe: an operation is already in flight on this model");
    }

    job->hasOnDone  = JS_IsFunction(ctx, onDone);
    job->onDone     = job->hasOnDone ? JS_DupValue(ctx, onDone) : JS_UNDEFINED;
    job->whisperRef = JS_DupValue(ctx, argv[0]);  // keep the model alive
    JS_FreeValue(ctx, onDone);

    WhisperWrapper* mw = w;

    // Background thread: run the transcribe and stash the ids. A barge-in
    // flips the async-job cancel flag; Whisper's greedy decode polls it once
    // per token and returns early, so the GPU stops and the model's `busy`
    // lock is released promptly (the result is discarded by `done`). The check
    // runs synchronously inside transcribe(), so capturing `cancel` by
    // reference is safe.
    auto work = [job, mw](const std::atomic<bool>& cancel) {
        brotensor::DeviceScope scope(mw->device);
        auto out = mw->whisper->transcribe(
            job->audio, job->prompt, job->maxNew,
            [&cancel] { return cancel.load(std::memory_order_acquire); });
        job->token_ids = std::move(out.token_ids);
    };

    // JS thread, once: hand the id array + {cancelled,error} to onDone, free the
    // dup'd values, release the model.
    auto done = [job, mw](JSContext* c, bool cancelled, const std::string& error) {
        if (job->hasOnDone) {
            JSValue arr  = qjsbind::make_int32_array(c, job->token_ids);
            JSValue info = JS_NewObject(c);
            JS_SetPropertyStr(c, info, "cancelled", JS_NewBool(c, cancelled));
            if (!error.empty())
                JS_SetPropertyStr(c, info, "error", JS_NewString(c, error.c_str()));
            JSValue args[2] = { arr, info };
            JSValue r = JS_Call(c, job->onDone, JS_UNDEFINED, 2, args);
            if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
            JS_FreeValue(c, r);
            JS_FreeValue(c, arr);
            JS_FreeValue(c, info);
        }
        if (job->hasOnDone) JS_FreeValue(c, job->onDone);
        JS_FreeValue(c, job->whisperRef);
        mw->busy.store(false, std::memory_order_release);
    };

    return launchAsyncJob(ctx, std::move(work), nullptr, std::move(done));
}

// ═══════════════════════════════════════════════════════════════════════════
// Install
// ═══════════════════════════════════════════════════════════════════════════

void installSttBindings(JSContext* ctx) {
    registerTokenizerClass(ctx);
    registerWhisperClass(ctx);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        JS_FreeValue(ctx, broObj);
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue stt = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, stt, "init",
        JS_NewCFunction(ctx, js_init, "init", 0));
    JS_SetPropertyStr(ctx, stt, "loadWhisper",
        JS_NewCFunction(ctx, js_loadWhisper, "loadWhisper", 2));
    JS_SetPropertyStr(ctx, stt, "loadTokenizer",
        JS_NewCFunction(ctx, js_loadTokenizer, "loadTokenizer", 1));
    JS_SetPropertyStr(ctx, stt, "transcribe",
        JS_NewCFunction(ctx, js_stt_transcribe, "transcribe", 4));
    JS_SetPropertyStr(ctx, broObj, "stt", stt);

    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

void cleanupSttBindings(JSContext* /*ctx*/) {}

}  // namespace bro::js
