#if BRO_WITH_SOUNDML
// JS bindings for brosoundml speech-to-text inference — Whisper, Parakeet,
// and Qwen3-ASR.
//
// Installed onto bro.stt.* by installSttBindings(). The models (Whisper:
// encoder + KV-cached decoder, ~150 MB - ~3 GB; Parakeet-TDT-0.6B-v3:
// FastConformer encoder + TDT decoder, ~2.4 GB; Qwen3-ASR-0.6B/-1.7B: AuT
// encoder + Qwen3 decoder, 52-language + language ID) and their tokenizers
// (brolm::whisper::Tokenizer / brolm::t5::Tokenizer SentencePiece) live
// behind opaque qjsbind handles. Qwen3-ASR detokenizes with the Qwen BPE
// tokenizer already bound as bro.lm.loadTokenizer.
//
// All models run on GPU by default — the loaders place them on CUDA when a
// GPU backend is available (pass opts.device 'cpu' to force CPU). transcribe()
// takes 16 kHz mono FP32 audio and returns the raw token id stream; the caller
// detokenizes. Whisper needs a decoder prompt (tokenizer.buildPrompt());
// Parakeet is unconditional and additionally reports per-token encoder-frame
// positions for word timestamps; Qwen3-ASR is unconditional with optional
// context biasing and a streaming encoder tap (loadQwenAsrStream).

#include "js/stt_bindings.h"
#include "util/interrupt.h"
#include "js/async_job.h"
#include "js/model_gate.h"
#include "js/marshal.h"

#include <qjsbind/qjsbind.h>

#include <api/api.h>  // brokit::api::resolveAssetPath

#include <brosoundml/whisper.h>
#include <brosoundml/parakeet.h>
#include <brosoundml/qwen_asr.h>
#include <brosoundml/audio.h>

#include <brolm/whisper_tokenizer.h>
#include <brolm/tokenizer_t5.h>

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

// The model + its single-owner gate are held by shared_ptr so they outlive the
// JS model handle whenever a session (see *SessionWrapper below) is still alive,
// and so EVERY inference over one model — module-level bro.stt.transcribe(model)
// AND every session.transcribe() — serializes on the ONE busy flag. brosoundml's
// session tier for these models is SHARED WEIGHTS / SERIALIZED decode (the GPU
// runs one stream and the captured decoder step-graph is shared), so concurrent
// decode over sessions of one model is unsupported and must be gated here.
struct WhisperWrapper {
    std::shared_ptr<brosoundml::Whisper> whisper;
    brotensor::Device device = brotensor::Device::CPU;  // captured at load
    // Set while an async bro.stt.transcribe() / session.transcribe() runs on a
    // background thread; rejects a second concurrent op (the decoder + KV cache
    // are single-owner). Cleared on the JS thread when the job's done() fires.
    // shared_ptr so sessions share this exact gate with the model.
    ModelGate busy;
};

// A Whisper decode session over shared weights: its own KV-cache, one per stream.
// Holds the model + busy gate alive by shared_ptr (the model JS handle may be
// dropped while a session lives) and the move-only brosoundml::WhisperSession.
struct WhisperSessionWrapper {
    std::shared_ptr<brosoundml::Whisper> model;
    ModelGate busy;   // shared with the model + siblings
    brotensor::Device                    device = brotensor::Device::CPU;
    brosoundml::WhisperSession           session;
};

struct WhisperTokenizerWrapper {
    std::unique_ptr<brolm::whisper::Tokenizer> tok;
};

struct ParakeetWrapper {
    std::shared_ptr<brosoundml::Parakeet> parakeet;
    brotensor::Device device = brotensor::Device::CPU;  // captured at load
    // Same single-owner discipline as Whisper: one async transcribe in flight.
    // shared_ptr so sessions share this exact gate with the model.
    ModelGate busy;
};

// A Parakeet decode session over shared weights: its own TDT prediction-net
// state, one per stream. brosoundml's tier here is CONCURRENT (the forward
// touches no shared mutable state), but the bro async runner still serializes
// on the shared busy gate to match the other models and the single GPU stream —
// correctness is identical, only parallelism is given up.
struct ParakeetSessionWrapper {
    std::shared_ptr<brosoundml::Parakeet> model;
    ModelGate busy;
    brotensor::Device                     device = brotensor::Device::CPU;
    brosoundml::ParakeetSession           session;
};

// Parakeet's tokenizer is a HF tokenizer.json SentencePiece unigram — the
// brolm::t5::Tokenizer parses that format and its decode() skips out-of-vocab
// ids, which is exactly what an ASR id stream (no blank/pad) needs.
struct ParakeetTokenizerWrapper {
    std::unique_ptr<brolm::t5::Tokenizer> tok;
};

// Qwen3-ASR detokenizes with the Qwen BPE tokenizer already bound as
// bro.lm.loadTokenizer (vocab.json + merges.txt sit in the model dir), so no
// new tokenizer wrapper here — brosoundml emits and consumes raw id streams.
struct QwenAsrWrapper {
    std::shared_ptr<brosoundml::QwenAsr> asr;
    brotensor::Device device = brotensor::Device::CPU;  // captured at load
    // Same single-owner discipline as Whisper: one async transcribe in flight.
    // shared_ptr so sessions share this exact gate with the model.
    ModelGate busy;
};

// A Qwen3-ASR decode session over shared weights: its own decoder KV-cache, one
// per stream. CONCURRENT tier in brosoundml, serialized here on the shared gate.
struct QwenAsrSessionWrapper {
    std::shared_ptr<brosoundml::QwenAsr> model;
    ModelGate busy;
    brotensor::Device                    device = brotensor::Device::CPU;
    brosoundml::QwenAsrSession           session;
};

// Encoder-only streaming tap. feed()/finish() run synchronously on the JS
// thread (one encoder block is ~1 s of audio — cheap on GPU), so no busy flag.
struct QwenAsrStreamWrapper {
    std::unique_ptr<brosoundml::QwenAsrStream> stream;
    brotensor::Device device = brotensor::Device::CPU;  // captured at load
};

// createSession() factory methods, registered on the model classes below but
// defined down in the session section (after the async-job structs they reuse).
static JSValue js_whisper_createSession(JSContext*, JSValueConst, int, JSValueConst*);
static JSValue js_parakeet_createSession(JSContext*, JSValueConst, int, JSValueConst*);
static JSValue js_qwenasr_createSession(JSContext*, JSValueConst, int, JSValueConst*);

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
    switch (d.type) {
        case brotensor::DeviceType::CUDA:  return "CUDA";
        case brotensor::DeviceType::Metal: return "Metal";
        case brotensor::DeviceType::CPU:   return "CPU";
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
    auto ids = qjsbind::read_int32_array(ctx, argv[0]);
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
// ParakeetTokenizer methods
// ═══════════════════════════════════════════════════════════════════════════

static ParakeetTokenizerWrapper* ptokSelf(JSContext* ctx, JSValueConst this_val) {
    return qjsbind::unwrap<ParakeetTokenizerWrapper>(ctx, this_val);
}

// tokenize(text) -> Int32Array — unigram pieces only, no eos/padding.
static JSValue js_ptok_tokenize(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv) {
    auto* w = ptokSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "tokenize: not a ParakeetTokenizer");
    std::string text;
    if (argc < 1 || !argStr(ctx, argv[0], text))
        return JS_ThrowTypeError(ctx, "tokenize(text): text required");
    try {
        auto ids = w->tok->tokenize(text);
        return qjsbind::make_int32_array(ctx, ids);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "tokenize: %s", e.what());
    }
}

// decode(ids) -> string — ids outside the vocab (blank/pad) are skipped, so
// the raw Parakeet id stream decodes directly.
static JSValue js_ptok_decode(JSContext* ctx, JSValueConst this_val,
                              int argc, JSValueConst* argv) {
    auto* w = ptokSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "decode: not a ParakeetTokenizer");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "decode(ids): ids required");
    auto ids = qjsbind::read_int32_array(ctx, argv[0]);
    try {
        return JS_NewString(ctx, w->tok->decode(ids).c_str());
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "decode: %s", e.what());
    }
}

