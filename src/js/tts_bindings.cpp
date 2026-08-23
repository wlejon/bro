#if BRO_WITH_SOUNDML
#include "js/tts_bindings.h"
#include "util/interrupt.h"
#include "js/async_job.h"
#include "js/model_gate.h"
#include <api/api.h>  // brokit::api::resolveAssetPath
#include <qjsbind/qjsbind.h>
#include <brosoundml/kokoro.h>
#include <brosoundml/qwen_tts.h>
#include <brosoundml/supertonic.h>
#include <brosoundml/speaker_encoder.h>
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
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

// u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550
// Wrapper structs
// u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550

// The model + its single-owner gate are held by shared_ptr so they outlive the
// JS model handle whenever a KokoroSession over it is still alive, and so EVERY
// synthesis over one model u2014 module-level bro.tts.synthesize(model) AND every
// session.synthesize() u2014 serializes on the ONE busy flag. brosoundml's Kokoro
// session tier is SHARED WEIGHTS / SERIALIZED synthesis (one GPU stream, a
// lazily captured CUDA graph with shared step buffers), so sessions isolate the
// VOICE, not parallel execution; concurrent synthesis must be gated here.
struct KokoroWrapper {
    std::shared_ptr<brosoundml::Kokoro> kokoro;
    // Lazily constructed on first encodePhonemes() call. Borrows the
    // Kokoro instance's vocab map (lifetime tied to `kokoro` above).
    std::unique_ptr<brosoundml::g2p::PhonemeAdapter> adapter;
    brotensor::Device device = brotensor::Device::CPU;  // captured at load
    // Set while an async bro.tts.synthesize() / session.synthesize() runs on a
    // background thread; rejects a second concurrent op (the model is
    // single-owner). Cleared on the JS thread when the job's done() fires.
    // shared_ptr so sessions share this exact gate with the model.
    ModelGate busy;
};

struct VoiceWrapper {
    brosoundml::Voice voice;
};

// Qwen3-TTS u2014 text-driven (no phoneme frontend, no voice pack). Holds the
// pipeline (Talker + Code Predictor + bundled codec) and the device it loaded
// on. Like Kokoro it is single-owner: `busy` rejects a second concurrent op.
struct QwenTtsWrapper {
    std::shared_ptr<brosoundml::QwenTts> qwen;
    brotensor::Device device = brotensor::Device::CPU;  // captured at load
    // shared_ptr so sessions share this exact gate with the model.
    ModelGate busy;
};

// Supertonic-3 u2014 flow-matching multilingual TTS (Supertone). Text-driven
// (codepoint frontend, no G2P, no phoneme step); a voice is a VoiceStyle preset
// loaded from the model's voice_styles/. Like Kokoro it is single-owner: `busy`
// rejects a second concurrent synthesis on the model.
struct SupertonicWrapper {
    std::shared_ptr<brosoundml::Supertonic> model;
    brotensor::Device device = brotensor::Device::CPU;  // captured at load
    ModelGate busy;
};

// An opaque Supertonic voice preset (the two style matrices), returned by
// supertonic.loadVoiceStyle() and passed back via synthesize opts.voice.
struct SupertonicVoiceWrapper {
    brosoundml::VoiceStyle style;
    std::string            name;
};

// u2500u2500 Session wrappers (multi-voice / multi-stream over shared weights) u2500u2500
// A Kokoro speaking handle bound to a Voice: brosoundml::KokoroSession holds the
// model by shared_ptr<const Kokoro> internally; we add the shared busy gate +
// device so session.synthesize() serializes with every other op on the model.
struct KokoroSessionWrapper {
    ModelGate busy;    // shared with the model + siblings
    brotensor::Device                  device = brotensor::Device::CPU;
    std::unique_ptr<brosoundml::KokoroSession> session;  // move-only; ctor needs model+voice
};

// A QwenTts voice session: its own Talker + Code Predictor AR scratch over the
// shared weights. SERIALIZED tier u2014 gated on the shared busy flag.
struct QwenTtsSessionWrapper {
    std::shared_ptr<brosoundml::QwenTts> model;
    ModelGate busy;
    brotensor::Device                    device = brotensor::Device::CPU;
    brosoundml::QwenTtsSession           session;
};

// createSession() factory methods, registered on the model classes below but
// defined down in the session section (after the async-job structs they reuse).
static JSValue js_kokoro_createSession(JSContext*, JSValueConst, int, JSValueConst*);
static JSValue js_qwen_createSession(JSContext*, JSValueConst, int, JSValueConst*);

// Standalone ECAPA-TDNN speaker encoder (bro.tts.loadSpeakerEncoder) u2014 the
// voice-clone enrollment front-end on its own, without loading all of Qwen-Base.
// Host-side CPU; no device state.
struct SpeakerEncoderWrapper {
    std::unique_ptr<brosoundml::SpeakerEncoder> enc;
};

static const char* qwenVariantName(brosoundml::QwenTtsVariant v) {
    switch (v) {
        case brosoundml::QwenTtsVariant::Base:        return "base";
        case brosoundml::QwenTtsVariant::CustomVoice: return "customvoice";
        case brosoundml::QwenTtsVariant::VoiceDesign: return "voicedesign";
    }
    return "?";
}

// u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550
// Helpers
// u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550

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

// Read a truthy boolean option (absent / undefined => false).
static bool getBool(JSContext* ctx, JSValueConst obj, const char* key) {
    if (!JS_IsObject(obj)) return false;
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    const bool r = JS_ToBool(ctx, v) == 1;
    JS_FreeValue(ctx, v);
    return r;
}

// Pick the default device u2014 CUDA, then Metal, then CPU. brotensor::init()
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

// u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550
// Voice class
// u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550

static void registerVoiceClass(JSContext* ctx) {
    qjsbind::Class<VoiceWrapper>(ctx, "Voice", qjsbind::NoGlobal)
        .get("name", [](VoiceWrapper* w) { return w->voice.name; })
        .get("rows", [](VoiceWrapper* w) { return w->voice.packs.rows; })
        .get("cols", [](VoiceWrapper* w) { return w->voice.packs.cols; })
        // The full style table as a Float32Array (rows*cols, row-major). Lets
        // the app read a pack's style vectors out to blend/perturb them and
        // feed the result back through kokoro.createVoice().
        .get("data", [](VoiceWrapper* w, JSContext* c) -> JSValue {
            return qjsbind::make_float32_array(c, w->voice.packs.to_host_vector());
        });
}

// u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550
// Kokoro methods
// u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550

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

    // u2500u2500 Sync path (back-compat) u2500u2500
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

    // u2500u2500 Async path u2500u2500
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

// createVoice(data, name?) -> Voice
//   Build a voice from raw style floats instead of a file, so the app can
//   author or blend voices and play them through synthesize()/synthesizeTraced().
//   `data` is a Float32Array (or number[]): either voice_dim (= 2*style_dim)
//   values u2014 a single style point broadcast across all length rows u2014 or a whole
//   multiple of voice_dim for a full rows*voice_dim table.
static JSValue js_kokoro_createVoice(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    auto* w = kokoroSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "createVoice: not a Kokoro");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "createVoice(data, name?): data required");

    std::vector<float> data = qjsbind::read_float32_array(ctx, argv[0]);
    if (data.empty() && JS_IsArray(argv[0])) {       // accept a plain number[]
        std::uint32_t n = 0;
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        JS_ToUint32(ctx, &n, lv);
        JS_FreeValue(ctx, lv);
        data.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            JSValue e = JS_GetPropertyUint32(ctx, argv[0], i);
            double d = 0; JS_ToFloat64(ctx, &d, e);
            JS_FreeValue(ctx, e);
            data.push_back(static_cast<float>(d));
        }
    }
    if (data.empty())
        return JS_ThrowTypeError(ctx,
            "createVoice: data must be a non-empty Float32Array or number[]");

    std::string name = "custom";
    if (argc >= 2) { std::string s; if (argStr(ctx, argv[1], s)) name = s; }

    try {
        auto vw = std::make_unique<VoiceWrapper>();
        vw->voice = w->kokoro->make_voice(data, name);
        return qjsbind::wrap<VoiceWrapper>(ctx, vw.release());
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "createVoice: %s", e.what());
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
        brotensor::DeviceScope scope(w->device);   // ops create tensors on the model's device
        std::vector<int32_t> pred_dur;
        auto buf = w->kokoro->synthesize(ids, vw->voice, speed, &pred_dur,
                                         bro::util::interrupted);
        JSValue out = audioBufferToJs(ctx, buf);
        JS_SetPropertyStr(ctx, out, "durations",
            qjsbind::make_int32_array(ctx, pred_dur));
        return out;
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "synthesize: %s", e.what());
    }
}

// Build a JS array of { name, h, w, data } stage objects from a KokoroTrace.
// Shared by the sync (synthesizeTraced) and async (bro.tts.synthesize, trace:true)
// paths. The trace's per-stage host copies are already made during synthesize();
// this just wraps each as a row-major Float32Array for the renderer.
static JSValue traceStagesToJs(JSContext* ctx, const brosoundml::KokoroTrace& tr) {
    JSValue stages = JS_NewArray(ctx);
    std::uint32_t i = 0;
    for (const auto& s : tr.stages) {
        JSValue st = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, st, "name", JS_NewString(ctx, s.name.c_str()));
        JS_SetPropertyStr(ctx, st, "h",    JS_NewInt32(ctx, s.h));
        JS_SetPropertyStr(ctx, st, "w",    JS_NewInt32(ctx, s.w));
        JS_SetPropertyStr(ctx, st, "data", qjsbind::make_float32_array(ctx, s.data));
        JS_SetPropertyUint32(ctx, stages, i++, st);
    }
    return stages;
}

// synthesizeTraced(phonemeIds, voice, opts?)
//   -> { samples, sampleRate, durations, stages: [{ name, h, w, data }] }
//   Same as synthesize() but also returns each pipeline intermediate (see
//   KokoroTrace) as a row-major (h x w) Float32Array, for visualization. The
//   model is run synchronously; the extra cost is one host copy per stage.
//   For a non-blocking variant, use bro.tts.synthesize(kokoro, ids, voice,
//   { trace: true, onDone }) u2014 same stages, built on a background thread.
static JSValue js_kokoro_synthesizeTraced(JSContext* ctx, JSValueConst this_val,
                                          int argc, JSValueConst* argv) {
    auto* w = kokoroSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "synthesizeTraced: not a Kokoro");
    if (argc < 2)
        return JS_ThrowTypeError(ctx,
            "synthesizeTraced(phonemeIds, voice, opts?): phonemeIds and voice required");

    std::vector<int32_t> ids = readIdArray(ctx, argv[0]);
    if (ids.empty())
        return JS_ThrowTypeError(ctx,
            "synthesizeTraced: phonemeIds must be a non-empty Int32Array or number[]");

    auto* vw = qjsbind::unwrap<VoiceWrapper>(ctx, argv[1]);
    if (!vw)
        return JS_ThrowTypeError(ctx,
            "synthesizeTraced: voice must be a Voice (returned by loadVoice)");

    float speed = 1.0f;
    if (argc >= 3 && JS_IsObject(argv[2]))
        getNum(ctx, argv[2], "speed", speed);

    try {
        brotensor::DeviceScope scope(w->device);   // ops create tensors on the model's device
        std::vector<int32_t> pred_dur;
        brosoundml::KokoroTrace tr;
        auto buf = w->kokoro->synthesize(ids, vw->voice, speed, &pred_dur,
                                         bro::util::interrupted, &tr);
        JSValue out = audioBufferToJs(ctx, buf);
        JS_SetPropertyStr(ctx, out, "durations",
            qjsbind::make_int32_array(ctx, pred_dur));

        JS_SetPropertyStr(ctx, out, "stages", traceStagesToJs(ctx, tr));
        return out;
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "synthesizeTraced: %s", e.what());
    }
}

