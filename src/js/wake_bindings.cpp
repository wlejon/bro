// JS bindings for brosoundml::WakeWord — streaming wake-word detection.
//
// Installed onto bro.wake.* by installWakeBindings(). One detector at a
// time per JS context (the typical app needs one wake phrase).
//
// Threading model:
//   - bro.wake.listen() registers a broaudio MicTap configured for the wake
//     model's expected sample rate, with AGC enabled (the brosoundml training
//     pipeline peak-normalises every clip to 0.99, so live mic input has to
//     be lifted to match). broaudio owns the SDL polyphase resampler and the
//     AGC; the tap callback just runs WakeWord::feed() on the audio thread.
//   - When feed() returns true (debounced event), the audio thread increments
//     an atomic fire counter. No JS code runs on the audio thread.
//   - The main thread drains the counter once per frame via tickWake() and
//     invokes the stored JS onFire callback exactly once per pending fire.
//
// Lifetime:
//   - The detector is stored as shared_ptr<WakeWord> in the binding state.
//     The tap callback captures another shared_ptr so even if bro.wake.stop()
//     drops the binding's reference while the audio thread is mid-feed(),
//     the WakeWord stays alive until broaudio's RCU domain reclaims the
//     previous tap list.

#include "js/wake_bindings.h"

#include <broaudio/engine.h>
#include <broaudio/mic_tap.h>
#include <brosoundml/wake.h>
#include <brotensor/runtime.h>
#include <brotensor/tensor.h>

#include <quickjs.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace bro::js {

namespace {

// TU-local singleton state — one wake-word detector per JS context. A second
// listen() implicitly stop()s the previous detector.
struct WakeState {
    broaudio::Engine* audioEngine = nullptr;
    JSContext*        ctx         = nullptr;

    std::shared_ptr<brosoundml::WakeWord> wake;
    JSValue                                onFire = JS_UNDEFINED;

    // Audio thread → main thread fire counter.
    std::atomic<int> firePending{0};

    // Suspend gate read by the audio thread (relaxed).
    std::atomic<bool> suspended{false};

    // The broaudio mic tap that drives this detector. kInvalidMicTapId when
    // no detector is active.
    broaudio::MicTapId tapId = broaudio::kInvalidMicTapId;

    // Per-call Agc + scratch used by js_feed when the caller hands samples at
    // the model's native rate. Main-thread only; lives separately from the
    // tap's audio-thread AGC because the two streams (live mic vs scripted
    // replay) have independent loudness histories.
    broaudio::Agc      feedAgc{};
    std::vector<float> feedScratch;

