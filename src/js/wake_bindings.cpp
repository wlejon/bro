// JS bindings for brosoundml::WakeWord — streaming wake-word detection.
//
// Installed onto bro.wake.* by installWakeBindings(). One detector at a
// time per JS context (the typical app needs one wake phrase).
//
// Threading model:
//   - bro.wake.listen() installs a mic-frame callback on broaudio::Engine.
//     The callback fires on the SDL audio thread and synchronously runs
//     WakeWord::feed() — single-producer-safe by brosoundml's contract.
//   - When feed() returns true (debounced event), the audio thread increments
//     an atomic fire counter. No JS code runs on the audio thread.
//   - The main thread drains the counter once per frame via tickWake() and
//     invokes the stored JS onFire callback exactly once per pending fire.
//
// Lifetime:
//   - The detector is stored as shared_ptr<WakeWord> in the binding state.
//     The audio-thread mic callback captures another shared_ptr so even if
//     bro.wake.stop() drops the binding's reference while the audio thread
//     is mid-feed(), the WakeWord stays alive until broaudio's RCU domain
//     reclaims the previous callback.

#include "js/wake_bindings.h"

#include <broaudio/engine.h>
#include <brosoundml/wake.h>
#include <brotensor/runtime.h>
#include <brotensor/tensor.h>

#include <quickjs.h>

#include <atomic>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>

namespace bro::js {

namespace {

// TU-local singleton state — one wake-word detector per JS context. A second
// listen() implicitly stop()s the previous detector. Keeping the state file-
// local (rather than per-context QuickJS opaque) is simpler and matches the
// product reality: an app installs exactly one wake phrase at startup.
struct WakeState {
    broaudio::Engine* audioEngine = nullptr;
    JSContext*        ctx         = nullptr;

    // Detector + JS callback. shared_ptr so the audio-thread lambda can hold
    // its own reference; main-thread mutators see the same instance.
    std::shared_ptr<brosoundml::WakeWord> wake;

    // JS onFire callback. JS_DupValue'd on listen, JS_FreeValue'd on stop.
    JSValue onFire = JS_UNDEFINED;

    // Audio thread → main thread fire counter. Incremented relaxed by the
    // mic callback when feed() returns true; drained on the main thread by
    // tickWake().
    std::atomic<int> firePending{0};

    // Suspend gate read by the audio thread (relaxed). True while a TTS or
    // self-audio period would otherwise re-trigger the wake.
    std::atomic<bool> suspended{false};

    bool active = false;
};

WakeState g_wake;

// Detach the mic callback and drop the detector. Safe to call repeatedly.
// Audio-thread RCU in broaudio holds the previous callback until the next
// safe reclamation point — that callback owns the only other shared_ptr to
// `wake`, so the WakeWord destructor runs once both refs drop.
void shutdownActiveDetector() {
    if (!g_wake.active) return;
    if (g_wake.audioEngine) {
        g_wake.audioEngine->setMicFrameCallback(nullptr);
    }
    g_wake.wake.reset();
    if (g_wake.ctx && !JS_IsUndefined(g_wake.onFire)) {
        JS_FreeValue(g_wake.ctx, g_wake.onFire);
        g_wake.onFire = JS_UNDEFINED;
    }
    g_wake.firePending.store(0, std::memory_order_relaxed);
    g_wake.suspended.store(false, std::memory_order_relaxed);
    g_wake.active = false;
}

// ─── Helpers ───────────────────────────────────────────────────────────────

bool argStr(JSContext* ctx, JSValueConst v, std::string& out) {
    if (!JS_IsString(v)) return false;
    const char* s = JS_ToCString(ctx, v);
    if (!s) return false;
    out = s;
    JS_FreeCString(ctx, s);
    return true;
}

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
    return brotensor::is_available(brotensor::Device::CUDA)
        ? brotensor::Device::CUDA
        : brotensor::Device::CPU;
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
        err = "opts.device must be a string ('cpu' or 'cuda')";
        return false;
    }
    const char* s = JS_ToCString(ctx, v);
    std::string sv = s ? s : "";
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);
    if (sv == "cpu")   { out = brotensor::Device::CPU;   return true; }
    if (sv == "cuda")  { out = brotensor::Device::CUDA;  return true; }
    if (sv == "metal") { out = brotensor::Device::Metal; return true; }
    err = "opts.device must be 'cpu' or 'cuda' (got '" + sv + "')";
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

// ─── JS-callable functions ─────────────────────────────────────────────────

