// JS bindings for brosoundml::Kokoro — text-to-speech inference.
//
// Installed onto bro.tts.* by installTtsBindings(). The Kokoro pipeline
// (plBERT + text encoder + prosody predictor + iSTFTNet decoder, ~82M params)
// and its voice packs live behind opaque qjsbind handles.
//
// Kokoro consumes phoneme token ids — G2P (text -> phonemes) is the caller's
// responsibility; the upstream Kokoro distribution uses misaki, which we do
// not bundle. The token-id <-> phoneme mapping is exposed via the model's
// vocab (Kokoro::config().vocab) for callers that build a small JS-side G2P.
// Output is mono 24 kHz FP32 in a Float32Array.

#include "js/tts_bindings.h"
#include "js/async_job.h"

#include <qjsbind/qjsbind.h>

#include <brosoundml/kokoro.h>
#include <brosoundml/qwen_tts.h>
#include <brosoundml/audio.h>

#include <brosoundml/g2p/lexicon.h>
#include <brosoundml/g2p/morphology.h>
#include <brosoundml/g2p/special_cases.h>
#include <brosoundml/g2p/pos_tagger.h>
#include <brosoundml/g2p/phoneme_adapter.h>
#include <brosoundml/g2p/phonemizer.h>
#include <brosoundml/detail/json.h>

#include <brotensor/runtime.h>
#include <brotensor/tensor.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bro::js {

// ═══════════════════════════════════════════════════════════════════════════
// Wrapper structs
// ═══════════════════════════════════════════════════════════════════════════

struct KokoroWrapper {
    std::unique_ptr<brosoundml::Kokoro> kokoro;
    // Lazily constructed on first encodePhonemes() call. Borrows the
    // Kokoro instance's vocab map (lifetime tied to `kokoro` above).
    std::unique_ptr<brosoundml::g2p::PhonemeAdapter> adapter;
    brotensor::Device device = brotensor::Device::CPU;  // captured at load
    // Set while an async bro.tts.synthesize() runs on this model's background
    // thread; rejects a second concurrent op (the model is single-owner).
    // Cleared on the JS thread when the job's done() fires.
    std::atomic<bool> busy{false};
};

struct VoiceWrapper {
    brosoundml::Voice voice;
};

// Qwen3-TTS — text-driven (no phoneme frontend, no voice pack). Holds the
// pipeline (Talker + Code Predictor + bundled codec) and the device it loaded
// on. Like Kokoro it is single-owner: `busy` rejects a second concurrent op.
struct QwenTtsWrapper {
    std::unique_ptr<brosoundml::QwenTts> qwen;
    brotensor::Device device = brotensor::Device::CPU;  // captured at load
    std::atomic<bool> busy{false};
};

static const char* qwenVariantName(brosoundml::QwenTtsVariant v) {
    switch (v) {
        case brosoundml::QwenTtsVariant::Base:        return "base";
        case brosoundml::QwenTtsVariant::CustomVoice: return "customvoice";
        case brosoundml::QwenTtsVariant::VoiceDesign: return "voicedesign";
    }
    return "?";
}

// ═══════════════════════════════════════════════════════════════════════════
// Helpers
// ═══════════════════════════════════════════════════════════════════════════

static bool argStr(JSContext* ctx, JSValueConst v, std::string& out) {
    if (!JS_IsString(v)) return false;
    const char* s = JS_ToCString(ctx, v);
    if (!s) return false;
    out = s;
    JS_FreeCString(ctx, s);
    return true;
}

static void getNum(JSContext* ctx, JSValueConst obj, const char* key,
                   float& dst) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsNumber(v)) { double t = dst; JS_ToFloat64(ctx, &t, v); dst = (float)t; }
    JS_FreeValue(ctx, v);
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

// Wrap an AudioBuffer as a JS result: { samples: Float32Array, sampleRate }.
// Drop-in shape for a Web Audio AudioBuffer-like consumer or a brokit
// WAV writer.
static JSValue audioBufferToJs(JSContext* ctx,
                               const brosoundml::AudioBuffer& buf) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "samples",
        qjsbind::make_float32_array(ctx, buf.samples));
    JS_SetPropertyStr(ctx, obj, "sampleRate", JS_NewInt32(ctx, buf.sample_rate));
    return obj;
}

// ═══════════════════════════════════════════════════════════════════════════
// Voice class
// ═══════════════════════════════════════════════════════════════════════════

static void registerVoiceClass(JSContext* ctx) {
    qjsbind::Class<VoiceWrapper>(ctx, "Voice", qjsbind::NoGlobal)
        .get("name", [](VoiceWrapper* w) { return w->voice.name; })
        .get("rows", [](VoiceWrapper* w) { return w->voice.packs.rows; })
        .get("cols", [](VoiceWrapper* w) { return w->voice.packs.cols; });
}

// ═══════════════════════════════════════════════════════════════════════════
// Kokoro methods
// ═══════════════════════════════════════════════════════════════════════════

static KokoroWrapper* kokoroSelf(JSContext* ctx, JSValueConst this_val) {
    return qjsbind::unwrap<KokoroWrapper>(ctx, this_val);
}

// State for an async kokoro.loadVoice: the work thread fills vw (or error); the
// JS-thread done() wraps it and invokes onReady/onError. Holds a dup of the
// Kokoro JS object so the model (whose load_voice() does the parse) stays alive.
struct VoiceLoadState {
    KokoroWrapper*                 kw = nullptr;
    std::string                    path;
    std::unique_ptr<VoiceWrapper>  vw;
    JSValue kokoroRef = JS_UNDEFINED;
    JSValue onReady   = JS_UNDEFINED;
    JSValue onError   = JS_UNDEFINED;
    bool    hasReady = false, hasError = false;
};

