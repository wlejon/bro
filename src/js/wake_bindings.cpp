// JS bindings for brosoundml::WakeWord — streaming wake-word detection.
//
// Installed onto bro.wake.* by installWakeBindings(). One detector at a time per
// JS context (the typical app needs one wake phrase).
//
// bro.wake is a tenant of the engine's SHARED listen host (listen_host.h):
// one raw (no-AGC) 16 kHz tap + one ring + one inference task drive a
// brosoundml::ListenBus whose single PCEN mel pass feeds every attached
// member — running bro.wake alongside bro.kws/bro.sense costs one feature
// pass, with one forward per model. The detector hears the SAME raw stream
// as the rest of the stack: the AGC-free training recipe (random
// presentation level) made the model level-invariant, so no AGC exists
// anywhere on this path. Three concerns, three threads:
//
//   - PRODUCER (real-time audio thread): the host's tap callback copies
//     resampled raw samples into the shared lock-free SPSC ring; nothing
//     else. It never touches the model, the GPU, or the heap.
//   - INFERENCE thread (engine::AudioInference worker; or, headless, the
//     calling thread): the host's task drains the ring, runs the bus
//     (mel → WakeWord::feed_mel), and hands the fire flag to this binding's
//     onWake hook, which publishes score/fire telemetry into atomics.
//   - MAIN thread: tickWake() drains the atomic fire counter and invokes the
//     JS onFire callback, so onFire always runs single-threaded with the
//     rest of the app.
//
// The detector rolls continuously (the host feeds everything it drains);
// suspend() only gates whether a fire is delivered to onFire, never whether
// audio is processed — so there is no freeze/thaw of the streaming window.
//
// Lifetime: WakeWord (shared_ptr) is captured into the host's task closure,
// which the subsystem worker owns. The binding holds a second shared_ptr
// only for the atomic tunable setters / score reads, and drops it on stop()
// after detaching — so the model's destructor (and its CUDA frees) runs on
// the worker when the old closure is reclaimed.

#include "js/wake_bindings.h"

#include "audio_inference/audio_inference.h"
#include "js/listen_host.h"

#include <broaudio/engine.h>
#include <broaudio/mic_tap.h>
#include <brosoundml/wake.h>
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

// TU-local singleton state — one wake-word detector per JS context. A second
// listen() implicitly stop()s the previous detector.
struct WakeState {
    broaudio::Engine* audioEngine = nullptr;
    AudioInference*   inference   = nullptr;
    JSContext*        ctx         = nullptr;

    // Held for the atomic tunable setters (set_threshold) and score reads. The
    // inference task's process closure holds the strong ref that runs feed();
    // this one is dropped on stop() so the model is destroyed on the worker.
    std::shared_ptr<brosoundml::WakeWord> wake;
    JSValue                                onFire = JS_UNDEFINED;

    // Published by the inference thread, drained by tickWake (main thread).
    // scoreMaxX10000: max of wake->last_score()*10000 across every feed since
    // listen(), so a JS probe sees transient spikes a low-cadence lastScore()
    // sample would miss. firePending: detected-and-not-suspended fires awaiting
    // onFire delivery.
    std::atomic<int> scoreMaxX10000{0};
    std::atomic<int> firePending{0};

    // Action gate. Written by suspend()/resume() (main thread), read by the
    // inference thread inside the host's onWake hook. When true the detector
    // KEEPS feeding (the window must roll continuously so resume never faces a
    // frozen or warmup-gapped window) but fires are not counted. It gates the
    // action, not the audio.
    std::atomic<bool> suspended{false};

    bool active = false;
};

WakeState g_wake;

void shutdownActiveDetector() {
    if (!g_wake.active) return;
    // Detach from the shared listen host. The host replaces (or tears down)
    // the inference task; if bro.kws / bro.sense are still members, their
    // stream keeps rolling untouched. The old task closure — whose strong ref
    // owns the model on the worker — is reclaimed there, so the model's
    // destructor (CUDA frees) runs on the worker thread once we drop ours.
    listenHostSetWake(nullptr, brotensor::Device::CPU, nullptr);
    g_wake.wake.reset();
    if (g_wake.ctx && !JS_IsUndefined(g_wake.onFire)) {
        JS_FreeValue(g_wake.ctx, g_wake.onFire);
        g_wake.onFire = JS_UNDEFINED;
    }
    g_wake.firePending.store(0, std::memory_order_relaxed);
    g_wake.scoreMaxX10000.store(0, std::memory_order_relaxed);
    g_wake.suspended.store(false, std::memory_order_relaxed);
    g_wake.active = false;
}

// ─── Helpers ───────────────────────────────────────────────────────────────

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