static void registerParakeetTokenizerClass(JSContext* ctx) {
    qjsbind::Class<ParakeetTokenizerWrapper>(ctx, "ParakeetTokenizer",
                                             qjsbind::NoGlobal)
        .get("vocabCount", [](ParakeetTokenizerWrapper* w) { return (int)w->tok->vocab_count(); })
        .get("padId",      [](ParakeetTokenizerWrapper* w) { return w->tok->pad_id(); })
        .get("eosId",      [](ParakeetTokenizerWrapper* w) { return w->tok->eos_id(); })
        .get("unkId",      [](ParakeetTokenizerWrapper* w) { return w->tok->unk_id(); })
        .method_raw("tokenize", js_ptok_tokenize, 1)
        .method_raw("decode",   js_ptok_decode,   1);
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
//                      In long-form mode this caps EACH 30 s window independently.
//   opts.timestampBeginId: tokenizer.firstTimestampId — when set (>= 0) AND the
//                      audio is longer than 30 s, the input is windowed into 30 s
//                      segments (Whisper sequential long-form decode) instead of
//                      truncated to the first window. Requires a timestamps prompt
//                      (buildPrompt(lang, task, true)).
//   opts.noTimestampsId: tokenizer.noTimestampsId — when set (>= 0) the decoder is
//                      never allowed to pick <|notimestamps|>. Pass it whenever you
//                      want timings: a timestamps prompt only omits the token, it
//                      does not stop the model generating it, and once generated
//                      the segment carries no timestamp at all.
//   opts.onToken(id):  invoked once per decoded token, in order, as it is produced
//                      (synchronously on this thread) — for live partial decode.
//   opts.onWindow(t):  invoked as each long-form window opens, with the absolute
//                      offset in seconds of that window's <|0.00|>. Pair it with
//                      onToken whenever the timestamps are being placed in time:
//                      every window restarts at <|0.00|>, so a timestamp in the
//                      returned stream is meaningless without knowing which
//                      window it fell in.
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

    std::vector<int32_t> prompt = qjsbind::read_int32_array(ctx, argv[1]);
    if (prompt.empty())
        return JS_ThrowTypeError(ctx,
            "transcribe: promptIds must be a non-empty Int32Array or number[] "
            "(use tokenizer.buildPrompt(lang, task))");

    brosoundml::Whisper::TranscribeOptions opts;
    JSValue onToken = JS_UNDEFINED, onWindow = JS_UNDEFINED;
    if (argc >= 3 && JS_IsObject(argv[2])) {
        getInt(ctx, argv[2], "maxNewTokens", opts.max_new_tokens);
        opts.timestamp_begin_id = -1;
        getInt(ctx, argv[2], "timestampBeginId", opts.timestamp_begin_id);
        getInt(ctx, argv[2], "noTimestampsId", opts.no_timestamps_id);
        onToken = JS_GetPropertyStr(ctx, argv[2], "onToken");
        onWindow = JS_GetPropertyStr(ctx, argv[2], "onWindow");
    }
    if (JS_IsFunction(ctx, onWindow)) {
        opts.on_window = [ctx, onWindow](double start) {
            JSValue a = JS_NewFloat64(ctx, start);
            JSValue r = JS_Call(ctx, onWindow, JS_UNDEFINED, 1, &a);
            if (JS_IsException(r)) JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, r);
            JS_FreeValue(ctx, a);
        };
    }
    // The callback fires synchronously inside transcribe() on this (JS) thread,
    // so it can call straight back into JS — no handoff needed for the sync path.
    if (JS_IsFunction(ctx, onToken)) {
        opts.on_token = [ctx, onToken](int32_t id) {
            JSValue a = JS_NewInt32(ctx, id);
            JSValue r = JS_Call(ctx, onToken, JS_UNDEFINED, 1, &a);
            if (JS_IsException(r)) JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, r);
            JS_FreeValue(ctx, a);
        };
    }

    // Abort on process interrupt (Ctrl+C / window close / engine teardown) —
    // the sync call would otherwise block shutdown for the full transcription.
    opts.cancel = [] { return bro::util::interrupted(); };

    JSValue result;
    try {
        auto out = w->whisper->transcribe(audio, prompt, opts);
        result = qjsbind::make_int32_array(ctx, out.token_ids);
    } catch (const std::exception& e) {
        result = JS_ThrowInternalError(ctx, "transcribe: %s", e.what());
    }
    JS_FreeValue(ctx, onToken);
    JS_FreeValue(ctx, onWindow);
    return result;
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
        .method_raw("transcribe", js_whisper_transcribe, 3)
        .method_raw("createSession", js_whisper_createSession, 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// Parakeet methods
// ═══════════════════════════════════════════════════════════════════════════

static ParakeetWrapper* parakeetSelf(JSContext* ctx, JSValueConst this_val) {
    return qjsbind::unwrap<ParakeetWrapper>(ctx, this_val);
}

// { tokenIds: Int32Array, tokenFrames: Int32Array } — tokenFrames[i] is the
// encoder-frame index token i was emitted at; * frameSeconds for a start time.
static JSValue makeParakeetResult(JSContext* c,
                                  const std::vector<int32_t>& ids,
                                  const std::vector<int32_t>& frames) {
    JSValue obj = JS_NewObject(c);
    JS_SetPropertyStr(c, obj, "tokenIds",    qjsbind::make_int32_array(c, ids));
    JS_SetPropertyStr(c, obj, "tokenFrames", qjsbind::make_int32_array(c, frames));
    return obj;
}

// transcribe(audio, opts?) -> { tokenIds, tokenFrames }
//   audio: Float32Array @ 16 kHz, OR { samples, sampleRate } object. Parakeet
//          is unconditional — no prompt.
//   opts.maxNewTokens: cap on emitted tokens (0 = decode the whole clip).
//   opts.onToken(id):  invoked once per emitted token, in order, synchronously
//                      on this thread — for live partial decode.
static JSValue js_parakeet_transcribe(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv) {
    auto* w = parakeetSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "transcribe: not a Parakeet");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "transcribe(audio, opts?): audio required");

    brosoundml::AudioBuffer audio;
    std::string err;
    if (!readAudioBuffer(ctx, argv[0], audio, err))
        return JS_ThrowTypeError(ctx, "transcribe: %s", err.c_str());

    brosoundml::Parakeet::TranscribeOptions opts;
    JSValue onToken = JS_UNDEFINED;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        getInt(ctx, argv[1], "maxNewTokens", opts.max_new_tokens);
        onToken = JS_GetPropertyStr(ctx, argv[1], "onToken");
    }
    // Fires synchronously inside transcribe() on this (JS) thread, so it can
    // call straight back into JS — no handoff needed for the sync path.
    if (JS_IsFunction(ctx, onToken)) {
        opts.on_token = [ctx, onToken](int32_t id) {
            JSValue a = JS_NewInt32(ctx, id);
            JSValue r = JS_Call(ctx, onToken, JS_UNDEFINED, 1, &a);
            if (JS_IsException(r)) JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, r);
            JS_FreeValue(ctx, a);
        };
    }

    // Abort on process interrupt (Ctrl+C / window close / engine teardown).
    opts.cancel = [] { return bro::util::interrupted(); };

    JSValue result;
    try {
        brotensor::DeviceScope scope(w->device);
        auto out = w->parakeet->transcribe(audio, opts);
        result = makeParakeetResult(ctx, out.token_ids, out.token_frames);
    } catch (const std::exception& e) {
        result = JS_ThrowInternalError(ctx, "transcribe: %s", e.what());
    }
    JS_FreeValue(ctx, onToken);
    return result;
}

static void registerParakeetClass(JSContext* ctx) {
    qjsbind::Class<ParakeetWrapper>(ctx, "Parakeet", qjsbind::NoGlobal)
        .get("loaded",       [](ParakeetWrapper* w) { return w->parakeet->loaded(); })
        .get("sampleRate",   [](ParakeetWrapper* w) { return w->parakeet->config().sample_rate; })
        .get("vocabSize",    [](ParakeetWrapper* w) { return w->parakeet->config().vocab_size; })
        .get("blankTokenId", [](ParakeetWrapper* w) { return w->parakeet->config().blank_token_id; })
        .get("frameSeconds", [](ParakeetWrapper* w) { return w->parakeet->config().frame_seconds(); })
        .method_raw("transcribe", js_parakeet_transcribe, 2)
        .method_raw("createSession", js_parakeet_createSession, 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// QwenAsr methods
// ═══════════════════════════════════════════════════════════════════════════

static QwenAsrWrapper* qwenAsrSelf(JSContext* ctx, JSValueConst this_val) {
    return qjsbind::unwrap<QwenAsrWrapper>(ctx, this_val);
}

// transcribe(audio, opts?) -> Int32Array (generated ids only)
//   audio: Float32Array @ 16 kHz, OR { samples, sampleRate } object.
//   opts.maxNewTokens: cap on the autoregressive loop (0 = 1024).
//   opts.contextIds:   Int32Array of Qwen BPE ids (bro.lm tokenizer.encode())
//                      placed in the chat template's system block — biases
//                      recognition toward names / domain terms.
//   opts.onToken(id):  invoked once per decoded token, in order, synchronously
//                      on this thread — for live partial decode.
// The stream is the model's native "language <Language><asr_text>transcript"
// format — split the id stream on model.asrTextId, then decode each side
// with the Qwen tokenizer (the marker detokenizes to an empty string, so a
// text-level split does not work).
static JSValue js_qwenasr_transcribe(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    auto* w = qwenAsrSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "transcribe: not a QwenAsr");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "transcribe(audio, opts?): audio required");

    brosoundml::AudioBuffer audio;
    std::string err;
    if (!readAudioBuffer(ctx, argv[0], audio, err))
        return JS_ThrowTypeError(ctx, "transcribe: %s", err.c_str());

    brosoundml::QwenAsr::TranscribeOptions opts;
    JSValue onToken = JS_UNDEFINED;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        getInt(ctx, argv[1], "maxNewTokens", opts.max_new_tokens);
        JSValue cv = JS_GetPropertyStr(ctx, argv[1], "contextIds");
        if (!JS_IsUndefined(cv) && !JS_IsNull(cv))
            opts.context_ids = qjsbind::read_int32_array(ctx, cv);
        JS_FreeValue(ctx, cv);
        onToken = JS_GetPropertyStr(ctx, argv[1], "onToken");
    }
    // Fires synchronously inside transcribe() on this (JS) thread, so it can
    // call straight back into JS — no handoff needed for the sync path.
    if (JS_IsFunction(ctx, onToken)) {
        opts.on_token = [ctx, onToken](int32_t id) {
            JSValue a = JS_NewInt32(ctx, id);
            JSValue r = JS_Call(ctx, onToken, JS_UNDEFINED, 1, &a);
            if (JS_IsException(r)) JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, r);
            JS_FreeValue(ctx, a);
        };
    }

    // Abort on process interrupt (Ctrl+C / window close / engine teardown).
    opts.cancel = [] { return bro::util::interrupted(); };

    JSValue result;
    try {
        brotensor::DeviceScope scope(w->device);
        auto out = w->asr->transcribe(audio, opts);
        result = qjsbind::make_int32_array(ctx, out.token_ids);
    } catch (const std::exception& e) {
        result = JS_ThrowInternalError(ctx, "transcribe: %s", e.what());
    }
    JS_FreeValue(ctx, onToken);
    return result;
}

// encode(audio) -> { latents: Float32Array, frames, latentDim, latentHz }
//   Latent tap: AuT encoder + projector only (no decoder). `latents` is
//   row-major (frames, latentDim) on the host — the rows transcribe() splices
//   over the <|audio_pad|> block, at latentHz rows/second.
static JSValue js_qwenasr_encode(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    auto* w = qwenAsrSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "encode: not a QwenAsr");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "encode(audio): audio required");

    brosoundml::AudioBuffer audio;
    std::string err;
    if (!readAudioBuffer(ctx, argv[0], audio, err))
        return JS_ThrowTypeError(ctx, "encode: %s", err.c_str());

    try {
        brotensor::DeviceScope scope(w->device);
        std::vector<float> latents;
        const int frames = w->asr->encode_to_host(audio, latents);
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "latents",
                          qjsbind::make_float32_array(ctx, latents));
        JS_SetPropertyStr(ctx, obj, "frames", JS_NewInt32(ctx, frames));
        JS_SetPropertyStr(ctx, obj, "latentDim",
                          JS_NewInt32(ctx, w->asr->config().latent_dim));
        JS_SetPropertyStr(ctx, obj, "latentHz",
                          JS_NewFloat64(ctx, w->asr->config().latent_hz));
        return obj;
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "encode: %s", e.what());
    }
}

static void registerQwenAsrClass(JSContext* ctx) {
    qjsbind::Class<QwenAsrWrapper>(ctx, "QwenAsr", qjsbind::NoGlobal)
        .get("loaded",     [](QwenAsrWrapper* w) { return w->asr->loaded(); })
        .get("sampleRate", [](QwenAsrWrapper* w) { return w->asr->config().sample_rate; })
        .get("latentDim",  [](QwenAsrWrapper* w) { return w->asr->config().latent_dim; })
        .get("latentHz",   [](QwenAsrWrapper* w) { return (double)w->asr->config().latent_hz; })
        .get("vocabSize",  [](QwenAsrWrapper* w) { return w->asr->config().vocab_size; })
        .get("asrTextId",  [](QwenAsrWrapper* w) { return w->asr->config().asr_text_token_id; })
        .method_raw("transcribe", js_qwenasr_transcribe, 2)
        .method_raw("encode",     js_qwenasr_encode,     1)
        .method_raw("createSession", js_qwenasr_createSession, 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// QwenAsrStream methods
// ═══════════════════════════════════════════════════════════════════════════

static QwenAsrStreamWrapper* qwenAsrStreamSelf(JSContext* ctx,
                                               JSValueConst this_val) {
    return qjsbind::unwrap<QwenAsrStreamWrapper>(ctx, this_val);
}

// feed(samples) -> number of newly finalized latent rows
//   samples: Float32Array of mono 16 kHz PCM (e.g. a bro.mic chunk). Encodes
//   every block that completed on this call; 0 if no block boundary crossed.
static JSValue js_qwenasrstream_feed(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    auto* w = qwenAsrStreamSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "feed: not a QwenAsrStream");
    size_t cnt = 0;
    const float* p = (argc >= 1) ? qjsbind::read_float32_view(ctx, argv[0], cnt) : nullptr;
    if (!p)
        return JS_ThrowTypeError(ctx, "feed(samples): Float32Array required");
    try {
        brotensor::DeviceScope scope(w->device);
        return JS_NewInt32(ctx, w->stream->feed(p, (int)cnt));
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "feed: %s", e.what());
    }
}