// decodeFrom(voice, asr, F0, N, nPhonemes, opts?)
//   -> { samples, sampleRate, stages? }
//   Re-decode from EDITED intermediates u2014 the prosody-editing entry point. asr,
//   F0 and N are the 'asr', 'F0_pred' and 'N_pred' Float32Array stages from a
//   prior synthesizeTraced(); edit any of them and re-run only the decoder back
//   half (skips plBERT / encoders / predictor). nPhonemes is the 'phonemes'
//   stage length (picks the voice style row). total is inferred as F0.length/2;
//   asr must be hiddenDim*total. opts.trace true returns the back-half stages.
static JSValue js_kokoro_decodeFrom(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    auto* w = kokoroSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "decodeFrom: not a Kokoro");
    if (argc < 5)
        return JS_ThrowTypeError(ctx,
            "decodeFrom(voice, asr, F0, N, nPhonemes, opts?): need voice, asr, F0, N, nPhonemes");

    auto* vw = qjsbind::unwrap<VoiceWrapper>(ctx, argv[0]);
    if (!vw)
        return JS_ThrowTypeError(ctx, "decodeFrom: voice must be a Voice (from createVoice/loadVoice)");

    std::vector<float> asr = qjsbind::read_float32_array(ctx, argv[1]);
    std::vector<float> F0  = qjsbind::read_float32_array(ctx, argv[2]);
    std::vector<float> N   = qjsbind::read_float32_array(ctx, argv[3]);
    if (asr.empty() || F0.empty() || N.empty())
        return JS_ThrowTypeError(ctx, "decodeFrom: asr, F0 and N must be non-empty Float32Arrays");
    if (F0.size() % 2 != 0)
        return JS_ThrowTypeError(ctx, "decodeFrom: F0 length must be even (2*total)");

    int32_t nph = 0; JS_ToInt32(ctx, &nph, argv[4]);
    const int total = static_cast<int>(F0.size() / 2);

    bool want_trace = false;
    if (argc >= 6 && JS_IsObject(argv[5])) {
        JSValue tv = JS_GetPropertyStr(ctx, argv[5], "trace");
        want_trace = JS_ToBool(ctx, tv) > 0;
        JS_FreeValue(ctx, tv);
    }

    try {
        brotensor::DeviceScope scope(w->device);   // ops create tensors on the model's device
        brosoundml::KokoroTrace tr;
        auto buf = w->kokoro->decode_from(vw->voice, nph, asr, total, F0, N, {},
                                          want_trace ? &tr : nullptr);
        JSValue out = audioBufferToJs(ctx, buf);
        if (want_trace)
            JS_SetPropertyStr(ctx, out, "stages", traceStagesToJs(ctx, tr));
        return out;
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "decodeFrom: %s", e.what());
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
//   Runs only the codepointu2192id stage via brosoundml::g2p::PhonemeAdapter
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
        .method_raw("createVoice",    js_kokoro_createVoice,    2)
        .method_raw("synthesize",     js_kokoro_synthesize,     3)
        .method_raw("synthesizeTraced", js_kokoro_synthesizeTraced, 3)
        .method_raw("decodeFrom",     js_kokoro_decodeFrom,     6)
        .method_raw("vocab",          js_kokoro_vocab,          0)
        .method_raw("encodePhonemes", js_kokoro_encodePhonemes, 1)
        .method_raw("createSession",  js_kokoro_createSession,  1);
}

// u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550
// QwenTts methods
// u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550

static QwenTtsWrapper* qwenSelf(JSContext* ctx, JSValueConst this_val) {
    return qjsbind::unwrap<QwenTtsWrapper>(ctx, this_val);
}

// Read opts.speaker / opts.language / opts.instruct (all optional strings).
// Defaults match QwenTts::synthesize: speaker "" (the model resolves the first
// preset only if it has speakers), language "english", instruct "" (no voice
// instruction u2014 the VoiceDesign description, or the 1.7B CustomVoice instruct).
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

// Read the optional sampling controls into a QwenTtsSampling. Omitted keys keep
// the struct defaults u2014 temperature 0 (deterministic greedy), repetition penalty
// 1.05 (the upstream policy). Shared by the sync + async synth paths. The Talker
// steering knobs:
//   opts.repetitionPenalty  > 1 discourages the AR Talker's droning / looping.
//   opts.logitBias          { codeId: delta, ... } additive codebook-0 bias
//                           (delta -Infinity forbids a code). Opaque RVQ ids.
//   opts.adaptive           > 0 scales the codebook-0 temperature per frame by
//                           how unsure the model is u2014 hotter only where it hedged.
//   opts.voiceSteer         Float32Array, talker-hidden width: an additive offset
//                           on the prefill speaker-slot row (the emotion / masc-fem
//                           direction-add). Works on any variant with a speaker
//                           slot u2014 CustomVoice presets and Base x-vectors alike.
//   opts.speakerVector      Float32Array, talker-hidden width: REPLACES the speaker
//                           slot with a designed voice (e.g. a voice-basis point
//                           rendered through CustomVoice instead of a preset).
static void readQwenSampling(JSContext* ctx, JSValueConst opts,
                             brosoundml::QwenTtsSampling& s) {
    if (!JS_IsObject(opts)) return;
    getNum(ctx, opts, "temperature", s.temperature);
    getNum(ctx, opts, "topP", s.top_p);
    getNum(ctx, opts, "repetitionPenalty", s.repetition_penalty);
    getNum(ctx, opts, "adaptive", s.adaptive);
    JSValue tk = JS_GetPropertyStr(ctx, opts, "topK");
    if (JS_IsNumber(tk)) { int32_t t = s.top_k; JS_ToInt32(ctx, &t, tk); s.top_k = t; }
    JS_FreeValue(ctx, tk);
    JSValue sd = JS_GetPropertyStr(ctx, opts, "seed");
    if (JS_IsNumber(sd)) {
        int64_t t = 0; JS_ToInt64(ctx, &t, sd);
        s.seed = static_cast<std::uint64_t>(t);
    }
    JS_FreeValue(ctx, sd);
    // logitBias: a plain object mapping codebook-0 id -> additive logit delta.
    JSValue lb = JS_GetPropertyStr(ctx, opts, "logitBias");
    if (JS_IsObject(lb) && !JS_IsFunction(ctx, lb)) {
        JSPropertyEnum* tab = nullptr;
        uint32_t len = 0;
        if (JS_GetOwnPropertyNames(ctx, &tab, &len, lb, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < len; ++i) {
                const char* key = JS_AtomToCString(ctx, tab[i].atom);
                JSValue v = JS_GetProperty(ctx, lb, tab[i].atom);
                double d = 0;
                if (key && JS_IsNumber(v) && JS_ToFloat64(ctx, &d, v) == 0) {
                    int id = std::atoi(key);
                    s.logit_bias.emplace_back(id, static_cast<float>(d));
                }
                if (key) JS_FreeCString(ctx, key);
                JS_FreeValue(ctx, v);
                JS_FreeAtom(ctx, tab[i].atom);
            }
            js_free(ctx, tab);
        }
    }
    JS_FreeValue(ctx, lb);
    // voiceSteer: a Float32Array additive offset on the prefill speaker slot.
    // Length is validated downstream against the talker hidden width.
    JSValue vs = JS_GetPropertyStr(ctx, opts, "voiceSteer");
    if (!JS_IsUndefined(vs) && !JS_IsNull(vs))
        s.voice_steer = qjsbind::read_float32_array(ctx, vs);
    JS_FreeValue(ctx, vs);
    // speakerVector: a Float32Array that REPLACES the speaker slot (a designed
    // voice on any variant u2014 e.g. a voice-basis point rendered through CustomVoice).
    JSValue sv = JS_GetPropertyStr(ctx, opts, "speakerVector");
    if (!JS_IsUndefined(sv) && !JS_IsNull(sv))
        s.speaker_vector = qjsbind::read_float32_array(ctx, sv);
    JS_FreeValue(ctx, sv);
}

// Marshal a QwenTtsTrace to a JS stages array [{ name, h, w, data:Float32Array }] u2014
// the same shape Kokoro's traceStagesToJs uses, so a UI renders both uniformly.
static JSValue qwenTraceToJs(JSContext* ctx, const brosoundml::QwenTtsTrace& tr) {
    JSValue stages = JS_NewArray(ctx);
    std::uint32_t i = 0;
    for (const auto& s : tr.stages) {
        JSValue st = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, st, "name", JS_NewString(ctx, s.name.c_str()));
        JS_SetPropertyStr(ctx, st, "h",    JS_NewInt32(ctx, s.h));
        JS_SetPropertyStr(ctx, st, "w",    JS_NewInt32(ctx, s.w));
        JS_SetPropertyStr(ctx, st, "data", qjsbind::make_float32_array(ctx, s.data));
        JS_SetPropertyUint32(ctx, stages, i++, st);
    }
    return stages;
}

// qwen.synthesize(text, opts?) -> { samples, sampleRate }      (sync, blocking)
//   opts.speaker:  preset speaker name (CustomVoice; e.g. 'serena').
//   opts.language: 'english' (default), 'chinese', 'auto', ...
//   opts.instruct: natural-language voice description (VoiceDesign; ignored by
//                  the 0.6B CustomVoice checkpoint).
//   opts.temperature / topK / topP / seed: sampling controls (temperature 0 =
//                  greedy/deterministic, the default; >0 draws a varied take).
//   The async, cancellable form is bro.tts.synthesize(qwen, text, opts).
static JSValue js_qwen_synthesize(JSContext* ctx, JSValueConst this_val,
                                  int argc, JSValueConst* argv) {
    auto* w = qwenSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "synthesize: not a QwenTts");
    std::string text;
    if (argc < 1 || !argStr(ctx, argv[0], text))
        return JS_ThrowTypeError(ctx, "synthesize(text, opts?): text string required");
    std::string speaker, language = "english", instruct;
    brosoundml::QwenTtsSampling sampling;
    bool wantTrace = false;
    if (argc >= 2) {
        readQwenSynthOpts(ctx, argv[1], speaker, language, instruct);
        readQwenSampling(ctx, argv[1], sampling);
        wantTrace = getBool(ctx, argv[1], "trace");
    }
    try {
        brotensor::DeviceScope scope(w->device);
        brosoundml::QwenTtsTrace trace;
        auto buf = w->qwen->synthesize(text, speaker, language, instruct,
                                       bro::util::interrupted, sampling,
                                       wantTrace ? &trace : nullptr);
        JSValue obj = audioBufferToJs(ctx, buf);
        if (wantTrace) JS_SetPropertyStr(ctx, obj, "stages", qwenTraceToJs(ctx, trace));
        return obj;
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "synthesize: %s", e.what());
    }
}

// qwen.synthesizeClone(text, refPath, opts?) -> { samples, sampleRate }  (sync, blocking)
//   Zero-shot voice clone (Base variant only): synthesize `text` in the voice of
//   a reference WAV at `refPath`. The clip is encoded to an ECAPA-TDNN speaker
//   x-vector and spliced into the Talker prefill. opts.language: 'english'
//   (default), 'chinese', 'auto', ...  Throws if the loaded checkpoint is not a
//   Base variant (no speaker encoder) or the WAV can't be read.
static JSValue js_qwen_synthesize_clone(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv) {
    auto* w = qwenSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "synthesizeClone: not a QwenTts");
    std::string text, refPath;
    if (argc < 1 || !argStr(ctx, argv[0], text))
        return JS_ThrowTypeError(ctx, "synthesizeClone(text, refPath, opts?): text string required");
    if (argc < 2 || !argStr(ctx, argv[1], refPath))
        return JS_ThrowTypeError(ctx, "synthesizeClone(text, refPath, opts?): refPath (WAV path) string required");
    std::string language = "english";
    brosoundml::QwenTtsSampling sampling;
    if (argc >= 3 && JS_IsObject(argv[2])) {
        JSValue lv = JS_GetPropertyStr(ctx, argv[2], "language");
        std::string l;
        if (JS_IsString(lv) && argStr(ctx, lv, l)) language = std::move(l);
        JS_FreeValue(ctx, lv);
        readQwenSampling(ctx, argv[2], sampling);
    }
    try {
        brotensor::DeviceScope scope(w->device);
        brosoundml::AudioBuffer ref = brosoundml::read_wav(refPath);
        auto buf = w->qwen->synthesize_clone(text, ref, language,
                                             bro::util::interrupted, sampling);
        return audioBufferToJs(ctx, buf);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "synthesizeClone: %s", e.what());
    }
}

// qwen.synthesizeFromXvector(text, xvec, opts?) -> { samples, sampleRate }  (sync)
//   Render `text` from a caller-supplied speaker x-vector (Float32Array of
//   enc_dim u2014 1024 u2014 floats, as embedSpeaker returns), bypassing the WAV
//   enrollment of synthesizeClone. This is the voice-designer seam: enroll real
//   voices to x-vectors, interpolate / morph / steer in that space, then render
//   the designed point. Base variant only. opts.language + sampling controls as
//   synthesize(). Throws if the checkpoint is not Base or the vector width wrong.
static JSValue js_qwen_synthesize_from_xvector(JSContext* ctx, JSValueConst this_val,
                                               int argc, JSValueConst* argv) {
    auto* w = qwenSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "synthesizeFromXvector: not a QwenTts");
    std::string text;
    if (argc < 1 || !argStr(ctx, argv[0], text))
        return JS_ThrowTypeError(ctx,
            "synthesizeFromXvector(text, xvec, opts?): text string required");
    std::vector<float> xvec = qjsbind::read_float32_array(ctx, argv[1]);
    if (xvec.empty())
        return JS_ThrowTypeError(ctx,
            "synthesizeFromXvector: xvec must be a non-empty Float32Array");
    std::string language = "english";
    brosoundml::QwenTtsSampling sampling;
    bool wantTrace = false;
    if (argc >= 3 && JS_IsObject(argv[2])) {
        JSValue lv = JS_GetPropertyStr(ctx, argv[2], "language");
        std::string l;
        if (JS_IsString(lv) && argStr(ctx, lv, l)) language = std::move(l);
        JS_FreeValue(ctx, lv);
        readQwenSampling(ctx, argv[2], sampling);
        wantTrace = getBool(ctx, argv[2], "trace");
    }
    try {
        brotensor::DeviceScope scope(w->device);
        brosoundml::QwenTtsTrace trace;
        auto buf = w->qwen->synthesize_with_xvector(text, xvec, language,
                                                    bro::util::interrupted, sampling,
                                                    wantTrace ? &trace : nullptr);
        JSValue obj = audioBufferToJs(ctx, buf);
        if (wantTrace) JS_SetPropertyStr(ctx, obj, "stages", qwenTraceToJs(ctx, trace));
        return obj;
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "synthesizeFromXvector: %s", e.what());
    }
}