JSValue js_listen(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!g_wake.audioEngine) {
        return JS_ThrowInternalError(ctx,
            "bro.wake.listen: audio engine not available");
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

    brotensor::Device dev = autoDevice();
    {
        std::string err;
        if (!parseDeviceOpt(ctx, opts, dev, err)) {
            JS_FreeValue(ctx, onFireVal);
            return JS_ThrowTypeError(ctx, "bro.wake.listen: %s", err.c_str());
        }
    }

    // Optional detector-policy overrides.
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

        g_wake.ctx    = ctx;
        g_wake.wake   = wake;
        g_wake.onFire = JS_DupValue(ctx, onFireVal);
        g_wake.firePending.store(0, std::memory_order_relaxed);
        g_wake.suspended.store(false, std::memory_order_relaxed);
        g_wake.active = true;
        JS_FreeValue(ctx, onFireVal);

        // Audio-thread callback. Captures a shared_ptr to the WakeWord so the
        // detector stays alive across an in-flight feed() even if main-thread
        // stop() drops the binding's reference. Reads the shared atomics
        // (suspended, firePending) directly — they outlive the lambda via
        // file-scope storage and broaudio's RCU keeps the lambda itself alive
        // long enough for any in-progress audio callback to finish.
        auto wakeRef = wake;
        g_wake.audioEngine->setMicFrameCallback(
            [wakeRef](const float* samples, int n) {
                if (g_wake.suspended.load(std::memory_order_relaxed)) return;
                if (!wakeRef) return;
                if (wakeRef->feed(samples, n)) {
                    g_wake.firePending.fetch_add(1, std::memory_order_relaxed);
                }
            });

        // Make sure the mic is actually capturing — otherwise the callback
        // will never fire. Safe to call when already started.
        if (!g_wake.audioEngine->isMicCapturing()) {
            g_wake.audioEngine->startMicCapture();
        }

        std::fprintf(stderr,
            "[INFO] [wake] listening (device=%s, threshold=%.3f)\n",
            deviceName(dev), threshold);
        return JS_UNDEFINED;
    } catch (const std::exception& e) {
        JS_FreeValue(ctx, onFireVal);
        shutdownActiveDetector();
        return JS_ThrowInternalError(ctx, "bro.wake.listen: %s", e.what());
    }
}

JSValue js_stop(JSContext*, JSValueConst, int, JSValueConst*) {
    shutdownActiveDetector();
    return JS_UNDEFINED;
}

JSValue js_suspend(JSContext*, JSValueConst, int, JSValueConst*) {
    g_wake.suspended.store(true, std::memory_order_relaxed);
    return JS_UNDEFINED;
}

JSValue js_resume(JSContext*, JSValueConst, int, JSValueConst*) {
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
    if (g_wake.wake) g_wake.wake->set_threshold(static_cast<float>(t));
    return JS_UNDEFINED;
}

// Manual feed for tests/scripted scenarios — drives the same code path as the
// audio thread, but synchronously on the caller. Skips the suspend gate so
// tests don't need to flip it; that matches stt's "feed me an array" seam.
JSValue js_feed(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!g_wake.wake)
        return JS_ThrowInternalError(ctx, "bro.wake.feed: no active detector");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "bro.wake.feed(Float32Array)");
    size_t byteOff = 0, viewLen = 0, bpe = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, argv[0], &byteOff, &viewLen, &bpe);
    if (JS_IsException(abuf)) return JS_EXCEPTION;
    size_t abufLen = 0;
    std::uint8_t* p = JS_GetArrayBuffer(ctx, &abufLen, abuf);
    JS_FreeValue(ctx, abuf);
    if (!p || bpe != sizeof(float)) {
        return JS_ThrowTypeError(ctx, "bro.wake.feed: argument must be a Float32Array");
    }
    int n = (int)(viewLen / sizeof(float));
    bool fired = false;
    try {
        fired = g_wake.wake->feed(reinterpret_cast<const float*>(p + byteOff), n);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "bro.wake.feed: %s", e.what());
    }
    if (fired) g_wake.firePending.fetch_add(1, std::memory_order_relaxed);
    return JS_NewBool(ctx, fired);
}

} // namespace

void installWakeBindings(JSContext* ctx, broaudio::Engine* audioEngine) {
    g_wake.audioEngine = audioEngine;
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
    JS_SetPropertyStr(ctx, wake, "feed",
        JS_NewCFunction(ctx, js_feed, "feed", 1));
    JS_SetPropertyStr(ctx, broObj, "wake", wake);

    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

void tickWake(JSContext* ctx) {
    if (!g_wake.active) return;
    // Drain the audio-thread fire counter. Exchange-with-zero so concurrent
    // increments after the read still arrive on a subsequent tick.
    int fires = g_wake.firePending.exchange(0, std::memory_order_acquire);
    if (fires <= 0) return;
    if (JS_IsUndefined(g_wake.onFire)) return;
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
    g_wake.ctx         = nullptr;
}

}  // namespace bro::js