// finish() -> number of newly finalized latent rows (flush the partial block)
static JSValue js_qwenasrstream_finish(JSContext* ctx, JSValueConst this_val,
                                       int, JSValueConst*) {
    auto* w = qwenAsrStreamSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "finish: not a QwenAsrStream");
    try {
        brotensor::DeviceScope scope(w->device);
        return JS_NewInt32(ctx, w->stream->finish());
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "finish: %s", e.what());
    }
}

// latents(startFrame=0, count=rest) -> Float32Array
//   Copy of the finalized latent rows [startFrame, startFrame+count), row-major
//   (count, latentDim). Callers polling feed()'s return value slice just the
//   new rows: stream.latents(stream.frames - newRows, newRows).
static JSValue js_qwenasrstream_latents(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv) {
    auto* w = qwenAsrStreamSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "latents: not a QwenAsrStream");
    const int dim    = w->stream->config().latent_dim;
    const int total  = w->stream->frames();
    int start = 0, count = -1;
    if (argc >= 1 && JS_IsNumber(argv[0])) JS_ToInt32(ctx, &start, argv[0]);
    if (argc >= 2 && JS_IsNumber(argv[1])) JS_ToInt32(ctx, &count, argv[1]);
    if (start < 0 || start > total)
        return JS_ThrowRangeError(ctx, "latents: startFrame out of range");
    if (count < 0) count = total - start;
    if (count > total - start)
        return JS_ThrowRangeError(ctx, "latents: count out of range");
    const auto& all = w->stream->latents();
    return qjsbind::make_float32_array(
        ctx, all.data() + (size_t)start * dim, (size_t)count * dim);
}

