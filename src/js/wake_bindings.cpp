// JS bindings for brosoundml::WakeWord — streaming wake-word detection.
//
// Installed onto bro.wake.* by installWakeBindings(). One detector at a time per
// JS context (the typical app needs one wake phrase).
//
// bro.wake is a thin tenant of the engine's audio-inference subsystem
// (engine::AudioInference). It holds no threading of its own — the subsystem
// owns the worker thread; broaudio owns the real-time DSP. Three concerns, three
// threads:
//
//   - PRODUCER (real-time audio thread): bro.wake.listen() registers a broaudio
//     MicTap at the wake model's sample rate with AGC enabled (the brosoundml
//     training pipeline peak-normalises every clip to ~0.99, so live mic input
//     must be lifted to match). broaudio owns the polyphase resampler and the
//     AGC — both CPU. The tap callback does the one thing an audio-thread
//     callback may always safely do: copy the samples into a lock-free SPSC
//     ring. It never touches the model, the GPU, or the heap, so it can never
//     stall mic capture no matter what other threads (e.g. a worker running an
//     8B LLM on CUDA) are doing on the device.
//   - INFERENCE thread (engine::AudioInference worker; or, headless, the calling
//     thread via stepInline()): drains the ring and runs WakeWord::feed() —
//     which issues host-synchronizing CUDA (per-frame alloc/free + a logit
//     read-back). Running it here keeps those syncs off BOTH the audio thread
//     (no mic starvation) and the main thread (no UI hiccups during a response).
//   - MAIN thread: tickWake() drains an atomic fire counter the inference thread
//     publishes and invokes the JS onFire callback, so onFire always runs
//     single-threaded with the rest of the app.
//
// The detector rolls continuously (we feed everything we drain); suspend() only
// gates whether a fire is delivered to onFire, never whether audio is processed
// — so there is no freeze/thaw of the streaming window.
//
// Lifetime:
//   - WakeWord (shared_ptr) is captured into the inference task's process
//     closure, which the subsystem worker owns. The binding holds a second
//     shared_ptr only for the atomic tunable setters / score reads, and drops it
//     on stop() before the worker reclaims the task — so the model's destructor
//     (and its CUDA frees) runs on the worker thread.
//   - The ring (shared_ptr<PcmRing>) is held by both the binding and the tap
//     callback, so a callback still in flight after stop()/re-listen() writes
//     into its own (now unread) ring rather than racing a freshly installed one.

#include "js/wake_bindings.h"

#include "audio_inference/audio_inference.h"

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
using engine::PcmRing;

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
    // scoreMaxX10000: max of wake->last_score()*10000 across every feed() since
    // listen(), so a JS probe sees transient spikes a low-cadence lastScore()
    // sample would miss. firePending: detected-and-not-suspended fires awaiting
    // onFire delivery.
    std::atomic<int> scoreMaxX10000{0};
    std::atomic<int> firePending{0};

    // The SPSC ring carrying wake-rate PCM from the producer to the inference
    // thread, and the AudioInference task that consumes it. Per-instance; the
    // shared_ptr lets a late tap callback write safely after a re-listen.
    std::shared_ptr<PcmRing>     ring;
    AudioInference::TaskId       taskId = AudioInference::kInvalidTask;

    // Action gate. Written by suspend()/resume() (main thread), read by the
    // inference thread inside the process closure. When true the detector KEEPS
    // feeding (the window must roll continuously so resume never faces a frozen
    // or warmup-gapped window) but fires are not counted. It gates the action,
    // not the audio.
    std::atomic<bool> suspended{false};

    // The broaudio mic tap that drives this detector. kInvalidMicTapId when no
    // detector is active.
    broaudio::MicTapId tapId = broaudio::kInvalidMicTapId;

    // Per-call Agc + scratch used by js_feed when the caller hands samples at the
    // model's native rate. Main-thread only; lives separately from the tap's
    // audio-thread AGC because the two streams (live mic vs scripted replay) have
    // independent loudness histories.
    broaudio::Agc      feedAgc{};
    std::vector<float> feedScratch;

    bool active = false;
};

WakeState g_wake;

