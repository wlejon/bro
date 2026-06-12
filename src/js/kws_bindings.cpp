// JS bindings for brosoundml::PhonemeSpotter — open-vocabulary streaming
// keyword spotting.
//
// Installed onto bro.kws.* by installKwsBindings(). One spotter at a time per
// JS context, holding any number of enrolled phrase templates.
//
// bro.kws is the open-vocab sibling of bro.wake: where bro.wake runs one
// trained-in keyword logit, bro.kws aligns enrolled phoneme-sequence templates
// (from bro.tts.phonemize ids, raw class ids, or reference audio) against
// PhonemeNet's streaming per-frame posteriors and fires named events.
//
// The audio plumbing lives in the shared listen host (listen_host.h): ONE raw
// no-AGC mic tap + ring + inference task drive a brosoundml::ListenBus that
// this binding's spotter joins as a member (alongside bro.sense's SensorHub),
// so the whole stack costs one PCEN feature pass and one PhonemeNet forward.
// Three concerns on three threads:
//
//   - PRODUCER (real-time audio thread): the host's tap callback copies
//     samples into the shared lock-free SPSC ring; nothing else.
//   - INFERENCE thread (engine::AudioInference worker; or, headless, the
//     calling thread): the host's task drains the ring, runs the bus (mel →
//     PhonemeSpotter::feed_mel), and hands fired events to this binding's
//     onSpots hook, which publishes them into a fixed SPSC slot ring
//     (template index + confidence — no strings cross threads).
//   - MAIN thread: tickKws() drains the slots and invokes the JS onSpot
//     callback, so onSpot always runs single-threaded with the rest of the app.
//
// Single-producer discipline: PhonemeSpotter's mutators (enroll / remove /
// clear / reset) share a thread with feed(). While listening, feed() runs on
// the inference thread, so the binding rejects mutators until stop() — enroll
// first, then listen. The lock-free cross-thread readers the library provides
// (prefix_progress) stay available while live.
//
// Lifetime: unlike bro.wake, the spotter must SURVIVE stop() so templates can
// be re-enrolled or listening resumed without reloading weights — the binding
// keeps its shared_ptr across stop() and only drops it on unload()/cleanup.
// The inference task's closure holds a second ref while listening.

#include "js/kws_bindings.h"

#include "audio_inference/audio_inference.h"
#include "js/listen_host.h"

#include <broaudio/engine.h>
#include <broaudio/mic_tap.h>
#include <brosoundml/phoneme_spotter.h>
#include <brotensor/runtime.h>
#include <brotensor/tensor.h>

#include <quickjs.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace bro::js {

namespace {

using engine::AudioInference;

// Fired events cross inference -> main as (template index, confidence) pairs
// in a fixed SPSC slot ring. 64 slots is generous — spot events arrive at
// human speech cadence; on overflow the newest event is dropped.
constexpr std::uint64_t kEventSlots = 64;

struct KwsState {
    broaudio::Engine* audioEngine = nullptr;
    AudioInference*   inference   = nullptr;
    JSContext*        ctx         = nullptr;

    // The spotter. Owned by the binding across load()/stop(); the inference
    // task closure holds a second strong ref while listening.
    std::shared_ptr<brosoundml::PhonemeSpotter> spotter;
    brotensor::Device device = brotensor::Device::CPU;

    // Template-name snapshot taken at listen() — index i names eventIdx[i].
    // The enrolled set cannot change while listening, so the snapshot is
    // stable for the task's lifetime. Main thread reads it in tickKws.
    std::vector<std::string> names;

    JSValue onSpot = JS_UNDEFINED;

    // SPSC event ring: the inference thread writes slot produced%kEventSlots
    // then bumps produced; tickKws (main) drains up to produced and advances
    // drained. The producer drops events when the ring is full.
    int                        eventIdx[kEventSlots]  = {};
    float                      eventConf[kEventSlots] = {};
    std::atomic<std::uint64_t> produced{0};
    std::atomic<std::uint64_t> drained{0};

    // Gates onSpot delivery, never audio processing (the posterior stream and
    // every template's DP state keep rolling so resume never faces a cold
    // matcher). Written by suspend()/resume() (main), read by the inference
    // thread.
    std::atomic<bool> suspended{false};

