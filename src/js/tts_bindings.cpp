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

#include <qjsbind/qjsbind.h>

#include <brosoundml/kokoro.h>
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
};

struct VoiceWrapper {
    brosoundml::Voice voice;
};

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

// loadVoice(path) -> Voice
//   Loads a raw little-endian FP32 voice pack (rows * voice_dim floats).
//   Returns a Voice handle. PyTorch .pt voice packs must be pre-converted
//   to this raw format by the caller (brosoundml deliberately doesn't
//   pull in a pickle reader).
static JSValue js_kokoro_loadVoice(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    auto* w = kokoroSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "loadVoice: not a Kokoro");
    std::string path;
    if (argc < 1 || !argStr(ctx, argv[0], path))
        return JS_ThrowTypeError(ctx, "loadVoice(path): path string required");
    try {
        auto vw = std::make_unique<VoiceWrapper>();
        vw->voice = w->kokoro->load_voice(path);
        return qjsbind::wrap<VoiceWrapper>(ctx, vw.release());
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "loadVoice: %s", e.what());
    }
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
        .method_raw("loadVoice",      js_kokoro_loadVoice,      1)
        .method_raw("synthesize",     js_kokoro_synthesize,     3)
        .method_raw("vocab",          js_kokoro_vocab,          0)
        .method_raw("encodePhonemes", js_kokoro_encodePhonemes, 1);
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
// bro.tts.phonemize() and re-built if bro.tts.setAssetRoot() is called.
//
// Required runtime assets:
//   - <data_root>/g2p/lexicon_en_us.bin
//   - <repo_root>/weights/pos_tagger/model.bin
//   - <repo_root>/weights/kokoro/config.json    (for the phoneme vocab)
//
// Default search probes well-known sibling paths relative to the current
// working directory. setAssetRoot(path) overrides the brosoundml repo root
// (and implicitly the data root via the standard `../brosoundml-data` sibling).

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

// Module-scope singletons. Reset when setAssetRoot() is called.
static std::string                              g_assetRoot;      // brosoundml repo root
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
    const std::string lexBin = data + "/g2p/lexicon_en_us.bin";
    const std::string posBin = repo + "/weights/pos_tagger/model.bin";
    const std::string kokCfg = repo + "/weights/kokoro/config.json";

    if (!fileExists(lexBin))
        throw std::runtime_error("missing lexicon: " + lexBin
            + " (set bro.tts.setAssetRoot('../brosoundml'))");
    if (!fileExists(posBin))
        throw std::runtime_error("missing POS tagger weights: " + posBin
            + " (set bro.tts.setAssetRoot('../brosoundml'))");
    if (!fileExists(kokCfg))
        throw std::runtime_error("missing Kokoro config.json: " + kokCfg
            + " (set bro.tts.setAssetRoot('../brosoundml'))");

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
//   sibling is assumed at <path>/../brosoundml-data. Resets any cached state
//   so the next phonemize() call rebuilds against the new root.
static JSValue js_setAssetRoot(JSContext* ctx, JSValueConst,
                               int argc, JSValueConst* argv) {
    std::string path;
    if (argc < 1 || !argStr(ctx, argv[0], path))
        return JS_ThrowTypeError(ctx, "setAssetRoot(path): path string required");
    g_assetRoot = std::move(path);
    g_phonemizerState.reset();
    return JS_UNDEFINED;
}

// bro.tts.phonemize(text, opts?) -> Int32Array
//   Lazily constructs an English Phonemizer on first call. `opts.lang` is
//   accepted but ignored — reserved for accent variants. Default is en-us.
static JSValue js_phonemize(JSContext* ctx, JSValueConst,
                            int argc, JSValueConst* argv) {
    std::string text;
    if (argc < 1 || !argStr(ctx, argv[0], text))
        return JS_ThrowTypeError(ctx, "phonemize(text, opts?): text required");
    // opts.lang is parsed but not yet acted on.
    (void)argv;
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

// bro.tts.loadKokoro(modelDir, opts?) -> Kokoro
//   modelDir contains config.json + model.safetensors.
//   opts.device: 'cuda' | 'cpu' — defaults to CUDA when available, else CPU.
static JSValue js_loadKokoro(JSContext* ctx, JSValueConst,
                             int argc, JSValueConst* argv) {
    std::string dir;
    if (argc < 1 || !argStr(ctx, argv[0], dir))
        return JS_ThrowTypeError(ctx, "loadKokoro(modelDir, opts?): path required");
    try {
        brotensor::init();
        brotensor::Device dev = autoDevice();
        if (argc >= 2) {
            std::string err;
            if (!parseDeviceOpt(ctx, argv[1], dev, err))
                return JS_ThrowTypeError(ctx, "loadKokoro: %s", err.c_str());
        }
        auto w = std::make_unique<KokoroWrapper>();
        w->kokoro = std::make_unique<brosoundml::Kokoro>();
        w->kokoro->load(dir, dev);
        std::fprintf(stderr, "[INFO] [tts] Kokoro loaded on %s\n", deviceName(dev));
        return qjsbind::wrap<KokoroWrapper>(ctx, w.release());
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "loadKokoro: %s", e.what());
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Install
// ═══════════════════════════════════════════════════════════════════════════

void installTtsBindings(JSContext* ctx) {
    registerVoiceClass(ctx);
    registerKokoroClass(ctx);

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
    JS_SetPropertyStr(ctx, tts, "phonemize",
        JS_NewCFunction(ctx, js_phonemize, "phonemize", 2));
    JS_SetPropertyStr(ctx, tts, "setAssetRoot",
        JS_NewCFunction(ctx, js_setAssetRoot, "setAssetRoot", 1));
    JS_SetPropertyStr(ctx, broObj, "tts", tts);

    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

void cleanupTtsBindings(JSContext* /*ctx*/) {
    g_phonemizerState.reset();
    g_assetRoot.clear();
}

}  // namespace bro::js