// qwen.decodeCodes(codes, numQuantizers, numFrames) -> { samples, sampleRate }  (sync)
//   Decode a precomputed RVQ code stream straight through the bundled 12 Hz codec
//   to a 24 kHz waveform u2014 the deterministic tail of synthesis. `codes` is an
//   Int32Array (or number[]) of numQuantizers*numFrames codes, codebook-major
//   (codes[k*numFrames + t]); the same layout encodeAudio returns. Lets an editor
//   splice / prefix-lock / round-trip a code stream and re-render it.
static JSValue js_qwen_decode_codes(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    auto* w = qwenSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "decodeCodes: not a QwenTts");
    if (argc < 3)
        return JS_ThrowTypeError(ctx,
            "decodeCodes(codes, numQuantizers, numFrames): three arguments required");
    std::vector<int32_t> codes = readIdArray(ctx, argv[0]);
    if (codes.empty())
        return JS_ThrowTypeError(ctx, "decodeCodes: codes must be a non-empty Int32Array");
    int32_t nq = 0, nf = 0;
    JS_ToInt32(ctx, &nq, argv[1]);
    JS_ToInt32(ctx, &nf, argv[2]);
    if (nq <= 0 || nf <= 0 ||
        static_cast<size_t>(nq) * static_cast<size_t>(nf) != codes.size())
        return JS_ThrowTypeError(ctx,
            "decodeCodes: numQuantizers*numFrames must equal codes.length");
    try {
        brotensor::DeviceScope scope(w->device);
        auto buf = w->qwen->decode_codes(codes, nq, nf);
        return audioBufferToJs(ctx, buf);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "decodeCodes: %s", e.what());
    }
}

// qwen.encodeAudio(audio, opts?) -> { codes, numQuantizers, numFrames }  (sync)
//   The analysis path (inverse of decodeCodes): encode mono PCM into the codec's
//   RVQ codes. audio: Float32Array of mono samples; opts.sampleRate (default
//   24000, resampled internally). codes is an Int32Array of numQuantizers*
//   numFrames, codebook-major u2014 feed straight back to decodeCodes to round-trip.
static JSValue js_qwen_encode_audio(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    auto* w = qwenSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "encodeAudio: not a QwenTts");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "encodeAudio(audio, opts?): audio required");
    std::vector<float> audio = qjsbind::read_float32_array(ctx, argv[0]);
    if (audio.empty())
        return JS_ThrowTypeError(ctx, "encodeAudio: audio must be a non-empty Float32Array");
    float sr = 24000.0f;
    if (argc >= 2 && JS_IsObject(argv[1])) getNum(ctx, argv[1], "sampleRate", sr);
    try {
        brotensor::DeviceScope scope(w->device);
        brosoundml::AudioBuffer ref;
        ref.samples     = std::move(audio);
        ref.sample_rate = static_cast<int>(sr);
        int nf = 0;
        std::vector<int32_t> codes = w->qwen->encode_audio(ref, &nf);
        const int nq = nf > 0 ? static_cast<int>(codes.size() / nf) : 0;
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "codes", qjsbind::make_int32_array(ctx, codes));
        JS_SetPropertyStr(ctx, obj, "numQuantizers", JS_NewInt32(ctx, nq));
        JS_SetPropertyStr(ctx, obj, "numFrames", JS_NewInt32(ctx, nf));
        return obj;
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "encodeAudio: %s", e.what());
    }
}

// qwen.embedSpeaker(audio, opts?) -> Float32Array(enc_dim)   (sync, Base only)
//   Encode mono PCM to the ECAPA-TDNN speaker x-vector (1024-D) u2014 the same
//   enrollment synthesizeClone does, on its own. audio: Float32Array of mono
//   samples; opts.sampleRate: input rate (default 24000, resampled to the
//   encoder's 24 kHz as needed). The audio->identity front-end for harvesting
//   speaker embeddings to train a voice/style adapter. Throws if the loaded
//   checkpoint is not a Base variant.
static JSValue js_qwen_embed_speaker(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    auto* w = qwenSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "embedSpeaker: not a QwenTts");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "embedSpeaker(audio, opts?): audio required");
    std::vector<float> audio = qjsbind::read_float32_array(ctx, argv[0]);
    if (audio.empty())
        return JS_ThrowTypeError(ctx,
            "embedSpeaker: audio must be a non-empty Float32Array");
    float sr = 24000.0f;
    if (argc >= 2 && JS_IsObject(argv[1])) getNum(ctx, argv[1], "sampleRate", sr);
    try {
        brotensor::DeviceScope scope(w->device);
        brosoundml::AudioBuffer ref;
        ref.samples     = std::move(audio);
        ref.sample_rate = static_cast<int>(sr);
        return qjsbind::make_float32_array(ctx, w->qwen->embed_speaker(ref));
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "embedSpeaker: %s", e.what());
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

// u2500u2500u2500 SpeakerEncoder handle u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500

static SpeakerEncoderWrapper* speakerSelf(JSContext* ctx, JSValueConst this_val) {
    return qjsbind::unwrap<SpeakerEncoderWrapper>(ctx, this_val);
}

// State for an async embedSpeaker. Holds a strong ref to the SpeakerEncoder JS
// object so the borrowed `w` pointer stays valid across the worker thread.
struct SpkEmbedState {
    SpeakerEncoderWrapper* w = nullptr;
    JSValue            self       = JS_UNDEFINED;
    std::vector<float> audio;
    int                sampleRate = 24000;
    std::vector<float> out;
    JSValue onDone  = JS_UNDEFINED;
    JSValue onError = JS_UNDEFINED;
    bool    hasError = false;
};

// enc.embedSpeaker(audio, opts?) -> Float32Array(enc_dim)   (sync)
//                                -> AsyncHandle             (async, if opts.onDone)
//   Encode mono PCM to the ECAPA-TDNN speaker x-vector (1024-D) u2014 the same
//   enrollment QwenTts does, but from the standalone ~18 MB artifact. audio:
//   Float32Array of mono samples; opts.sampleRate: input rate (default 24000,
//   resampled to the encoder's 24 kHz as needed). Drop-in for qwen.embedSpeaker.
//   The forward pass runs on the encoder's device (GPU when available). It is a
//   multi-GFLOP convolution stack, so opts.onDone(embedding) / opts.onError(msg)
//   run it on a background thread (firing on the JS thread) to keep the UI live.
static JSValue js_speaker_embed(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv) {
    auto* w = speakerSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "embedSpeaker: not a SpeakerEncoder");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "embedSpeaker(audio, opts?): audio required");
    std::vector<float> audio = qjsbind::read_float32_array(ctx, argv[0]);
    if (audio.empty())
        return JS_ThrowTypeError(ctx,
            "embedSpeaker: audio must be a non-empty Float32Array");
    float sr = 24000.0f;
    const bool haveOpts = (argc >= 2) && JS_IsObject(argv[1]);
    if (haveOpts) getNum(ctx, argv[1], "sampleRate", sr);

    JSValue onDone  = haveOpts ? JS_GetPropertyStr(ctx, argv[1], "onDone")
                               : JS_UNDEFINED;
    JSValue onError = haveOpts ? JS_GetPropertyStr(ctx, argv[1], "onError")
                               : JS_UNDEFINED;
    const bool async = JS_IsFunction(ctx, onDone);

    // u2500u2500 Sync path u2500u2500
    if (!async) {
        JS_FreeValue(ctx, onDone);
        JS_FreeValue(ctx, onError);
        try {
            brosoundml::AudioBuffer ref;
            ref.samples     = std::move(audio);
            ref.sample_rate = static_cast<int>(sr);
            return qjsbind::make_float32_array(ctx, w->enc->embed(ref));
        } catch (const std::exception& e) {
            return JS_ThrowInternalError(ctx, "embedSpeaker: %s", e.what());
        }
    }

    // u2500u2500 Async path u2500u2500
    auto st = std::make_shared<SpkEmbedState>();
    st->w          = w;
    st->self       = JS_DupValue(ctx, this_val);   // keep the encoder alive
    st->audio      = std::move(audio);
    st->sampleRate = static_cast<int>(sr);
    st->onDone     = JS_DupValue(ctx, onDone);
    st->hasError   = JS_IsFunction(ctx, onError);
    st->onError    = st->hasError ? JS_DupValue(ctx, onError) : JS_UNDEFINED;
    JS_FreeValue(ctx, onDone);
    JS_FreeValue(ctx, onError);

    auto work = [st](const std::atomic<bool>&) {
        brosoundml::AudioBuffer ref;
        ref.samples     = std::move(st->audio);
        ref.sample_rate = st->sampleRate;
        st->out = st->w->enc->embed(ref);   // throws -> error
    };
    auto done = [st](JSContext* c, bool /*cancelled*/, const std::string& error) {
        if (!error.empty()) {
            if (st->hasError) {
                JSValue e = JS_NewString(c, error.c_str());
                JSValue r = JS_Call(c, st->onError, JS_UNDEFINED, 1, &e);
                if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
                JS_FreeValue(c, r);
                JS_FreeValue(c, e);
            }
        } else {
            JSValue arr = qjsbind::make_float32_array(c, st->out);
            JSValue r = JS_Call(c, st->onDone, JS_UNDEFINED, 1, &arr);
            if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
            JS_FreeValue(c, r);
            JS_FreeValue(c, arr);
        }
        JS_FreeValue(c, st->onDone);
        if (st->hasError) JS_FreeValue(c, st->onError);
        JS_FreeValue(c, st->self);
    };
    return launchAsyncJob(ctx, std::move(work), nullptr, std::move(done));
}

static void registerSpeakerEncoderClass(JSContext* ctx) {
    qjsbind::Class<SpeakerEncoderWrapper>(ctx, "SpeakerEncoder", qjsbind::NoGlobal)
        .get("loaded",     [](SpeakerEncoderWrapper* w) { return w->enc->loaded(); })
        .get("encDim",     [](SpeakerEncoderWrapper* w) { return w->enc->enc_dim(); })
        .get("sampleRate", [](SpeakerEncoderWrapper* w) { return w->enc->sample_rate(); })
        .method_raw("embedSpeaker", js_speaker_embed, 2)
        .method_raw("embed",        js_speaker_embed, 2);
}

static void registerQwenClass(JSContext* ctx) {
    qjsbind::Class<QwenTtsWrapper>(ctx, "QwenTts", qjsbind::NoGlobal)
        .get("loaded",     [](QwenTtsWrapper* w) { return w->qwen->loaded(); })
        .get("sampleRate", [](QwenTtsWrapper* w) { return w->qwen->config().sample_rate; })
        .get("variant",    [](QwenTtsWrapper* w) {
            return std::string(qwenVariantName(w->qwen->config().variant)); })
        .get("modelSize",  [](QwenTtsWrapper* w) { return w->qwen->config().model_size; })
        .method_raw("synthesize",      js_qwen_synthesize,       2)
        .method_raw("synthesizeClone", js_qwen_synthesize_clone, 3)
        .method_raw("synthesizeFromXvector", js_qwen_synthesize_from_xvector, 3)
        .method_raw("decodeCodes",     js_qwen_decode_codes,     3)
        .method_raw("encodeAudio",     js_qwen_encode_audio,     2)
        .method_raw("embedSpeaker",    js_qwen_embed_speaker,    2)
        .method_raw("speakers",       js_qwen_speakers,        0)
        .method_raw("languages",      js_qwen_languages,       0)
        .method_raw("speakerDialect", js_qwen_speaker_dialect, 1)
        .method_raw("createSession",  js_qwen_createSession,   0);
}

// u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550
// bro.tts free functions
// u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550

static JSValue js_init(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    try {
        brotensor::init();
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "bro.tts.init: %s", e.what());
    }
    return JS_UNDEFINED;
}