    bool listening = false;
};

KwsState g_kws;

// ─── Helpers (same shapes as wake_bindings) ─────────────────────────────────

bool getStr(JSContext* ctx, JSValueConst obj, const char* key, std::string& out) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    bool ok = false;
    if (JS_IsString(v)) {
        const char* s = JS_ToCString(ctx, v);
        if (s) { out = s; JS_FreeCString(ctx, s); ok = true; }
    }
    JS_FreeValue(ctx, v);
    return ok;
}

bool getNum(JSContext* ctx, JSValueConst obj, const char* key, double& dst) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    bool ok = false;
    if (JS_IsNumber(v)) { JS_ToFloat64(ctx, &dst, v); ok = true; }
    JS_FreeValue(ctx, v);
    return ok;
}

bool getInt(JSContext* ctx, JSValueConst obj, const char* key, int& dst) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    bool ok = false;
    if (JS_IsNumber(v)) {
        int32_t t = dst;
        JS_ToInt32(ctx, &t, v);
        dst = t;
        ok = true;
    }
    JS_FreeValue(ctx, v);
    return ok;
}

brotensor::Device autoDevice() {
    if (brotensor::is_available(brotensor::Device::CUDA))  return brotensor::Device::CUDA;
    if (brotensor::is_available(brotensor::Device::Metal)) return brotensor::Device::Metal;
    return brotensor::Device::CPU;
}

bool parseDeviceOpt(JSContext* ctx, JSValueConst opts,
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
    if (sv == "cpu")   { out = brotensor::Device::CPU;   return true; }
    if (sv == "cuda")  { out = brotensor::Device::CUDA;  return true; }
    if (sv == "metal") { out = brotensor::Device::Metal; return true; }
    err = "opts.device must be 'cpu', 'cuda', or 'metal' (got '" + sv + "')";
    return false;
}

const char* deviceName(brotensor::Device d) {
    switch (d) {
        case brotensor::Device::CUDA:  return "CUDA";
        case brotensor::Device::Metal: return "Metal";
        case brotensor::Device::CPU:   return "CPU";
    }
    return "?";
}

// Overlay detector-policy keys present on `obj` onto `cfg`:
//   threshold, refractoryMs, minPhonemes, entrySilenceFrames, emissionFloor,
//   enrollGaps, gapMinFrames, gapTolerance, smoothing: { hits, window }.
// Used for the global defaults (load) and per-template overrides (enroll).
void readPolicy(JSContext* ctx, JSValueConst obj, brosoundml::SpotterConfig& cfg) {
    if (!JS_IsObject(obj)) return;
    double d;
    if (getNum(ctx, obj, "threshold", d))     cfg.threshold      = (float)d;
    if (getNum(ctx, obj, "emissionFloor", d)) cfg.emission_floor = (float)d;
    if (getNum(ctx, obj, "gapTolerance", d))  cfg.gap_tolerance  = (float)d;
    getInt(ctx, obj, "refractoryMs",       cfg.refractory_ms);
    getInt(ctx, obj, "minPhonemes",        cfg.min_phonemes);
    getInt(ctx, obj, "entrySilenceFrames", cfg.entry_silence_frames);
    getInt(ctx, obj, "gapMinFrames",       cfg.gap_min_frames);
    JSValue gv = JS_GetPropertyStr(ctx, obj, "enrollGaps");
    if (!JS_IsUndefined(gv) && !JS_IsNull(gv))
        cfg.enroll_gaps = JS_ToBool(ctx, gv) > 0;
    JS_FreeValue(ctx, gv);
    JSValue sm = JS_GetPropertyStr(ctx, obj, "smoothing");
    if (JS_IsObject(sm)) {
        getInt(ctx, sm, "hits",   cfg.smoothing_hits);
        getInt(ctx, sm, "window", cfg.smoothing_window);
    }
    JS_FreeValue(ctx, sm);
}

bool readPolicyOverride(JSContext* ctx, JSValueConst obj,
                        const brosoundml::PhonemeSpotter& spotter,
                        brosoundml::SpotterConfig& out) {
    if (!JS_IsObject(obj)) return false;
    out = spotter.config();
    readPolicy(ctx, obj, out);
    return true;
}

