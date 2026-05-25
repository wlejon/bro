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

#include <brotensor/runtime.h>
#include <brotensor/tensor.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bro::js {

// ═══════════════════════════════════════════════════════════════════════════
// Wrapper structs
// ═══════════════════════════════════════════════════════════════════════════

struct KokoroWrapper {
    std::unique_ptr<brosoundml::Kokoro> kokoro;
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

// synthesize(phonemeIds, voice, opts?) -> { samples: Float32Array, sampleRate }
//   opts.speed: duration multiplier (>1 faster, <1 slower; default 1.0).
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
        auto buf = w->kokoro->synthesize(ids, vw->voice, speed);
        return audioBufferToJs(ctx, buf);
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

static void registerKokoroClass(JSContext* ctx) {
    qjsbind::Class<KokoroWrapper>(ctx, "Kokoro", qjsbind::NoGlobal)
        .get("loaded",       [](KokoroWrapper* w) { return w->kokoro->loaded(); })
        .get("sampleRate",   [](KokoroWrapper* w) { return w->kokoro->config().sample_rate; })
        .get("nTokens",      [](KokoroWrapper* w) { return w->kokoro->config().n_tokens; })
        .get("hiddenDim",    [](KokoroWrapper* w) { return w->kokoro->config().hidden_dim; })
        .get("styleDim",     [](KokoroWrapper* w) { return w->kokoro->config().style_dim; })
        .get("nLayer",       [](KokoroWrapper* w) { return w->kokoro->config().n_layer; })
        .method_raw("loadVoice",   js_kokoro_loadVoice,   1)
        .method_raw("synthesize",  js_kokoro_synthesize,  3)
        .method_raw("vocab",       js_kokoro_vocab,       0);
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

// bro.tts.loadKokoro(modelDir) -> Kokoro
//   modelDir contains config.json + model.safetensors. CPU-only today.
static JSValue js_loadKokoro(JSContext* ctx, JSValueConst,
                             int argc, JSValueConst* argv) {
    std::string dir;
    if (argc < 1 || !argStr(ctx, argv[0], dir))
        return JS_ThrowTypeError(ctx, "loadKokoro(modelDir): path required");
    try {
        brotensor::init();
        auto w = std::make_unique<KokoroWrapper>();
        w->kokoro = std::make_unique<brosoundml::Kokoro>();
        w->kokoro->load(dir, brotensor::Device::CPU);
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
        JS_NewCFunction(ctx, js_loadKokoro, "loadKokoro", 1));
    JS_SetPropertyStr(ctx, broObj, "tts", tts);

    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

void cleanupTtsBindings(JSContext* /*ctx*/) {}

}  // namespace bro::js