// loadVoice(path, opts?) -> Voice          (sync)
//                        -> AsyncHandle     (async, if opts.onReady)
//   Loads a raw little-endian FP32 voice pack (rows * voice_dim floats).
//   Returns a Voice handle. PyTorch .pt voice packs must be pre-converted
//   to this raw format by the caller (brosoundml deliberately doesn't
//   pull in a pickle reader).
//   opts.onReady(voice) / opts.onError(message): when onReady is a function the
//   load runs on a background thread and these fire on the JS thread.
static JSValue js_kokoro_loadVoice(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    auto* w = kokoroSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "loadVoice: not a Kokoro");
    std::string path;
    if (argc < 1 || !argStr(ctx, argv[0], path))
        return JS_ThrowTypeError(ctx, "loadVoice(path, opts?): path string required");

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
            auto vw = std::make_unique<VoiceWrapper>();
            vw->voice = w->kokoro->load_voice(path);
            return qjsbind::wrap<VoiceWrapper>(ctx, vw.release());
        } catch (const std::exception& e) {
            return JS_ThrowInternalError(ctx, "loadVoice: %s", e.what());
        }
    }

    // ── Async path ──
    auto ls = std::make_shared<VoiceLoadState>();
    ls->kw        = w;
    ls->path      = path;
    ls->kokoroRef = JS_DupValue(ctx, this_val);  // keep the model alive
    ls->hasReady  = true;
    ls->onReady   = JS_DupValue(ctx, onReady);
    ls->hasError  = JS_IsFunction(ctx, onError);
    ls->onError   = ls->hasError ? JS_DupValue(ctx, onError) : JS_UNDEFINED;
    JS_FreeValue(ctx, onReady);
    JS_FreeValue(ctx, onError);

    auto work = [ls](const std::atomic<bool>&) {
        brotensor::DeviceScope scope(ls->kw->device);
        auto vw = std::make_unique<VoiceWrapper>();
        vw->voice = ls->kw->kokoro->load_voice(ls->path);  // throws -> error
        ls->vw = std::move(vw);
    };
    auto done = [ls](JSContext* c, bool /*cancelled*/, const std::string& error) {
        if (!error.empty() || !ls->vw) {
            if (ls->hasError) {
                JSValue e = JS_NewString(c, error.empty() ? "loadVoice failed"
                                                          : error.c_str());
                JSValue r = JS_Call(c, ls->onError, JS_UNDEFINED, 1, &e);
                if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
                JS_FreeValue(c, r);
                JS_FreeValue(c, e);
            }
        } else {
            JSValue out = qjsbind::wrap<VoiceWrapper>(c, ls->vw.release());
            JSValue r = JS_Call(c, ls->onReady, JS_UNDEFINED, 1, &out);
            if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
            JS_FreeValue(c, r);
            JS_FreeValue(c, out);
        }
        if (ls->hasReady) JS_FreeValue(c, ls->onReady);
        if (ls->hasError) JS_FreeValue(c, ls->onError);
        JS_FreeValue(c, ls->kokoroRef);
    };
    return launchAsyncJob(ctx, std::move(work), nullptr, std::move(done));
}

// synthesize(phonemeIds, voice, opts?) -> { samples, sampleRate, durations }
//   opts.speed: duration multiplier (>1 faster, <1 slower; default 1.0).
//   durations: Int32Array of per-phoneme frame counts (length = phonemeIds + 2
//   for Kokoro's BOS/EOS wrap). Per-phoneme sample offset =
//   frameOffset * (samples.length / sum(durations)); lets callers map phonemes
//   (and, via the inter-word separator token, words) to playback time.
static JSValue js_kokoro_synthesize(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    auto* w = kokoroSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "synthesize: not a Kokoro");
    if (argc < 2)
        return JS_ThrowTypeError(ctx,
            "synthesize(phonemeIds, voice, opts?): phonemeIds and voice required");

    std::vector<int32_t> ids = readIdArray(ctx, argv[0]);
    if (ids.empty())
        return JS_ThrowTypeError(ctx,
            "synthesize: phonemeIds must be a non-empty Int32Array or number[]");

    auto* vw = qjsbind::unwrap<VoiceWrapper>(ctx, argv[1]);
    if (!vw)
        return JS_ThrowTypeError(ctx,
            "synthesize: voice must be a Voice (returned by loadVoice)");

    float speed = 1.0f;
    if (argc >= 3 && JS_IsObject(argv[2]))
        getNum(ctx, argv[2], "speed", speed);

    try {
        std::vector<int32_t> pred_dur;
        auto buf = w->kokoro->synthesize(ids, vw->voice, speed, &pred_dur);
        JSValue out = audioBufferToJs(ctx, buf);
        JS_SetPropertyStr(ctx, out, "durations",
            qjsbind::make_int32_array(ctx, pred_dur));
        return out;
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "synthesize: %s", e.what());
    }
}

// vocab() -> { phoneme: id, ... }
//   Returns the phoneme->id map from KokoroConfig. Empty when the model
//   directory's config.json omits it.
static JSValue js_kokoro_vocab(JSContext* ctx, JSValueConst this_val,
                               int, JSValueConst*) {
    auto* w = kokoroSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "vocab: not a Kokoro");
    JSValue obj = JS_NewObject(ctx);
    for (const auto& kv : w->kokoro->config().vocab)
        JS_SetPropertyStr(ctx, obj, kv.first.c_str(), JS_NewInt32(ctx, kv.second));
    return obj;
}

// encodePhonemes(ipa) -> Int32Array
//   Runs only the codepoint→id stage via brosoundml::g2p::PhonemeAdapter
//   against this Kokoro's vocab. Adapter is lazily built on first call and
//   cached on the wrapper.
static JSValue js_kokoro_encodePhonemes(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv) {
    auto* w = kokoroSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "encodePhonemes: not a Kokoro");
    std::string ipa;
    if (argc < 1 || !argStr(ctx, argv[0], ipa))
        return JS_ThrowTypeError(ctx, "encodePhonemes(ipa): string required");
    try {
        if (!w->adapter) {
            w->adapter = std::make_unique<brosoundml::g2p::PhonemeAdapter>(
                w->kokoro->config().vocab);
        }
        auto ids = w->adapter->encode(ipa);
        return qjsbind::make_int32_array(ctx, ids);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "encodePhonemes: %s", e.what());
    }
}