// Read int ids from an Int32Array or number[].
std::vector<int> readIds(JSContext* ctx, JSValueConst v) {
    std::vector<int> out;
    size_t byteOff = 0, viewLen = 0, bpe = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, v, &byteOff, &viewLen, &bpe);
    if (!JS_IsException(abuf)) {
        size_t abufLen = 0;
        std::uint8_t* p = JS_GetArrayBuffer(ctx, &abufLen, abuf);
        JS_FreeValue(ctx, abuf);
        if (p && bpe == sizeof(int32_t)) {
            const auto* ids = reinterpret_cast<const int32_t*>(p + byteOff);
            out.assign(ids, ids + viewLen / sizeof(int32_t));
            return out;
        }
    } else {
        JS_FreeValue(ctx, JS_GetException(ctx));
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

// Read a Float32Array's element pointer + count (nullptr if not one).
const float* readFloats(JSContext* ctx, JSValueConst v, int& count) {
    count = 0;
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
    count = static_cast<int>(viewLen / sizeof(float));
    return reinterpret_cast<const float*>(p + byteOff);
}

// Publish one fired event into the SPSC ring (inference thread). Drops on
// overflow — a stalled main thread loses the oldest unprocessed... rather,
// the newest event, never corrupts the ring.
void publishEvent(int nameIdx, float confidence) {
    const std::uint64_t p = g_kws.produced.load(std::memory_order_relaxed);
    if (p - g_kws.drained.load(std::memory_order_acquire) >= kEventSlots) return;
    g_kws.eventIdx[p % kEventSlots]  = nameIdx;
    g_kws.eventConf[p % kEventSlots] = confidence;
    g_kws.produced.store(p + 1, std::memory_order_release);
}

int nameIndexOf(const std::vector<std::string>& names, const std::string& n) {
    for (std::size_t i = 0; i < names.size(); ++i)
        if (names[i] == n) return static_cast<int>(i);
    return -1;
}

void stopListening() {
    if (!g_kws.listening) return;
    // Detach from the shared listen host. The host replaces (or tears down)
    // the inference task; if bro.sense is still a member, its stream keeps
    // rolling untouched.
    listenHostSetSpotter(nullptr, brotensor::Device::CPU, nullptr);
    if (g_kws.ctx && !JS_IsUndefined(g_kws.onSpot)) {
        JS_FreeValue(g_kws.ctx, g_kws.onSpot);
        g_kws.onSpot = JS_UNDEFINED;
    }
    g_kws.produced.store(0, std::memory_order_relaxed);
    g_kws.drained.store(0, std::memory_order_relaxed);
    g_kws.suspended.store(false, std::memory_order_relaxed);
    g_kws.names.clear();
    g_kws.listening = false;
}

void unloadSpotter() {
    stopListening();
    g_kws.spotter.reset();
}

// Reject single-producer mutators while the inference thread owns feed().
JSValue throwIfListening(JSContext* ctx, const char* what) {
    if (!g_kws.listening) return JS_UNDEFINED;
    return JS_ThrowInternalError(ctx,
        "bro.kws.%s: not allowed while listening (enroll/remove/clear/reset "
        "share the spotter's feed thread — call bro.kws.stop() first)", what);
}

// ─── JS-callable functions ─────────────────────────────────────────────────

// bro.kws.load({ weights, device?, threshold?, refractoryMs?, smoothing?,
//                minPhonemes?, entrySilenceFrames?, emissionFloor?,
//                enrollGaps?, gapMinFrames?, gapTolerance? })
// Load the PhonemeNet checkpoint (+ its embedded class map) and set the
// global detector-policy defaults. Enroll templates next, then listen().
JSValue js_load(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx,
            "bro.kws.load(opts): opts object required (weights, ...)");
    }
    JSValue guard = throwIfListening(ctx, "load");
    if (JS_IsException(guard)) return guard;

    std::string weights;
    if (!getStr(ctx, argv[0], "weights", weights) || weights.empty()) {
        return JS_ThrowTypeError(ctx,
            "bro.kws.load: opts.weights (PhonemeNet checkpoint path) required");
    }
    // init() BEFORE the device probe — the GPU backends only register on the
    // driver probe, so autoDevice() before init() silently lands on CPU.
    try {
        brotensor::init();
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "bro.kws.load: %s", e.what());
    }
    brotensor::Device dev = autoDevice();
    {
        std::string err;
        if (!parseDeviceOpt(ctx, argv[0], dev, err))
            return JS_ThrowTypeError(ctx, "bro.kws.load: %s", err.c_str());
    }

    try {
        auto spotter = std::make_shared<brosoundml::PhonemeSpotter>();
        {
            brotensor::DeviceScope scope(dev);
            spotter->load(weights, dev);
        }
        brosoundml::SpotterConfig cfg = spotter->config();
        readPolicy(ctx, argv[0], cfg);
        spotter->set_config(cfg);

        g_kws.spotter = std::move(spotter);
        g_kws.device  = dev;
        std::fprintf(stderr,
            "[INFO] [kws] PhonemeSpotter loaded on %s (K=%d, %d Hz, threshold=%.2f)\n",
            deviceName(dev), g_kws.spotter->class_map().num_classes,
            g_kws.spotter->sample_rate(), cfg.threshold);
        return JS_UNDEFINED;
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "bro.kws.load: %s", e.what());
    }
}