static void registerQwenAsrStreamClass(JSContext* ctx) {
    qjsbind::Class<QwenAsrStreamWrapper>(ctx, "QwenAsrStream", qjsbind::NoGlobal)
        .get("loaded",      [](QwenAsrStreamWrapper* w) { return w->stream->loaded(); })
        .get("sampleRate",  [](QwenAsrStreamWrapper* w) { return w->stream->config().sample_rate; })
        .get("latentDim",   [](QwenAsrStreamWrapper* w) { return w->stream->config().latent_dim; })
        .get("latentHz",    [](QwenAsrStreamWrapper* w) { return (double)w->stream->config().latent_hz; })
        .get("frames",      [](QwenAsrStreamWrapper* w) { return w->stream->frames(); })
        .get("blockChunks", [](QwenAsrStreamWrapper* w) { return w->stream->block_chunks(); })
        .get("blockFrames", [](QwenAsrStreamWrapper* w) { return w->stream->block_frames(); })
        .method_raw("feed",    js_qwenasrstream_feed,    1)
        .method_raw("finish",  js_qwenasrstream_finish,  0)
        .method_raw("latents", js_qwenasrstream_latents, 2);
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
    // Resolve the same way fs.existsSync() does (app-relative base paths),
    // not against the raw OS process cwd.
    dir = brokit::api::resolveAssetPath(ctx, dir);

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
    vocab  = brokit::api::resolveAssetPath(ctx, vocab);
    merges = brokit::api::resolveAssetPath(ctx, merges);

    // Optional: an upstream openai/whisper added_tokens.json carrying the
    // "<|...|>" specials. Absent for the converted (merged) layout.
    std::string addedTokens;
    if (getStr(ctx, argv[0], "addedTokensPath", addedTokens))
        addedTokens = brokit::api::resolveAssetPath(ctx, addedTokens);

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

// Build + load the Parakeet model from a checkpoint dir. Heavy + blocking
// (file IO + GPU upload); shared by the sync and async loadParakeet paths.
// Throws on error.
static void buildParakeet(const std::string& dir, brotensor::Device dev,
                          std::unique_ptr<ParakeetWrapper>& w_out) {
    auto w = std::make_unique<ParakeetWrapper>();
    w->device   = dev;
    w->parakeet = std::make_unique<brosoundml::Parakeet>();
    {
        brotensor::DeviceScope scope(dev);
        w->parakeet->load(dir, dev);
    }
    std::fprintf(stderr, "[INFO] [stt] Parakeet loaded on %s\n", deviceName(dev));
    w_out = std::move(w);
}

// State for an async loadParakeet — same shape as WhisperLoadState.
struct ParakeetLoadState {
    std::string                      dir;
    brotensor::Device                dev = brotensor::Device::CPU;
    std::unique_ptr<ParakeetWrapper> w;
    JSValue onReady = JS_UNDEFINED;
    JSValue onError = JS_UNDEFINED;
    bool    hasReady = false, hasError = false;
};

// bro.stt.loadParakeet(modelDir, opts?) -> Parakeet         (sync)
//                                       -> AsyncHandle      (async, if opts.onReady)
//   modelDir contains config.json + model.safetensors (HF Parakeet-TDT
//   checkpoint, e.g. nvidia/parakeet-tdt-0.6b-v3).
//   opts.device: 'cuda' | 'cpu' — defaults to CUDA when available, else CPU.
//   opts.onReady(parakeet) / opts.onError(message): when onReady is a function
//   the load runs on a background thread and these fire on the JS thread.
static JSValue js_loadParakeet(JSContext* ctx, JSValueConst,
                               int argc, JSValueConst* argv) {
    std::string dir;
    if (argc < 1 || !argStr(ctx, argv[0], dir))
        return JS_ThrowTypeError(ctx, "loadParakeet(modelDir, opts?): path required");
    dir = brokit::api::resolveAssetPath(ctx, dir);

    brotensor::init();
    brotensor::Device dev = autoDevice();
    if (argc >= 2) {
        std::string err;
        if (!parseDeviceOpt(ctx, argv[1], dev, err))
            return JS_ThrowTypeError(ctx, "loadParakeet: %s", err.c_str());
    }

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
            std::unique_ptr<ParakeetWrapper> w;
            buildParakeet(dir, dev, w);
            return qjsbind::wrap<ParakeetWrapper>(ctx, w.release());
        } catch (const std::exception& e) {
            return JS_ThrowInternalError(ctx, "loadParakeet: %s", e.what());
        }
    }

    // ── Async path ──
    auto ls = std::make_shared<ParakeetLoadState>();
    ls->dir      = dir;
    ls->dev      = dev;
    ls->hasReady = true;
    ls->onReady  = JS_DupValue(ctx, onReady);
    ls->hasError = JS_IsFunction(ctx, onError);
    ls->onError  = ls->hasError ? JS_DupValue(ctx, onError) : JS_UNDEFINED;
    JS_FreeValue(ctx, onReady);
    JS_FreeValue(ctx, onError);

    auto work = [ls](const std::atomic<bool>&) {
        buildParakeet(ls->dir, ls->dev, ls->w);  // throws -> error
    };
    auto done = [ls](JSContext* c, bool /*cancelled*/, const std::string& error) {
        if (!error.empty() || !ls->w) {
            if (ls->hasError) {
                JSValue e = JS_NewString(c, error.empty() ? "loadParakeet failed"
                                                          : error.c_str());
                JSValue r = JS_Call(c, ls->onError, JS_UNDEFINED, 1, &e);
                if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
                JS_FreeValue(c, r);
                JS_FreeValue(c, e);
            }
        } else {
            JSValue out = qjsbind::wrap<ParakeetWrapper>(c, ls->w.release());
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

// Build the SentencePiece tokenizer from a HF tokenizer.json. Cheap relative
// to the model, but the async path keeps the loader API uniform.
static void buildParakeetTokenizer(const std::string& path,
                                   std::unique_ptr<ParakeetTokenizerWrapper>& tw_out) {
    auto tw = std::make_unique<ParakeetTokenizerWrapper>();
    tw->tok = std::make_unique<brolm::t5::Tokenizer>(
        brolm::t5::Tokenizer::load(path));
    tw_out = std::move(tw);
}

// State for an async loadParakeetTokenizer.
struct ParakeetTokLoadState {
    std::string                               path;
    std::unique_ptr<ParakeetTokenizerWrapper> tw;
    JSValue onReady = JS_UNDEFINED;
    JSValue onError = JS_UNDEFINED;
    bool    hasReady = false, hasError = false;
};

// bro.stt.loadParakeetTokenizer(tokenizerJsonPath, opts?)
//   -> ParakeetTokenizer  (sync)
//   -> AsyncHandle        (async, if opts.onReady)
//   tokenizerJsonPath: the HF tokenizer.json beside the model checkpoint.
static JSValue js_loadParakeetTokenizer(JSContext* ctx, JSValueConst,
                                        int argc, JSValueConst* argv) {
    std::string path;
    if (argc < 1 || !argStr(ctx, argv[0], path))
        return JS_ThrowTypeError(ctx,
            "loadParakeetTokenizer(tokenizerJsonPath, opts?): path required");
    path = brokit::api::resolveAssetPath(ctx, path);

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
            std::unique_ptr<ParakeetTokenizerWrapper> tw;
            buildParakeetTokenizer(path, tw);
            return qjsbind::wrap<ParakeetTokenizerWrapper>(ctx, tw.release());
        } catch (const std::exception& e) {
            return JS_ThrowInternalError(ctx, "loadParakeetTokenizer: %s", e.what());
        }
    }

    // ── Async path ──
    auto ls = std::make_shared<ParakeetTokLoadState>();
    ls->path     = path;
    ls->hasReady = true;
    ls->onReady  = JS_DupValue(ctx, onReady);
    ls->hasError = JS_IsFunction(ctx, onError);
    ls->onError  = ls->hasError ? JS_DupValue(ctx, onError) : JS_UNDEFINED;
    JS_FreeValue(ctx, onReady);
    JS_FreeValue(ctx, onError);

    auto work = [ls](const std::atomic<bool>&) {
        buildParakeetTokenizer(ls->path, ls->tw);  // throws -> error
    };
    auto done = [ls](JSContext* c, bool /*cancelled*/, const std::string& error) {
        if (!error.empty() || !ls->tw) {
            if (ls->hasError) {
                JSValue e = JS_NewString(c, error.empty()
                                                ? "loadParakeetTokenizer failed"
                                                : error.c_str());
                JSValue r = JS_Call(c, ls->onError, JS_UNDEFINED, 1, &e);
                if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
                JS_FreeValue(c, r);
                JS_FreeValue(c, e);
            }
        } else {
            JSValue out = qjsbind::wrap<ParakeetTokenizerWrapper>(c, ls->tw.release());
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

// Build + load the Qwen3-ASR model from a checkpoint dir. Heavy + blocking
// (file IO + GPU upload); shared by the sync and async loadQwenAsr paths.
// Throws on error.
static void buildQwenAsr(const std::string& dir, brotensor::Device dev,
                         std::unique_ptr<QwenAsrWrapper>& w_out) {
    auto w = std::make_unique<QwenAsrWrapper>();
    w->device = dev;
    w->asr    = std::make_unique<brosoundml::QwenAsr>();
    {
        brotensor::DeviceScope scope(dev);
        w->asr->load(dir, dev);
    }
    std::fprintf(stderr, "[INFO] [stt] Qwen3-ASR loaded on %s\n", deviceName(dev));
    w_out = std::move(w);
}

// State for an async loadQwenAsr — same shape as WhisperLoadState.
struct QwenAsrLoadState {
    std::string                     dir;
    brotensor::Device               dev = brotensor::Device::CPU;
    std::unique_ptr<QwenAsrWrapper> w;
    JSValue onReady = JS_UNDEFINED;
    JSValue onError = JS_UNDEFINED;
    bool    hasReady = false, hasError = false;
};

// bro.stt.loadQwenAsr(modelDir, opts?) -> QwenAsr           (sync)
//                                      -> AsyncHandle       (async, if opts.onReady)
//   modelDir contains config.json + model.safetensors (HF Qwen3-ASR
//   checkpoint, e.g. Qwen/Qwen3-ASR-0.6B). The Qwen BPE tokenizer files
//   (vocab.json + merges.txt) sit in the same dir — load them with
//   bro.lm.loadTokenizer to detokenize the id stream.
//   opts.device: 'cuda' | 'cpu' — defaults to CUDA when available, else CPU.
//   opts.onReady(asr) / opts.onError(message): when onReady is a function the
//   load runs on a background thread and these fire on the JS thread.
static JSValue js_loadQwenAsr(JSContext* ctx, JSValueConst,
                              int argc, JSValueConst* argv) {
    std::string dir;
    if (argc < 1 || !argStr(ctx, argv[0], dir))
        return JS_ThrowTypeError(ctx, "loadQwenAsr(modelDir, opts?): path required");
    dir = brokit::api::resolveAssetPath(ctx, dir);

    brotensor::init();
    brotensor::Device dev = autoDevice();
    if (argc >= 2) {
        std::string err;
        if (!parseDeviceOpt(ctx, argv[1], dev, err))
            return JS_ThrowTypeError(ctx, "loadQwenAsr: %s", err.c_str());
    }

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
            std::unique_ptr<QwenAsrWrapper> w;
            buildQwenAsr(dir, dev, w);
            return qjsbind::wrap<QwenAsrWrapper>(ctx, w.release());
        } catch (const std::exception& e) {
            return JS_ThrowInternalError(ctx, "loadQwenAsr: %s", e.what());
        }
    }

    // ── Async path ──
    auto ls = std::make_shared<QwenAsrLoadState>();
    ls->dir      = dir;
    ls->dev      = dev;
    ls->hasReady = true;
    ls->onReady  = JS_DupValue(ctx, onReady);
    ls->hasError = JS_IsFunction(ctx, onError);
    ls->onError  = ls->hasError ? JS_DupValue(ctx, onError) : JS_UNDEFINED;
    JS_FreeValue(ctx, onReady);
    JS_FreeValue(ctx, onError);

    auto work = [ls](const std::atomic<bool>&) {
        buildQwenAsr(ls->dir, ls->dev, ls->w);  // throws -> error
    };
    auto done = [ls](JSContext* c, bool /*cancelled*/, const std::string& error) {
        if (!error.empty() || !ls->w) {
            if (ls->hasError) {
                JSValue e = JS_NewString(c, error.empty() ? "loadQwenAsr failed"
                                                          : error.c_str());
                JSValue r = JS_Call(c, ls->onError, JS_UNDEFINED, 1, &e);
                if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
                JS_FreeValue(c, r);
                JS_FreeValue(c, e);
            }
        } else {
            JSValue out = qjsbind::wrap<QwenAsrWrapper>(c, ls->w.release());
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

// Build + load the encoder-only streaming tap. Lighter than the full model
// (no decoder weights) but still file IO + GPU upload.
static void buildQwenAsrStream(const std::string& dir, int blockChunks,
                               brotensor::Device dev,
                               std::unique_ptr<QwenAsrStreamWrapper>& w_out) {
    auto w = std::make_unique<QwenAsrStreamWrapper>();
    w->device = dev;
    w->stream = std::make_unique<brosoundml::QwenAsrStream>();
    {
        brotensor::DeviceScope scope(dev);
        w->stream->load(dir, blockChunks, dev);
    }
    std::fprintf(stderr, "[INFO] [stt] Qwen3-ASR stream encoder loaded on %s\n",
                 deviceName(dev));
    w_out = std::move(w);
}

// State for an async loadQwenAsrStream.
struct QwenAsrStreamLoadState {
    std::string                           dir;
    int                                   blockChunks = 1;
    brotensor::Device                     dev = brotensor::Device::CPU;
    std::unique_ptr<QwenAsrStreamWrapper> w;
    JSValue onReady = JS_UNDEFINED;
    JSValue onError = JS_UNDEFINED;
    bool    hasReady = false, hasError = false;
};

// bro.stt.loadQwenAsrStream(modelDir, opts?) -> QwenAsrStream  (sync)
//                                            -> AsyncHandle    (async, if opts.onReady)
//   Encoder-only incremental tap: feed() mic chunks, get finalized latent rows
//   back per ~blockChunks seconds of audio. Same modelDir as loadQwenAsr; only
//   the audio-tower weights are read.
//   opts.blockChunks: block size in ~1 s conv-chunks (default 1 = lowest
//   latency, ~13 latents per second-block).
//   opts.device / opts.onReady / opts.onError: as loadQwenAsr.
static JSValue js_loadQwenAsrStream(JSContext* ctx, JSValueConst,
                                    int argc, JSValueConst* argv) {
    std::string dir;
    if (argc < 1 || !argStr(ctx, argv[0], dir))
        return JS_ThrowTypeError(ctx,
            "loadQwenAsrStream(modelDir, opts?): path required");
    dir = brokit::api::resolveAssetPath(ctx, dir);

    brotensor::init();
    brotensor::Device dev = autoDevice();
    int blockChunks = 1;
    if (argc >= 2) {
        std::string err;
        if (!parseDeviceOpt(ctx, argv[1], dev, err))
            return JS_ThrowTypeError(ctx, "loadQwenAsrStream: %s", err.c_str());
        if (JS_IsObject(argv[1])) getInt(ctx, argv[1], "blockChunks", blockChunks);
    }

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
            std::unique_ptr<QwenAsrStreamWrapper> w;
            buildQwenAsrStream(dir, blockChunks, dev, w);
            return qjsbind::wrap<QwenAsrStreamWrapper>(ctx, w.release());
        } catch (const std::exception& e) {
            return JS_ThrowInternalError(ctx, "loadQwenAsrStream: %s", e.what());
        }
    }

    // ── Async path ──
    auto ls = std::make_shared<QwenAsrStreamLoadState>();
    ls->dir         = dir;
    ls->blockChunks = blockChunks;
    ls->dev         = dev;
    ls->hasReady    = true;
    ls->onReady     = JS_DupValue(ctx, onReady);
    ls->hasError    = JS_IsFunction(ctx, onError);
    ls->onError     = ls->hasError ? JS_DupValue(ctx, onError) : JS_UNDEFINED;
    JS_FreeValue(ctx, onReady);
    JS_FreeValue(ctx, onError);

    auto work = [ls](const std::atomic<bool>&) {
        buildQwenAsrStream(ls->dir, ls->blockChunks, ls->dev, ls->w);
    };
    auto done = [ls](JSContext* c, bool /*cancelled*/, const std::string& error) {
        if (!error.empty() || !ls->w) {
            if (ls->hasError) {
                JSValue e = JS_NewString(c, error.empty()
                                                ? "loadQwenAsrStream failed"
                                                : error.c_str());
                JSValue r = JS_Call(c, ls->onError, JS_UNDEFINED, 1, &e);
                if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
                JS_FreeValue(c, r);
                JS_FreeValue(c, e);
            }
        } else {
            JSValue out = qjsbind::wrap<QwenAsrStreamWrapper>(c, ls->w.release());
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
//                       bro.stt.transcribe(parakeet, audio, opts)
//                       bro.stt.transcribe(qwenAsr, audio, opts)
// ═══════════════════════════════════════════════════════════════════════════
//
// Runs Whisper's autoregressive decode on a background thread via the async-job
// runner, so the JS thread stays responsive. Cancellation is real: brosoundml's
// greedy loop checks the async-job cancel flag once per token, so .cancel() stops
// the decode within ~1 token and frees the model's busy lock rather than running
// to completion. Returns an AsyncHandle with .cancel(); opts.onDone(ids, info)
// fires once on the JS thread with info being { cancelled, error? }.
//
// opts.onToken(id) streams each decoded token id as it is produced: the work
// thread (sole writer) publishes a committed token prefix via an atomic and the
// JS-thread poll drains it and fires onToken — lock-free, no mutex, same SPSC
// handoff as the Qwen-TTS streaming path. opts.timestampBeginId enables long-form
// (>30 s) windowed decode instead of truncating to the first 30 s window.

// Shared between the work thread (sole writer of token_ids) and the JS thread
// (sole reader / caller of onDone). Held by shared_ptr.
struct SttJob {
    brosoundml::AudioBuffer audio;
    std::vector<int32_t>    prompt;
    int                     maxNew = 0;
    int                     timestampBeginId = -1;     // long-form seek anchor; <0 = off
    int                     noTimestampsId   = -1;     // suppressed token; <0 = off

    // Long-form window marks, published by the decode thread and drained by the
    // JS-thread poll alongside the tokens. Each carries the token index it
    // precedes, so the poll can fire onWindow *before* the tokens belonging to
    // that window — without which a caller placing timestamps live would attach
    // them to the previous window. Same lock-free shape as tokenSlots: the
    // producer stores the payload then releases the count.
    struct WindowSlot { double start = 0.0; size_t at = 0; };
    std::vector<WindowSlot> windowSlots;
    std::atomic<size_t>     windowsProduced{0};
    size_t                  windowsDrained = 0;
    bool                    hasOnWindow = false;
    JSValue                 onWindow = JS_UNDEFINED;   // dup'd; UNDEFINED if absent
    // Filled by work(): where every window began, for the onDone answer.
    std::vector<std::pair<double, size_t>> windows;
    std::vector<int32_t>    token_ids;          // filled by work()
    JSValue                 onDone     = JS_UNDEFINED;  // dup'd; UNDEFINED if absent
    JSValue                 onToken    = JS_UNDEFINED;  // dup'd; UNDEFINED if absent
    JSValue                 whisperRef = JS_UNDEFINED;  // dup of the whisper JS object
    bool                    hasOnDone  = false;
    bool                    hasOnToken = false;
    // SPSC token handoff: work thread writes tokenSlots[produced] then bumps
    // `produced`; the JS-thread poll reads up to `produced` and advances `drained`.
    std::vector<int32_t>    tokenSlots;
    std::atomic<size_t>     produced{0};
    size_t                  drained = 0;
};

// Shared between the work thread (sole writer of the result vectors) and the
// JS thread (sole reader / caller of onDone). Held by shared_ptr. Same SPSC
// token handoff as SttJob.
struct ParakeetJob {
    brosoundml::AudioBuffer audio;
    int                     maxNew = 0;
    std::vector<int32_t>    token_ids;     // filled by work()
    std::vector<int32_t>    token_frames;  // filled by work()
    JSValue                 onDone      = JS_UNDEFINED;
    JSValue                 onToken     = JS_UNDEFINED;
    JSValue                 parakeetRef = JS_UNDEFINED;
    bool                    hasOnDone   = false;
    bool                    hasOnToken  = false;
    std::vector<int32_t>    tokenSlots;
    std::atomic<size_t>     produced{0};
    size_t                  drained = 0;
};

// Shared between the work thread (sole writer of token_ids) and the JS thread
// (sole reader / caller of onDone). Held by shared_ptr. Same SPSC token
// handoff as SttJob.
struct QwenAsrJob {
    brosoundml::AudioBuffer audio;
    int                     maxNew = 0;
    std::vector<int32_t>    contextIds;
    std::vector<int32_t>    token_ids;     // filled by work()
    JSValue                 onDone  = JS_UNDEFINED;
    JSValue                 onToken = JS_UNDEFINED;
    JSValue                 asrRef  = JS_UNDEFINED;
    bool                    hasOnDone  = false;
    bool                    hasOnToken = false;
    std::vector<int32_t>    tokenSlots;
    std::atomic<size_t>     produced{0};
    size_t                  drained = 0;
};

// bro.stt.transcribe(qwenAsr, audio, opts?) — async Qwen3-ASR decode on a
// background thread. Mirrors the Parakeet path below: real cancellation (the
// greedy loop polls the flag once per token), SPSC onToken streaming,
// onDone(tokenIds, info). opts.contextIds biases recognition (see the sync
// method).
static JSValue js_stt_transcribe_qwenasr(JSContext* ctx, QwenAsrWrapper* w,
                                         int argc, JSValueConst* argv) {
    auto job = std::make_shared<QwenAsrJob>();
    std::string err;
    if (!readAudioBuffer(ctx, argv[1], job->audio, err))
        return JS_ThrowTypeError(ctx, "transcribe: %s", err.c_str());

    JSValue onDone = JS_UNDEFINED, onToken = JS_UNDEFINED;
    if (argc >= 3 && JS_IsObject(argv[2])) {
        getInt(ctx, argv[2], "maxNewTokens", job->maxNew);
        JSValue cv = JS_GetPropertyStr(ctx, argv[2], "contextIds");
        if (!JS_IsUndefined(cv) && !JS_IsNull(cv))
            job->contextIds = qjsbind::read_int32_array(ctx, cv);
        JS_FreeValue(ctx, cv);
        onDone  = JS_GetPropertyStr(ctx, argv[2], "onDone");
        onToken = JS_GetPropertyStr(ctx, argv[2], "onToken");
    }

    // Claim the model for this transcription (single-owner; one in flight).
    if (!w->busy.tryClaim()) {
        JS_FreeValue(ctx, onDone);
        JS_FreeValue(ctx, onToken);
        return JS_ThrowInternalError(ctx,
            "transcribe: an operation is already in flight on this model");
    }

    job->hasOnDone  = JS_IsFunction(ctx, onDone);
    job->onDone     = job->hasOnDone ? JS_DupValue(ctx, onDone) : JS_UNDEFINED;
    job->hasOnToken = JS_IsFunction(ctx, onToken);
    job->onToken    = job->hasOnToken ? JS_DupValue(ctx, onToken) : JS_UNDEFINED;
    job->asrRef     = JS_DupValue(ctx, argv[0]);  // keep the model alive
    JS_FreeValue(ctx, onDone);
    JS_FreeValue(ctx, onToken);
    if (job->hasOnToken) job->tokenSlots.resize(1u << 16);

    QwenAsrWrapper* mw = w;

    auto work = [job, mw](const std::atomic<bool>& cancel) {
        brotensor::DeviceScope scope(mw->device);
        brosoundml::QwenAsr::TranscribeOptions opts;
        opts.max_new_tokens = job->maxNew;
        opts.context_ids    = job->contextIds;
        opts.cancel = [&cancel] {
            return cancel.load(std::memory_order_acquire) || bro::util::interrupted();
        };
        if (job->hasOnToken) {
            opts.on_token = [job](int32_t id) {
                const size_t idx = job->produced.load(std::memory_order_relaxed);
                if (idx >= job->tokenSlots.size()) return;   // bound guard
                job->tokenSlots[idx] = id;
                job->produced.store(idx + 1, std::memory_order_release);
            };
        }
        auto out = mw->asr->transcribe(job->audio, opts);
        job->token_ids = std::move(out.token_ids);
    };

    auto poll = [job](JSContext* c) {
        if (!job->hasOnToken) return;
        const size_t n = job->produced.load(std::memory_order_acquire);
        while (job->drained < n) {
            JSValue a = JS_NewInt32(c, job->tokenSlots[job->drained]);
            JSValue r = JS_Call(c, job->onToken, JS_UNDEFINED, 1, &a);
            if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
            JS_FreeValue(c, r);
            JS_FreeValue(c, a);
            job->drained++;
        }
    };

    auto done = [job, mw](JSContext* c, bool cancelled, const std::string& error) {
        // Release the single-owner lock BEFORE invoking onDone so a callback that
        // synchronously starts the next transcription on this model (the natural
        // serialized-queue pattern, e.g. listen-lab's rolling transcript) succeeds
        // instead of tripping the in-flight guard. The work thread has finished
        // and joined; the result's host data is already on `job`, so a new op
        // claiming the model can't disturb what onDone reads here. (Matches the
        // TTS bindings, which release before onDone for the same reason.)
        mw->busy.release();
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
        if (job->hasOnDone)  JS_FreeValue(c, job->onDone);
        if (job->hasOnToken) JS_FreeValue(c, job->onToken);
        JS_FreeValue(c, job->asrRef);
    };

    return launchAsyncJob(ctx, std::move(work), std::move(poll), std::move(done));
}

// bro.stt.transcribe(parakeet, audio, opts?) — async Parakeet decode on a
// background thread. Mirrors the Whisper path below: real cancellation (the
// TDT loop polls the flag once per encoder frame), SPSC onToken streaming,
// onDone(result, info) with result = { tokenIds, tokenFrames }.
static JSValue js_stt_transcribe_parakeet(JSContext* ctx, ParakeetWrapper* w,
                                          int argc, JSValueConst* argv) {
    auto job = std::make_shared<ParakeetJob>();
    std::string err;
    if (!readAudioBuffer(ctx, argv[1], job->audio, err))
        return JS_ThrowTypeError(ctx, "transcribe: %s", err.c_str());

    JSValue onDone = JS_UNDEFINED, onToken = JS_UNDEFINED;
    if (argc >= 3 && JS_IsObject(argv[2])) {
        getInt(ctx, argv[2], "maxNewTokens", job->maxNew);
        onDone  = JS_GetPropertyStr(ctx, argv[2], "onDone");
        onToken = JS_GetPropertyStr(ctx, argv[2], "onToken");
    }

    // Claim the model for this transcription (single-owner; one in flight).
    if (!w->busy.tryClaim()) {
        JS_FreeValue(ctx, onDone);
        JS_FreeValue(ctx, onToken);
        return JS_ThrowInternalError(ctx,
            "transcribe: an operation is already in flight on this model");
    }

    job->hasOnDone   = JS_IsFunction(ctx, onDone);
    job->onDone      = job->hasOnDone ? JS_DupValue(ctx, onDone) : JS_UNDEFINED;
    job->hasOnToken  = JS_IsFunction(ctx, onToken);
    job->onToken     = job->hasOnToken ? JS_DupValue(ctx, onToken) : JS_UNDEFINED;
    job->parakeetRef = JS_DupValue(ctx, argv[0]);  // keep the model alive
    JS_FreeValue(ctx, onDone);
    JS_FreeValue(ctx, onToken);
    if (job->hasOnToken) job->tokenSlots.resize(1u << 16);

    ParakeetWrapper* mw = w;

    auto work = [job, mw](const std::atomic<bool>& cancel) {
        brotensor::DeviceScope scope(mw->device);
        brosoundml::Parakeet::TranscribeOptions opts;
        opts.max_new_tokens = job->maxNew;
        opts.cancel = [&cancel] {
            return cancel.load(std::memory_order_acquire) || bro::util::interrupted();
        };
        if (job->hasOnToken) {
            opts.on_token = [job](int32_t id) {
                const size_t idx = job->produced.load(std::memory_order_relaxed);
                if (idx >= job->tokenSlots.size()) return;   // bound guard
                job->tokenSlots[idx] = id;
                job->produced.store(idx + 1, std::memory_order_release);
            };
        }
        auto out = mw->parakeet->transcribe(job->audio, opts);
        job->token_ids    = std::move(out.token_ids);
        job->token_frames = std::move(out.token_frames);
    };

    auto poll = [job](JSContext* c) {
        if (!job->hasOnToken) return;
        const size_t n = job->produced.load(std::memory_order_acquire);
        while (job->drained < n) {
            JSValue a = JS_NewInt32(c, job->tokenSlots[job->drained]);
            JSValue r = JS_Call(c, job->onToken, JS_UNDEFINED, 1, &a);
            if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
            JS_FreeValue(c, r);
            JS_FreeValue(c, a);
            job->drained++;
        }
    };

    auto done = [job, mw](JSContext* c, bool cancelled, const std::string& error) {
        // Release the single-owner lock BEFORE invoking onDone so a callback that
        // synchronously starts the next transcription on this model (a serialized
        // queue) succeeds instead of tripping the in-flight guard. The work thread
        // has finished and joined; the result's host data is already on `job`, so
        // a new op claiming the model can't disturb what onDone reads here.
        mw->busy.release();
        if (job->hasOnDone) {
            JSValue res  = makeParakeetResult(c, job->token_ids, job->token_frames);
            JSValue info = JS_NewObject(c);
            JS_SetPropertyStr(c, info, "cancelled", JS_NewBool(c, cancelled));
            if (!error.empty())
                JS_SetPropertyStr(c, info, "error", JS_NewString(c, error.c_str()));
            JSValue args[2] = { res, info };
            JSValue r = JS_Call(c, job->onDone, JS_UNDEFINED, 2, args);
            if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
            JS_FreeValue(c, r);
            JS_FreeValue(c, res);
            JS_FreeValue(c, info);
        }
        if (job->hasOnDone)  JS_FreeValue(c, job->onDone);
        if (job->hasOnToken) JS_FreeValue(c, job->onToken);
        JS_FreeValue(c, job->parakeetRef);
    };

    return launchAsyncJob(ctx, std::move(work), std::move(poll), std::move(done));
}

static JSValue js_stt_transcribe(JSContext* ctx, JSValueConst,
                                 int argc, JSValueConst* argv) {
    if (argc < 2)
        return JS_ThrowTypeError(ctx,
            "transcribe(model, audio, ...): model and audio required");

    // Parakeet path — no prompt: transcribe(parakeet, audio, opts?).
    if (auto* p = qjsbind::unwrap<ParakeetWrapper>(ctx, argv[0]))
        return js_stt_transcribe_parakeet(ctx, p, argc, argv);

    // Qwen3-ASR path — no prompt: transcribe(qwenAsr, audio, opts?).
    if (auto* q = qjsbind::unwrap<QwenAsrWrapper>(ctx, argv[0]))
        return js_stt_transcribe_qwenasr(ctx, q, argc, argv);

    if (argc < 3)
        return JS_ThrowTypeError(ctx,
            "transcribe(whisper, audio, promptIds, opts?): whisper, audio and "
            "promptIds required");
    auto* w = qjsbind::unwrap<WhisperWrapper>(ctx, argv[0]);
    if (!w) return JS_ThrowTypeError(ctx,
        "transcribe: arg 0 must be a Whisper, Parakeet, or QwenAsr");

    auto job = std::make_shared<SttJob>();
    std::string err;
    if (!readAudioBuffer(ctx, argv[1], job->audio, err))
        return JS_ThrowTypeError(ctx, "transcribe: %s", err.c_str());

    job->prompt = qjsbind::read_int32_array(ctx, argv[2]);
    if (job->prompt.empty())
        return JS_ThrowTypeError(ctx,
            "transcribe: promptIds must be a non-empty Int32Array or number[] "
            "(use tokenizer.buildPrompt(lang, task))");

    JSValue onDone = JS_UNDEFINED, onToken = JS_UNDEFINED, onWindow = JS_UNDEFINED;
    if (argc >= 4 && JS_IsObject(argv[3])) {
        getInt(ctx, argv[3], "maxNewTokens", job->maxNew);
        getInt(ctx, argv[3], "timestampBeginId", job->timestampBeginId);
        getInt(ctx, argv[3], "noTimestampsId", job->noTimestampsId);
        onDone   = JS_GetPropertyStr(ctx, argv[3], "onDone");
        onToken  = JS_GetPropertyStr(ctx, argv[3], "onToken");
        onWindow = JS_GetPropertyStr(ctx, argv[3], "onWindow");
    }

    // Claim the model for this transcription (single-owner; one in flight).
    if (!w->busy.tryClaim()) {
        JS_FreeValue(ctx, onDone);
        JS_FreeValue(ctx, onToken);
        JS_FreeValue(ctx, onWindow);
        return JS_ThrowInternalError(ctx,
            "transcribe: an operation is already in flight on this model");
    }

    job->hasOnDone  = JS_IsFunction(ctx, onDone);
    job->onDone     = job->hasOnDone ? JS_DupValue(ctx, onDone) : JS_UNDEFINED;
    job->hasOnToken = JS_IsFunction(ctx, onToken);
    job->onToken    = job->hasOnToken ? JS_DupValue(ctx, onToken) : JS_UNDEFINED;
    job->hasOnWindow = JS_IsFunction(ctx, onWindow);
    job->onWindow    = job->hasOnWindow ? JS_DupValue(ctx, onWindow) : JS_UNDEFINED;
    job->whisperRef = JS_DupValue(ctx, argv[0]);  // keep the model alive
    JS_FreeValue(ctx, onDone);
    JS_FreeValue(ctx, onToken);
    JS_FreeValue(ctx, onWindow);
    // Pre-size the token slots so the work thread never reallocates while the JS
    // thread reads. Generous fixed cap (covers a long-form transcription of many
    // 30 s windows); if exceeded, streaming stops emitting but the full id stream
    // still arrives via onDone.
    if (job->hasOnToken) job->tokenSlots.resize(1u << 16);
    // One per 30 s window: 4096 covers thirty-four hours of audio, well past
    // anything the token cap above allows through.
    if (job->hasOnWindow) job->windowSlots.resize(4096);

    WhisperWrapper* mw = w;

    // Background thread: run the transcribe and stash the ids. A barge-in
    // flips the async-job cancel flag; Whisper's greedy decode polls it once
    // per token and returns early, so the GPU stops and the model's `busy`
    // lock is released promptly (the result is discarded by `done`). The check
    // runs synchronously inside transcribe(), so capturing `cancel` by
    // reference is safe.
    auto work = [job, mw](const std::atomic<bool>& cancel) {
        brotensor::DeviceScope scope(mw->device);
        brosoundml::Whisper::TranscribeOptions opts;
        opts.max_new_tokens      = job->maxNew;
        opts.timestamp_begin_id  = job->timestampBeginId;
        opts.no_timestamps_id    = job->noTimestampsId;
        opts.cancel = [&cancel] {
            return cancel.load(std::memory_order_acquire) || bro::util::interrupted();
        };
        if (job->hasOnToken) {
            // Runs on THIS (background) thread: publish into the next slot; the
            // JS-thread poll fires onToken. No JS here.
            opts.on_token = [job](int32_t id) {
                const size_t idx = job->produced.load(std::memory_order_relaxed);
                if (idx >= job->tokenSlots.size()) return;   // bound guard
                job->tokenSlots[idx] = id;
                job->produced.store(idx + 1, std::memory_order_release);
            };
        }
        if (job->hasOnWindow) {
            // Same thread, same shape. `at` is the count of tokens streamed so
            // far, which is what the poll compares its drain cursor against.
            opts.on_window = [job](double start) {
                const size_t idx = job->windowsProduced.load(std::memory_order_relaxed);
                if (idx >= job->windowSlots.size()) return;  // bound guard
                job->windowSlots[idx] = {start,
                                         job->produced.load(std::memory_order_relaxed)};
                job->windowsProduced.store(idx + 1, std::memory_order_release);
            };
        }
        auto out = mw->whisper->transcribe(job->audio, job->prompt, opts);
        job->token_ids = std::move(out.token_ids);
        for (const auto& w : out.windows)
            job->windows.emplace_back(w.start_seconds, w.first_token);
    };

    // JS thread, drains committed window marks and tokens and fires the
    // callbacks for each, **interleaved in decode order**.
    auto poll = [job](JSContext* c) {
        /// One callback, one argument, swallowing a throw as the drain always has.
        auto call = [c](JSValue fn, JSValue arg) {
            JSValue r = JS_Call(c, fn, JS_UNDEFINED, 1, &arg);
            if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
            JS_FreeValue(c, r);
            JS_FreeValue(c, arg);
        };

        const size_t nw = job->hasOnWindow
                              ? job->windowsProduced.load(std::memory_order_acquire)
                              : 0;
        const size_t n = job->hasOnToken
                             ? job->produced.load(std::memory_order_acquire)
                             : 0;

        // A window mark fires before the tokens that belong to it. Draining the
        // tokens first would hand a caller that window's own timestamps while it
        // still believed itself to be in the previous window, which is exactly
        // the misplacement onWindow exists to prevent.
        for (;;) {
            if (job->windowsDrained < nw &&
                job->windowSlots[job->windowsDrained].at <= job->drained) {
                call(job->onWindow,
                     JS_NewFloat64(c, job->windowSlots[job->windowsDrained].start));
                job->windowsDrained++;
                continue;
            }
            if (job->drained < n) {
                call(job->onToken, JS_NewInt32(c, job->tokenSlots[job->drained]));
                job->drained++;
                continue;
            }
            break;
        }
    };

    // JS thread, once: hand the id array + {cancelled,error} to onDone, free the
    // dup'd values, release the model.
    auto done = [job, mw](JSContext* c, bool cancelled, const std::string& error) {
        // Release the single-owner lock BEFORE invoking onDone so a callback that
        // synchronously starts the next transcription on this model (a serialized
        // queue) succeeds instead of tripping the in-flight guard. The work thread
        // has finished and joined; the result's host data is already on `job`, so
        // a new op claiming the model can't disturb what onDone reads here.
        mw->busy.release();
        if (job->hasOnDone) {
            JSValue arr  = qjsbind::make_int32_array(c, job->token_ids);
            JSValue info = JS_NewObject(c);
            JS_SetPropertyStr(c, info, "cancelled", JS_NewBool(c, cancelled));
            if (!error.empty())
                JS_SetPropertyStr(c, info, "error", JS_NewString(c, error.c_str()));
            // Where each long-form window began: [{ start, at }], `at` indexing
            // the id array beside it. A caller that did not stream still needs
            // this to read the timestamps in `arr` as absolute times, because
            // every window restarts them at zero. Absent for a short-form decode.
            if (!job->windows.empty()) {
                JSValue ws = JS_NewArray(c);
                uint32_t i = 0;
                for (const auto& w : job->windows) {
                    JSValue o = JS_NewObject(c);
                    JS_SetPropertyStr(c, o, "start", JS_NewFloat64(c, w.first));
                    JS_SetPropertyStr(c, o, "at",
                                      JS_NewInt64(c, static_cast<int64_t>(w.second)));
                    JS_SetPropertyUint32(c, ws, i++, o);
                }
                JS_SetPropertyStr(c, info, "windows", ws);
            }
            JSValue args[2] = { arr, info };
            JSValue r = JS_Call(c, job->onDone, JS_UNDEFINED, 2, args);
            if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
            JS_FreeValue(c, r);
            JS_FreeValue(c, arr);
            JS_FreeValue(c, info);
        }
        if (job->hasOnDone)   JS_FreeValue(c, job->onDone);
        if (job->hasOnToken)  JS_FreeValue(c, job->onToken);
        if (job->hasOnWindow) JS_FreeValue(c, job->onWindow);
        JS_FreeValue(c, job->whisperRef);
    };

    return launchAsyncJob(ctx, std::move(work), std::move(poll), std::move(done));
}

// ═══════════════════════════════════════════════════════════════════════════
// Multi-stream sessions — model.createSession() + session.transcribe()/reset()
// ═══════════════════════════════════════════════════════════════════════════
//
// One loaded model behind N per-stream sessions (own KV-cache / prediction
// state) over ONE shared weight set — the STT analog of N wake detectors on one
// shared net. session.transcribe(...) reuses the exact async-job dispatch as the
// module-level bro.stt.transcribe(model, ...) (real cancellation, SPSC onToken
// streaming, onDone(result, info)) but drives the session's own state and claims
// the model's SHARED busy gate. brosoundml's tier for these models is SHARED
// WEIGHTS / SERIALIZED decode (one GPU stream, shared captured step-graph), so
// calls over one model — module-level and any session — never overlap: a second
// in-flight op throws. Sessions isolate STATE, not parallel execution; drive
// them from one worker / queue. Each session's transcript is bit-identical to
// the same call on a fresh model.

// ── Whisper session ──
static WhisperSessionWrapper* whisperSessionSelf(JSContext* ctx, JSValueConst v) {
    return qjsbind::unwrap<WhisperSessionWrapper>(ctx, v);
}

// whisper.createSession() -> WhisperSession
static JSValue js_whisper_createSession(JSContext* ctx, JSValueConst this_val,
                                        int, JSValueConst*) {
    auto* w = whisperSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "createSession: not a Whisper");
    if (!w->whisper || !w->whisper->loaded())
        return JS_ThrowInternalError(ctx, "createSession: model is not loaded");
    try {
        brotensor::DeviceScope scope(w->device);
        auto sw = std::make_unique<WhisperSessionWrapper>();
        sw->model   = w->whisper;   // share weights (outlives the model handle)
        sw->busy    = w->busy;      // share the single-owner gate
        sw->device  = w->device;
        sw->session = w->whisper->make_session();
        return qjsbind::wrap<WhisperSessionWrapper>(ctx, sw.release());
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "createSession: %s", e.what());
    }
}

// session.transcribe(audio, promptIds, opts?) -> AsyncHandle
static JSValue js_whisper_session_transcribe(JSContext* ctx, JSValueConst this_val,
                                             int argc, JSValueConst* argv) {
    auto* sw = whisperSessionSelf(ctx, this_val);
    if (!sw) return JS_ThrowTypeError(ctx, "transcribe: not a WhisperSession");
    if (argc < 2)
        return JS_ThrowTypeError(ctx,
            "transcribe(audio, promptIds, opts?): audio and promptIds required");

    auto job = std::make_shared<SttJob>();
    std::string err;
    if (!readAudioBuffer(ctx, argv[0], job->audio, err))
        return JS_ThrowTypeError(ctx, "transcribe: %s", err.c_str());

    job->prompt = qjsbind::read_int32_array(ctx, argv[1]);
    if (job->prompt.empty())
        return JS_ThrowTypeError(ctx,
            "transcribe: promptIds must be a non-empty Int32Array or number[] "
            "(use tokenizer.buildPrompt(lang, task))");

    JSValue onDone = JS_UNDEFINED, onToken = JS_UNDEFINED;
    if (argc >= 3 && JS_IsObject(argv[2])) {
        getInt(ctx, argv[2], "maxNewTokens", job->maxNew);
        getInt(ctx, argv[2], "timestampBeginId", job->timestampBeginId);
        getInt(ctx, argv[2], "noTimestampsId", job->noTimestampsId);
        onDone  = JS_GetPropertyStr(ctx, argv[2], "onDone");
        onToken = JS_GetPropertyStr(ctx, argv[2], "onToken");
    }

    if (!sw->busy.tryClaim()) {
        JS_FreeValue(ctx, onDone);
        JS_FreeValue(ctx, onToken);
        return JS_ThrowInternalError(ctx,
            "transcribe: an operation is already in flight on this model "
            "(sessions over one model serialize — drive them from one queue)");
    }

    job->hasOnDone  = JS_IsFunction(ctx, onDone);
    job->onDone     = job->hasOnDone ? JS_DupValue(ctx, onDone) : JS_UNDEFINED;
    job->hasOnToken = JS_IsFunction(ctx, onToken);
    job->onToken    = job->hasOnToken ? JS_DupValue(ctx, onToken) : JS_UNDEFINED;
    job->whisperRef = JS_DupValue(ctx, this_val);  // keep the session (+ model) alive
    JS_FreeValue(ctx, onDone);
    JS_FreeValue(ctx, onToken);
    if (job->hasOnToken) job->tokenSlots.resize(1u << 16);

    WhisperSessionWrapper* mw = sw;

    auto work = [job, mw](const std::atomic<bool>& cancel) {
        brotensor::DeviceScope scope(mw->device);
        brosoundml::Whisper::TranscribeOptions opts;
        opts.max_new_tokens     = job->maxNew;
        opts.timestamp_begin_id = job->timestampBeginId;
        opts.no_timestamps_id   = job->noTimestampsId;
        opts.cancel = [&cancel] {
            return cancel.load(std::memory_order_acquire) || bro::util::interrupted();
        };
        if (job->hasOnToken) {
            opts.on_token = [job](int32_t id) {
                const size_t idx = job->produced.load(std::memory_order_relaxed);
                if (idx >= job->tokenSlots.size()) return;
                job->tokenSlots[idx] = id;
                job->produced.store(idx + 1, std::memory_order_release);
            };
        }
        auto out = mw->model->transcribe(mw->session, job->audio, job->prompt, opts);
        job->token_ids = std::move(out.token_ids);
    };

    auto poll = [job](JSContext* c) {
        if (!job->hasOnToken) return;
        const size_t n = job->produced.load(std::memory_order_acquire);
        while (job->drained < n) {
            JSValue a = JS_NewInt32(c, job->tokenSlots[job->drained]);
            JSValue r = JS_Call(c, job->onToken, JS_UNDEFINED, 1, &a);
            if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
            JS_FreeValue(c, r);
            JS_FreeValue(c, a);
            job->drained++;
        }
    };

    auto done = [job, mw](JSContext* c, bool cancelled, const std::string& error) {
        // Release the single-owner lock BEFORE invoking onDone so a callback that
        // synchronously starts the next transcription on this model (a serialized
        // queue) succeeds instead of tripping the in-flight guard. The work thread
        // has finished and joined; the result's host data is already on `job`, so
        // a new op claiming the model can't disturb what onDone reads here.
        mw->busy.release();
        if (job->hasOnDone) {
            JSValue arr  = qjsbind::make_int32_array(c, job->token_ids);
            JSValue info = JS_NewObject(c);
            JS_SetPropertyStr(c, info, "cancelled", JS_NewBool(c, cancelled));
            if (!error.empty())
                JS_SetPropertyStr(c, info, "error", JS_NewString(c, error.c_str()));
            // Where each long-form window began: [{ start, at }], `at` indexing
            // the id array beside it. A caller that did not stream still needs
            // this to read the timestamps in `arr` as absolute times, because
            // every window restarts them at zero. Absent for a short-form decode.
            if (!job->windows.empty()) {
                JSValue ws = JS_NewArray(c);
                uint32_t i = 0;
                for (const auto& w : job->windows) {
                    JSValue o = JS_NewObject(c);
                    JS_SetPropertyStr(c, o, "start", JS_NewFloat64(c, w.first));
                    JS_SetPropertyStr(c, o, "at",
                                      JS_NewInt64(c, static_cast<int64_t>(w.second)));
                    JS_SetPropertyUint32(c, ws, i++, o);
                }
                JS_SetPropertyStr(c, info, "windows", ws);
            }
            JSValue args[2] = { arr, info };
            JSValue r = JS_Call(c, job->onDone, JS_UNDEFINED, 2, args);
            if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
            JS_FreeValue(c, r);
            JS_FreeValue(c, arr);
            JS_FreeValue(c, info);
        }
        if (job->hasOnDone)   JS_FreeValue(c, job->onDone);
        if (job->hasOnToken)  JS_FreeValue(c, job->onToken);
        if (job->hasOnWindow) JS_FreeValue(c, job->onWindow);
        JS_FreeValue(c, job->whisperRef);
    };

    return launchAsyncJob(ctx, std::move(work), std::move(poll), std::move(done));
}

// session.reset() — clear the session's KV-cache for a fresh, unrelated clip.
static JSValue js_whisper_session_reset(JSContext* ctx, JSValueConst this_val,
                                        int, JSValueConst*) {
    auto* sw = whisperSessionSelf(ctx, this_val);
    if (!sw) return JS_ThrowTypeError(ctx, "reset: not a WhisperSession");
    if (sw->busy.isBusy())
        return JS_ThrowInternalError(ctx, "reset: a transcribe is in flight on this model");
    try {
        brotensor::DeviceScope scope(sw->device);
        sw->model->reset(sw->session);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "reset: %s", e.what());
    }
    return JS_UNDEFINED;
}

static void registerWhisperSessionClass(JSContext* ctx) {
    qjsbind::Class<WhisperSessionWrapper>(ctx, "WhisperSession", qjsbind::NoGlobal)
        .get("loaded", [](WhisperSessionWrapper* w) { return w->model && w->model->loaded(); })
        .method_raw("transcribe", js_whisper_session_transcribe, 3)
        .method_raw("reset",      js_whisper_session_reset,      0);
}

// ── Parakeet session ──
static ParakeetSessionWrapper* parakeetSessionSelf(JSContext* ctx, JSValueConst v) {
    return qjsbind::unwrap<ParakeetSessionWrapper>(ctx, v);
}

static JSValue js_parakeet_createSession(JSContext* ctx, JSValueConst this_val,
                                         int, JSValueConst*) {
    auto* w = parakeetSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "createSession: not a Parakeet");
    if (!w->parakeet || !w->parakeet->loaded())
        return JS_ThrowInternalError(ctx, "createSession: model is not loaded");
    try {
        brotensor::DeviceScope scope(w->device);
        auto sw = std::make_unique<ParakeetSessionWrapper>();
        sw->model   = w->parakeet;
        sw->busy    = w->busy;
        sw->device  = w->device;
        sw->session = w->parakeet->make_session();
        return qjsbind::wrap<ParakeetSessionWrapper>(ctx, sw.release());
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "createSession: %s", e.what());
    }
}