static void registerKokoroClass(JSContext* ctx) {
    qjsbind::Class<KokoroWrapper>(ctx, "Kokoro", qjsbind::NoGlobal)
        .get("loaded",       [](KokoroWrapper* w) { return w->kokoro->loaded(); })
        .get("sampleRate",   [](KokoroWrapper* w) { return w->kokoro->config().sample_rate; })
        .get("nTokens",      [](KokoroWrapper* w) { return w->kokoro->config().n_tokens; })
        .get("hiddenDim",    [](KokoroWrapper* w) { return w->kokoro->config().hidden_dim; })
        .get("styleDim",     [](KokoroWrapper* w) { return w->kokoro->config().style_dim; })
        .get("nLayer",       [](KokoroWrapper* w) { return w->kokoro->config().n_layer; })
        .method_raw("loadVoice",      js_kokoro_loadVoice,      2)
        .method_raw("synthesize",     js_kokoro_synthesize,     3)
        .method_raw("vocab",          js_kokoro_vocab,          0)
        .method_raw("encodePhonemes", js_kokoro_encodePhonemes, 1);
}

// ═══════════════════════════════════════════════════════════════════════════
// QwenTts methods
// ═══════════════════════════════════════════════════════════════════════════

static QwenTtsWrapper* qwenSelf(JSContext* ctx, JSValueConst this_val) {
    return qjsbind::unwrap<QwenTtsWrapper>(ctx, this_val);
}

// Read opts.speaker / opts.language / opts.instruct (all optional strings).
// Defaults match QwenTts::synthesize: speaker "" (the model resolves the first
// preset only if it has speakers), language "english", instruct "" (no voice
// instruction — the VoiceDesign description, or the 1.7B CustomVoice instruct).
static void readQwenSynthOpts(JSContext* ctx, JSValueConst opts,
                              std::string& speaker, std::string& language,
                              std::string& instruct) {
    if (!JS_IsObject(opts)) return;
    JSValue sp = JS_GetPropertyStr(ctx, opts, "speaker");
    std::string s;
    if (JS_IsString(sp) && argStr(ctx, sp, s)) speaker = std::move(s);
    JS_FreeValue(ctx, sp);
    JSValue lg = JS_GetPropertyStr(ctx, opts, "language");
    std::string l;
    if (JS_IsString(lg) && argStr(ctx, lg, l)) language = std::move(l);
    JS_FreeValue(ctx, lg);
    JSValue ins = JS_GetPropertyStr(ctx, opts, "instruct");
    std::string i;
    if (JS_IsString(ins) && argStr(ctx, ins, i)) instruct = std::move(i);
    JS_FreeValue(ctx, ins);
}

// qwen.synthesize(text, opts?) -> { samples, sampleRate }      (sync, blocking)
//   opts.speaker:  preset speaker name (CustomVoice; e.g. 'serena').
//   opts.language: 'english' (default), 'chinese', 'auto', ...
//   opts.instruct: natural-language voice description (VoiceDesign; ignored by
//                  the 0.6B CustomVoice checkpoint).
//   The async, cancellable form is bro.tts.synthesize(qwen, text, opts).
static JSValue js_qwen_synthesize(JSContext* ctx, JSValueConst this_val,
                                  int argc, JSValueConst* argv) {
    auto* w = qwenSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "synthesize: not a QwenTts");
    std::string text;
    if (argc < 1 || !argStr(ctx, argv[0], text))
        return JS_ThrowTypeError(ctx, "synthesize(text, opts?): text string required");
    std::string speaker, language = "english", instruct;
    if (argc >= 2) readQwenSynthOpts(ctx, argv[1], speaker, language, instruct);
    try {
        brotensor::DeviceScope scope(w->device);
        auto buf = w->qwen->synthesize(text, speaker, language, instruct);
        return audioBufferToJs(ctx, buf);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "synthesize: %s", e.what());
    }
}

// Marshal a std::vector<std::string> to a JS string[].
static JSValue stringVecToJs(JSContext* ctx, const std::vector<std::string>& v) {
    JSValue arr = JS_NewArray(ctx);
    for (std::uint32_t i = 0; i < v.size(); ++i)
        JS_SetPropertyUint32(ctx, arr, i, JS_NewString(ctx, v[i].c_str()));
    return arr;
}

// speakers() -> string[]
//   Preset CustomVoice speaker names (empty for Base / VoiceDesign).
static JSValue js_qwen_speakers(JSContext* ctx, JSValueConst this_val,
                                int, JSValueConst*) {
    auto* w = qwenSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "speakers: not a QwenTts");
    return stringVecToJs(ctx, w->qwen->speakers());
}

// languages() -> string[]
//   Selectable language names for synthesize() (dialects excluded; "auto" is
//   always valid but not listed). Same for every variant.
static JSValue js_qwen_languages(JSContext* ctx, JSValueConst this_val,
                                 int, JSValueConst*) {
    auto* w = qwenSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "languages: not a QwenTts");
    return stringVecToJs(ctx, w->qwen->languages());
}

// speakerDialect(name) -> string
//   The dialect tag of a preset speaker ("sichuan_dialect" / "beijing_dialect"),
//   or "" if the speaker is not a dialect voice / is unknown.
static JSValue js_qwen_speaker_dialect(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv) {
    auto* w = qwenSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "speakerDialect: not a QwenTts");
    std::string name;
    if (argc < 1 || !argStr(ctx, argv[0], name))
        return JS_ThrowTypeError(ctx, "speakerDialect(name): name string required");
    return JS_NewString(ctx, w->qwen->speaker_dialect(name).c_str());
}

static void registerQwenClass(JSContext* ctx) {
    qjsbind::Class<QwenTtsWrapper>(ctx, "QwenTts", qjsbind::NoGlobal)
        .get("loaded",     [](QwenTtsWrapper* w) { return w->qwen->loaded(); })
        .get("sampleRate", [](QwenTtsWrapper* w) { return w->qwen->config().sample_rate; })
        .get("variant",    [](QwenTtsWrapper* w) {
            return std::string(qwenVariantName(w->qwen->config().variant)); })
        .get("modelSize",  [](QwenTtsWrapper* w) { return w->qwen->config().model_size; })
        .method_raw("synthesize",     js_qwen_synthesize,      2)
        .method_raw("speakers",       js_qwen_speakers,        0)
        .method_raw("languages",      js_qwen_languages,       0)
        .method_raw("speakerDialect", js_qwen_speaker_dialect, 1);
}