    bool active = false;
};

WakeState g_wake;

// Configure the AGC the wake tap installs into broaudio.
//
// brosoundml's wake training pipeline peak-normalises every clip to 0.99,
// so BC-ResNet expects loud input. A real desktop mic typically delivers
// peak 0.05–0.2 and never fires the model without normalisation.
//
// Half-life of 1 s bridges a typical word (300 ms speech + 700 ms silence)
// without pumping within an utterance; the 0.01 gate keeps room hiss from
// being amplified to speech-band loudness.
broaudio::AgcConfig wakeAgcConfig() {
    broaudio::AgcConfig c;
    c.targetPeak  = 0.95f;
    c.halfLifeSec = 1.0f;
    c.noiseGate   = 0.01f;
    c.maxGain     = 10.0f;
    return c;
}

void shutdownActiveDetector() {
    if (!g_wake.active) return;
    if (g_wake.audioEngine && g_wake.tapId != broaudio::kInvalidMicTapId) {
        g_wake.audioEngine->removeMicTap(g_wake.tapId);
    }
    g_wake.tapId = broaudio::kInvalidMicTapId;
    g_wake.wake.reset();
    if (g_wake.ctx && !JS_IsUndefined(g_wake.onFire)) {
        JS_FreeValue(g_wake.ctx, g_wake.onFire);
        g_wake.onFire = JS_UNDEFINED;
    }
    g_wake.firePending.store(0, std::memory_order_relaxed);
    g_wake.suspended.store(false, std::memory_order_relaxed);
    g_wake.feedAgc.reset();
    g_wake.feedScratch.clear();
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
        g_wake.feedAgc.setConfig(wakeAgcConfig());
        g_wake.feedAgc.reset();
        JS_FreeValue(ctx, onFireVal);

        // Configure the tap. broaudio owns the resampler (mic rate → wake
        // rate, polyphase windowed-sinc) and the AGC. The callback shrinks
        // to: suspend gate + wake->feed + bump firePending on debounced
        // fire. Everything else lives in broaudio.
        broaudio::MicTapConfig tapCfg;
        tapCfg.targetRate  = wake->config().sample_rate;
        tapCfg.chunkFrames = 0;   // pass the resampler's natural cadence through
        tapCfg.agc         = true;
        tapCfg.agcCfg      = wakeAgcConfig();

        auto wakeRef = wake;
        g_wake.tapId = g_wake.audioEngine->addMicTap(
            tapCfg,
            [wakeRef](const float* samples, int n) {
                if (g_wake.suspended.load(std::memory_order_relaxed)) return;
                if (!wakeRef || n <= 0) return;
                if (wakeRef->feed(samples, n)) {
                    g_wake.firePending.fetch_add(1, std::memory_order_relaxed);
                }
            });
        if (g_wake.tapId == broaudio::kInvalidMicTapId) {
            shutdownActiveDetector();
            return JS_ThrowInternalError(ctx,
                "bro.wake.listen: addMicTap failed");
        }

        // Mic capture must be running for the tap to receive frames. Safe to
        // call when already started — getUserMedia in audio_bindings shares
        // the same SDL stream.
        if (!g_wake.audioEngine->isMicCapturing()) {
            g_wake.audioEngine->startMicCapture();
        }
        g_wake.active = true;

        const int micRate  = g_wake.audioEngine->sampleRate();
        const int wakeRate = wake->config().sample_rate;
        std::fprintf(stderr,
            "[INFO] [wake] listening (device=%s, threshold=%.3f, mic=%d Hz, model=%d Hz%s)\n",
            deviceName(dev), threshold, micRate, wakeRate,
            (micRate != wakeRate) ? ", resampling" : "");
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

// Diagnostic surface over the underlying broaudio mic tap. Returns
// { framesDelivered, samplesDelivered, rollingPeak } or null when no tap is
// installed. Lets a test or a debug UI confirm mic frames are reaching the
// detector without instrumenting the binding.
JSValue js_stats(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (!g_wake.active || !g_wake.audioEngine ||
        g_wake.tapId == broaudio::kInvalidMicTapId) {
        return JS_NULL;
    }
    auto s = g_wake.audioEngine->getMicTapStats(g_wake.tapId);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "framesDelivered",
        JS_NewInt64(ctx, static_cast<int64_t>(s.framesDelivered)));
    JS_SetPropertyStr(ctx, o, "samplesDelivered",
        JS_NewInt64(ctx, static_cast<int64_t>(s.samplesDelivered)));
    JS_SetPropertyStr(ctx, o, "rollingPeak", JS_NewFloat64(ctx, s.rollingPeak));
    return o;
}

// Manual feed for tests / scripted scenarios. Two modes:
//
//   1. sampleRate omitted or equal to the wake model's native rate: copy the
//      samples into a local scratch buffer, run the binding's per-call Agc
//      (independent state from the live tap), and forward to WakeWord::feed.
//      Returns whether the model fired on this chunk.
//
//   2. sampleRate equal to the engine's mic rate: route through
//      audioEngine->testFeedMicSamples, which dispatches to the same tap the
//      live audio thread uses. Exercises broaudio's resample + AGC + feed
//      path end-to-end. Returns whether the tap fired during this call,
//      measured by the firePending delta.
JSValue js_feed(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!g_wake.wake)
        return JS_ThrowInternalError(ctx, "bro.wake.feed: no active detector");
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
    const int wakeRate = g_wake.wake->config().sample_rate;

    int srcRate = wakeRate;
    if (argc >= 2 && JS_IsNumber(argv[1])) {
        int32_t r = 0; JS_ToInt32(ctx, &r, argv[1]);
        if (r > 0) srcRate = r;
    }

    bool fired = false;
    try {
        if (srcRate == wakeRate) {
            if (static_cast<int>(g_wake.feedScratch.size()) < n) {
                g_wake.feedScratch.resize(static_cast<std::size_t>(n));
            }
            std::memcpy(g_wake.feedScratch.data(), p + byteOff,
                        static_cast<std::size_t>(n) * sizeof(float));
            g_wake.feedAgc.apply(g_wake.feedScratch.data(), n, wakeRate);
            fired = g_wake.wake->feed(g_wake.feedScratch.data(), n);
            if (fired) g_wake.firePending.fetch_add(1, std::memory_order_relaxed);
        } else {
            if (!g_wake.audioEngine) {
                return JS_ThrowInternalError(ctx,
                    "bro.wake.feed: audio engine not available");
            }
            const int engineRate = g_wake.audioEngine->sampleRate();
            if (srcRate != engineRate) {
                return JS_ThrowTypeError(ctx,
                    "bro.wake.feed: sampleRate=%d must equal either wake rate=%d "
                    "or engine mic rate=%d",
                    srcRate, wakeRate, engineRate);
            }
            const int before = g_wake.firePending.load(std::memory_order_acquire);
            g_wake.audioEngine->testFeedMicSamples(
                reinterpret_cast<const float*>(p + byteOff), n);
            const int after = g_wake.firePending.load(std::memory_order_acquire);
            fired = (after > before);
        }
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "bro.wake.feed: %s", e.what());
    }
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