// u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550
// G2P (text u2192 phoneme ids) u2014 module-scope lazy Phonemizer
// u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550
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
//      kokoroConfig}) u2014 for loading from a flat per-user cache
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
    return "../brosoundml";  // best guess u2014 error reporter will mention it
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
//   opts.device: 'cuda' | 'cpu' u2014 defaults to CUDA when available, else CPU.
//   opts.onReady(kokoro) / opts.onError(message): when onReady is a function the
//   load runs on a background thread (non-blocking, parallelizable with other
//   loads) and these fire on the JS thread.
static JSValue js_loadKokoro(JSContext* ctx, JSValueConst,
                             int argc, JSValueConst* argv) {
    std::string dir;
    if (argc < 1 || !argStr(ctx, argv[0], dir))
        return JS_ThrowTypeError(ctx, "loadKokoro(modelDir, opts?): path required");
    // Resolve the same way fs.existsSync() does (app-relative base paths),
    // not against the raw OS process cwd u2014 callers commonly probe candidate
    // dirs with fs.existsSync() first, which would otherwise silently
    // disagree with what this native loader actually opens.
    dir = brokit::api::resolveAssetPath(ctx, dir);

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

    // u2500u2500 Sync path (back-compat) u2500u2500
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

    // u2500u2500 Async path u2500u2500
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

// u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550
// QwenTts loader
// u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550

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
//   end-to-end u2014 no phonemize() / loadVoice() step.
//   opts.device: 'cuda' | 'cpu' u2014 defaults to CUDA when available, else CPU.
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

    // u2500u2500 Sync path u2500u2500
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

    // u2500u2500 Async path u2500u2500
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

// u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550
// SpeakerEncoder loader
// u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550

// Build + load a standalone speaker encoder from an artifact dir (config.json +
// model.safetensors). Cheap I/O (~18 MB); the encoder places its conv stack on
// the default device (GPU when available) u2014 so DON'T wrap this in a CPU
// DeviceScope, or load()'s default_device() query would pin it to CPU. Throws
// on error.
static void buildSpeakerEncoder(const std::string& dir,
                                std::unique_ptr<SpeakerEncoderWrapper>& w_out) {
    auto w = std::make_unique<SpeakerEncoderWrapper>();
    w->enc = std::make_unique<brosoundml::SpeakerEncoder>();
    w->enc->load(dir);
    std::fprintf(stderr, "[INFO] [tts] speaker encoder loaded (device=%s)\n",
                 brotensor::device_name(brotensor::default_device()));
    w_out = std::move(w);
}

struct SpeakerEncoderLoadState {
    std::string                          dir;
    std::unique_ptr<SpeakerEncoderWrapper> w;
    JSValue onReady = JS_UNDEFINED;
    JSValue onError = JS_UNDEFINED;
    bool    hasReady = false, hasError = false;
};

// bro.tts.loadSpeakerEncoder(dir, opts?) -> SpeakerEncoder       (sync)
//                                        -> AsyncHandle          (async, if opts.onReady)
//   dir holds config.json + model.safetensors u2014 the standalone ECAPA-TDNN
//   speaker encoder (brosoundml-data/qwen-tts/speaker-encoder). Loading just this
//   enrolls a voice-clone reference clip without pulling in all of Qwen-Base.
//   The returned handle exposes embedSpeaker(audio, opts?) -> Float32Array. The
//   encoder runs host-side, so there is no device option.
//   opts.onReady(enc) / opts.onError(message): when onReady is a function the
//   load runs on a background thread and these fire on the JS thread.
static JSValue js_loadSpeakerEncoder(JSContext* ctx, JSValueConst,
                                     int argc, JSValueConst* argv) {
    std::string dir;
    if (argc < 1 || !argStr(ctx, argv[0], dir))
        return JS_ThrowTypeError(ctx,
            "loadSpeakerEncoder(dir, opts?): path required");

    brotensor::init();

    const bool haveOpts = (argc >= 2) && JS_IsObject(argv[1]);
    JSValue onReady = haveOpts ? JS_GetPropertyStr(ctx, argv[1], "onReady")
                               : JS_UNDEFINED;
    JSValue onError = haveOpts ? JS_GetPropertyStr(ctx, argv[1], "onError")
                               : JS_UNDEFINED;
    const bool async = JS_IsFunction(ctx, onReady);

    // u2500u2500 Sync path u2500u2500
    if (!async) {
        JS_FreeValue(ctx, onReady);
        JS_FreeValue(ctx, onError);
        try {
            std::unique_ptr<SpeakerEncoderWrapper> w;
            buildSpeakerEncoder(dir, w);
            return qjsbind::wrap<SpeakerEncoderWrapper>(ctx, w.release());
        } catch (const std::exception& e) {
            return JS_ThrowInternalError(ctx, "loadSpeakerEncoder: %s", e.what());
        }
    }

    // u2500u2500 Async path u2500u2500
    auto ls = std::make_shared<SpeakerEncoderLoadState>();
    ls->dir      = dir;
    ls->hasReady = true;
    ls->onReady  = JS_DupValue(ctx, onReady);
    ls->hasError = JS_IsFunction(ctx, onError);
    ls->onError  = ls->hasError ? JS_DupValue(ctx, onError) : JS_UNDEFINED;
    JS_FreeValue(ctx, onReady);
    JS_FreeValue(ctx, onError);

    auto work = [ls](const std::atomic<bool>&) {
        buildSpeakerEncoder(ls->dir, ls->w);  // throws -> error
    };
    auto done = [ls](JSContext* c, bool /*cancelled*/, const std::string& error) {
        if (!error.empty() || !ls->w) {
            if (ls->hasError) {
                JSValue e = JS_NewString(c, error.empty() ? "loadSpeakerEncoder failed"
                                                          : error.c_str());
                JSValue r = JS_Call(c, ls->onError, JS_UNDEFINED, 1, &e);
                if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
                JS_FreeValue(c, r);
                JS_FreeValue(c, e);
            }
        } else {
            JSValue out = qjsbind::wrap<SpeakerEncoderWrapper>(c, ls->w.release());
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

// u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550
// Supertonic loader + class
// u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550

// Build + load a Supertonic model from a converted directory (tts.json +
// per-model *.safetensors + unicode_indexer.json + voice_styles/). Heavy +
// blocking; shared by the sync and async loadSupertonic paths. Throws on error.
static void buildSupertonic(const std::string& dir, brotensor::Device dev,
                            std::unique_ptr<SupertonicWrapper>& w_out) {
    auto w = std::make_unique<SupertonicWrapper>();
    w->device = dev;
    w->model = std::make_shared<brosoundml::Supertonic>();
    {
        brotensor::DeviceScope scope(dev);
        w->model->load(dir, dev);
    }
    std::fprintf(stderr, "[INFO] [tts] Supertonic loaded on %s\n", deviceName(dev));
    w_out = std::move(w);
}

struct SupertonicLoadState {
    std::string                         dir;
    brotensor::Device                   dev = brotensor::Device::CPU;
    std::unique_ptr<SupertonicWrapper>  w;
    JSValue onReady = JS_UNDEFINED;
    JSValue onError = JS_UNDEFINED;
    bool    hasReady = false, hasError = false;
};

// bro.tts.loadSupertonic(modelDir, opts?) -> Supertonic     (sync)
//                                         -> AsyncHandle     (async, if opts.onReady)
//   modelDir is the converted layout (tts.json + *.safetensors + the codepoint
//   frontend tables + voice_styles/). Supertonic is text-driven end to end u2014 no
//   phonemize() step; a voice is a VoiceStyle preset via loadVoiceStyle().
//   opts.device: 'cuda' | 'cpu' | 'metal' u2014 defaults to CUDA when available.
//   opts.onReady(supertonic) / opts.onError(message): when onReady is a function
//   the load runs on a background thread and these fire on the JS thread.
static JSValue js_loadSupertonic(JSContext* ctx, JSValueConst,
                                 int argc, JSValueConst* argv) {
    std::string dir;
    if (argc < 1 || !argStr(ctx, argv[0], dir))
        return JS_ThrowTypeError(ctx, "loadSupertonic(modelDir, opts?): path required");

    brotensor::init();
    brotensor::Device dev = autoDevice();
    if (argc >= 2) {
        std::string err;
        if (!parseDeviceOpt(ctx, argv[1], dev, err))
            return JS_ThrowTypeError(ctx, "loadSupertonic: %s", err.c_str());
    }

    const bool haveOpts = (argc >= 2) && JS_IsObject(argv[1]);
    JSValue onReady = haveOpts ? JS_GetPropertyStr(ctx, argv[1], "onReady")
                               : JS_UNDEFINED;
    JSValue onError = haveOpts ? JS_GetPropertyStr(ctx, argv[1], "onError")
                               : JS_UNDEFINED;
    const bool async = JS_IsFunction(ctx, onReady);

    // u2500u2500 Sync path u2500u2500
    if (!async) {
        JS_FreeValue(ctx, onReady);
        JS_FreeValue(ctx, onError);
        try {
            std::unique_ptr<SupertonicWrapper> w;
            buildSupertonic(dir, dev, w);
            return qjsbind::wrap<SupertonicWrapper>(ctx, w.release());
        } catch (const std::exception& e) {
            return JS_ThrowInternalError(ctx, "loadSupertonic: %s", e.what());
        }
    }

    // u2500u2500 Async path u2500u2500
    auto ls = std::make_shared<SupertonicLoadState>();
    ls->dir      = dir;
    ls->dev      = dev;
    ls->hasReady = true;
    ls->onReady  = JS_DupValue(ctx, onReady);
    ls->hasError = JS_IsFunction(ctx, onError);
    ls->onError  = ls->hasError ? JS_DupValue(ctx, onError) : JS_UNDEFINED;
    JS_FreeValue(ctx, onReady);
    JS_FreeValue(ctx, onError);

    auto work = [ls](const std::atomic<bool>&) {
        buildSupertonic(ls->dir, ls->dev, ls->w);  // throws -> error
    };
    auto done = [ls](JSContext* c, bool /*cancelled*/, const std::string& error) {
        if (!error.empty() || !ls->w) {
            if (ls->hasError) {
                JSValue e = JS_NewString(c, error.empty() ? "loadSupertonic failed"
                                                          : error.c_str());
                JSValue r = JS_Call(c, ls->onError, JS_UNDEFINED, 1, &e);
                if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
                JS_FreeValue(c, r);
                JS_FreeValue(c, e);
            }
        } else {
            JSValue out = qjsbind::wrap<SupertonicWrapper>(c, ls->w.release());
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

// supertonic.loadVoiceStyle(path) -> SupertonicVoice
//   Parse a voice_styles/<name>.json preset (the style_ttl / style_dp matrices)
//   into an opaque voice handle. Host-side + small; runs synchronously.
static JSValue js_supertonic_loadVoiceStyle(JSContext* ctx, JSValueConst this_val,
                                            int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<SupertonicWrapper>(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "loadVoiceStyle: not a Supertonic");
    std::string path;
    if (argc < 1 || !argStr(ctx, argv[0], path))
        return JS_ThrowTypeError(ctx, "loadVoiceStyle(path): path string required");
    try {
        auto vw = std::make_unique<SupertonicVoiceWrapper>();
        vw->style = w->model->load_voice_style(path);
        vw->name  = std::filesystem::path(path).stem().string();
        return qjsbind::wrap<SupertonicVoiceWrapper>(ctx, vw.release());
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "loadVoiceStyle: %s", e.what());
    }
}


// supertonic.createVoice(ttl, dp, name?) -> SupertonicVoice
//   Author a voice from raw style matrices instead of a file: ttl is 50*256
//   token-major (style_ttl), dp is 8*16 row-major (style_dp). Lets the app read
//   presets out via the SupertonicVoice .ttl / .dp getters, blend them, build a
//   masc<->fem axis or scale a voice's identity, and play the result through
//   synthesize(). Throws if either matrix is the wrong size.
static JSValue js_supertonic_createVoice(JSContext* ctx, JSValueConst this_val,
                                         int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<SupertonicWrapper>(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "createVoice: not a Supertonic");
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "createVoice(ttl, dp, name?): ttl and dp required");
    std::vector<float> ttl = qjsbind::read_float32_array(ctx, argv[0]);
    std::vector<float> dp  = qjsbind::read_float32_array(ctx, argv[1]);
    if (ttl.size() != 50u * 256u)
        return JS_ThrowTypeError(ctx, "createVoice: ttl must have 50*256 = 12800 floats");
    if (dp.size() != 8u * 16u)
        return JS_ThrowTypeError(ctx, "createVoice: dp must have 8*16 = 128 floats");
    std::string name = "custom";
    if (argc >= 3) { std::string s; if (argStr(ctx, argv[2], s)) name = s; }
    auto vw = std::make_unique<SupertonicVoiceWrapper>();
    vw->style.ttl = std::move(ttl);
    vw->style.dp  = std::move(dp);
    vw->name      = std::move(name);
    return qjsbind::wrap<SupertonicVoiceWrapper>(ctx, vw.release());
}

static void registerSupertonicVoiceClass(JSContext* ctx) {
    qjsbind::Class<SupertonicVoiceWrapper>(ctx, "SupertonicVoice", qjsbind::NoGlobal)
        .get("name", [](SupertonicVoiceWrapper* w) { return w->name; })
        // The two style matrices as row-major Float32Arrays, so the app can read
        // presets out, blend / axis-project / scale them, and feed the result to
        // supertonic.createVoice(). ttl = 50x256 (style_ttl), dp = 8x16 (style_dp).
        .get("ttl", [](SupertonicVoiceWrapper* w, JSContext* c) -> JSValue {
            return qjsbind::make_float32_array(c, w->style.ttl);
        })
        .get("dp", [](SupertonicVoiceWrapper* w, JSContext* c) -> JSValue {
            return qjsbind::make_float32_array(c, w->style.dp);
        })
        .get("ttlRows", [](SupertonicVoiceWrapper*) { return 50; })
        .get("ttlCols", [](SupertonicVoiceWrapper*) { return 256; })
        .get("dpRows",  [](SupertonicVoiceWrapper*) { return 8; })
        .get("dpCols",  [](SupertonicVoiceWrapper*) { return 16; });
}

static JSValue js_supertonic_synthesize(JSContext*, JSValueConst, int, JSValueConst*);

static void registerSupertonicClass(JSContext* ctx) {
    qjsbind::Class<SupertonicWrapper>(ctx, "Supertonic", qjsbind::NoGlobal)
        .get("loaded",     [](SupertonicWrapper* w) { return w->model->loaded(); })
        .get("sampleRate", [](SupertonicWrapper* w) { return w->model->config().sample_rate; })
        .method_raw("loadVoiceStyle", js_supertonic_loadVoiceStyle, 1)
        .method_raw("createVoice",    js_supertonic_createVoice, 3)
        .method_raw("synthesize",     js_supertonic_synthesize, 2);
}

// Read the scalar Supertonic synth opts (everything but the voice handle, which
// the sync/async callers resolve themselves). Omitted keys keep the defaults.
static void readSupertonicOpts(JSContext* ctx, JSValueConst opts,
                               std::string& language, int& steps, float& speed,
                               std::uint64_t& seed, bool& longForm, float& gapSeconds,
                               float& guidance) {
    if (!JS_IsObject(opts)) return;
    std::string lang;
    JSValue lg = JS_GetPropertyStr(ctx, opts, "language");
    if (JS_IsString(lg) && argStr(ctx, lg, lang)) language = std::move(lang);
    JS_FreeValue(ctx, lg);
    JSValue st = JS_GetPropertyStr(ctx, opts, "steps");
    if (JS_IsNumber(st)) { int32_t t = steps; JS_ToInt32(ctx, &t, st); steps = t; }
    JS_FreeValue(ctx, st);
    getNum(ctx, opts, "speed", speed);
    getNum(ctx, opts, "gapSeconds", gapSeconds);
    getNum(ctx, opts, "guidance", guidance);
    JSValue sd = JS_GetPropertyStr(ctx, opts, "seed");
    if (JS_IsNumber(sd)) { int64_t t = 0; JS_ToInt64(ctx, &t, sd); seed = (std::uint64_t)t; }
    JS_FreeValue(ctx, sd);
    longForm = getBool(ctx, opts, "longForm");
    if (steps < 1) steps = 1;
}

// supertonic.synthesize(text, opts) -> { samples, sampleRate }   (sync, blocking)
//   opts.voice (required SupertonicVoice), opts.language / steps / speed / seed /
//   longForm / gapSeconds as documented on bro.tts.synthesize. The async,
//   latest-wins form is bro.tts.synthesize(supertonic, text, opts).
static JSValue js_supertonic_synthesize(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<SupertonicWrapper>(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "synthesize: not a Supertonic");
    std::string text;
    if (argc < 1 || !argStr(ctx, argv[0], text))
        return JS_ThrowTypeError(ctx, "synthesize(text, opts): text string required");

    SupertonicVoiceWrapper* vw = nullptr;
    std::string language = "en";
    int steps = 8; float speed = 1.05f; std::uint64_t seed = 0;
    bool longForm = false; float gap = 0.3f; float guidance = 3.0f;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue vv = JS_GetPropertyStr(ctx, argv[1], "voice");
        vw = qjsbind::unwrap<SupertonicVoiceWrapper>(ctx, vv);
        JS_FreeValue(ctx, vv);
        readSupertonicOpts(ctx, argv[1], language, steps, speed, seed, longForm, gap, guidance);
    }
    if (!vw)
        return JS_ThrowTypeError(ctx,
            "synthesize: opts.voice must be a SupertonicVoice (from loadVoiceStyle)");
    try {
        brotensor::DeviceScope scope(w->device);
        brosoundml::AudioBuffer buf =
            longForm ? w->model->synthesize_long(text, language, vw->style, steps, speed, seed, gap, 300, guidance)
                     : w->model->synthesize(text, language, vw->style, steps, speed, seed, guidance);
        return audioBufferToJs(ctx, buf);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "synthesize: %s", e.what());
    }
}

// Shared by the Supertonic async synthesize path. The work thread is the sole
// writer of samples/sample_rate; the JS thread reads them in done().
struct SupertonicSynthJob {
    std::string        text;
    std::string        language = "en";
    int                steps    = 8;
    float              speed    = 1.05f;
    std::uint64_t      seed     = 0;
    bool               longForm = false;
    float              gapSeconds = 0.3f;
    float              guidance = 3.0f;
    SupertonicVoiceWrapper* vw = nullptr;          // borrowed via voiceRef dup
    std::vector<float> samples;                    // filled by work()
    int                sample_rate = 44100;        // filled by work()
    JSValue            onDone   = JS_UNDEFINED;     // dup'd; UNDEFINED if absent
    JSValue            modelRef = JS_UNDEFINED;     // dup of the supertonic JS object
    JSValue            voiceRef = JS_UNDEFINED;     // dup of the voice JS object
    bool               hasOnDone = false;
};

// bro.tts.synthesize(supertonic, text, opts) -> AsyncHandle
//   opts.voice       SupertonicVoice (required) u2014 a loadVoiceStyle() preset.
//   opts.language    ISO tag ('en' default; one of the 31 supported, or 'na').
//   opts.steps       flow-matching Euler steps (8 default; more = smoother/slower).
//   opts.speed       > 1 shortens the utterance (1.05 upstream default).
//   opts.seed        Philox seed for the N(0,1) flow noise (0 default).
//   opts.longForm    bool u2014 split into sentences + concat (synthesize_long).
//   opts.gapSeconds  silence between sentences when longForm (0.3 default).
//   opts.guidance    CFG scale w (3 default): higher = crisper/more articulated,
//                    lower = flatter/breathier. Flow-matching only (no Kokoro/Qwen).
//   opts.onDone(result, info) fires once on the JS thread; result =
//   { samples, sampleRate }, info = { cancelled, error? }. Single-owner: a second
//   op while one is in flight throws.
static JSValue js_supertonic_synthesize_async(JSContext* ctx, int argc,
                                              JSValueConst* argv) {
    auto* w = qjsbind::unwrap<SupertonicWrapper>(ctx, argv[0]);  // non-null (caller)
    std::string text;
    if (argc < 2 || !argStr(ctx, argv[1], text))
        return JS_ThrowTypeError(ctx,
            "synthesize(supertonic, text, opts): text string required");

    auto job = std::make_shared<SupertonicSynthJob>();
    job->text = std::move(text);

    JSValue onDone = JS_UNDEFINED;
    JSValue voiceVal = JS_UNDEFINED;
    if (argc >= 3 && JS_IsObject(argv[2])) {
        voiceVal = JS_GetPropertyStr(ctx, argv[2], "voice");
        job->vw = qjsbind::unwrap<SupertonicVoiceWrapper>(ctx, voiceVal);
        readSupertonicOpts(ctx, argv[2], job->language, job->steps, job->speed,
                           job->seed, job->longForm, job->gapSeconds, job->guidance);
        onDone = JS_GetPropertyStr(ctx, argv[2], "onDone");
    }

    if (!job->vw) {
        JS_FreeValue(ctx, voiceVal);
        JS_FreeValue(ctx, onDone);
        return JS_ThrowTypeError(ctx,
            "synthesize: opts.voice must be a SupertonicVoice (from loadVoiceStyle)");
    }

    // Claim the model for this synthesis (single-owner; one in flight).
    if (!w->busy.tryClaim()) {
        JS_FreeValue(ctx, voiceVal);
        JS_FreeValue(ctx, onDone);
        return JS_ThrowInternalError(ctx,
            "synthesize: an operation is already in flight on this model");
    }

    job->hasOnDone = JS_IsFunction(ctx, onDone);
    job->onDone    = job->hasOnDone ? JS_DupValue(ctx, onDone) : JS_UNDEFINED;
    job->modelRef  = JS_DupValue(ctx, argv[0]);    // keep the model alive
    job->voiceRef  = JS_DupValue(ctx, voiceVal);   // keep the voice alive
    JS_FreeValue(ctx, voiceVal);
    JS_FreeValue(ctx, onDone);

    SupertonicWrapper* mw = w;

    // Background thread: run the flow-matching pipeline + vocoder. Supertonic's
    // synthesize() has no per-step cancel hook (the flow loop is short), so the
    // async-job cancel flag isn't polled; barge-in is realised by the busy gate
    // (latest-wins re-kicks once this run completes).
    auto work = [job, mw](const std::atomic<bool>&) {
        brotensor::DeviceScope scope(mw->device);
        brosoundml::AudioBuffer buf =
            job->longForm
                ? mw->model->synthesize_long(job->text, job->language, job->vw->style,
                                             job->steps, job->speed, job->seed,
                                             job->gapSeconds, 300, job->guidance)
                : mw->model->synthesize(job->text, job->language, job->vw->style,
                                        job->steps, job->speed, job->seed, job->guidance);
        job->samples     = std::move(buf.samples);
        job->sample_rate = buf.sample_rate;
    };

    auto done = [job, mw](JSContext* c, bool cancelled, const std::string& error) {
        // Release the model BEFORE invoking onDone so the callback may start the
        // next synth on this same model without tripping the in-flight guard.
        mw->busy.release();
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
        JS_FreeValue(c, job->modelRef);
        JS_FreeValue(c, job->voiceRef);
    };

    return launchAsyncJob(ctx, std::move(work), nullptr, std::move(done));
}

// u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550
// Async synthesis u2014 bro.tts.synthesize(kokoro, phonemeIds, voice, opts)
//                   bro.tts.synthesize(qwen, text, opts)
// u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550
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
    bool                 wantTrace = false;         // opts.trace u2014 capture stages
    brosoundml::KokoroTrace trace;                  // filled by work() if wantTrace
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
    std::vector<float> xvector;                   // designer x-vector (Base); empty = speaker/instruct path
    brosoundml::QwenTtsSampling sampling;         // temperature/top_k/top_p/seed
    std::vector<float> samples;                  // filled by work()
    int                sample_rate = 24000;       // filled by work()
    bool               wantTrace = false;         // capture the AR trace
    brosoundml::QwenTtsTrace trace;               // filled by work() when wantTrace
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
        readQwenSampling(ctx, argv[2], job->sampling);
        job->wantTrace = getBool(ctx, argv[2], "trace");
        // opts.xvector (Base designer): render off-thread from a designed speaker
        // x-vector instead of a preset/instruct u2014 the async twin of the sync
        // synthesizeFromXvector, so the voice designer never blocks the JS thread.
        JSValue xv = JS_GetPropertyStr(ctx, argv[2], "xvector");
        if (!JS_IsUndefined(xv) && !JS_IsNull(xv))
            job->xvector = qjsbind::read_float32_array(ctx, xv);
        JS_FreeValue(ctx, xv);
        onDone = JS_GetPropertyStr(ctx, argv[2], "onDone");
    }

    // Claim the model for this synthesis (single-owner; one in flight).
    if (!w->busy.tryClaim()) {
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
        auto cancelFn = [&cancel] {
            return cancel.load(std::memory_order_acquire) || bro::util::interrupted();
        };
        brosoundml::AudioBuffer buf;
        if (!job->xvector.empty())
            buf = mw->qwen->synthesize_with_xvector(
                job->text, job->xvector, job->language, cancelFn,
                job->sampling, job->wantTrace ? &job->trace : nullptr);
        else
            buf = mw->qwen->synthesize(
                job->text, job->speaker, job->language, job->instruct,
                cancelFn, job->sampling, job->wantTrace ? &job->trace : nullptr);
        job->samples     = std::move(buf.samples);
        job->sample_rate = buf.sample_rate;
    };

    auto done = [job, mw](JSContext* c, bool cancelled, const std::string& error) {
        // Release the model BEFORE invoking onDone so the callback may start the
        // next synth on this same model without tripping the in-flight guard.
        mw->busy.release();
        if (job->hasOnDone) {
            JSValue result = JS_NewObject(c);
            JS_SetPropertyStr(c, result, "samples",
                qjsbind::make_float32_array(c, job->samples));
            JS_SetPropertyStr(c, result, "sampleRate",
                JS_NewInt32(c, job->sample_rate));
            if (job->wantTrace)
                JS_SetPropertyStr(c, result, "stages", qwenTraceToJs(c, job->trace));
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
    };

    return launchAsyncJob(ctx, std::move(work), nullptr, std::move(done));
}

// Streaming Qwen synthesis. Same as the async synthesize, but audio is delivered
// in chunks as the AR loop produces it: opts.onChunk(Float32Array) fires on the JS
// thread as each codec chunk is decoded (so playback can start before generation
// finishes), then opts.onDone(result, info) fires once with the full buffer. The
// work thread is the sole writer of a committed chunk prefix published via an
// atomic; the JS-thread poll drains it u2014 lock-free, no mutex.
struct QwenStreamJob {
    std::string        text;
    std::string        speaker;
    std::string        language = "english";
    std::string        instruct;
    int                chunkFrames = 25;          // ~2 s at 12.5 Hz
    brosoundml::QwenTtsSampling sampling;
    std::vector<float> xvector;                    // Base designer: stream a designed x-vector
    std::vector<float> samples;                   // full audio (onDone), filled by work
    int                sample_rate = 24000;
    JSValue            onChunk = JS_UNDEFINED;     // dup'd; UNDEFINED if absent
    JSValue            onDone  = JS_UNDEFINED;
    JSValue            qwenRef = JS_UNDEFINED;
    bool               hasOnChunk = false;
    bool               hasOnDone  = false;
    // SPSC handoff: work thread writes chunkSlots[produced] then bumps `produced`;
    // the JS-thread poll reads up to `produced` and advances `drained`.
    std::vector<std::vector<float>> chunkSlots;
    std::atomic<size_t> produced{0};
    size_t              drained = 0;
};

// bro.tts.synthesizeStream(qwen, text, opts?) -> AsyncHandle
static JSValue js_qwen_synthesize_stream(JSContext* ctx, int argc,
                                         JSValueConst* argv) {
    auto* w = qjsbind::unwrap<QwenTtsWrapper>(ctx, argv[0]);  // non-null (caller)
    std::string text;
    if (argc < 2 || !argStr(ctx, argv[1], text))
        return JS_ThrowTypeError(ctx,
            "synthesizeStream(qwen, text, opts?): text string required");

    auto job = std::make_shared<QwenStreamJob>();
    job->text = std::move(text);

    JSValue onChunk = JS_UNDEFINED, onDone = JS_UNDEFINED;
    if (argc >= 3 && JS_IsObject(argv[2])) {
        readQwenSynthOpts(ctx, argv[2], job->speaker, job->language, job->instruct);
        readQwenSampling(ctx, argv[2], job->sampling);
        // opts.xvector (Base designer): stream from a designed speaker x-vector
        // instead of a preset u2014 the streaming twin of the async synthesize path,
        // so the Base voice designer can stream too (not just Render).
        JSValue xv = JS_GetPropertyStr(ctx, argv[2], "xvector");
        if (!JS_IsUndefined(xv) && !JS_IsNull(xv))
            job->xvector = qjsbind::read_float32_array(ctx, xv);
        JS_FreeValue(ctx, xv);
        JSValue cf = JS_GetPropertyStr(ctx, argv[2], "chunkFrames");
        if (JS_IsNumber(cf)) {
            int32_t t = job->chunkFrames; JS_ToInt32(ctx, &t, cf);
            if (t > 0) job->chunkFrames = t;
        }
        JS_FreeValue(ctx, cf);
        onChunk = JS_GetPropertyStr(ctx, argv[2], "onChunk");
        onDone  = JS_GetPropertyStr(ctx, argv[2], "onDone");
    }
    // Pre-size the chunk slots so the work thread never reallocates while the JS
    // thread reads: at most (max_frames / chunkFrames) + 1 (final flush) chunks,
    // bounded by the generator's 4096-frame cap regardless of chunkFrames.
    job->chunkSlots.resize(4098);

    // Claim the model for this synthesis (single-owner; one in flight).
    if (!w->busy.tryClaim()) {
        JS_FreeValue(ctx, onChunk);
        JS_FreeValue(ctx, onDone);
        return JS_ThrowInternalError(ctx,
            "synthesizeStream: an operation is already in flight on this model");
    }

    job->hasOnChunk = JS_IsFunction(ctx, onChunk);
    job->onChunk    = job->hasOnChunk ? JS_DupValue(ctx, onChunk) : JS_UNDEFINED;
    job->hasOnDone  = JS_IsFunction(ctx, onDone);
    job->onDone     = job->hasOnDone ? JS_DupValue(ctx, onDone) : JS_UNDEFINED;
    job->qwenRef    = JS_DupValue(ctx, argv[0]);
    JS_FreeValue(ctx, onChunk);
    JS_FreeValue(ctx, onDone);

    QwenTtsWrapper* mw = w;

    auto work = [job, mw](const std::atomic<bool>& cancel) {
        brotensor::DeviceScope scope(mw->device);
        // on_chunk runs on THIS (background) thread: copy the samples into the
        // next slot and publish it. No JS here u2014 the JS-thread poll fires onChunk.
        auto onChunkCb = [job](const float* s, int n) {
            const size_t idx = job->produced.load(std::memory_order_relaxed);
            if (idx >= job->chunkSlots.size()) return;   // bound guard
            job->chunkSlots[idx].assign(s, s + n);
            job->produced.store(idx + 1, std::memory_order_release);
        };
        auto cancelFn = [&cancel] {
            return cancel.load(std::memory_order_acquire) || bro::util::interrupted();
        };
        brosoundml::AudioBuffer buf;
        if (!job->xvector.empty())
            buf = mw->qwen->synthesize_stream_with_xvector(
                job->text, job->xvector, job->chunkFrames, onChunkCb,
                job->language, cancelFn, job->sampling);
        else
            buf = mw->qwen->synthesize_stream(
                job->text, job->speaker, job->chunkFrames, onChunkCb,
                job->language, job->instruct, cancelFn, job->sampling);
        job->samples     = std::move(buf.samples);
        job->sample_rate = buf.sample_rate;
    };

    auto poll = [job](JSContext* c) {
        if (!job->hasOnChunk) return;
        const size_t n = job->produced.load(std::memory_order_acquire);
        while (job->drained < n) {
            std::vector<float>& chunk = job->chunkSlots[job->drained];
            JSValue arr = qjsbind::make_float32_array(c, chunk);
            JSValue r = JS_Call(c, job->onChunk, JS_UNDEFINED, 1, &arr);
            if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
            JS_FreeValue(c, r);
            JS_FreeValue(c, arr);
            std::vector<float>().swap(chunk);   // release the slot's memory
            job->drained++;
        }
    };

    auto done = [job, mw](JSContext* c, bool cancelled, const std::string& error) {
        mw->busy.release();
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
        if (job->hasOnChunk) JS_FreeValue(c, job->onChunk);
        if (job->hasOnDone)  JS_FreeValue(c, job->onDone);
        JS_FreeValue(c, job->qwenRef);
    };

    return launchAsyncJob(ctx, std::move(work), std::move(poll), std::move(done));
}

// u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500
// Streaming Kokoro synthesis u2014 bro.tts.synthesizeStream(kokoro, chunks, voice, opts)
// u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500
//
// Kokoro is a single non-autoregressive forward pass, so unlike Qwen there is no
// internal point at which a prefix is final. Streaming therefore chunks the
// *input*: the caller hands an array of phoneme-id chunks (split at sentence /
// clause / word boundaries, e.g. on the space token kokoro.vocab()[' ']), each
// chunk is synthesized as an independent forward pass, and its 24 kHz samples are
// emitted via opts.onChunk(Float32Array) the moment that chunk completes u2014 so
// playback can start before the whole script is synthesized. opts.onDone(result,
// info) then fires once with the full concatenated buffer. Same SPSC handoff as
// the Qwen path: the work thread is the sole writer of a committed chunk prefix
// published via an atomic; the JS-thread poll drains it u2014 lock-free, no mutex.
struct KokoroStreamJob {
    std::vector<std::vector<int32_t>> chunks;     // one phoneme-id vector per chunk
    VoiceWrapper*      vw = nullptr;               // borrowed via voiceRef dup
    float              speed = 1.0f;
    std::vector<float> samples;                    // full audio (onDone), filled by work
    int                sample_rate = 24000;
    JSValue            onChunk = JS_UNDEFINED;     // dup'd; UNDEFINED if absent
    JSValue            onDone  = JS_UNDEFINED;
    JSValue            kokoroRef = JS_UNDEFINED;
    JSValue            voiceRef  = JS_UNDEFINED;
    bool               hasOnChunk = false;
    bool               hasOnDone  = false;
    // SPSC handoff: work thread writes chunkSlots[produced] (samples + the
    // chunk's per-phoneme durations) then bumps `produced`; the JS-thread poll
    // reads up to `produced` and advances `drained`.
    std::vector<std::vector<float>>   chunkSlots;
    std::vector<std::vector<int32_t>> durSlots;
    std::atomic<size_t> produced{0};
    size_t              drained = 0;
};

// bro.tts.synthesizeStream(kokoro, phonemeChunks, voice, opts?) -> AsyncHandle
//   phonemeChunks: an array of phoneme-id chunks (each an Int32Array or number[]),
//                  OR a single flat Int32Array/number[] treated as one chunk.
static JSValue js_kokoro_synthesize_stream(JSContext* ctx, int argc,
                                           JSValueConst* argv) {
    auto* w = qjsbind::unwrap<KokoroWrapper>(ctx, argv[0]);  // non-null (caller)
    if (argc < 3)
        return JS_ThrowTypeError(ctx,
            "synthesizeStream(kokoro, phonemeChunks, voice, opts?): kokoro, "
            "phonemeChunks and voice required");

    auto job = std::make_shared<KokoroStreamJob>();

    // phonemeChunks is an array of id-arrays. To stay friendly, a flat id array
    // (first element is a number, not an array) is treated as a single chunk.
    JSValueConst chunksArg = argv[1];
    bool isChunkArray = false;
    if (JS_IsArray(chunksArg)) {
        JSValue first = JS_GetPropertyUint32(ctx, chunksArg, 0);
        size_t firstLen = 0;
        isChunkArray = JS_IsArray(first) ||
                       getInt32Array(ctx, first, firstLen) != nullptr;
        JS_FreeValue(ctx, first);
    }
    if (isChunkArray) {
        std::uint32_t n = 0;
        JSValue lv = JS_GetPropertyStr(ctx, chunksArg, "length");
        JS_ToUint32(ctx, &n, lv);
        JS_FreeValue(ctx, lv);
        for (std::uint32_t i = 0; i < n; ++i) {
            JSValue e = JS_GetPropertyUint32(ctx, chunksArg, i);
            std::vector<int32_t> ids = readIdArray(ctx, e);
            JS_FreeValue(ctx, e);
            if (!ids.empty()) job->chunks.push_back(std::move(ids));  // skip empties
        }
    } else {
        std::vector<int32_t> ids = readIdArray(ctx, chunksArg);
        if (!ids.empty()) job->chunks.push_back(std::move(ids));
    }
    if (job->chunks.empty())
        return JS_ThrowTypeError(ctx,
            "synthesizeStream: phonemeChunks must be a non-empty array of "
            "Int32Array/number[] chunks (or a single id array)");

    auto* vw = qjsbind::unwrap<VoiceWrapper>(ctx, argv[2]);
    if (!vw)
        return JS_ThrowTypeError(ctx,
            "synthesizeStream: voice must be a Voice (returned by loadVoice)");
    job->vw = vw;

    JSValue onChunk = JS_UNDEFINED, onDone = JS_UNDEFINED;
    if (argc >= 4 && JS_IsObject(argv[3])) {
        getNum(ctx, argv[3], "speed", job->speed);
        onChunk = JS_GetPropertyStr(ctx, argv[3], "onChunk");
        onDone  = JS_GetPropertyStr(ctx, argv[3], "onDone");
    }
    // One slot per chunk; the work thread never reallocates while the JS thread
    // reads (each chunk emits exactly once, in order).
    job->chunkSlots.resize(job->chunks.size());
    job->durSlots.resize(job->chunks.size());

    // Claim the model for this synthesis (single-owner; one in flight).
    if (!w->busy.tryClaim()) {
        JS_FreeValue(ctx, onChunk);
        JS_FreeValue(ctx, onDone);
        return JS_ThrowInternalError(ctx,
            "synthesizeStream: an operation is already in flight on this model");
    }

    job->hasOnChunk = JS_IsFunction(ctx, onChunk);
    job->onChunk    = job->hasOnChunk ? JS_DupValue(ctx, onChunk) : JS_UNDEFINED;
    job->hasOnDone  = JS_IsFunction(ctx, onDone);
    job->onDone     = job->hasOnDone ? JS_DupValue(ctx, onDone) : JS_UNDEFINED;
    job->kokoroRef  = JS_DupValue(ctx, argv[0]);
    job->voiceRef   = JS_DupValue(ctx, argv[2]);
    JS_FreeValue(ctx, onChunk);
    JS_FreeValue(ctx, onDone);

    KokoroWrapper* mw = w;

    auto work = [job, mw](const std::atomic<bool>& cancel) {
        brotensor::DeviceScope scope(mw->device);
        // on_chunk runs on THIS (background) thread: copy the samples + the
        // chunk's per-phoneme durations into the next slot and publish it. No JS
        // here u2014 the JS-thread poll fires onChunk.
        auto onChunkCb = [job](const float* s, int n,
                               const int32_t* d, int nd) {
            const size_t idx = job->produced.load(std::memory_order_relaxed);
            if (idx >= job->chunkSlots.size()) return;   // bound guard
            job->chunkSlots[idx].assign(s, s + n);
            if (d && nd > 0) job->durSlots[idx].assign(d, d + nd);
            job->produced.store(idx + 1, std::memory_order_release);
        };
        auto buf = mw->kokoro->synthesize_stream(
            job->chunks, job->vw->voice, onChunkCb, job->speed,
            [&cancel] {
                return cancel.load(std::memory_order_acquire) || bro::util::interrupted();
            });
        job->samples     = std::move(buf.samples);
        job->sample_rate = buf.sample_rate;
    };

    auto poll = [job](JSContext* c) {
        if (!job->hasOnChunk) return;
        const size_t n = job->produced.load(std::memory_order_acquire);
        while (job->drained < n) {
            std::vector<float>&   chunk = job->chunkSlots[job->drained];
            std::vector<int32_t>& dur   = job->durSlots[job->drained];
            // onChunk(samples: Float32Array, durations: Int32Array) u2014 durations is
            // the chunk's per-phoneme frame counts (BOS/EOS-wrapped), so a caller
            // can align words to this chunk's audio precisely.
            JSValue args[2] = { qjsbind::make_float32_array(c, chunk),
                                qjsbind::make_int32_array(c, dur) };
            JSValue r = JS_Call(c, job->onChunk, JS_UNDEFINED, 2, args);
            if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
            JS_FreeValue(c, r);
            JS_FreeValue(c, args[0]);
            JS_FreeValue(c, args[1]);
            std::vector<float>().swap(chunk);      // release the slot's memory
            std::vector<int32_t>().swap(dur);
            job->drained++;
        }
    };

    auto done = [job, mw](JSContext* c, bool cancelled, const std::string& error) {
        mw->busy.release();
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
        if (job->hasOnChunk) JS_FreeValue(c, job->onChunk);
        if (job->hasOnDone)  JS_FreeValue(c, job->onDone);
        JS_FreeValue(c, job->kokoroRef);
        JS_FreeValue(c, job->voiceRef);
    };

    return launchAsyncJob(ctx, std::move(work), std::move(poll), std::move(done));
}

// bro.tts.synthesizeStream(model, ...) u2014 streaming synthesis, dispatched by type.
//   QwenTts:  synthesizeStream(qwen, text, opts?)               u2014 chunks the AR tail
//   Kokoro:   synthesizeStream(kokoro, phonemeChunks, voice, opts?) u2014 chunks the input
static JSValue js_tts_synthesize_stream(JSContext* ctx, JSValueConst,
                                        int argc, JSValueConst* argv) {
    if (argc < 1)
        return JS_ThrowTypeError(ctx,
            "synthesizeStream(model, ...): a Kokoro or QwenTts model is required");
    if (qjsbind::unwrap<QwenTtsWrapper>(ctx, argv[0]))
        return js_qwen_synthesize_stream(ctx, argc, argv);
    if (qjsbind::unwrap<KokoroWrapper>(ctx, argv[0]))
        return js_kokoro_synthesize_stream(ctx, argc, argv);
    return JS_ThrowTypeError(ctx,
        "synthesizeStream: arg 0 must be a Kokoro or QwenTts");
}

static JSValue js_tts_synthesize(JSContext* ctx, JSValueConst,
                                 int argc, JSValueConst* argv) {
    if (argc < 1)
        return JS_ThrowTypeError(ctx,
            "synthesize(model, ...): a Kokoro or QwenTts model is required");

    // Dispatch on model type. QwenTts and Supertonic are text-driven u2014
    // synthesize(model, text, opts?). Kokoro takes phoneme ids u2014
    // synthesize(kokoro, phonemeIds, voice).
    if (qjsbind::unwrap<QwenTtsWrapper>(ctx, argv[0]))
        return js_qwen_synthesize_async(ctx, argc, argv);
    if (qjsbind::unwrap<SupertonicWrapper>(ctx, argv[0]))
        return js_supertonic_synthesize_async(ctx, argc, argv);

    auto* w = qjsbind::unwrap<KokoroWrapper>(ctx, argv[0]);
    if (!w) return JS_ThrowTypeError(ctx,
        "synthesize: arg 0 must be a Kokoro, QwenTts, or Supertonic");
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
        JSValue tv = JS_GetPropertyStr(ctx, argv[3], "trace");
        job->wantTrace = JS_ToBool(ctx, tv) == 1;   // opts.trace: also return stages
        JS_FreeValue(ctx, tv);
        onDone = JS_GetPropertyStr(ctx, argv[3], "onDone");
    }

    // Claim the model for this synthesis (single-owner; one in flight).
    if (!w->busy.tryClaim()) {
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
            [&cancel] {
                return cancel.load(std::memory_order_acquire) || bro::util::interrupted();
            },
            job->wantTrace ? &job->trace : nullptr);
        job->samples     = std::move(buf.samples);
        job->sample_rate = buf.sample_rate;
        job->durations   = std::move(pred_dur);
    };

    // JS thread, once: build { samples, sampleRate, durations } + {cancelled,
    // error}, hand them to onDone, free the dup'd values, release the model.
    auto done = [job, mw](JSContext* c, bool cancelled, const std::string& error) {
        // Release the model BEFORE invoking onDone so the callback can start the
        // next synth on this same model without tripping the single-in-flight
        // guard (e.g. kokoro-lab's fast audio pass immediately chaining a trace
        // pass). The result's host data is already copied off `job`, so a new
        // op claiming the model can't disturb what onDone is reading here.
        mw->busy.release();
        if (job->hasOnDone) {
            JSValue result = JS_NewObject(c);
            JS_SetPropertyStr(c, result, "samples",
                qjsbind::make_float32_array(c, job->samples));
            JS_SetPropertyStr(c, result, "sampleRate",
                JS_NewInt32(c, job->sample_rate));
            JS_SetPropertyStr(c, result, "durations",
                qjsbind::make_int32_array(c, job->durations));
            if (job->wantTrace && !cancelled && error.empty())
                JS_SetPropertyStr(c, result, "stages",
                    traceStagesToJs(c, job->trace));
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
    };

    return launchAsyncJob(ctx, std::move(work), nullptr, std::move(done));
}

// u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550
// Async re-decode u2014 bro.tts.decodeFrom(kokoro, voice, asr, F0, N, nPhonemes, opts)
// u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550
//
// The prosody-editing seam (kokoro.decodeFrom) on a background thread, so the UI
// never blocks while the user scrubs the timing / pitch / energy and we re-decode
// the back half on every change. Same result as the sync method
// ({ samples, sampleRate, stages? }); opts.onDone(result, info) fires once on the
// JS thread, info = { cancelled, error? }. Single-owner like synthesize: a second
// op while one is in flight throws, and .cancel() aborts the in-flight decode.

// Shared between the work thread (sole writer) and the JS thread (sole reader /
// caller of onDone). Held by shared_ptr.
struct DecodeJob {
    VoiceWrapper*           vw = nullptr;             // borrowed via voiceRef dup
    std::vector<float>      asr, F0, N;               // edited back-half inputs
    int                     total = 0;
    int                     nph = 0;
    bool                    wantTrace = false;
    brosoundml::KokoroTrace trace;                    // filled by work() if wantTrace
    std::vector<float>      samples;                  // filled by work()
    int                     sample_rate = 24000;      // filled by work()
    JSValue                 onDone    = JS_UNDEFINED; // dup'd; UNDEFINED if absent
    JSValue                 kokoroRef = JS_UNDEFINED; // dup of the kokoro JS object
    JSValue                 voiceRef  = JS_UNDEFINED; // dup of the voice JS object
    bool                    hasOnDone = false;
};

static JSValue js_kokoro_decodeFrom_async(JSContext* ctx, int argc,
                                          JSValueConst* argv) {
    auto* w = qjsbind::unwrap<KokoroWrapper>(ctx, argv[0]);  // non-null (caller)
    if (argc < 6)
        return JS_ThrowTypeError(ctx, "decodeFrom(kokoro, voice, asr, F0, N, "
            "nPhonemes, opts?): voice, asr, F0, N and nPhonemes required");

    auto* vw = qjsbind::unwrap<VoiceWrapper>(ctx, argv[1]);
    if (!vw)
        return JS_ThrowTypeError(ctx, "decodeFrom: voice must be a Voice");

    // Parse + validate everything BEFORE claiming the model, so an early return
    // never leaks the busy lock.
    auto job = std::make_shared<DecodeJob>();
    job->vw  = vw;
    job->asr = qjsbind::read_float32_array(ctx, argv[2]);
    job->F0  = qjsbind::read_float32_array(ctx, argv[3]);
    job->N   = qjsbind::read_float32_array(ctx, argv[4]);
    if (job->asr.empty() || job->F0.empty() || job->N.empty())
        return JS_ThrowTypeError(ctx,
            "decodeFrom: asr, F0 and N must be non-empty Float32Arrays");
    if (job->F0.size() % 2 != 0)
        return JS_ThrowTypeError(ctx, "decodeFrom: F0 length must be even (2*total)");
    JS_ToInt32(ctx, &job->nph, argv[5]);
    job->total = static_cast<int>(job->F0.size() / 2);

    JSValue onDone = JS_UNDEFINED;
    if (argc >= 7 && JS_IsObject(argv[6])) {
        JSValue tv = JS_GetPropertyStr(ctx, argv[6], "trace");
        job->wantTrace = JS_ToBool(ctx, tv) > 0;
        JS_FreeValue(ctx, tv);
        onDone = JS_GetPropertyStr(ctx, argv[6], "onDone");
    }

    // Claim the model for this decode (single-owner; one in flight).
    if (!w->busy.tryClaim()) {
        JS_FreeValue(ctx, onDone);
        return JS_ThrowInternalError(ctx,
            "decodeFrom: an operation is already in flight on this model");
    }

    job->hasOnDone  = JS_IsFunction(ctx, onDone);
    job->onDone     = job->hasOnDone ? JS_DupValue(ctx, onDone) : JS_UNDEFINED;
    job->kokoroRef  = JS_DupValue(ctx, argv[0]);  // keep the model alive
    job->voiceRef   = JS_DupValue(ctx, argv[1]);  // keep the voice alive
    JS_FreeValue(ctx, onDone);

    KokoroWrapper* mw = w;

    // Background thread: re-decode the back half from the edited asr/F0/N. A
    // barge-in flips the cancel flag; decode_from polls it between stages and in
    // the generator's upsample loop, returning an empty buffer so the GPU stops
    // and `busy` is released promptly (the cancelled result is discarded).
    auto work = [job, mw](const std::atomic<bool>& cancel) {
        brotensor::DeviceScope scope(mw->device);
        auto buf = mw->kokoro->decode_from(
            job->vw->voice, job->nph, job->asr, job->total, job->F0, job->N,
            [&cancel] {
                return cancel.load(std::memory_order_acquire) || bro::util::interrupted();
            },
            job->wantTrace ? &job->trace : nullptr);
        job->samples     = std::move(buf.samples);
        job->sample_rate = buf.sample_rate;
    };

    auto done = [job, mw](JSContext* c, bool cancelled, const std::string& error) {
        // Release the model BEFORE onDone so the callback can immediately launch
        // the next decode of the latest edit without tripping the in-flight guard.
        mw->busy.release();
        if (job->hasOnDone) {
            JSValue result = JS_NewObject(c);
            JS_SetPropertyStr(c, result, "samples",
                qjsbind::make_float32_array(c, job->samples));
            JS_SetPropertyStr(c, result, "sampleRate",
                JS_NewInt32(c, job->sample_rate));
            if (job->wantTrace && !cancelled && error.empty())
                JS_SetPropertyStr(c, result, "stages",
                    traceStagesToJs(c, job->trace));
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
    };

    return launchAsyncJob(ctx, std::move(work), nullptr, std::move(done));
}

static JSValue js_tts_decodeFrom(JSContext* ctx, JSValueConst,
                                 int argc, JSValueConst* argv) {
    if (argc < 1 || !qjsbind::unwrap<KokoroWrapper>(ctx, argv[0]))
        return JS_ThrowTypeError(ctx,
            "decodeFrom(kokoro, voice, asr, F0, N, nPhonemes, opts?): "
            "a Kokoro model is required");
    return js_kokoro_decodeFrom_async(ctx, argc, argv);
}

// u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550
// Multi-voice / multi-stream sessions u2014 model.createSession() + session.*
// u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550u2550
//
// One loaded model behind N per-stream sessions over ONE shared weight set u2014 the
// TTS analog of N wake detectors on one shared net (give N NPCs distinct voices
// without N weight copies). session.synthesize(...) reuses the exact async-job
// dispatch as the module-level bro.tts.synthesize(model, ...) (real cancellation,
// onDone(result, info)) but drives the session's own scratch and claims the
// model's SHARED busy gate. brosoundml's tier here is SHARED WEIGHTS / SERIALIZED
// synthesis (one GPU stream, shared captured graph), so calls over one model u2014
// module-level and any session u2014 never overlap: a second in-flight op throws.
// Sessions isolate the VOICE / AR scratch, not parallel execution; drive them
// from one synth worker / queue (the NPC turn-taking pattern). Output is
// bit-identical to the same call on a fresh model.

// u2500u2500 Kokoro session (bound voice) u2500u2500
static KokoroSessionWrapper* kokoroSessionSelf(JSContext* ctx, JSValueConst v) {
    return qjsbind::unwrap<KokoroSessionWrapper>(ctx, v);
}

// kokoro.createSession(voice) -> KokoroSession (an NPC speaking handle)
static JSValue js_kokoro_createSession(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv) {
    auto* w = kokoroSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "createSession: not a Kokoro");
    if (!w->kokoro || !w->kokoro->loaded())
        return JS_ThrowInternalError(ctx, "createSession: model is not loaded");
    auto* vw = (argc >= 1) ? qjsbind::unwrap<VoiceWrapper>(ctx, argv[0]) : nullptr;
    if (!vw)
        return JS_ThrowTypeError(ctx,
            "createSession(voice): voice must be a Voice (loadVoice/createVoice)");
    try {
        brotensor::DeviceScope scope(w->device);
        auto sw = std::make_unique<KokoroSessionWrapper>();
        sw->busy    = w->busy;
        sw->device  = w->device;
        // KokoroSession holds the model by shared_ptr<const Kokoro> internally.
        sw->session = std::make_unique<brosoundml::KokoroSession>(w->kokoro,
                                                                  vw->voice);
        return qjsbind::wrap<KokoroSessionWrapper>(ctx, sw.release());
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "createSession: %s", e.what());
    }
}