// Configure the AGC the wake tap installs into broaudio.
//
// brosoundml's wake training pipeline peak-normalises every clip to 0.99, so
// BC-ResNet expects loud input. A real desktop mic typically delivers peak
// 0.05–0.2 and never fires the model without normalisation.
//
// Half-life of 1 s bridges a typical word (300 ms speech + 700 ms silence)
// without pumping within an utterance; the 0.01 gate keeps room hiss from being
// amplified to speech-band loudness.
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
    // Stop the producer first so no more samples enter the ring.
    if (g_wake.audioEngine && g_wake.tapId != broaudio::kInvalidMicTapId) {
        g_wake.audioEngine->removeMicTap(g_wake.tapId);
    }
    g_wake.tapId = broaudio::kInvalidMicTapId;
    // Unregister the inference task. The worker drops its strong ref to the
    // model when it processes this; dropping ours here first means the worker
    // holds the last ref, so the model's destructor (CUDA frees) runs there.
    if (g_wake.inference && g_wake.taskId != AudioInference::kInvalidTask) {
        g_wake.inference->removeTask(g_wake.taskId);
    }
    g_wake.taskId = AudioInference::kInvalidTask;
    g_wake.wake.reset();
    if (g_wake.ctx && !JS_IsUndefined(g_wake.onFire)) {
        JS_FreeValue(g_wake.ctx, g_wake.onFire);
        g_wake.onFire = JS_UNDEFINED;
    }
    g_wake.firePending.store(0, std::memory_order_relaxed);
    g_wake.scoreMaxX10000.store(0, std::memory_order_relaxed);
    g_wake.suspended.store(false, std::memory_order_relaxed);
    // Drop our reference to the ring. A tap callback still in flight holds its
    // own shared_ptr, so it writes into the (now unread) ring safely; the buffer
    // is freed once that callback returns and broaudio reclaims the tap.
    g_wake.ring.reset();
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