JSValue js_unload(JSContext*, JSValueConst, int, JSValueConst*) {
    unloadSpotter();
    return JS_UNDEFINED;
}

// bro.kws.enroll(name, phonemeIds, policy?) -> template length
//   phonemeIds: Int32Array/number[] of Kokoro phoneme ids — exactly what
//   bro.tts.phonemize(text) returns. Silence/suprasegmental ids are dropped
//   and duplicate adjacent classes collapsed by the library.
JSValue js_enroll(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!g_kws.spotter)
        return JS_ThrowInternalError(ctx, "bro.kws.enroll: call bro.kws.load first");
    JSValue guard = throwIfListening(ctx, "enroll");
    if (JS_IsException(guard)) return guard;
    std::string name;
    if (argc < 2 || !JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx,
            "bro.kws.enroll(name, phonemeIds, policy?): name and ids required");
    }
    getStrArg: {
        const char* s = JS_ToCString(ctx, argv[0]);
        if (!s) return JS_EXCEPTION;
        name = s;
        JS_FreeCString(ctx, s);
    }
    std::vector<int> ids = readIds(ctx, argv[1]);
    if (ids.empty()) {
        return JS_ThrowTypeError(ctx,
            "bro.kws.enroll: phonemeIds must be a non-empty Int32Array or "
            "number[] (use bro.tts.phonemize(text))");
    }
    try {
        brosoundml::SpotterConfig pol;
        const bool hasPol = (argc >= 3) && readPolicyOverride(ctx, argv[2],
                                                              *g_kws.spotter, pol);
        const int len = g_kws.spotter->enroll(name, ids, hasPol ? &pol : nullptr);
        if (len <= 0) {
            return JS_ThrowInternalError(ctx,
                "bro.kws.enroll: '%s' produced an empty template (only "
                "silence/suprasegmental ids?)", name.c_str());
        }
        return JS_NewInt32(ctx, len);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "bro.kws.enroll: %s", e.what());
    }
}