// session.synthesize(phonemeIds, opts?) -> AsyncHandle ({ samples, sampleRate,
// durations, stages? }, info). The bound voice is supplied for you.
static JSValue js_kokoro_session_synthesize(JSContext* ctx, JSValueConst this_val,
                                            int argc, JSValueConst* argv) {
    auto* sw = kokoroSessionSelf(ctx, this_val);
    if (!sw) return JS_ThrowTypeError(ctx, "synthesize: not a KokoroSession");
    if (argc < 1)
        return JS_ThrowTypeError(ctx,
            "synthesize(phonemeIds, opts?): phonemeIds required");

    auto job = std::make_shared<TtsJob>();
    job->ids = readIdArray(ctx, argv[0]);
    if (job->ids.empty())
        return JS_ThrowTypeError(ctx,
            "synthesize: phonemeIds must be a non-empty Int32Array or number[]");

    JSValue onDone = JS_UNDEFINED;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        getNum(ctx, argv[1], "speed", job->speed);
        JSValue tv = JS_GetPropertyStr(ctx, argv[1], "trace");
        job->wantTrace = JS_ToBool(ctx, tv) == 1;
        JS_FreeValue(ctx, tv);
        onDone = JS_GetPropertyStr(ctx, argv[1], "onDone");
    }

    if (!sw->busy.tryClaim()) {
        JS_FreeValue(ctx, onDone);
        return JS_ThrowInternalError(ctx,
            "synthesize: an operation is already in flight on this model "
            "(sessions over one model serialize u2014 drive them from one queue)");
    }

    job->hasOnDone = JS_IsFunction(ctx, onDone);
    job->onDone    = job->hasOnDone ? JS_DupValue(ctx, onDone) : JS_UNDEFINED;
    job->kokoroRef = JS_DupValue(ctx, this_val);  // keep the session (+ model + voice) alive
    job->voiceRef  = JS_UNDEFINED;                 // voice is owned by the session
    JS_FreeValue(ctx, onDone);

    KokoroSessionWrapper* mw = sw;

    auto work = [job, mw](const std::atomic<bool>& cancel) {
        brotensor::DeviceScope scope(mw->device);
        std::vector<int32_t> pred_dur;
        auto buf = mw->session->synthesize(
            job->ids, job->speed, &pred_dur,
            [&cancel] {
                return cancel.load(std::memory_order_acquire) || bro::util::interrupted();
            },
            job->wantTrace ? &job->trace : nullptr);
        job->samples     = std::move(buf.samples);
        job->sample_rate = buf.sample_rate;
        job->durations   = std::move(pred_dur);
    };

    auto done = [job, mw](JSContext* c, bool cancelled, const std::string& error) {
        mw->busy.release();
        if (job->hasOnDone) {
            JSValue result = JS_NewObject(c);
            JS_SetPropertyStr(c, result, "samples",
                qjsbind::make_float32_array(c, job->samples));
            JS_SetPropertyStr(c, result, "sampleRate",
                JS_NewInt32(c, job->sample_rate));
            JS_SetPropertyStr(c, result, "durations",
                qjsbind::make_int32_array(c, job->durations));
            if (job->wantTrace && !cancelled && error.empty())
                JS_SetPropertyStr(c, result, "stages",
                    traceStagesToJs(c, job->trace));
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
    };

    return launchAsyncJob(ctx, std::move(work), nullptr, std::move(done));
}