// session.transcribe(audio, opts?) -> AsyncHandle ({ tokenIds, tokenFrames })
static JSValue js_parakeet_session_transcribe(JSContext* ctx, JSValueConst this_val,
                                              int argc, JSValueConst* argv) {
    auto* sw = parakeetSessionSelf(ctx, this_val);
    if (!sw) return JS_ThrowTypeError(ctx, "transcribe: not a ParakeetSession");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "transcribe(audio, opts?): audio required");

    auto job = std::make_shared<ParakeetJob>();
    std::string err;
    if (!readAudioBuffer(ctx, argv[0], job->audio, err))
        return JS_ThrowTypeError(ctx, "transcribe: %s", err.c_str());

    JSValue onDone = JS_UNDEFINED, onToken = JS_UNDEFINED;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        getInt(ctx, argv[1], "maxNewTokens", job->maxNew);
        onDone  = JS_GetPropertyStr(ctx, argv[1], "onDone");
        onToken = JS_GetPropertyStr(ctx, argv[1], "onToken");
    }

    if (!sw->busy.tryClaim()) {
        JS_FreeValue(ctx, onDone);
        JS_FreeValue(ctx, onToken);
        return JS_ThrowInternalError(ctx,
            "transcribe: an operation is already in flight on this model "
            "(sessions over one model serialize — drive them from one queue)");
    }

    job->hasOnDone   = JS_IsFunction(ctx, onDone);
    job->onDone      = job->hasOnDone ? JS_DupValue(ctx, onDone) : JS_UNDEFINED;
    job->hasOnToken  = JS_IsFunction(ctx, onToken);
    job->onToken     = job->hasOnToken ? JS_DupValue(ctx, onToken) : JS_UNDEFINED;
    job->parakeetRef = JS_DupValue(ctx, this_val);  // keep the session (+ model) alive
    JS_FreeValue(ctx, onDone);
    JS_FreeValue(ctx, onToken);
    if (job->hasOnToken) job->tokenSlots.resize(1u << 16);

    ParakeetSessionWrapper* mw = sw;

    auto work = [job, mw](const std::atomic<bool>& cancel) {
        brotensor::DeviceScope scope(mw->device);
        brosoundml::Parakeet::TranscribeOptions opts;
        opts.max_new_tokens = job->maxNew;
        opts.cancel = [&cancel] {
            return cancel.load(std::memory_order_acquire) || bro::util::interrupted();
        };
        if (job->hasOnToken) {
            opts.on_token = [job](int32_t id) {
                const size_t idx = job->produced.load(std::memory_order_relaxed);
                if (idx >= job->tokenSlots.size()) return;
                job->tokenSlots[idx] = id;
                job->produced.store(idx + 1, std::memory_order_release);
            };
        }
        auto out = mw->model->transcribe(mw->session, job->audio, opts);
        job->token_ids    = std::move(out.token_ids);
        job->token_frames = std::move(out.token_frames);
    };

    auto poll = [job](JSContext* c) {
        if (!job->hasOnToken) return;
        const size_t n = job->produced.load(std::memory_order_acquire);
        while (job->drained < n) {
            JSValue a = JS_NewInt32(c, job->tokenSlots[job->drained]);
            JSValue r = JS_Call(c, job->onToken, JS_UNDEFINED, 1, &a);
            if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
            JS_FreeValue(c, r);
            JS_FreeValue(c, a);
            job->drained++;
        }
    };

    auto done = [job, mw](JSContext* c, bool cancelled, const std::string& error) {
        // Release the single-owner lock BEFORE invoking onDone so a callback that
        // synchronously starts the next transcription on this model (a serialized
        // queue) succeeds instead of tripping the in-flight guard. The work thread
        // has finished and joined; the result's host data is already on `job`, so
        // a new op claiming the model can't disturb what onDone reads here.
        mw->busy.release();
        if (job->hasOnDone) {
            JSValue res  = makeParakeetResult(c, job->token_ids, job->token_frames);
            JSValue info = JS_NewObject(c);
            JS_SetPropertyStr(c, info, "cancelled", JS_NewBool(c, cancelled));
            if (!error.empty())
                JS_SetPropertyStr(c, info, "error", JS_NewString(c, error.c_str()));
            JSValue args[2] = { res, info };
            JSValue r = JS_Call(c, job->onDone, JS_UNDEFINED, 2, args);
            if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
            JS_FreeValue(c, r);
            JS_FreeValue(c, res);
            JS_FreeValue(c, info);
        }
        if (job->hasOnDone)  JS_FreeValue(c, job->onDone);
        if (job->hasOnToken) JS_FreeValue(c, job->onToken);
        JS_FreeValue(c, job->parakeetRef);
    };

    return launchAsyncJob(ctx, std::move(work), std::move(poll), std::move(done));
}