// The host's onWake hook. Runs on the inference thread after every bus feed
// (not only on fires). `wake` is the binding's score-reading ref; the hook
// writes only g_wake's atomics, which live for the program's lifetime, so a
// stale hook that runs once more before the membership swap is harmless.
ListenWakeFn makeOnWake(std::shared_ptr<brosoundml::WakeWord> wake) {
    return [wake](bool fired) {
        const int sx = static_cast<int>(wake->last_score() * 10000.0f);
        int prev = g_wake.scoreMaxX10000.load(std::memory_order_relaxed);
        while (sx > prev &&
               !g_wake.scoreMaxX10000.compare_exchange_weak(
                   prev, sx, std::memory_order_relaxed)) {
            // prev reloaded by compare_exchange_weak on failure
        }
        if (fired && !g_wake.suspended.load(std::memory_order_relaxed)) {
            g_wake.firePending.fetch_add(1, std::memory_order_release);
        }
    };
}

// ─── JS-callable functions ─────────────────────────────────────────────────

JSValue js_listen(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!g_wake.audioEngine) {
        return JS_ThrowInternalError(ctx,
            "bro.wake.listen: audio engine not available");
    }
    if (!g_wake.inference) {
        return JS_ThrowInternalError(ctx,
            "bro.wake.listen: audio-inference subsystem not available");
    }
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx,
            "bro.wake.listen(opts): opts object required (weights, onFire, ...)");
    }
    JSValueConst opts = argv[0];

    std::string weights;
    if (!getStr(ctx, opts, "weights", weights) || weights.empty()) {
        return JS_ThrowTypeError(ctx,
            "bro.wake.listen: opts.weights (model path) required");
    }

    JSValue onFireVal = JS_GetPropertyStr(ctx, opts, "onFire");
    if (!JS_IsFunction(ctx, onFireVal)) {
        JS_FreeValue(ctx, onFireVal);
        return JS_ThrowTypeError(ctx,
            "bro.wake.listen: opts.onFire (function) required");
    }

    // init() BEFORE the device probe: GPU backends register on the driver
    // probe inside init(), so a fresh process would otherwise see only CPU
    // on its first listen() and silently fall back.
    brotensor::init();
    brotensor::Device dev = autoDevice();
    {
        std::string err;
        if (!parseDeviceOpt(ctx, opts, dev, err)) {
            JS_FreeValue(ctx, onFireVal);
            return JS_ThrowTypeError(ctx, "bro.wake.listen: %s", err.c_str());
        }
    }

    double threshold = 0.85;
    getNum(ctx, opts, "threshold", threshold);
    int refractoryMs = 500;
    getInt(ctx, opts, "refractoryMs", refractoryMs);

    int smoothingHits = -1, smoothingWindow = -1;
    {
        JSValue sm = JS_GetPropertyStr(ctx, opts, "smoothing");
        if (JS_IsObject(sm)) {
            getInt(ctx, sm, "hits",   smoothingHits);
            getInt(ctx, sm, "window", smoothingWindow);
        }
        JS_FreeValue(ctx, sm);
    }

    // Replace any prior detector before installing the new one.
    shutdownActiveDetector();

    try {
        brotensor::init();
        auto wake = std::make_shared<brosoundml::WakeWord>();
        wake->load(weights, dev);
        wake->set_threshold(static_cast<float>(threshold));
        if (smoothingHits > 0 && smoothingWindow > 0) {
            wake->set_smoothing(smoothingHits, smoothingWindow);
        }
        if (refractoryMs >= 0) wake->set_refractory_ms(refractoryMs);

        g_wake.firePending.store(0, std::memory_order_relaxed);
        g_wake.suspended.store(false, std::memory_order_relaxed);
        g_wake.scoreMaxX10000.store(0, std::memory_order_relaxed);

        // Join the shared listen host. The host owns the raw (no-AGC) tap, the
        // ring, and the single inference task; its bus runs ONE PCEN mel pass
        // and hands the new-frame block to WakeWord::feed_mel alongside any
        // other attached tenant. The AGC-free model is level-invariant, so it
        // hears the same raw stream as bro.kws / bro.sense. Throws on a
        // front-end framing mismatch or tap failure (the catch below detaches,
        // which also tears down member-less infra a failed first attach left).
        listenHostSetWake(wake, dev, makeOnWake(wake));

        g_wake.ctx    = ctx;
        g_wake.wake   = std::move(wake);
        g_wake.onFire = JS_DupValue(ctx, onFireVal);
        JS_FreeValue(ctx, onFireVal);
        g_wake.active = true;

        const int micRate  = g_wake.audioEngine->sampleRate();
        const int wakeRate = g_wake.wake->config().sample_rate;
        std::fprintf(stderr,
            "[INFO] [wake] listening on the shared listen host "
            "(device=%s, threshold=%.3f, mic=%d Hz, model=%d Hz%s)\n",
            deviceName(dev), threshold, micRate, wakeRate,
            (micRate != wakeRate) ? ", resampling" : "");
        return JS_UNDEFINED;
    } catch (const std::exception& e) {
        JS_FreeValue(ctx, onFireVal);
        listenHostSetWake(nullptr, brotensor::Device::CPU, nullptr);
        return JS_ThrowInternalError(ctx, "bro.wake.listen: %s", e.what());
    }
}