// bro.kws.enrollFromAudio(name, samples, policy?) -> template length
//   samples: Float32Array of mono PCM at bro.kws.sampleRate() — runs the model
//   over the reference audio and uses the argmax class sequence as the template.
JSValue js_enrollFromAudio(JSContext* ctx, JSValueConst, int argc,
                           JSValueConst* argv) {
    if (!g_kws.spotter || !g_kws.spotter->loaded())
        return JS_ThrowInternalError(ctx,
            "bro.kws.enrollFromAudio: call bro.kws.load first");
    JSValue guard = throwIfListening(ctx, "enrollFromAudio");
    if (JS_IsException(guard)) return guard;
    if (argc < 2 || !JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx,
            "bro.kws.enrollFromAudio(name, samples, policy?): name and samples "
            "required");
    }
    const char* s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_EXCEPTION;
    std::string name = s;
    JS_FreeCString(ctx, s);

    int n = 0;
    const float* p = readFloats(ctx, argv[1], n);
    if (!p || n <= 0) {
        return JS_ThrowTypeError(ctx,
            "bro.kws.enrollFromAudio: samples must be a non-empty Float32Array");
    }
    try {
        brosoundml::SpotterConfig pol;
        const bool hasPol = (argc >= 3) && readPolicyOverride(ctx, argv[2],
                                                              *g_kws.spotter, pol);
        brotensor::DeviceScope scope(g_kws.device);
        const int len = g_kws.spotter->enroll_from_audio(name, p, n,
                                                         hasPol ? &pol : nullptr);
        if (len <= 0) {
            return JS_ThrowInternalError(ctx,
                "bro.kws.enrollFromAudio: '%s' produced an empty template "
                "(silence-only audio?)", name.c_str());
        }
        return JS_NewInt32(ctx, len);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "bro.kws.enrollFromAudio: %s", e.what());
    }
}

JSValue js_remove(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!g_kws.spotter)
        return JS_ThrowInternalError(ctx, "bro.kws.remove: nothing loaded");
    JSValue guard = throwIfListening(ctx, "remove");
    if (JS_IsException(guard)) return guard;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "bro.kws.remove(name): name required");
    const char* s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_EXCEPTION;
    const bool removed = g_kws.spotter->remove(s);
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, removed);
}

JSValue js_clear(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (!g_kws.spotter) return JS_UNDEFINED;
    JSValue guard = throwIfListening(ctx, "clear");
    if (JS_IsException(guard)) return guard;
    g_kws.spotter->clear();
    return JS_UNDEFINED;
}

JSValue js_templates(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue arr = JS_NewArray(ctx);
    // While listening, return the snapshot (the live template list shares the
    // feed thread); otherwise read the spotter directly.
    const std::vector<std::string> names =
        g_kws.listening ? g_kws.names
        : (g_kws.spotter ? g_kws.spotter->templates()
                         : std::vector<std::string>{});
    for (std::uint32_t i = 0; i < names.size(); ++i) {
        JS_SetPropertyUint32(ctx, arr, i, JS_NewString(ctx, names[i].c_str()));
    }
    return arr;
}

JSValue js_reset(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (!g_kws.spotter) return JS_UNDEFINED;
    JSValue guard = throwIfListening(ctx, "reset");
    if (JS_IsException(guard)) return guard;
    g_kws.spotter->reset();
    return JS_UNDEFINED;
}

// bro.kws.listen({ onSpot }) — start live spotting on the mic. Requires a
// loaded spotter with at least one enrolled template.
JSValue js_listen(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!g_kws.audioEngine)
        return JS_ThrowInternalError(ctx, "bro.kws.listen: audio engine not available");
    if (!g_kws.inference)
        return JS_ThrowInternalError(ctx,
            "bro.kws.listen: audio-inference subsystem not available");
    if (!g_kws.spotter || !g_kws.spotter->loaded())
        return JS_ThrowInternalError(ctx, "bro.kws.listen: call bro.kws.load first");
    if (g_kws.listening)
        return JS_ThrowInternalError(ctx,
            "bro.kws.listen: already listening (bro.kws.stop() first)");
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "bro.kws.listen(opts): opts object required");

    JSValue onSpotVal = JS_GetPropertyStr(ctx, argv[0], "onSpot");
    if (!JS_IsFunction(ctx, onSpotVal)) {
        JS_FreeValue(ctx, onSpotVal);
        return JS_ThrowTypeError(ctx,
            "bro.kws.listen: opts.onSpot (function) required");
    }

    const std::vector<std::string> names = g_kws.spotter->templates();
    if (names.empty()) {
        JS_FreeValue(ctx, onSpotVal);
        return JS_ThrowInternalError(ctx,
            "bro.kws.listen: no templates enrolled (bro.kws.enroll first)");
    }

    try {
        g_kws.names  = names;
        g_kws.onSpot = JS_DupValue(ctx, onSpotVal);
        g_kws.produced.store(0, std::memory_order_relaxed);
        g_kws.drained.store(0, std::memory_order_relaxed);
        g_kws.suspended.store(false, std::memory_order_relaxed);
        JS_FreeValue(ctx, onSpotVal);

        // Join the shared listen host. The host's single task drives the bus
        // (mel -> one PhonemeNet forward) and calls this hook on the inference
        // thread with whatever fired. Captures the name snapshot — the
        // enrolled set cannot change while listening, so name -> index lookups
        // stay valid; g_kws's atomics live for the program's lifetime.
        listenHostSetSpotter(
            g_kws.spotter, g_kws.device,
            [names](const std::vector<brosoundml::SpotEvent>& events) {
                if (g_kws.suspended.load(std::memory_order_relaxed)) return;
                for (const auto& ev : events) {
                    const int idx = nameIndexOf(names, ev.name);
                    if (idx >= 0) publishEvent(idx, ev.confidence);
                }
            });
        g_kws.listening = true;

        std::fprintf(stderr,
            "[INFO] [kws] listening (device=%s, %zu template%s, mic=%d Hz, model=%d Hz)\n",
            deviceName(g_kws.device), names.size(), names.size() == 1 ? "" : "s",
            g_kws.audioEngine->sampleRate(), g_kws.spotter->sample_rate());
        return JS_UNDEFINED;
    } catch (const std::exception& e) {
        g_kws.listening = true;   // let stopListening clear the partial state
        stopListening();
        return JS_ThrowInternalError(ctx, "bro.kws.listen: %s", e.what());
    }
}