static JSValue js_parakeet_session_reset(JSContext* ctx, JSValueConst this_val,
                                         int, JSValueConst*) {
    auto* sw = parakeetSessionSelf(ctx, this_val);
    if (!sw) return JS_ThrowTypeError(ctx, "reset: not a ParakeetSession");
    if (sw->busy.isBusy())
        return JS_ThrowInternalError(ctx, "reset: a transcribe is in flight on this model");
    try {
        brotensor::DeviceScope scope(sw->device);
        sw->model->reset(sw->session);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "reset: %s", e.what());
    }
    return JS_UNDEFINED;
}

static void registerParakeetSessionClass(JSContext* ctx) {
    qjsbind::Class<ParakeetSessionWrapper>(ctx, "ParakeetSession", qjsbind::NoGlobal)
        .get("loaded", [](ParakeetSessionWrapper* w) { return w->model && w->model->loaded(); })
        .method_raw("transcribe", js_parakeet_session_transcribe, 2)
        .method_raw("reset",      js_parakeet_session_reset,      0);
}

// ── Qwen3-ASR session ──
static QwenAsrSessionWrapper* qwenAsrSessionSelf(JSContext* ctx, JSValueConst v) {
    return qjsbind::unwrap<QwenAsrSessionWrapper>(ctx, v);
}