// ═══════════════════════════════════════════════════════════════════════════
// bro.tts free functions
// ═══════════════════════════════════════════════════════════════════════════

static JSValue js_init(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    try {
        brotensor::init();
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "bro.tts.init: %s", e.what());
    }
    return JS_UNDEFINED;
}

// ═══════════════════════════════════════════════════════════════════════════
// G2P (text → phoneme ids) — module-scope lazy Phonemizer
// ═══════════════════════════════════════════════════════════════════════════
//
// The Phonemizer holds non-owning pointers to five dependencies. We keep them
// all alive in a single PhonemizerState that's lazy-built on first call to
// bro.tts.phonemize() and re-built when setAssetRoot()/setAssets() is called.
//
// Required runtime assets:
//   - the g2p lexicon          (sibling default: <data_root>/g2p/lexicon_en_us.bin)
//   - the POS-tagger weights    (sibling default: <data_root>/pos_tagger/model.bin)
//   - the Kokoro config.json    (sibling default: <repo_root>/weights/kokoro/config.json)
//                               (read only for the phoneme vocab)
//
// Path resolution, highest precedence first:
//   1. explicit per-asset paths set via bro.tts.setAssets({lexicon, posTagger,
//      kokoroConfig}) — for loading from a flat per-user cache
//   2. the sibling layout rooted at setAssetRoot(path) (data root derived as
//      <path>/../brosoundml-data)
//   3. a default search of well-known sibling paths relative to the cwd

struct PhonemizerState {
    // Vocab is owned here (extracted from kokoro config.json) and borrowed by
    // the adapter; all the others borrow from each other.
    std::unordered_map<std::string, int>          vocab;
    std::unique_ptr<brosoundml::g2p::Lexicon>        lexicon;
    std::unique_ptr<brosoundml::g2p::PosTagger>      tagger;
    std::unique_ptr<brosoundml::g2p::Morphology>     morphology;
    std::unique_ptr<brosoundml::g2p::SpecialCases>   special;
    std::unique_ptr<brosoundml::g2p::PhonemeAdapter> adapter;
    std::unique_ptr<brosoundml::g2p::Phonemizer>     phonemizer;
};

// Module-scope singletons. Reset when setAssetRoot()/setAssets() is called.
// g_assetRoot drives the default sibling-layout derivation; the explicit
// override paths below (when non-empty) take precedence so the phonemizer can
// load from a flat per-user cache that doesn't follow the dev sibling layout.
static std::string                              g_assetRoot;        // brosoundml repo root
static std::string                              g_lexiconPath;      // explicit g2p lexicon override
static std::string                              g_posPath;          // explicit POS-tagger override
static std::string                              g_kokoroConfigPath; // explicit Kokoro config.json override
static std::unique_ptr<PhonemizerState>         g_phonemizerState;

static bool fileExists(const std::string& p) {
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

// Resolve the brosoundml repo root. Order:
//   1. g_assetRoot (set via bro.tts.setAssetRoot)
//   2. ../brosoundml relative to cwd
//   3. ./brosoundml relative to cwd
static std::string resolveBrosoundmlRoot() {
    if (!g_assetRoot.empty()) return g_assetRoot;
    for (const char* p : { "../brosoundml", "./brosoundml" }) {
        if (fileExists(std::string(p) + "/weights/kokoro/config.json"))
            return p;
    }
    return "../brosoundml";  // best guess — error reporter will mention it
}

// Resolve the brosoundml-data sibling. Sits next to the brosoundml repo.
static std::string resolveBrosoundmlDataRoot() {
    std::string root = resolveBrosoundmlRoot();
    return root + "/../brosoundml-data";
}

static std::string slurpFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream os;
    os << f.rdbuf();
    return os.str();
}

// Load the Kokoro phoneme vocab from <kokoro_dir>/config.json without
// instantiating the heavy Kokoro model. Throws on parse failure.
static std::unordered_map<std::string, int> loadKokoroVocab(
        const std::string& configPath) {
    namespace j = brosoundml::detail::json;
    std::unordered_map<std::string, int> vocab;
    const std::string text = slurpFile(configPath);
    if (text.empty())
        throw std::runtime_error("config.json empty or unreadable: " + configPath);
    const j::Value root = j::parse(text);
    const j::Value* v = root.find("vocab");
    if (!v || !v->is_object())
        throw std::runtime_error("config.json missing 'vocab' object: " + configPath);
    for (const auto& m : v->as_object())
        vocab.emplace(m.first, static_cast<int>(m.second.as_number()));
    return vocab;
}

// Build the Phonemizer state from the resolved asset roots. Throws
// std::runtime_error naming any missing asset.
static std::unique_ptr<PhonemizerState> buildPhonemizerState() {
    const std::string repo   = resolveBrosoundmlRoot();
    const std::string data   = resolveBrosoundmlDataRoot();
    // Explicit overrides (bro.tts.setAssets) win; otherwise derive from the
    // sibling layout rooted at g_assetRoot / the default search.
    const std::string lexBin = !g_lexiconPath.empty()      ? g_lexiconPath
                                                           : data + "/g2p/lexicon_en_us.bin";
    const std::string posBin = !g_posPath.empty()          ? g_posPath
                                                           : data + "/pos_tagger/model.bin";
    const std::string kokCfg = !g_kokoroConfigPath.empty() ? g_kokoroConfigPath
                                                           : repo + "/weights/kokoro/config.json";

    const char* hint = " (pass an explicit path via bro.tts.setAssets({...}) "
                       "or a sibling root via bro.tts.setAssetRoot)";
    if (!fileExists(lexBin))
        throw std::runtime_error("missing lexicon: " + lexBin + hint);
    if (!fileExists(posBin))
        throw std::runtime_error("missing POS tagger weights: " + posBin + hint);
    if (!fileExists(kokCfg))
        throw std::runtime_error("missing Kokoro config.json: " + kokCfg + hint);

    auto st = std::make_unique<PhonemizerState>();
    st->vocab      = loadKokoroVocab(kokCfg);
    st->lexicon    = std::make_unique<brosoundml::g2p::Lexicon>(
                        brosoundml::g2p::Lexicon::load(lexBin));
    st->tagger     = std::make_unique<brosoundml::g2p::PosTagger>(
                        brosoundml::g2p::PosTagger::load(posBin));
    st->morphology = std::make_unique<brosoundml::g2p::Morphology>(*st->lexicon);
    st->special    = std::make_unique<brosoundml::g2p::SpecialCases>(*st->lexicon);
    st->adapter    = std::make_unique<brosoundml::g2p::PhonemeAdapter>(st->vocab);
    st->phonemizer = std::make_unique<brosoundml::g2p::Phonemizer>(
                        *st->tagger, *st->lexicon, *st->morphology,
                        *st->special, *st->adapter);
    return st;
}