JSValue js_stop(JSContext*, JSValueConst, int, JSValueConst*) {
    stopListening();
    return JS_UNDEFINED;
}

JSValue js_suspend(JSContext*, JSValueConst, int, JSValueConst*) {
    g_kws.suspended.store(true, std::memory_order_relaxed);
    return JS_UNDEFINED;
}

JSValue js_resume(JSContext*, JSValueConst, int, JSValueConst*) {
    g_kws.suspended.store(false, std::memory_order_relaxed);
    return JS_UNDEFINED;
}

JSValue js_isActive(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewBool(ctx, g_kws.listening);
}

JSValue js_isSuspended(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewBool(ctx, g_kws.suspended.load(std::memory_order_relaxed));
}

JSValue js_isLoaded(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewBool(ctx, g_kws.spotter && g_kws.spotter->loaded());
}

JSValue js_sampleRate(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewInt32(ctx, g_kws.spotter ? g_kws.spotter->sample_rate() : 0);
}

// Best current prefix progress across all templates, [0,1] — a lock-free
// library read, safe while the inference thread feeds. For UI meters.
JSValue js_prefixProgress(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (!g_kws.spotter) return JS_NewFloat64(ctx, 0.0);
    return JS_NewFloat64(ctx, g_kws.spotter->prefix_progress());
}

// Per-template alignment telemetry — the spotter's contribution to the fused
// listening surface. One coherent lock-free snapshot (all entries taken after
// the same posterior frame): prefix depth, partial confidence (the same
// statistic the firing threshold tests), and poll-safe completion counters,
// for fusing against bro.sense.snapshot(). Null until weights are loaded.
JSValue js_progress(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (!g_kws.spotter) return JS_NULL;
    const brosoundml::ProgressSnapshot s = g_kws.spotter->progress_snapshot();
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "frames", JS_NewInt64(ctx, s.frames));
    JS_SetPropertyStr(ctx, obj, "generation", JS_NewUint32(ctx, s.generation));
    JSValue arr = JS_NewArray(ctx);
    for (int i = 0; i < s.count; ++i) {
        const brosoundml::TemplateProgress& e = s.templates[i];
        JSValue t = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, t, "name", JS_NewString(ctx, e.name));
        JS_SetPropertyStr(ctx, t, "matched", JS_NewInt32(ctx, e.matched));
        JS_SetPropertyStr(ctx, t, "length", JS_NewInt32(ctx, e.length));
        JS_SetPropertyStr(ctx, t, "progress", JS_NewFloat64(ctx, e.progress));
        JS_SetPropertyStr(ctx, t, "confidence",
                          JS_NewFloat64(ctx, e.confidence));
        JS_SetPropertyStr(ctx, t, "completions",
                          JS_NewInt64(ctx, e.completions));
        JS_SetPropertyStr(ctx, t, "lastAdvanceFrame",
                          JS_NewInt64(ctx, e.last_advance_frame));
        JS_SetPropertyStr(ctx, t, "lastFireFrame",
                          JS_NewInt64(ctx, e.last_fire_frame));
        JS_SetPropertyUint32(ctx, arr, static_cast<std::uint32_t>(i), t);
    }
    JS_SetPropertyStr(ctx, obj, "templates", arr);
    return obj;
}