static JSValue js_qwenasr_createSession(JSContext* ctx, JSValueConst this_val,
                                        int, JSValueConst*) {
    auto* w = qwenAsrSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "createSession: not a QwenAsr");
    if (!w->asr || !w->asr->loaded())
        return JS_ThrowInternalError(ctx, "createSession: model is not loaded");
    try {
        brotensor::DeviceScope scope(w->device);
        auto sw = std::make_unique<QwenAsrSessionWrapper>();
        sw->model   = w->asr;
        sw->busy    = w->busy;
        sw->device  = w->device;
        sw->session = w->asr->make_session();
        return qjsbind::wrap<QwenAsrSessionWrapper>(ctx, sw.release());
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "createSession: %s", e.what());
    }
}

// session.transcribe(audio, opts?) -> AsyncHandle (Int32Array of generated ids)
static JSValue js_qwenasr_session_transcribe(JSContext* ctx, JSValueConst this_val,
                                             int argc, JSValueConst* argv) {
    auto* sw = qwenAsrSessionSelf(ctx, this_val);
    if (!sw) return JS_ThrowTypeError(ctx, "transcribe: not a QwenAsrSession");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "transcribe(audio, opts?): audio required");

    auto job = std::make_shared<QwenAsrJob>();
    std::string err;
    if (!readAudioBuffer(ctx, argv[0], job->audio, err))
        return JS_ThrowTypeError(ctx, "transcribe: %s", err.c_str());

    JSValue onDone = JS_UNDEFINED, onToken = JS_UNDEFINED;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        getInt(ctx, argv[1], "maxNewTokens", job->maxNew);
        JSValue cv = JS_GetPropertyStr(ctx, argv[1], "contextIds");
        if (!JS_IsUndefined(cv) && !JS_IsNull(cv))
            job->contextIds = qjsbind::read_int32_array(ctx, cv);
        JS_FreeValue(ctx, cv);
        onDone  = JS_GetPropertyStr(ctx, argv[1], "onDone");
        onToken = JS_GetPropertyStr(ctx, argv[1], "onToken");
    }

    if (!sw->busy.tryClaim()) {
        JS_FreeValue(ctx, onDone);
        JS_FreeValue(ctx, onToken);
        return JS_ThrowInternalError(ctx,
            "transcribe: an operation is already in flight on this model "
            "(sessions over one model serialize — drive them from one queue)");
    }

    job->hasOnDone  = JS_IsFunction(ctx, onDone);
    job->onDone     = job->hasOnDone ? JS_DupValue(ctx, onDone) : JS_UNDEFINED;
    job->hasOnToken = JS_IsFunction(ctx, onToken);
    job->onToken    = job->hasOnToken ? JS_DupValue(ctx, onToken) : JS_UNDEFINED;
    job->asrRef     = JS_DupValue(ctx, this_val);  // keep the session (+ model) alive
    JS_FreeValue(ctx, onDone);
    JS_FreeValue(ctx, onToken);
    if (job->hasOnToken) job->tokenSlots.resize(1u << 16);

    QwenAsrSessionWrapper* mw = sw;

    auto work = [job, mw](const std::atomic<bool>& cancel) {
        brotensor::DeviceScope scope(mw->device);
        brosoundml::QwenAsr::TranscribeOptions opts;
        opts.max_new_tokens = job->maxNew;
        opts.context_ids    = job->contextIds;
        opts.cancel = [&cancel] {
            return cancel.load(std::memory_order_acquire) || bro::util::interrupted();
        };
        if (job->hasOnToken) {
            opts.on_token = [job](int32_t id) {
                const size_t idx = job->produced.load(std::memory_order_relaxed);
                if (idx >= job->tokenSlots.size()) return;
                job->tokenSlots[idx] = id;
                job->produced.store(idx + 1, std::memory_order_release);
            };
        }
        auto out = mw->model->transcribe(mw->session, job->audio, opts);
        job->token_ids = std::move(out.token_ids);
    };

    auto poll = [job](JSContext* c) {
        if (!job->hasOnToken) return;
        const size_t n = job->produced.load(std::memory_order_acquire);
        while (job->drained < n) {
            JSValue a = JS_NewInt32(c, job->tokenSlots[job->drained]);
            JSValue r = JS_Call(c, job->onToken, JS_UNDEFINED, 1, &a);
            if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
            JS_FreeValue(c, r);
            JS_FreeValue(c, a);
            job->drained++;
        }
    };

    auto done = [job, mw](JSContext* c, bool cancelled, const std::string& error) {
        // Release the single-owner lock BEFORE invoking onDone so a callback that
        // synchronously starts the next transcription on this model (the natural
        // serialized-queue pattern, e.g. listen-lab's rolling transcript) succeeds
        // instead of tripping the in-flight guard. The work thread has finished
        // and joined; the result's host data is already on `job`, so a new op
        // claiming the model can't disturb what onDone reads here. (Matches the
        // TTS bindings, which release before onDone for the same reason.)
        mw->busy.release();
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
        if (job->hasOnDone)  JS_FreeValue(c, job->onDone);
        if (job->hasOnToken) JS_FreeValue(c, job->onToken);
        JS_FreeValue(c, job->asrRef);
    };

    return launchAsyncJob(ctx, std::move(work), std::move(poll), std::move(done));
}

