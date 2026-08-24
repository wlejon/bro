#if BRO_WITH_SOUNDML

#include "js/wake_bindings.h"
#include "audio_inference/audio_inference.h"
#include "js/listen_host.h"
#include <broaudio/engine.h>
#include <broaudio/mic_tap.h>
#include <brosoundml/bc_resnet2d.h>
#include <brosoundml/wake.h>
#include <brotensor/runtime.h>
#include <brotensor/tensor.h>
#include <qjsbind/qjsbind.h>
#include <quickjs.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

namespace {

using engine::AudioInference;

// u2500u2500u2500 One stream's wake-word tenant u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500
//
// Address-stable (held by unique_ptr in g_wake.tenants), so the inference-thread
// onWake closure can capture a raw WakeTenant* and publish into its atomics.
struct WakeTenant {
    StreamId streamId = kInvalidStream;

    // This stream's detector, built over the shared net. Held for score reads /
    // atomic setters; the inference task closure holds the strong ref that runs
    // feed(). Dropped on stop() so the model is destroyed on the worker.
    std::shared_ptr<brosoundml::WakeWord> wake;
    JSValue                               onFire = JS_UNDEFINED;

    // Published by the inference thread, drained by tickWake (main thread).
    // scoreMaxX10000: max of wake->last_score()*10000 across every feed since
    // listen(). firePending: detected-and-not-suspended fires awaiting delivery.
    std::atomic<int> scoreMaxX10000{0};
    std::atomic<int> firePending{0};

    // Action gate. Written by suspend()/resume() (main), read by the inference
    // thread inside the onWake hook. When true the detector KEEPS feeding (the
    // window must roll continuously) but fires are not counted.
    std::atomic<bool> suspended{false};

    bool active = false;
};

// u2500u2500u2500 Namespace-level state (the shared net + the per-stream tenants) u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500
struct WakeNamespace {
    broaudio::Engine* audioEngine = nullptr;
    AudioInference*   inference   = nullptr;
    JSContext*        ctx         = nullptr;

    // The weights, loaded once and shared by every stream's detector.
    std::shared_ptr<const brosoundml::BcResnet2d> net;
    brotensor::Device                             device = brotensor::Device::CPU;

    std::unordered_map<StreamId, std::unique_ptr<WakeTenant>> tenants;
};

WakeNamespace g_wake;

// A per-stream view object: `stream.wake`. Carries only the stream id.
struct WakeView {
    StreamId streamId = kInvalidStream;
};

// u2500u2500u2500 Helpers u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500

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
    switch (d.type) {
        case brotensor::DeviceType::CUDA:  return "CUDA";
        case brotensor::DeviceType::Metal: return "Metal";
        case brotensor::DeviceType::CPU:   return "CPU";
    }
    return "?";
}

// u2500u2500u2500 Tenant registry u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500

WakeTenant* findTenant(StreamId id) {
    auto it = g_wake.tenants.find(id);
    return it == g_wake.tenants.end() ? nullptr : it->second.get();
}

WakeTenant* ensureTenant(StreamId id) {
    if (id == kInvalidStream) return nullptr;
    if (WakeTenant* t = findTenant(id)) return t;
    auto t = std::make_unique<WakeTenant>();
    t->streamId = id;
    WakeTenant* p = t.get();
    g_wake.tenants[id] = std::move(t);
    return p;
}

// Stop a tenant's detection: detach its WakeWord from its stream (a host
// barrier), drop the score ref, free its onFire, clear its atomics.
void stopTenant(WakeTenant* t) {
    if (!t->active) {
        // A tenant may hold a stale onFire only while active; nothing to do.
        return;
    }
    // Detach from the listen host. The host replaces (or tears down) the
    // stream's task; any other member (bro.kws / bro.sense) keeps rolling. The
    // old closure (owning the model on the worker) is reclaimed there, so the
    // model's destructor runs on the worker once we drop our ref.
    listenStreamSetWake(t->streamId, nullptr, brotensor::Device::CPU, nullptr);
    t->wake.reset();
    if (g_wake.ctx && !JS_IsUndefined(t->onFire)) {
        JS_FreeValue(g_wake.ctx, t->onFire);
        t->onFire = JS_UNDEFINED;
    }
    t->firePending.store(0, std::memory_order_relaxed);
    t->scoreMaxX10000.store(0, std::memory_order_relaxed);
    t->suspended.store(false, std::memory_order_relaxed);
    t->active = false;
}