// Diagnostic surface over the SHARED listen-host mic tap (cf. bro.wake.stats).
JSValue js_stats(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    const broaudio::MicTapId tap = listenHostTapId();
    if (!g_kws.listening || !g_kws.audioEngine ||
        tap == broaudio::kInvalidMicTapId) {
        return JS_NULL;
    }
    auto s = g_kws.audioEngine->getMicTapStats(tap);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "framesDelivered",
        JS_NewInt64(ctx, static_cast<int64_t>(s.framesDelivered)));
    JS_SetPropertyStr(ctx, o, "samplesDelivered",
        JS_NewInt64(ctx, static_cast<int64_t>(s.samplesDelivered)));
    JS_SetPropertyStr(ctx, o, "rollingPeak", JS_NewFloat64(ctx, s.rollingPeak));
    return o;
}

JSValue makeEventArray(JSContext* ctx,
                       const std::vector<brosoundml::SpotEvent>& events) {
    JSValue arr = JS_NewArray(ctx);
    for (std::uint32_t i = 0; i < events.size(); ++i) {
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, events[i].name.c_str()));
        JS_SetPropertyStr(ctx, o, "confidence",
                          JS_NewFloat64(ctx, events[i].confidence));
        JS_SetPropertyUint32(ctx, arr, i, o);
    }
    return arr;
}

// Manual feed for tests / scripted scenarios. Samples must already be at the
// spotter's rate. Mirrors bro.wake.feed's mode split:
//   - Headless (no inference worker): the shared bus runs synchronously on
//     this (the inference) thread — it is ONE stream, so the feed advances
//     every attached tenant (bro.sense included) — and the fired events come
//     back as [{name, confidence}]. Suspended fires are still returned (the
//     caller asked) but not queued for onSpot.
//   - Threaded: samples go into the live shared ring; events surface via
//     onSpot. Returns undefined.
// Refuses to run while live capture is active (two-producer race on the ring).
JSValue js_feed(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!g_kws.listening)
        return JS_ThrowInternalError(ctx, "bro.kws.feed: not listening");
    if (g_kws.audioEngine && g_kws.audioEngine->isMicCapturing()) {
        return JS_ThrowInternalError(ctx,
            "bro.kws.feed: cannot feed while live mic capture is active "
            "(feed is for headless/offline use; the live tap already writes "
            "the ring)");
    }
    int n = 0;
    const float* p = (argc >= 1) ? readFloats(ctx, argv[0], n) : nullptr;
    if (!p)
        return JS_ThrowTypeError(ctx, "bro.kws.feed(Float32Array)");

    if (g_kws.inference && g_kws.inference->threaded()) {
        listenHostWriteRing(p, n);
        return JS_UNDEFINED;
    }

    brosoundml::ListenFeedResult r;
    try {
        // The host runs the device scope and delivers spots through the same
        // onSpots hook the live path uses (suspended-gated there).
        r = listenHostFeedInline(p, n);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "bro.kws.feed: %s", e.what());
    }
    return makeEventArray(ctx, r.spots);
}

} // namespace