// session.setVoice(voice) u2014 re-skin this NPC without touching shared weights.
static JSValue js_kokoro_session_setVoice(JSContext* ctx, JSValueConst this_val,
                                          int argc, JSValueConst* argv) {
    auto* sw = kokoroSessionSelf(ctx, this_val);
    if (!sw) return JS_ThrowTypeError(ctx, "setVoice: not a KokoroSession");
    auto* vw = (argc >= 1) ? qjsbind::unwrap<VoiceWrapper>(ctx, argv[0]) : nullptr;
    if (!vw)
        return JS_ThrowTypeError(ctx, "setVoice(voice): voice must be a Voice");
    if (sw->busy.isBusy())
        return JS_ThrowInternalError(ctx, "setVoice: a synthesis is in flight on this model");
    sw->session->set_voice(vw->voice);
    return JS_UNDEFINED;
}

static void registerKokoroSessionClass(JSContext* ctx) {
    qjsbind::Class<KokoroSessionWrapper>(ctx, "KokoroSession", qjsbind::NoGlobal)
        .method_raw("synthesize", js_kokoro_session_synthesize, 2)
        .method_raw("setVoice",   js_kokoro_session_setVoice,   1);
}

// u2500u2500 QwenTts session u2500u2500
static QwenTtsSessionWrapper* qwenSessionSelf(JSContext* ctx, JSValueConst v) {
    return qjsbind::unwrap<QwenTtsSessionWrapper>(ctx, v);
}