// The inference task body. Runs on the AudioInference thread (or, headless, the
// calling thread). `wake` is the strong ref that owns the model on the worker;
// the closure reads/writes only g_wake's atomics, which live for the program's
// lifetime, so a stale closure that runs once more before removal is harmless.
AudioInference::ProcessFn makeProcess(std::shared_ptr<brosoundml::WakeWord> wake,
                                      brotensor::Device device) {
    return [wake, device](const float* samples, int n) {
        bool fired = false;
        try {
            // Run feed() under the model's device scope — exactly as the LLM/STT/
            // TTS async workers do on their threads. brotensor's CUDA kernels
            // launch on the THREAD-LOCAL current stream that DeviceScope sets up;
            // without it this worker thread has no scope, so feed()'s per-frame
            // logit read-backs (~100/s) fall onto the default stream, serialising
            // against all other device work and host-syncing each call. That
            // stalls real-time keep-up, the 2 s ring overflows, and PcmRing drops
            // the NEWEST samples — i.e. the word being spoken — so the detector
            // never sees a complete utterance even though audio is arriving.
            brotensor::DeviceScope scope(device);
            fired = wake->feed(samples, n);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[ERROR] [wake] feed: %s\n", e.what());
            return;
        }
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

        // Ring spans ~2 s of wake-rate audio — orders of magnitude more than the
        // one-or-two-frame consumer latency, so it only ever drops if the
        // inference thread stalls for seconds.
        const int wakeRate = wake->config().sample_rate;
        auto ring = std::make_shared<PcmRing>(
            static_cast<std::size_t>(wakeRate) * 2u);

        g_wake.ctx    = ctx;
        g_wake.wake   = wake;
        g_wake.ring   = ring;
        g_wake.onFire = JS_DupValue(ctx, onFireVal);
        g_wake.firePending.store(0, std::memory_order_relaxed);
        g_wake.suspended.store(false, std::memory_order_relaxed);
        g_wake.scoreMaxX10000.store(0, std::memory_order_relaxed);
        g_wake.feedAgc.setConfig(wakeAgcConfig());
        g_wake.feedAgc.reset();
        JS_FreeValue(ctx, onFireVal);

        // Register the inference task: drain `ring`, run feed() on the inference
        // thread, publish fires. The closure captures a strong ref to the model.
        g_wake.taskId = g_wake.inference->addTask(ring, makeProcess(wake, dev));

        // Configure the tap. broaudio owns the resampler (mic rate → wake rate,
        // polyphase windowed-sinc) and the AGC — both CPU, real-time safe. The
        // callback copies the samples into the SPSC ring; nothing else.
        broaudio::MicTapConfig tapCfg;
        tapCfg.targetRate  = wakeRate;
        tapCfg.chunkFrames = 0;   // pass the resampler's natural cadence through
        tapCfg.agc         = true;
        tapCfg.agcCfg      = wakeAgcConfig();

        // Capture the ring (not the model) — the audio thread must never touch
        // the detector. The shared_ptr keeps this instance's ring alive for any
        // callback still in flight after stop()/re-listen().
        auto ringRef = ring;
        g_wake.tapId = g_wake.audioEngine->addMicTap(
            tapCfg,
            [ringRef](const float* samples, int n) {
                ringRef->write(samples, n);
            });
        if (g_wake.tapId == broaudio::kInvalidMicTapId) {
            shutdownActiveDetector();
            return JS_ThrowInternalError(ctx,
                "bro.wake.listen: addMicTap failed");
        }

        // Mic capture must be running for the tap to receive frames. Safe to
        // call when already started — getUserMedia in audio_bindings shares the
        // same SDL stream.
        if (!g_wake.audioEngine->isMicCapturing()) {
            g_wake.audioEngine->startMicCapture();
        }
        g_wake.active = true;

        const int micRate  = g_wake.audioEngine->sampleRate();
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

// Diagnostic surface over the underlying broaudio mic tap. Returns
// { framesDelivered, samplesDelivered, rollingPeak, scoreMax } or null when no
// tap is installed. Lets a test or a debug UI confirm mic frames are reaching
// the detector without instrumenting the binding.
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
    // Per-inference max observed since listen(). Captures transient score spikes
    // that lastScore() misses when sampled at low cadence.
    JS_SetPropertyStr(ctx, o, "scoreMax",
        JS_NewFloat64(ctx,
            g_wake.scoreMaxX10000.load(std::memory_order_relaxed) / 10000.0));
    return o;
}

// Manual feed for tests / scripted scenarios. Samples must already be at the
// wake model's native rate (pass it as the optional second arg to assert). The
// binding copies into scratch and runs its own per-call Agc (state independent
// of the live tap, so scripted replay and the live mic keep separate loudness
// histories).
//
// Then it honours the one-thread-for-feed() invariant by mode:
//   - Headless (no inference worker): the scripting thread IS the inference
//     thread (stepInline runs here too), so feed() runs synchronously and the
//     call returns whether it fired — the per-chunk contract scripted tests use.
//   - Threaded (windowed/server): feed() may run only on the worker, so the
//     samples are written into the SAME ring the live tap feeds and the fire
//     surfaces via onFire on the next tickWake. Returns undefined.
//
// Refuses to run while live capture is active: that would make the ring a
// two-producer race (the audio-thread tap + this call). Headless/offline has no
// live capture.
//
// To exercise broaudio's resample + AGC + chunk path with mic-rate audio, use
// bro.mic instead — bro.wake.feed deliberately does not reach into the audio
// engine's tap dispatch.
JSValue js_feed(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!g_wake.active || !g_wake.ring)
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
    const int wakeRate = g_wake.wake ? g_wake.wake->config().sample_rate : 0;

    if (argc >= 2 && JS_IsNumber(argv[1])) {
        int32_t r = 0; JS_ToInt32(ctx, &r, argv[1]);
        if (r > 0 && r != wakeRate) {
            return JS_ThrowTypeError(ctx,
                "bro.wake.feed: sampleRate=%d must equal the wake rate=%d "
                "(use bro.mic to feed mic-rate audio through the resampler)",
                r, wakeRate);
        }
    }

    if (static_cast<int>(g_wake.feedScratch.size()) < n) {
        g_wake.feedScratch.resize(static_cast<std::size_t>(n));
    }
    std::memcpy(g_wake.feedScratch.data(), p + byteOff,
                static_cast<std::size_t>(n) * sizeof(float));
    g_wake.feedAgc.apply(g_wake.feedScratch.data(), n, wakeRate);

    // Threaded (windowed/server): hand off via the ring; the worker runs feed().
    if (g_wake.inference && g_wake.inference->threaded()) {
        g_wake.ring->write(g_wake.feedScratch.data(), n);
        return JS_UNDEFINED;
    }

    // Headless: run feed() synchronously on this (the inference) thread and
    // return whether it fired, mirroring the live path's bookkeeping.
    bool fired = false;
    try {
        fired = g_wake.wake->feed(g_wake.feedScratch.data(), n);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "bro.wake.feed: %s", e.what());
    }
    const int sx = static_cast<int>(g_wake.wake->last_score() * 10000.0f);
    if (sx > g_wake.scoreMaxX10000.load(std::memory_order_relaxed)) {
        g_wake.scoreMaxX10000.store(sx, std::memory_order_relaxed);
    }
    if (fired && !g_wake.suspended.load(std::memory_order_relaxed)) {
        g_wake.firePending.fetch_add(1, std::memory_order_release);
    }
    return JS_NewBool(ctx, fired);
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