// bro.tts.setAssetRoot(path)
//   Override the brosoundml repo root used by the lazy Phonemizer. The data
//   sibling is assumed at <path>/../brosoundml-data and the Kokoro config at
//   <path>/weights/kokoro/config.json. Clears any explicit setAssets() paths
//   (full reset to sibling-layout mode) and the cached state so the next
//   phonemize() call rebuilds against the new root.
static JSValue js_setAssetRoot(JSContext* ctx, JSValueConst,
                               int argc, JSValueConst* argv) {
    std::string path;
    if (argc < 1 || !argStr(ctx, argv[0], path))
        return JS_ThrowTypeError(ctx, "setAssetRoot(path): path string required");
    g_assetRoot = std::move(path);
    g_lexiconPath.clear();
    g_posPath.clear();
    g_kokoroConfigPath.clear();
    g_phonemizerState.reset();
    return JS_UNDEFINED;
}

// bro.tts.setAssets(opts)
//   Set explicit phonemizer asset paths, for loading from a flat per-user cache
//   that doesn't follow the dev sibling layout. opts may contain any of:
//     { root, lexicon, posTagger, kokoroConfig }
//   An explicit file path (lexicon/posTagger/kokoroConfig) overrides the
//   root-derived default for that asset; `root` sets the sibling-layout base
//   used for any asset not given explicitly. Omitted keys are left unchanged.
//   Resets cached state so the next phonemize() rebuilds.
static JSValue js_setAssets(JSContext* ctx, JSValueConst,
                            int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "setAssets(opts): options object required");
    auto getStr = [&](const char* key, std::string& out) {
        JSValue v = JS_GetPropertyStr(ctx, argv[0], key);
        std::string s;
        if (JS_IsString(v) && argStr(ctx, v, s)) out = std::move(s);
        JS_FreeValue(ctx, v);
    };
    getStr("root", g_assetRoot);
    getStr("lexicon", g_lexiconPath);
    getStr("posTagger", g_posPath);
    getStr("kokoroConfig", g_kokoroConfigPath);
    g_phonemizerState.reset();
    return JS_UNDEFINED;
}

// bro.tts.phonemize(text) -> Int32Array
//   Lazily constructs the in-tree English (en-us) Phonemizer on first call and
//   returns Kokoro phoneme ids. The frontend is English-only today; there is no
//   language/accent option (a non-English g2p would need its own lexicon + POS
//   model). Kept single-arg so a future opts can be added without a break.
static JSValue js_phonemize(JSContext* ctx, JSValueConst,
                            int argc, JSValueConst* argv) {
    std::string text;
    if (argc < 1 || !argStr(ctx, argv[0], text))
        return JS_ThrowTypeError(ctx, "phonemize(text): text required");
    try {
        if (!g_phonemizerState) {
            brotensor::init();
            g_phonemizerState = buildPhonemizerState();
        }
        auto ids = g_phonemizerState->phonemizer->phonemize(text);
        return qjsbind::make_int32_array(ctx, ids);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "phonemize: %s", e.what());
    }
}

// Build + load the Kokoro model from a checkpoint dir. Heavy + blocking (file
// IO + GPU upload); shared by the sync and async loadKokoro paths. Throws on
// error.
static void buildKokoro(const std::string& dir, brotensor::Device dev,
                        std::unique_ptr<KokoroWrapper>& w_out) {
    auto w = std::make_unique<KokoroWrapper>();
    w->device = dev;
    w->kokoro = std::make_unique<brosoundml::Kokoro>();
    {
        brotensor::DeviceScope scope(dev);
        w->kokoro->load(dir, dev);
    }
    std::fprintf(stderr, "[INFO] [tts] Kokoro loaded on %s\n", deviceName(dev));
    w_out = std::move(w);
}

// State for an async loadKokoro.
struct KokoroLoadState {
    std::string                    dir;
    brotensor::Device              dev = brotensor::Device::CPU;
    std::unique_ptr<KokoroWrapper> w;
    JSValue onReady = JS_UNDEFINED;
    JSValue onError = JS_UNDEFINED;
    bool    hasReady = false, hasError = false;
};