JSValue js_stop(JSContext*, JSValueConst, int, JSValueConst*) {
    shutdownActiveDetector();
    return JS_UNDEFINED;
}

JSValue js_suspend(JSContext*, JSValueConst, int, JSValueConst*) {
    // Stop ACTING on fires (recording / thinking / speaking). The detector keeps
    // feeding on the inference thread so its rolling window stays continuous.
    g_wake.suspended.store(true, std::memory_order_relaxed);
    return JS_UNDEFINED;
}

JSValue js_resume(JSContext*, JSValueConst, int, JSValueConst*) {
    // Re-enable acting on fires. No reset: the window has been rolling the whole
    // time, so it already reflects current audio and is warmed.
    g_wake.suspended.store(false, std::memory_order_relaxed);
    return JS_UNDEFINED;
}

JSValue js_lastScore(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (!g_wake.wake) return JS_NewFloat64(ctx, 0.0);
    return JS_NewFloat64(ctx, g_wake.wake->last_score());
}

JSValue js_isActive(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewBool(ctx, g_wake.active);
}

JSValue js_isSuspended(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewBool(ctx, g_wake.suspended.load(std::memory_order_relaxed));
}

JSValue js_setThreshold(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsNumber(argv[0])) {
        return JS_ThrowTypeError(ctx, "bro.wake.setThreshold(value): number required");
    }
    double t = 0.0;
    JS_ToFloat64(ctx, &t, argv[0]);
    // WakeWord setters are atomic; safe to call while the inference thread feeds.
    if (g_wake.wake) g_wake.wake->set_threshold(static_cast<float>(t));
    return JS_UNDEFINED;
}

// Diagnostic surface over the SHARED listen-host mic tap (cf. bro.kws.stats —
// same tap while both are live). Returns { framesDelivered, samplesDelivered,
// rollingPeak, scoreMax } or null when no tap is installed. Lets a test or a
// debug UI confirm mic frames are reaching the detector without instrumenting
// the binding.
JSValue js_stats(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    const broaudio::MicTapId tap = listenHostTapId();
    if (!g_wake.active || !g_wake.audioEngine ||
        tap == broaudio::kInvalidMicTapId) {
        return JS_NULL;
    }
    auto s = g_wake.audioEngine->getMicTapStats(tap);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "framesDelivered",
        JS_NewInt64(ctx, static_cast<int64_t>(s.framesDelivered)));
    JS_SetPropertyStr(ctx, o, "samplesDelivered",
        JS_NewInt64(ctx, static_cast<int64_t>(s.samplesDelivered)));
    JS_SetPropertyStr(ctx, o, "rollingPeak", JS_NewFloat64(ctx, s.rollingPeak));
    // Per-inference max observed since listen(). Captures transient score spikes
    // that lastScore() misses when sampled at low cadence.
    JS_SetPropertyStr(ctx, o, "scoreMax",
        JS_NewFloat64(ctx,
            g_wake.scoreMaxX10000.load(std::memory_order_relaxed) / 10000.0));
    return o;
}