static JSValue js_qwenasr_session_reset(JSContext* ctx, JSValueConst this_val,
                                        int, JSValueConst*) {
    auto* sw = qwenAsrSessionSelf(ctx, this_val);
    if (!sw) return JS_ThrowTypeError(ctx, "reset: not a QwenAsrSession");
    if (sw->busy.isBusy())
        return JS_ThrowInternalError(ctx, "reset: a transcribe is in flight on this model");
    try {
        brotensor::DeviceScope scope(sw->device);
        sw->model->reset(sw->session);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "reset: %s", e.what());
    }
    return JS_UNDEFINED;
}

static void registerQwenAsrSessionClass(JSContext* ctx) {
    qjsbind::Class<QwenAsrSessionWrapper>(ctx, "QwenAsrSession", qjsbind::NoGlobal)
        .get("loaded", [](QwenAsrSessionWrapper* w) { return w->model && w->model->loaded(); })
        .method_raw("transcribe", js_qwenasr_session_transcribe, 2)
        .method_raw("reset",      js_qwenasr_session_reset,      0);
}

// ═══════════════════════════════════════════════════════════════════════════
// Install
// ═══════════════════════════════════════════════════════════════════════════

void installSttBindings(JSContext* ctx) {
    registerTokenizerClass(ctx);
    registerWhisperClass(ctx);
    registerParakeetTokenizerClass(ctx);
    registerParakeetClass(ctx);
    registerQwenAsrClass(ctx);
    registerQwenAsrStreamClass(ctx);
    registerWhisperSessionClass(ctx);
    registerParakeetSessionClass(ctx);
    registerQwenAsrSessionClass(ctx);

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
    JS_SetPropertyStr(ctx, stt, "loadParakeet",
        JS_NewCFunction(ctx, js_loadParakeet, "loadParakeet", 2));
    JS_SetPropertyStr(ctx, stt, "loadParakeetTokenizer",
        JS_NewCFunction(ctx, js_loadParakeetTokenizer, "loadParakeetTokenizer", 2));
    JS_SetPropertyStr(ctx, stt, "loadQwenAsr",
        JS_NewCFunction(ctx, js_loadQwenAsr, "loadQwenAsr", 2));
    JS_SetPropertyStr(ctx, stt, "loadQwenAsrStream",
        JS_NewCFunction(ctx, js_loadQwenAsrStream, "loadQwenAsrStream", 2));
    JS_SetPropertyStr(ctx, stt, "transcribe",
        JS_NewCFunction(ctx, js_stt_transcribe, "transcribe", 4));
    JS_SetPropertyStr(ctx, broObj, "stt", stt);

    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

void cleanupSttBindings(JSContext* /*ctx*/) {}

}  // namespace bro::js

#endif  // BRO_WITH_SOUNDML