// bro.tts.loadKokoro(modelDir, opts?) -> Kokoro         (sync)
//                                     -> AsyncHandle     (async, if opts.onReady)
//   modelDir contains config.json + model.safetensors.
//   opts.device: 'cuda' | 'cpu' — defaults to CUDA when available, else CPU.
//   opts.onReady(kokoro) / opts.onError(message): when onReady is a function the
//   load runs on a background thread (non-blocking, parallelizable with other
//   loads) and these fire on the JS thread.
static JSValue js_loadKokoro(JSContext* ctx, JSValueConst,
                             int argc, JSValueConst* argv) {
    std::string dir;
    if (argc < 1 || !argStr(ctx, argv[0], dir))
        return JS_ThrowTypeError(ctx, "loadKokoro(modelDir, opts?): path required");

    brotensor::init();
    brotensor::Device dev = autoDevice();
    if (argc >= 2) {
        std::string err;
        if (!parseDeviceOpt(ctx, argv[1], dev, err))
            return JS_ThrowTypeError(ctx, "loadKokoro: %s", err.c_str());
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
            std::unique_ptr<KokoroWrapper> w;
            buildKokoro(dir, dev, w);
            return qjsbind::wrap<KokoroWrapper>(ctx, w.release());
        } catch (const std::exception& e) {
            return JS_ThrowInternalError(ctx, "loadKokoro: %s", e.what());
        }
    }

    // ── Async path ──
    auto ls = std::make_shared<KokoroLoadState>();
    ls->dir      = dir;
    ls->dev      = dev;
    ls->hasReady = true;
    ls->onReady  = JS_DupValue(ctx, onReady);
    ls->hasError = JS_IsFunction(ctx, onError);
    ls->onError  = ls->hasError ? JS_DupValue(ctx, onError) : JS_UNDEFINED;
    JS_FreeValue(ctx, onReady);
    JS_FreeValue(ctx, onError);

    auto work = [ls](const std::atomic<bool>&) {
        buildKokoro(ls->dir, ls->dev, ls->w);  // throws -> error
    };
    auto done = [ls](JSContext* c, bool /*cancelled*/, const std::string& error) {
        if (!error.empty() || !ls->w) {
            if (ls->hasError) {
                JSValue e = JS_NewString(c, error.empty() ? "loadKokoro failed"
                                                          : error.c_str());
                JSValue r = JS_Call(c, ls->onError, JS_UNDEFINED, 1, &e);
                if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
                JS_FreeValue(c, r);
                JS_FreeValue(c, e);
            }
        } else {
            JSValue out = qjsbind::wrap<KokoroWrapper>(c, ls->w.release());
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
// QwenTts loader
// ═══════════════════════════════════════════════════════════════════════════

// Build + load a Qwen3-TTS model from a checkpoint dir (config.json +
// model.safetensors + vocab.json + merges.txt + speech_tokenizer/). Heavy +
// blocking; shared by the sync and async loadQwen paths. Throws on error.
static void buildQwenTts(const std::string& dir, brotensor::Device dev,
                         std::unique_ptr<QwenTtsWrapper>& w_out) {
    auto w = std::make_unique<QwenTtsWrapper>();
    w->device = dev;
    w->qwen = std::make_unique<brosoundml::QwenTts>();
    {
        brotensor::DeviceScope scope(dev);
        w->qwen->load(dir, dev);
    }
    std::fprintf(stderr, "[INFO] [tts] Qwen3-TTS loaded on %s\n", deviceName(dev));
    w_out = std::move(w);
}

struct QwenTtsLoadState {
    std::string                     dir;
    brotensor::Device               dev = brotensor::Device::CPU;
    std::unique_ptr<QwenTtsWrapper> w;
    JSValue onReady = JS_UNDEFINED;
    JSValue onError = JS_UNDEFINED;
    bool    hasReady = false, hasError = false;
};

// bro.tts.loadQwen(modelDir, opts?) -> QwenTts          (sync)
//                                   -> AsyncHandle       (async, if opts.onReady)
//   modelDir holds config.json + model.safetensors + vocab.json + merges.txt and
//   the bundled speech_tokenizer/ codec. Unlike Kokoro, Qwen3-TTS is text-driven
//   end-to-end — no phonemize() / loadVoice() step.
//   opts.device: 'cuda' | 'cpu' — defaults to CUDA when available, else CPU.
//   opts.onReady(qwen) / opts.onError(message): when onReady is a function the
//   load runs on a background thread and these fire on the JS thread.
static JSValue js_loadQwen(JSContext* ctx, JSValueConst,
                           int argc, JSValueConst* argv) {
    std::string dir;
    if (argc < 1 || !argStr(ctx, argv[0], dir))
        return JS_ThrowTypeError(ctx, "loadQwen(modelDir, opts?): path required");

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

    // ── Sync path ──
    if (!async) {
        JS_FreeValue(ctx, onReady);
        JS_FreeValue(ctx, onError);
        try {
            std::unique_ptr<QwenTtsWrapper> w;
            buildQwenTts(dir, dev, w);
            return qjsbind::wrap<QwenTtsWrapper>(ctx, w.release());
        } catch (const std::exception& e) {
            return JS_ThrowInternalError(ctx, "loadQwen: %s", e.what());
        }
    }

    // ── Async path ──
    auto ls = std::make_shared<QwenTtsLoadState>();
    ls->dir      = dir;
    ls->dev      = dev;
    ls->hasReady = true;
    ls->onReady  = JS_DupValue(ctx, onReady);
    ls->hasError = JS_IsFunction(ctx, onError);
    ls->onError  = ls->hasError ? JS_DupValue(ctx, onError) : JS_UNDEFINED;
    JS_FreeValue(ctx, onReady);
    JS_FreeValue(ctx, onError);

    auto work = [ls](const std::atomic<bool>&) {
        buildQwenTts(ls->dir, ls->dev, ls->w);  // throws -> error
    };
    auto done = [ls](JSContext* c, bool /*cancelled*/, const std::string& error) {
        if (!error.empty() || !ls->w) {
            if (ls->hasError) {
                JSValue e = JS_NewString(c, error.empty() ? "loadQwen failed"
                                                          : error.c_str());
                JSValue r = JS_Call(c, ls->onError, JS_UNDEFINED, 1, &e);
                if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
                JS_FreeValue(c, r);
                JS_FreeValue(c, e);
            }
        } else {
            JSValue out = qjsbind::wrap<QwenTtsWrapper>(c, ls->w.release());
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
// Async synthesis — bro.tts.synthesize(kokoro, phonemeIds, voice, opts)
//                   bro.tts.synthesize(qwen, text, opts)
// ═══════════════════════════════════════════════════════════════════════════
//
// Runs Kokoro's forward pass on a background thread via the async-job runner, so
// the JS thread stays responsive. Kokoro has no autoregressive loop, so there
// is no per-step streaming poll, but cancellation is real: brosoundml checks the
// async-job cancel flag between pipeline stages and inside the generator's
// upsample loop, so .cancel() aborts synthesis (returning an empty buffer) and
// frees the model's busy lock rather than running to completion. Returns an
// AsyncHandle with .cancel(); opts.onDone(result, info) fires once on the JS
// thread, where result is { samples, sampleRate, durations } (same shape as the
// sync synthesize() method) and info is { cancelled, error? }.

// Shared between the work thread (sole writer) and the JS thread (sole reader /
// caller of onDone). Held by shared_ptr.
struct TtsJob {
    std::vector<int32_t> ids;
    float                speed = 1.0f;
    VoiceWrapper*        vw = nullptr;             // borrowed via voiceRef dup
    std::vector<float>   samples;                  // filled by work()
    int                  sample_rate = 24000;      // filled by work()
    std::vector<int32_t> durations;                // filled by work()
    JSValue              onDone    = JS_UNDEFINED;  // dup'd; UNDEFINED if absent
    JSValue              kokoroRef = JS_UNDEFINED;  // dup of the kokoro JS object
    JSValue              voiceRef  = JS_UNDEFINED;  // dup of the voice JS object
    bool                 hasOnDone = false;
};

// Shared by the QwenTts async synthesize path. The work thread is the sole
// writer of samples/sample_rate; the JS thread reads them in done().
struct QwenSynthJob {
    std::string        text;
    std::string        speaker;
    std::string        language = "english";
    std::string        instruct;                  // VoiceDesign voice description
    std::vector<float> samples;                  // filled by work()
    int                sample_rate = 24000;       // filled by work()
    JSValue            onDone  = JS_UNDEFINED;     // dup'd; UNDEFINED if absent
    JSValue            qwenRef = JS_UNDEFINED;     // dup of the qwen JS object
    bool               hasOnDone = false;
};

// bro.tts.synthesize(qwen, text, opts?) -> AsyncHandle
//   Runs QwenTts::synthesize on a background thread; opts.onDone(result, info)
//   fires once on the JS thread (result = { samples, sampleRate }, info =
//   { cancelled, error? }). .cancel() flips the per-frame cancel flag in the AR
//   loop, which returns an empty buffer (info.cancelled = true) and releases the
//   model. opts.speaker / opts.language select the preset voice and language.
static JSValue js_qwen_synthesize_async(JSContext* ctx, int argc,
                                        JSValueConst* argv) {
    auto* w = qjsbind::unwrap<QwenTtsWrapper>(ctx, argv[0]);  // non-null (caller)
    std::string text;
    if (argc < 2 || !argStr(ctx, argv[1], text))
        return JS_ThrowTypeError(ctx,
            "synthesize(qwen, text, opts?): text string required");

    auto job = std::make_shared<QwenSynthJob>();
    job->text = std::move(text);

    JSValue onDone = JS_UNDEFINED;
    if (argc >= 3 && JS_IsObject(argv[2])) {
        readQwenSynthOpts(ctx, argv[2], job->speaker, job->language, job->instruct);
        onDone = JS_GetPropertyStr(ctx, argv[2], "onDone");
    }

    // Claim the model for this synthesis (single-owner; one in flight).
    bool expected = false;
    if (!w->busy.compare_exchange_strong(expected, true)) {
        JS_FreeValue(ctx, onDone);
        return JS_ThrowInternalError(ctx,
            "synthesize: an operation is already in flight on this model");
    }

    job->hasOnDone = JS_IsFunction(ctx, onDone);
    job->onDone    = job->hasOnDone ? JS_DupValue(ctx, onDone) : JS_UNDEFINED;
    job->qwenRef   = JS_DupValue(ctx, argv[0]);  // keep the model alive
    JS_FreeValue(ctx, onDone);

    QwenTtsWrapper* mw = w;

    // Background thread: run the AR loop, polling the async-job cancel flag once
    // per frame (QwenTts::synthesize returns an empty buffer on cancel).
    auto work = [job, mw](const std::atomic<bool>& cancel) {
        brotensor::DeviceScope scope(mw->device);
        auto buf = mw->qwen->synthesize(
            job->text, job->speaker, job->language, job->instruct,
            [&cancel] { return cancel.load(std::memory_order_acquire); });
        job->samples     = std::move(buf.samples);
        job->sample_rate = buf.sample_rate;
    };

    auto done = [job, mw](JSContext* c, bool cancelled, const std::string& error) {
        if (job->hasOnDone) {
            JSValue result = JS_NewObject(c);
            JS_SetPropertyStr(c, result, "samples",
                qjsbind::make_float32_array(c, job->samples));
            JS_SetPropertyStr(c, result, "sampleRate",
                JS_NewInt32(c, job->sample_rate));
            JSValue info = JS_NewObject(c);
            JS_SetPropertyStr(c, info, "cancelled", JS_NewBool(c, cancelled));
            if (!error.empty())
                JS_SetPropertyStr(c, info, "error", JS_NewString(c, error.c_str()));
            JSValue args[2] = { result, info };
            JSValue r = JS_Call(c, job->onDone, JS_UNDEFINED, 2, args);
            if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
            JS_FreeValue(c, r);
            JS_FreeValue(c, result);
            JS_FreeValue(c, info);
        }
        if (job->hasOnDone) JS_FreeValue(c, job->onDone);
        JS_FreeValue(c, job->qwenRef);
        mw->busy.store(false, std::memory_order_release);
    };

    return launchAsyncJob(ctx, std::move(work), nullptr, std::move(done));
}

static JSValue js_tts_synthesize(JSContext* ctx, JSValueConst,
                                 int argc, JSValueConst* argv) {
    if (argc < 1)
        return JS_ThrowTypeError(ctx,
            "synthesize(model, ...): a Kokoro or QwenTts model is required");

    // Dispatch on model type. QwenTts is text-driven — synthesize(qwen, text,
    // opts?). Kokoro takes phoneme ids — synthesize(kokoro, phonemeIds, voice).
    if (qjsbind::unwrap<QwenTtsWrapper>(ctx, argv[0]))
        return js_qwen_synthesize_async(ctx, argc, argv);

    auto* w = qjsbind::unwrap<KokoroWrapper>(ctx, argv[0]);
    if (!w) return JS_ThrowTypeError(ctx,
        "synthesize: arg 0 must be a Kokoro or QwenTts");
    if (argc < 3)
        return JS_ThrowTypeError(ctx,
            "synthesize(kokoro, phonemeIds, voice, opts?): kokoro, phonemeIds "
            "and voice required");

    auto job = std::make_shared<TtsJob>();
    job->ids = readIdArray(ctx, argv[1]);
    if (job->ids.empty())
        return JS_ThrowTypeError(ctx,
            "synthesize: phonemeIds must be a non-empty Int32Array or number[]");

    auto* vw = qjsbind::unwrap<VoiceWrapper>(ctx, argv[2]);
    if (!vw)
        return JS_ThrowTypeError(ctx,
            "synthesize: voice must be a Voice (returned by loadVoice)");
    job->vw = vw;

    JSValue onDone = JS_UNDEFINED;
    if (argc >= 4 && JS_IsObject(argv[3])) {
        getNum(ctx, argv[3], "speed", job->speed);
        onDone = JS_GetPropertyStr(ctx, argv[3], "onDone");
    }

    // Claim the model for this synthesis (single-owner; one in flight).
    bool expected = false;
    if (!w->busy.compare_exchange_strong(expected, true)) {
        JS_FreeValue(ctx, onDone);
        return JS_ThrowInternalError(ctx,
            "synthesize: an operation is already in flight on this model");
    }

    job->hasOnDone  = JS_IsFunction(ctx, onDone);
    job->onDone     = job->hasOnDone ? JS_DupValue(ctx, onDone) : JS_UNDEFINED;
    job->kokoroRef  = JS_DupValue(ctx, argv[0]);  // keep the model alive
    job->voiceRef   = JS_DupValue(ctx, argv[2]);  // keep the voice alive
    JS_FreeValue(ctx, onDone);

    KokoroWrapper* mw = w;

    // Background thread: run the forward and stash the waveform. A barge-in
    // flips the async-job cancel flag; Kokoro polls it between pipeline stages
    // and inside the generator's upsample loop, returning an empty buffer so
    // the GPU stops and the model's `busy` lock is released promptly (the
    // empty/cancelled result is discarded by `done`). The check runs
    // synchronously inside synthesize(), so capturing `cancel` by reference is
    // safe.
    auto work = [job, mw](const std::atomic<bool>& cancel) {
        brotensor::DeviceScope scope(mw->device);
        std::vector<int32_t> pred_dur;
        auto buf = mw->kokoro->synthesize(
            job->ids, job->vw->voice, job->speed, &pred_dur,
            [&cancel] { return cancel.load(std::memory_order_acquire); });
        job->samples     = std::move(buf.samples);
        job->sample_rate = buf.sample_rate;
        job->durations   = std::move(pred_dur);
    };

    // JS thread, once: build { samples, sampleRate, durations } + {cancelled,
    // error}, hand them to onDone, free the dup'd values, release the model.
    auto done = [job, mw](JSContext* c, bool cancelled, const std::string& error) {
        if (job->hasOnDone) {
            JSValue result = JS_NewObject(c);
            JS_SetPropertyStr(c, result, "samples",
                qjsbind::make_float32_array(c, job->samples));
            JS_SetPropertyStr(c, result, "sampleRate",
                JS_NewInt32(c, job->sample_rate));
            JS_SetPropertyStr(c, result, "durations",
                qjsbind::make_int32_array(c, job->durations));
            JSValue info = JS_NewObject(c);
            JS_SetPropertyStr(c, info, "cancelled", JS_NewBool(c, cancelled));
            if (!error.empty())
                JS_SetPropertyStr(c, info, "error", JS_NewString(c, error.c_str()));
            JSValue args[2] = { result, info };
            JSValue r = JS_Call(c, job->onDone, JS_UNDEFINED, 2, args);
            if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
            JS_FreeValue(c, r);
            JS_FreeValue(c, result);
            JS_FreeValue(c, info);
        }
        if (job->hasOnDone) JS_FreeValue(c, job->onDone);
        JS_FreeValue(c, job->kokoroRef);
        JS_FreeValue(c, job->voiceRef);
        mw->busy.store(false, std::memory_order_release);
    };

    return launchAsyncJob(ctx, std::move(work), nullptr, std::move(done));
}

// ═══════════════════════════════════════════════════════════════════════════
// Install
// ═══════════════════════════════════════════════════════════════════════════

void installTtsBindings(JSContext* ctx) {
    registerVoiceClass(ctx);
    registerKokoroClass(ctx);
    registerQwenClass(ctx);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        JS_FreeValue(ctx, broObj);
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue tts = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, tts, "init",
        JS_NewCFunction(ctx, js_init, "init", 0));
    JS_SetPropertyStr(ctx, tts, "loadKokoro",
        JS_NewCFunction(ctx, js_loadKokoro, "loadKokoro", 2));
    JS_SetPropertyStr(ctx, tts, "loadQwen",
        JS_NewCFunction(ctx, js_loadQwen, "loadQwen", 2));
    JS_SetPropertyStr(ctx, tts, "phonemize",
        JS_NewCFunction(ctx, js_phonemize, "phonemize", 2));
    JS_SetPropertyStr(ctx, tts, "setAssetRoot",
        JS_NewCFunction(ctx, js_setAssetRoot, "setAssetRoot", 1));
    JS_SetPropertyStr(ctx, tts, "setAssets",
        JS_NewCFunction(ctx, js_setAssets, "setAssets", 1));
    JS_SetPropertyStr(ctx, tts, "synthesize",
        JS_NewCFunction(ctx, js_tts_synthesize, "synthesize", 4));
    JS_SetPropertyStr(ctx, broObj, "tts", tts);

    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

void cleanupTtsBindings(JSContext* /*ctx*/) {
    g_phonemizerState.reset();
    g_assetRoot.clear();
    g_lexiconPath.clear();
    g_posPath.clear();
    g_kokoroConfigPath.clear();
}

}  // namespace bro::js