// Manual feed for tests / scripted scenarios. Samples must already be at the
// wake model's native rate (pass it as the optional second arg to assert).
// Raw samples in, raw samples through — the AGC-free model hears scripted
// replay exactly as it hears the live tap. The listen host carries ONE
// stream, so this advances every attached tenant (audio fed here also moves
// bro.kws / bro.sense, and vice versa).
//
// Mode split (the host's usual one):
//   - Headless (no inference worker): the scripting thread IS the inference
//     thread, so the bus runs synchronously and the call returns whether the
//     detector fired — the per-chunk contract scripted tests use (onFire
//     also fires on the next tick unless suspended, via the host's hook).
//   - Threaded (windowed/server): the samples are written into the shared
//     ring and the fire surfaces via onFire on the next tickWake. Returns
//     undefined.
//
// Refuses to run while live capture is active: that would make the ring a
// two-producer race (the audio-thread tap + this call). Headless/offline has
// no live capture.
JSValue js_feed(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!g_wake.active || !g_wake.wake)
        return JS_ThrowInternalError(ctx, "bro.wake.feed: no active detector");
    if (g_wake.audioEngine && g_wake.audioEngine->isMicCapturing()) {
        return JS_ThrowInternalError(ctx,
            "bro.wake.feed: cannot feed while live mic capture is active "
            "(feed is for headless/offline use; the live tap already writes the "
            "ring)");
    }
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "bro.wake.feed(Float32Array, sampleRate?)");
    size_t byteOff = 0, viewLen = 0, bpe = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, argv[0], &byteOff, &viewLen, &bpe);
    if (JS_IsException(abuf)) return JS_EXCEPTION;
    size_t abufLen = 0;
    std::uint8_t* p = JS_GetArrayBuffer(ctx, &abufLen, abuf);
    JS_FreeValue(ctx, abuf);
    if (!p || bpe != sizeof(float)) {
        return JS_ThrowTypeError(ctx, "bro.wake.feed: argument must be a Float32Array");
    }
    const int n = static_cast<int>(viewLen / sizeof(float));
    const float* samples = reinterpret_cast<const float*>(p + byteOff);
    const int wakeRate = g_wake.wake->config().sample_rate;

    if (argc >= 2 && JS_IsNumber(argv[1])) {
        int32_t r = 0; JS_ToInt32(ctx, &r, argv[1]);
        if (r > 0 && r != wakeRate) {
            return JS_ThrowTypeError(ctx,
                "bro.wake.feed: sampleRate=%d must equal the wake rate=%d "
                "(use bro.mic to feed mic-rate audio through the resampler)",
                r, wakeRate);
        }
    }

    // Threaded (windowed/server): hand off via the shared ring; the host's
    // task runs the bus on the worker.
    if (g_wake.inference && g_wake.inference->threaded()) {
        listenHostWriteRing(samples, n);
        return JS_UNDEFINED;
    }

    // Headless: run the bus synchronously on this (the inference) thread and
    // return whether the detector fired. The host delivers score/fire
    // bookkeeping through the same onWake hook the live path uses.
    try {
        const brosoundml::ListenFeedResult r =
            listenHostFeedInline(samples, n);
        return JS_NewBool(ctx, r.wake_fired);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "bro.wake.feed: %s", e.what());
    }
}

} // namespace

void installWakeBindings(JSContext* ctx, broaudio::Engine* audioEngine,
                         engine::AudioInference* inference) {
    g_wake.audioEngine = audioEngine;
    g_wake.inference   = inference;
    g_wake.ctx         = ctx;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        JS_FreeValue(ctx, broObj);
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue wake = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, wake, "listen",
        JS_NewCFunction(ctx, js_listen, "listen", 1));
    JS_SetPropertyStr(ctx, wake, "stop",
        JS_NewCFunction(ctx, js_stop, "stop", 0));
    JS_SetPropertyStr(ctx, wake, "suspend",
        JS_NewCFunction(ctx, js_suspend, "suspend", 0));
    JS_SetPropertyStr(ctx, wake, "resume",
        JS_NewCFunction(ctx, js_resume, "resume", 0));
    JS_SetPropertyStr(ctx, wake, "lastScore",
        JS_NewCFunction(ctx, js_lastScore, "lastScore", 0));
    JS_SetPropertyStr(ctx, wake, "isActive",
        JS_NewCFunction(ctx, js_isActive, "isActive", 0));
    JS_SetPropertyStr(ctx, wake, "isSuspended",
        JS_NewCFunction(ctx, js_isSuspended, "isSuspended", 0));
    JS_SetPropertyStr(ctx, wake, "setThreshold",
        JS_NewCFunction(ctx, js_setThreshold, "setThreshold", 1));
    JS_SetPropertyStr(ctx, wake, "stats",
        JS_NewCFunction(ctx, js_stats, "stats", 0));
    JS_SetPropertyStr(ctx, wake, "feed",
        JS_NewCFunction(ctx, js_feed, "feed", 1));
    JS_SetPropertyStr(ctx, broObj, "wake", wake);

    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

void tickWake(JSContext* ctx) {
    if (!g_wake.active) return;

    // Deliver fires the inference thread published since last frame. Inference
    // itself ran on the AudioInference worker (windowed/server) or inline during
    // the headless pump — never here. This is pure main-thread result delivery.
    int fires = g_wake.firePending.exchange(0, std::memory_order_acquire);
    if (fires <= 0 || JS_IsUndefined(g_wake.onFire)) return;
    for (int i = 0; i < fires; ++i) {
        JSValue r = JS_Call(ctx, g_wake.onFire, JS_UNDEFINED, 0, nullptr);
        if (JS_IsException(r)) {
            JSValue exc = JS_GetException(ctx);
            const char* s = JS_ToCString(ctx, exc);
            std::fprintf(stderr, "[ERROR] [wake] onFire threw: %s\n", s ? s : "?");
            if (s) JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, exc);
        }
        JS_FreeValue(ctx, r);
    }
}

void cleanupWakeBindings(JSContext* /*ctx*/) {
    shutdownActiveDetector();
    g_wake.audioEngine = nullptr;
    g_wake.inference   = nullptr;
    g_wake.ctx         = nullptr;
}

}  // namespace bro::js