// qwen.createSession() -> QwenTtsSession (own Talker + Code Predictor scratch)
static JSValue js_qwen_createSession(JSContext* ctx, JSValueConst this_val,
                                     int, JSValueConst*) {
    auto* w = qwenSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "createSession: not a QwenTts");
    if (!w->qwen || !w->qwen->loaded())
        return JS_ThrowInternalError(ctx, "createSession: model is not loaded");
    try {
        brotensor::DeviceScope scope(w->device);
        auto sw = std::make_unique<QwenTtsSessionWrapper>();
        sw->model   = w->qwen;
        sw->busy    = w->busy;
        sw->device  = w->device;
        sw->session = w->qwen->make_session();
        return qjsbind::wrap<QwenTtsSessionWrapper>(ctx, sw.release());
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "createSession: %s", e.what());
    }
}

// session.synthesize(text, opts?) -> AsyncHandle ({ samples, sampleRate, stages? },
// info). opts: speaker/language/instruct + sampling controls (incl. voiceSteer /
// speakerVector for designed voices) + xvector (Base designer). Same opts as the
// module-level bro.tts.synthesize(qwen, ...).
static JSValue js_qwen_session_synthesize(JSContext* ctx, JSValueConst this_val,
                                          int argc, JSValueConst* argv) {
    auto* sw = qwenSessionSelf(ctx, this_val);
    if (!sw) return JS_ThrowTypeError(ctx, "synthesize: not a QwenTtsSession");
    std::string text;
    if (argc < 1 || !argStr(ctx, argv[0], text))
        return JS_ThrowTypeError(ctx,
            "synthesize(text, opts?): text string required");

    auto job = std::make_shared<QwenSynthJob>();
    job->text = std::move(text);

    JSValue onDone = JS_UNDEFINED;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        readQwenSynthOpts(ctx, argv[1], job->speaker, job->language, job->instruct);
        readQwenSampling(ctx, argv[1], job->sampling);
        job->wantTrace = getBool(ctx, argv[1], "trace");
        JSValue xv = JS_GetPropertyStr(ctx, argv[1], "xvector");
        if (!JS_IsUndefined(xv) && !JS_IsNull(xv))
            job->xvector = qjsbind::read_float32_array(ctx, xv);
        JS_FreeValue(ctx, xv);
        onDone = JS_GetPropertyStr(ctx, argv[1], "onDone");
    }

    if (!sw->busy.tryClaim()) {
        JS_FreeValue(ctx, onDone);
        return JS_ThrowInternalError(ctx,
            "synthesize: an operation is already in flight on this model "
            "(sessions over one model serialize u2014 drive them from one queue)");
    }

    job->hasOnDone = JS_IsFunction(ctx, onDone);
    job->onDone    = job->hasOnDone ? JS_DupValue(ctx, onDone) : JS_UNDEFINED;
    job->qwenRef   = JS_DupValue(ctx, this_val);  // keep the session (+ model) alive
    JS_FreeValue(ctx, onDone);

    QwenTtsSessionWrapper* mw = sw;

    auto work = [job, mw](const std::atomic<bool>& cancel) {
        brotensor::DeviceScope scope(mw->device);
        auto cancelFn = [&cancel] {
            return cancel.load(std::memory_order_acquire) || bro::util::interrupted();
        };
        brosoundml::AudioBuffer buf;
        if (!job->xvector.empty())
            buf = mw->model->synthesize_with_xvector(
                mw->session, job->text, job->xvector, job->language, cancelFn,
                job->sampling, job->wantTrace ? &job->trace : nullptr);
        else
            buf = mw->model->synthesize(
                mw->session, job->text, job->speaker, job->language, job->instruct,
                cancelFn, job->sampling, job->wantTrace ? &job->trace : nullptr);
        job->samples     = std::move(buf.samples);
        job->sample_rate = buf.sample_rate;
    };

    auto done = [job, mw](JSContext* c, bool cancelled, const std::string& error) {
        mw->busy.release();
        if (job->hasOnDone) {
            JSValue result = JS_NewObject(c);
            JS_SetPropertyStr(c, result, "samples",
                qjsbind::make_float32_array(c, job->samples));
            JS_SetPropertyStr(c, result, "sampleRate",
                JS_NewInt32(c, job->sample_rate));
            if (job->wantTrace)
                JS_SetPropertyStr(c, result, "stages", qwenTraceToJs(c, job->trace));
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
    };

    return launchAsyncJob(ctx, std::move(work), nullptr, std::move(done));
}