void installKwsBindings(JSContext* ctx, broaudio::Engine* audioEngine,
                        engine::AudioInference* inference) {
    g_kws.audioEngine = audioEngine;
    g_kws.inference   = inference;
    g_kws.ctx         = ctx;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        JS_FreeValue(ctx, broObj);
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue kws = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, kws, "load",
        JS_NewCFunction(ctx, js_load, "load", 1));
    JS_SetPropertyStr(ctx, kws, "unload",
        JS_NewCFunction(ctx, js_unload, "unload", 0));
    JS_SetPropertyStr(ctx, kws, "enroll",
        JS_NewCFunction(ctx, js_enroll, "enroll", 3));
    JS_SetPropertyStr(ctx, kws, "enrollFromAudio",
        JS_NewCFunction(ctx, js_enrollFromAudio, "enrollFromAudio", 3));
    JS_SetPropertyStr(ctx, kws, "remove",
        JS_NewCFunction(ctx, js_remove, "remove", 1));
    JS_SetPropertyStr(ctx, kws, "clear",
        JS_NewCFunction(ctx, js_clear, "clear", 0));
    JS_SetPropertyStr(ctx, kws, "templates",
        JS_NewCFunction(ctx, js_templates, "templates", 0));
    JS_SetPropertyStr(ctx, kws, "reset",
        JS_NewCFunction(ctx, js_reset, "reset", 0));
    JS_SetPropertyStr(ctx, kws, "listen",
        JS_NewCFunction(ctx, js_listen, "listen", 1));
    JS_SetPropertyStr(ctx, kws, "stop",
        JS_NewCFunction(ctx, js_stop, "stop", 0));
    JS_SetPropertyStr(ctx, kws, "suspend",
        JS_NewCFunction(ctx, js_suspend, "suspend", 0));
    JS_SetPropertyStr(ctx, kws, "resume",
        JS_NewCFunction(ctx, js_resume, "resume", 0));
    JS_SetPropertyStr(ctx, kws, "isActive",
        JS_NewCFunction(ctx, js_isActive, "isActive", 0));
    JS_SetPropertyStr(ctx, kws, "isSuspended",
        JS_NewCFunction(ctx, js_isSuspended, "isSuspended", 0));
    JS_SetPropertyStr(ctx, kws, "isLoaded",
        JS_NewCFunction(ctx, js_isLoaded, "isLoaded", 0));
    JS_SetPropertyStr(ctx, kws, "sampleRate",
        JS_NewCFunction(ctx, js_sampleRate, "sampleRate", 0));
    JS_SetPropertyStr(ctx, kws, "prefixProgress",
        JS_NewCFunction(ctx, js_prefixProgress, "prefixProgress", 0));
    JS_SetPropertyStr(ctx, kws, "progress",
        JS_NewCFunction(ctx, js_progress, "progress", 0));
    JS_SetPropertyStr(ctx, kws, "stats",
        JS_NewCFunction(ctx, js_stats, "stats", 0));
    JS_SetPropertyStr(ctx, kws, "feed",
        JS_NewCFunction(ctx, js_feed, "feed", 1));
    JS_SetPropertyStr(ctx, broObj, "kws", kws);

    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

void tickKws(JSContext* ctx) {
    if (!g_kws.listening) return;
    const std::uint64_t produced = g_kws.produced.load(std::memory_order_acquire);
    std::uint64_t drained = g_kws.drained.load(std::memory_order_relaxed);
    if (drained >= produced || JS_IsUndefined(g_kws.onSpot)) return;
    while (drained < produced) {
        const int   idx  = g_kws.eventIdx[drained % kEventSlots];
        const float conf = g_kws.eventConf[drained % kEventSlots];
        drained++;
        // Publish the consumption BEFORE the JS call: the producer only needs
        // the slot back, and onSpot may run for a while.
        g_kws.drained.store(drained, std::memory_order_release);
        if (idx < 0 || idx >= (int)g_kws.names.size()) continue;
        JSValue args[2] = {
            JS_NewString(ctx, g_kws.names[(std::size_t)idx].c_str()),
            JS_NewFloat64(ctx, conf),
        };
        JSValue r = JS_Call(ctx, g_kws.onSpot, JS_UNDEFINED, 2, args);
        if (JS_IsException(r)) {
            JSValue exc = JS_GetException(ctx);
            const char* s = JS_ToCString(ctx, exc);
            std::fprintf(stderr, "[ERROR] [kws] onSpot threw: %s\n", s ? s : "?");
            if (s) JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, exc);
        }
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
    }
}

void cleanupKwsBindings(JSContext* /*ctx*/) {
    unloadSpotter();
    g_kws.audioEngine = nullptr;
    g_kws.inference   = nullptr;
    g_kws.ctx         = nullptr;
}

}  // namespace bro::js