void unloadAll() {
    for (auto& kv : g_wake.tenants) stopTenant(kv.second.get());
    g_wake.tenants.clear();
    g_wake.net.reset();
}

// The onWake hook for one tenant. Runs on the inference thread after every bus
// feed (not only on fires). Captures the tenant pointer (address-stable) and
// the detector's score ref; writes only the tenant's atomics (which live until
// the tenant is dropped), so a stale hook that runs once more before a
// membership swap is harmless.
ListenWakeFn makeOnWake(WakeTenant* t, std::shared_ptr<brosoundml::WakeWord> wake) {
    return [t, wake](bool fired) {
        const int sx = static_cast<int>(wake->last_score() * 10000.0f);
        int prev = t->scoreMaxX10000.load(std::memory_order_relaxed);
        while (sx > prev &&
               !t->scoreMaxX10000.compare_exchange_weak(
                   prev, sx, std::memory_order_relaxed)) {
            // prev reloaded by compare_exchange_weak on failure
        }
        if (fired && !t->suspended.load(std::memory_order_relaxed)) {
            t->firePending.fetch_add(1, std::memory_order_release);
        }
    };
}

// Get-or-load the shared net. If already loaded, returns it (weights ignored u2014
// unload() first to swap). Otherwise loads from `weights` (required) on `dev`.
bool ensureNet(const std::string& weights, brotensor::Device dev, std::string& err) {
    if (g_wake.net) return true;
    if (weights.empty()) {
        err = "no model loaded u2014 pass opts.weights (or call bro.wake.load first)";
        return false;
    }
    try {
        std::shared_ptr<const brosoundml::BcResnet2d> net;
        {
            brotensor::DeviceScope scope(dev);
            net = std::make_shared<const brosoundml::BcResnet2d>(
                brosoundml::BcResnet2d::load(weights, dev));
        }
        g_wake.net    = std::move(net);
        g_wake.device = dev;
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

// u2500u2500u2500 Tenant resolution (the dual-home seam) u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500

StreamId streamOf(JSContext* ctx, JSValueConst this_val) {
    if (WakeView* v = qjsbind::unwrap<WakeView>(ctx, this_val)) return v->streamId;
    return listenHostDefaultMicId();
}

// u2500u2500u2500 JS-callable functions u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500

// bro.wake.load({ weights, device? }) u2014 load the BC-ResNet checkpoint ONCE into
// the shared net (optional; listen() lazy-loads from its own weights too).
// Drops any existing tenants (their detectors referenced the old net).
JSValue js_load(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "bro.wake.load(opts): opts object required (weights, ...)");
    std::string weights;
    if (!getStr(ctx, argv[0], "weights", weights) || weights.empty())
        return JS_ThrowTypeError(ctx, "bro.wake.load: opts.weights (model path) required");
    // init() BEFORE the device probe: GPU backends register on the driver probe.
    brotensor::init();
    brotensor::Device dev = autoDevice();
    {
        std::string err;
        if (!parseDeviceOpt(ctx, argv[0], dev, err))
            return JS_ThrowTypeError(ctx, "bro.wake.load: %s", err.c_str());
    }
    unloadAll();
    std::string err;
    if (!ensureNet(weights, dev, err))
        return JS_ThrowInternalError(ctx, "bro.wake.load: %s", err.c_str());
    std::fprintf(stderr,
        "[INFO] [wake] BcResnet2d loaded on %s, shared across streams\n",
        deviceName(dev));
    return JS_UNDEFINED;
}

JSValue js_unload(JSContext*, JSValueConst, int, JSValueConst*) {
    unloadAll();
    return JS_UNDEFINED;
}

// bro.wake.listen({ weights?, onFire, threshold?, refractoryMs?, smoothing? })
// Start wake detection on THIS stream. Loads the shared net from opts.weights if
// none is loaded yet. A second listen() on the same stream implicitly stops the
// previous detector first. The detector-policy fields apply to THIS stream's
// detector only.
JSValue js_listen(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (!g_wake.audioEngine)
        return JS_ThrowInternalError(ctx, "bro.wake.listen: audio engine not available");
    if (!g_wake.inference)
        return JS_ThrowInternalError(ctx,
            "bro.wake.listen: audio-inference subsystem not available");
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx,
            "bro.wake.listen(opts): opts object required (weights, onFire, ...)");
    JSValueConst opts = argv[0];

    std::string weights;
    getStr(ctx, opts, "weights", weights);   // optional once the net is loaded

    JSValue onFireVal = JS_GetPropertyStr(ctx, opts, "onFire");
    if (!JS_IsFunction(ctx, onFireVal)) {
        JS_FreeValue(ctx, onFireVal);
        return JS_ThrowTypeError(ctx, "bro.wake.listen: opts.onFire (function) required");
    }

    // init() BEFORE the device probe.
    brotensor::init();
    brotensor::Device dev = g_wake.net ? g_wake.device : autoDevice();
    if (!g_wake.net) {
        std::string err;
        if (!parseDeviceOpt(ctx, opts, dev, err)) {
            JS_FreeValue(ctx, onFireVal);
            return JS_ThrowTypeError(ctx, "bro.wake.listen: %s", err.c_str());
        }
    }
    {
        std::string err;
        if (!ensureNet(weights, dev, err)) {
            JS_FreeValue(ctx, onFireVal);
            return JS_ThrowInternalError(ctx, "bro.wake.listen: %s", err.c_str());
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

    const StreamId sid = streamOf(ctx, this_val);
    WakeTenant* t = ensureTenant(sid);
    if (!t) {
        JS_FreeValue(ctx, onFireVal);
        return JS_ThrowInternalError(ctx, "bro.wake.listen: no stream");
    }
    stopTenant(t);   // implicit-stop on re-listen

    try {
        auto wake = std::make_shared<brosoundml::WakeWord>(g_wake.net);
        wake->set_threshold(static_cast<float>(threshold));
        if (smoothingHits > 0 && smoothingWindow > 0)
            wake->set_smoothing(smoothingHits, smoothingWindow);
        if (refractoryMs >= 0) wake->set_refractory_ms(refractoryMs);

        t->firePending.store(0, std::memory_order_relaxed);
        t->suspended.store(false, std::memory_order_relaxed);
        t->scoreMaxX10000.store(0, std::memory_order_relaxed);

        // Join the listen host on this stream. The host runs ONE PCEN mel pass
        // and hands the new-frame block to WakeWord::feed_mel alongside any
        // other attached tenant. Throws on a front-end mismatch or source
        // failure (the catch detaches, tearing down member-less infra).
        listenStreamSetWake(sid, wake, dev, makeOnWake(t, wake));

        t->wake   = std::move(wake);
        t->onFire = JS_DupValue(ctx, onFireVal);
        JS_FreeValue(ctx, onFireVal);
        t->active = true;

        const int wakeRate = t->wake->config().sample_rate;
        std::fprintf(stderr,
            "[INFO] [wake] listening on stream %u (device=%s, threshold=%.3f, "
            "model=%d Hz)\n",
            sid, deviceName(dev), threshold, wakeRate);
        return JS_UNDEFINED;
    } catch (const std::exception& e) {
        JS_FreeValue(ctx, onFireVal);
        listenStreamSetWake(sid, nullptr, brotensor::Device::CPU, nullptr);
        return JS_ThrowInternalError(ctx, "bro.wake.listen: %s", e.what());
    }
}

JSValue js_stop(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    if (WakeTenant* t = findTenant(streamOf(ctx, this_val))) stopTenant(t);
    return JS_UNDEFINED;
}

JSValue js_suspend(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    if (WakeTenant* t = findTenant(streamOf(ctx, this_val)))
        t->suspended.store(true, std::memory_order_relaxed);
    return JS_UNDEFINED;
}

JSValue js_resume(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    if (WakeTenant* t = findTenant(streamOf(ctx, this_val)))
        t->suspended.store(false, std::memory_order_relaxed);
    return JS_UNDEFINED;
}

JSValue js_lastScore(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    WakeTenant* t = findTenant(streamOf(ctx, this_val));
    return JS_NewFloat64(ctx, (t && t->wake) ? t->wake->last_score() : 0.0);
}

JSValue js_isActive(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    WakeTenant* t = findTenant(streamOf(ctx, this_val));
    return JS_NewBool(ctx, t && t->active);
}

JSValue js_isSuspended(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    WakeTenant* t = findTenant(streamOf(ctx, this_val));
    return JS_NewBool(ctx, t && t->suspended.load(std::memory_order_relaxed));
}

JSValue js_isLoaded(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewBool(ctx, static_cast<bool>(g_wake.net));
}

JSValue js_setThreshold(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsNumber(argv[0]))
        return JS_ThrowTypeError(ctx, "bro.wake.setThreshold(value): number required");
    double t = 0.0;
    JS_ToFloat64(ctx, &t, argv[0]);
    // WakeWord setters are atomic; safe to call while the inference thread feeds.
    WakeTenant* ten = findTenant(streamOf(ctx, this_val));
    if (ten && ten->wake) ten->wake->set_threshold(static_cast<float>(t));
    return JS_UNDEFINED;
}

// Diagnostic surface over THIS stream's mic tap (cf. bro.kws.stats). Returns
// { framesDelivered, samplesDelivered, rollingPeak, scoreMax } or null when no
// tap is installed (e.g. a non-mic loopback stream).
JSValue js_stats(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    WakeTenant* t = findTenant(streamOf(ctx, this_val));
    const broaudio::MicTapId tap =
        t ? listenStreamTapId(t->streamId) : broaudio::kInvalidMicTapId;
    if (!t || !t->active || !g_wake.audioEngine ||
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
    JS_SetPropertyStr(ctx, o, "scoreMax",
        JS_NewFloat64(ctx,
            t->scoreMaxX10000.load(std::memory_order_relaxed) / 10000.0));
    return o;
}

// Manual feed for tests / scripted scenarios on THIS stream. Samples must
// already be at the wake model's native rate (pass it as the optional second
// arg to assert). Mode split:
//   - Headless (no inference worker): the stream's bus runs synchronously on
//     this (the inference) thread and the call returns whether the detector
//     fired (onFire also fires on the next tick unless suspended).
//   - Threaded: samples go into the stream's ring; the fire surfaces via onFire
//     on the next tickWake. Returns undefined.
// Refuses to run while live MIC capture is active (two-producer race).
JSValue js_feed(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    WakeTenant* t = findTenant(streamOf(ctx, this_val));
    if (!t || !t->active || !t->wake)
        return JS_ThrowInternalError(ctx, "bro.wake.feed: no active detector on this stream");
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
    if (!p || bpe != sizeof(float))
        return JS_ThrowTypeError(ctx, "bro.wake.feed: argument must be a Float32Array");
    const int n = static_cast<int>(viewLen / sizeof(float));
    const float* samples = reinterpret_cast<const float*>(p + byteOff);
    const int wakeRate = t->wake->config().sample_rate;

    if (argc >= 2 && JS_IsNumber(argv[1])) {
        int32_t r = 0; JS_ToInt32(ctx, &r, argv[1]);
        if (r > 0 && r != wakeRate) {
            return JS_ThrowTypeError(ctx,
                "bro.wake.feed: sampleRate=%d must equal the wake rate=%d "
                "(use bro.mic to feed mic-rate audio through the resampler)",
                r, wakeRate);
        }
    }

    if (g_wake.inference && g_wake.inference->threaded()) {
        listenStreamWriteRing(t->streamId, samples, n);
        return JS_UNDEFINED;
    }
    try {
        const brosoundml::ListenFeedResult r =
            listenStreamFeedInline(t->streamId, samples, n);
        return JS_NewBool(ctx, r.wake_fired);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "bro.wake.feed: %s", e.what());
    }
}

// Deliver one tenant's pending fires (main thread).
void drainTenant(JSContext* ctx, WakeTenant* t) {
    if (!t->active) return;
    int fires = t->firePending.exchange(0, std::memory_order_acquire);
    if (fires <= 0 || JS_IsUndefined(t->onFire)) return;
    for (int i = 0; i < fires; ++i) {
        JSValue r = JS_Call(ctx, t->onFire, JS_UNDEFINED, 0, nullptr);
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

// u2500u2500u2500 View class registration u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500

void registerWakeViewClass(JSContext* ctx) {
    qjsbind::Class<WakeView>(ctx, "WakeStreamView", qjsbind::NoGlobal)
        .get("active", [](WakeView* v) {
            WakeTenant* t = findTenant(v->streamId);
            return t && t->active;
        })
        .method_raw("listen",       js_listen, 1)
        .method_raw("stop",         js_stop, 0)
        .method_raw("suspend",      js_suspend, 0)
        .method_raw("resume",       js_resume, 0)
        .method_raw("lastScore",    js_lastScore, 0)
        .method_raw("isActive",     js_isActive, 0)
        .method_raw("isSuspended",  js_isSuspended, 0)
        .method_raw("isLoaded",     js_isLoaded, 0)
        .method_raw("setThreshold", js_setThreshold, 1)
        .method_raw("stats",        js_stats, 0)
        .method_raw("feed",         js_feed, 1);
}

} // namespace

JSValue wakeViewFor(JSContext* ctx, std::uint32_t id) {
    return qjsbind::wrap<WakeView>(ctx, new WakeView{static_cast<StreamId>(id)});
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void installWakeBindings(JSContext* ctx, broaudio::Engine* audioEngine, engine::AudioInference* inference) {
    g_wake.audioEngine = audioEngine;
        g_wake.inference   = inference;
        g_wake.ctx         = ctx;

        registerWakeViewClass(ctx);

        JSValue global = JS_GetGlobalObject(ctx);
        JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
        if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
            JS_FreeValue(ctx, broObj);
            broObj = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
        }

        JSValue wake = JS_NewObject(ctx);
        // Namespace ops (shared net u2014 not stream-scoped).
        JS_SetPropertyStr(ctx, wake, "load",
            JS_NewCFunction(ctx, js_load, "load", 1));
        JS_SetPropertyStr(ctx, wake, "unload",
            JS_NewCFunction(ctx, js_unload, "unload", 0));
        // Per-stream ops u2014 on bro.wake they target the shared default-mic stream;
        // the SAME functions are method_raw on WakeView for stream.wake.
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
        JS_SetPropertyStr(ctx, wake, "isLoaded",
            JS_NewCFunction(ctx, js_isLoaded, "isLoaded", 0));
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
    for (auto it = g_wake.tenants.begin(); it != g_wake.tenants.end(); ) {
        WakeTenant* t = it->second.get();
        // Prune a tenant whose stream has closed. The stream's teardown removed
        // its inference task (a barrier), so the onWake closure can no longer
        // run u2014 safe to drop. Default mic is never invalid.
        if (!listenHostValid(t->streamId)) {
            stopTenant(t);   // frees onFire (detach is a no-op u2014 stream gone)
            it = g_wake.tenants.erase(it);
            continue;
        }
        drainTenant(ctx, t);
        ++it;
    }
}

void cleanupWakeBindings(JSContext* /*ctx*/) {
    unloadAll();
    g_wake.audioEngine = nullptr;
    g_wake.inference   = nullptr;
    g_wake.ctx         = nullptr;
}


} // namespace bro::js

#endif // BRO_WITH_SOUNDML