// session.reset() u2014 zero the session's AR scratch (drops its captured graphs).
static JSValue js_qwen_session_reset(JSContext* ctx, JSValueConst this_val,
                                     int, JSValueConst*) {
    auto* sw = qwenSessionSelf(ctx, this_val);
    if (!sw) return JS_ThrowTypeError(ctx, "reset: not a QwenTtsSession");
    if (sw->busy.isBusy())
        return JS_ThrowInternalError(ctx, "reset: a synthesis is in flight on this model");
    try {
        brotensor::DeviceScope scope(sw->device);
        sw->model->reset(sw->session);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "reset: %s", e.what());
    }
    return JS_UNDEFINED;
}

static void registerQwenSessionClass(JSContext* ctx) {
    qjsbind::Class<QwenTtsSessionWrapper>(ctx, "QwenTtsSession", qjsbind::NoGlobal)
        .get("loaded",  [](QwenTtsSessionWrapper* w) { return w->model && w->model->loaded(); })
        .get("variant", [](QwenTtsSessionWrapper* w) {
            return std::string(qwenVariantName(w->model->config().variant)); })
        .method_raw("synthesize", js_qwen_session_synthesize, 2)
        .method_raw("reset",      js_qwen_session_reset,      0);
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void installTtsBindings(JSContext* ctx) {
    registerVoiceClass(ctx);
        registerKokoroClass(ctx);
        registerQwenClass(ctx);
        registerSupertonicVoiceClass(ctx);
        registerSupertonicClass(ctx);
        registerSpeakerEncoderClass(ctx);
        registerKokoroSessionClass(ctx);
        registerQwenSessionClass(ctx);
    
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
        JS_SetPropertyStr(ctx, tts, "loadSupertonic",
            JS_NewCFunction(ctx, js_loadSupertonic, "loadSupertonic", 2));
        JS_SetPropertyStr(ctx, tts, "loadSpeakerEncoder",
            JS_NewCFunction(ctx, js_loadSpeakerEncoder, "loadSpeakerEncoder", 2));
        JS_SetPropertyStr(ctx, tts, "phonemize",
            JS_NewCFunction(ctx, js_phonemize, "phonemize", 2));
        JS_SetPropertyStr(ctx, tts, "setAssetRoot",
            JS_NewCFunction(ctx, js_setAssetRoot, "setAssetRoot", 1));
        JS_SetPropertyStr(ctx, tts, "setAssets",
            JS_NewCFunction(ctx, js_setAssets, "setAssets", 1));
        JS_SetPropertyStr(ctx, tts, "synthesize",
            JS_NewCFunction(ctx, js_tts_synthesize, "synthesize", 4));
        JS_SetPropertyStr(ctx, tts, "synthesizeStream",
            JS_NewCFunction(ctx, js_tts_synthesize_stream, "synthesizeStream", 4));
        JS_SetPropertyStr(ctx, tts, "decodeFrom",
            JS_NewCFunction(ctx, js_tts_decodeFrom, "decodeFrom", 7));
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


} // namespace bro::js

#endif  // BRO_WITH_SOUNDML
