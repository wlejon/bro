// JS bindings for a general live-mic chunk consumer (bro.mic.*).
//
// This is the worked example of broaudio's chunkFrames feature. Where bro.wake
// drives a model that does its own internal framing (so it asks the tap for
// chunkFrames=0), bro.mic asks broaudio for fixed-size frames and shows them
// arriving at a steady cadence — a 16 kHz / 160-frame tap fires onChunk exactly
// once per 10 ms of audio.
//
// Threading model:
//   - bro.mic.start() registers a broaudio MicTap (targetRate, chunkFrames,
//     optional AGC). broaudio owns the polyphase resampler, the AGC, and the
//     chunk buffering; the tap callback runs on the audio thread and only
//     computes peak/RMS over the fixed-size frame and publishes them into a
//     lock-free SPSC ring (single producer = the audio thread or, in headless
//     feed mode, the JS thread; single consumer = the JS main thread).
//   - tickMic() drains the ring once per frame and fires onChunk per new chunk.
//     No JS runs on the audio thread.
//   - bro.mic.levels() / stats() read the ring + tap stats synchronously, so a
//     headless script can assert chunk counts and levels without a frame tick.
//
// Headless / offline:
//   - bro.mic.feed(Float32Array, sampleRate?) pushes synthetic mic-rate audio
//     through Engine::injectMicSamples, which drives the exact same tap
//     (resample → AGC → chunk → callback). It refuses to run while live capture
//     is active, since injectMicSamples and the recording callback would race
//     the same per-tap state.

#include "js/mic_bindings.h"

#include <broaudio/engine.h>
#include <broaudio/mic_tap.h>

#include <quickjs.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace bro::js {

namespace {

// Ring capacity in chunks. At 100 chunks/s (16 kHz / 160) and a 60 fps drain,
// only ~2 chunks accumulate per tick, so the ring never laps in practice; the
// dropped counter records pathological backlogs (a stalled main thread).
constexpr int kMicRing = 4096;

struct MicState {
    broaudio::Engine* audioEngine = nullptr;
    JSContext*        ctx         = nullptr;

    JSValue            onChunk = JS_UNDEFINED;
    broaudio::MicTapId tapId   = broaudio::kInvalidMicTapId;
    int                chunkFrames = 0;

    // Per-chunk ring. Fixed-point x10000 so the cross-thread reads are plain
    // relaxed atomics (no float tearing). Single producer, single consumer.
    std::atomic<int>      peakRingX10000[kMicRing];
    std::atomic<int>      rmsRingX10000[kMicRing];
    std::atomic<uint64_t> writeCount{0};   // total chunks ever published
    std::atomic<uint64_t> dropped{0};      // chunks the drain had to skip

    uint64_t lastFired = 0;   // main-thread only

    bool active = false;
};

MicState g_mic;

void shutdownActiveMic() {
    if (!g_mic.active) return;
    if (g_mic.audioEngine && g_mic.tapId != broaudio::kInvalidMicTapId) {
        g_mic.audioEngine->removeMicTap(g_mic.tapId);
    }
    g_mic.tapId = broaudio::kInvalidMicTapId;
    if (g_mic.ctx && !JS_IsUndefined(g_mic.onChunk)) {
        JS_FreeValue(g_mic.ctx, g_mic.onChunk);
        g_mic.onChunk = JS_UNDEFINED;
    }
    g_mic.writeCount.store(0, std::memory_order_relaxed);
    g_mic.dropped.store(0, std::memory_order_relaxed);
    g_mic.lastFired = 0;
    g_mic.chunkFrames = 0;
    g_mic.active = false;
}

// ─── Option helpers ──────────────────────────────────────────────────────────

bool getNum(JSContext* ctx, JSValueConst obj, const char* key, double& dst) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    bool ok = false;
    if (JS_IsNumber(v)) { JS_ToFloat64(ctx, &dst, v); ok = true; }
    JS_FreeValue(ctx, v);
    return ok;
}

bool getInt(JSContext* ctx, JSValueConst obj, const char* key, int& dst) {
    double d = dst;
    if (getNum(ctx, obj, key, d)) { dst = static_cast<int>(d); return true; }
    return false;
}

bool getBool(JSContext* ctx, JSValueConst obj, const char* key, bool& dst) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    bool ok = false;
    if (JS_IsBool(v)) { dst = JS_ToBool(ctx, v) != 0; ok = true; }
    JS_FreeValue(ctx, v);
    return ok;
}

// ─── JS-callable functions ─────────────────────────────────────────────────

JSValue js_start(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!g_mic.audioEngine) {
        return JS_ThrowInternalError(ctx, "bro.mic.start: audio engine not available");
    }
    JSValueConst opts = (argc >= 1 && JS_IsObject(argv[0])) ? argv[0] : JS_UNDEFINED;

    int  chunkFrames = 160;     // 10 ms at 16 kHz
    int  targetRate  = 16000;   // 0 = engine native rate (no resampling)
    bool agc         = false;   // meter shows true input level by default
    bool live        = true;    // open the recording device; false = feed-only
    broaudio::AgcConfig agcCfg;

    JSValue onChunkVal = JS_UNDEFINED;
    if (!JS_IsUndefined(opts)) {
        getInt(ctx, opts, "chunkFrames", chunkFrames);
        getInt(ctx, opts, "targetRate", targetRate);
        getBool(ctx, opts, "agc", agc);
        getBool(ctx, opts, "live", live);

        double d;
        if (getNum(ctx, opts, "targetPeak",  d)) agcCfg.targetPeak  = static_cast<float>(d);
        if (getNum(ctx, opts, "halfLifeSec", d)) agcCfg.halfLifeSec = static_cast<float>(d);
        if (getNum(ctx, opts, "noiseGate",   d)) agcCfg.noiseGate   = static_cast<float>(d);
        if (getNum(ctx, opts, "maxGain",     d)) agcCfg.maxGain     = static_cast<float>(d);

        onChunkVal = JS_GetPropertyStr(ctx, opts, "onChunk");
        if (!JS_IsUndefined(onChunkVal) && !JS_IsFunction(ctx, onChunkVal)) {
            JS_FreeValue(ctx, onChunkVal);
            return JS_ThrowTypeError(ctx, "bro.mic.start: opts.onChunk must be a function");
        }
    }

    if (chunkFrames < 0 || targetRate < 0) {
        JS_FreeValue(ctx, onChunkVal);
        return JS_ThrowRangeError(ctx,
            "bro.mic.start: chunkFrames and targetRate must be >= 0");
    }

    // Replace any prior consumer before installing the new one.
    shutdownActiveMic();

    broaudio::MicTapConfig cfg;
    cfg.targetRate  = targetRate;
    cfg.chunkFrames = chunkFrames;
    cfg.agc         = agc;
    cfg.agcCfg      = agcCfg;

    g_mic.ctx         = ctx;
    g_mic.chunkFrames = chunkFrames;
    g_mic.onChunk     = JS_IsFunction(ctx, onChunkVal) ? JS_DupValue(ctx, onChunkVal)
                                                       : JS_UNDEFINED;
    JS_FreeValue(ctx, onChunkVal);
    g_mic.writeCount.store(0, std::memory_order_relaxed);
    g_mic.dropped.store(0, std::memory_order_relaxed);
    g_mic.lastFired = 0;

    g_mic.tapId = g_mic.audioEngine->addMicTap(
        cfg,
        [](const float* samples, int n) {
            if (n <= 0) return;
            float peak = 0.0f, sumSq = 0.0f;
            for (int i = 0; i < n; ++i) {
                const float a = std::fabs(samples[i]);
                if (a > peak) peak = a;
                sumSq += samples[i] * samples[i];
            }
            const float rms = std::sqrt(sumSq / static_cast<float>(n));
            const uint64_t idx = g_mic.writeCount.load(std::memory_order_relaxed);
            const int slot = static_cast<int>(idx % kMicRing);
            g_mic.peakRingX10000[slot].store(
                static_cast<int>(peak * 10000.0f), std::memory_order_relaxed);
            g_mic.rmsRingX10000[slot].store(
                static_cast<int>(rms * 10000.0f), std::memory_order_relaxed);
            // Publish last — release pairs with the acquire load in tickMic /
            // levels so the slot writes are visible before the count advances.
            g_mic.writeCount.store(idx + 1, std::memory_order_release);
        });
    if (g_mic.tapId == broaudio::kInvalidMicTapId) {
        shutdownActiveMic();
        return JS_ThrowInternalError(ctx, "bro.mic.start: addMicTap failed");
    }

    // Live capture for windowed use. Harmless if already running (wake or a
    // getUserMedia node may share the same SDL stream). Pass live:false to skip
    // it — headless/offline scripts drive the tap via bro.mic.feed instead, and
    // must NOT open the device (feed and the recording callback would race).
    if (live && !g_mic.audioEngine->isMicCapturing()) {
        g_mic.audioEngine->startMicCapture();
    }
    g_mic.active = true;

    const int micRate = g_mic.audioEngine->sampleRate();
    const int effRate = targetRate > 0 ? targetRate : micRate;
    std::fprintf(stderr,
        "[INFO] [mic] started (mic=%d Hz, target=%d Hz%s, chunkFrames=%d, agc=%s, %s)\n",
        micRate, effRate, (targetRate > 0 && targetRate != micRate) ? ", resampling" : "",
        chunkFrames, agc ? "on" : "off", live ? "live" : "feed-only");
    return JS_UNDEFINED;
}

JSValue js_stop(JSContext*, JSValueConst, int, JSValueConst*) {
    shutdownActiveMic();
    return JS_UNDEFINED;
}

JSValue js_isActive(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewBool(ctx, g_mic.active);
}

// The engine's native mic rate. bro.mic.feed expects samples at this rate.
JSValue js_engineRate(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewInt32(ctx, g_mic.audioEngine ? g_mic.audioEngine->sampleRate() : 0);
}

JSValue js_stats(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (!g_mic.active || !g_mic.audioEngine ||
        g_mic.tapId == broaudio::kInvalidMicTapId) {
        return JS_NULL;
    }
    auto s = g_mic.audioEngine->getMicTapStats(g_mic.tapId);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "framesDelivered",
        JS_NewInt64(ctx, static_cast<int64_t>(s.framesDelivered)));
    JS_SetPropertyStr(ctx, o, "samplesDelivered",
        JS_NewInt64(ctx, static_cast<int64_t>(s.samplesDelivered)));
    JS_SetPropertyStr(ctx, o, "rollingPeak", JS_NewFloat64(ctx, s.rollingPeak));
    JS_SetPropertyStr(ctx, o, "chunkCount",
        JS_NewInt64(ctx,
            static_cast<int64_t>(g_mic.writeCount.load(std::memory_order_acquire))));
    JS_SetPropertyStr(ctx, o, "dropped",
        JS_NewInt64(ctx,
            static_cast<int64_t>(g_mic.dropped.load(std::memory_order_relaxed))));
    JS_SetPropertyStr(ctx, o, "chunkFrames", JS_NewInt32(ctx, g_mic.chunkFrames));
    return o;
}

// Snapshot of the most recent chunk peaks, oldest-first. Returns a JS Array of
// numbers in [0, ~1]. For a scrolling level meter that polls each frame.
JSValue js_levels(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    int maxCount = kMicRing;
    if (argc >= 1 && JS_IsNumber(argv[0])) {
        int32_t r = 0; JS_ToInt32(ctx, &r, argv[0]);
        if (r >= 0 && r < maxCount) maxCount = r;
    }
    const uint64_t w = g_mic.writeCount.load(std::memory_order_acquire);
    int avail = static_cast<int>(w < static_cast<uint64_t>(kMicRing)
                                     ? w : static_cast<uint64_t>(kMicRing));
    int count = avail < maxCount ? avail : maxCount;

    JSValue arr = JS_NewArray(ctx);
    for (int k = 0; k < count; ++k) {
        const uint64_t i = w - static_cast<uint64_t>(count) + static_cast<uint64_t>(k);
        const int pk = g_mic.peakRingX10000[i % kMicRing].load(std::memory_order_relaxed);
        JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(k),
                             JS_NewFloat64(ctx, pk / 10000.0));
    }
    return arr;
}

// Headless / offline feed. Pushes mic-rate audio through the same tap the live
// audio thread uses. Refuses to run while live capture is active.
JSValue js_feed(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!g_mic.active)
        return JS_ThrowInternalError(ctx, "bro.mic.feed: not started");
    if (!g_mic.audioEngine)
        return JS_ThrowInternalError(ctx, "bro.mic.feed: audio engine not available");
    if (g_mic.audioEngine->isMicCapturing()) {
        return JS_ThrowInternalError(ctx,
            "bro.mic.feed: cannot feed while live mic capture is active "
            "(feed is for headless/offline use)");
    }
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "bro.mic.feed(Float32Array, sampleRate?)");

    size_t byteOff = 0, viewLen = 0, bpe = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, argv[0], &byteOff, &viewLen, &bpe);
    if (JS_IsException(abuf)) return JS_EXCEPTION;
    size_t abufLen = 0;
    std::uint8_t* p = JS_GetArrayBuffer(ctx, &abufLen, abuf);
    JS_FreeValue(ctx, abuf);
    if (!p || bpe != sizeof(float)) {
        return JS_ThrowTypeError(ctx, "bro.mic.feed: argument must be a Float32Array");
    }
    const int n = static_cast<int>(viewLen / sizeof(float));

    const int engineRate = g_mic.audioEngine->sampleRate();
    if (argc >= 2 && JS_IsNumber(argv[1])) {
        int32_t r = 0; JS_ToInt32(ctx, &r, argv[1]);
        if (r > 0 && r != engineRate) {
            return JS_ThrowTypeError(ctx,
                "bro.mic.feed: sampleRate=%d must equal the engine mic rate=%d "
                "(samples are injected as if from the device, then the tap "
                "resamples to its target rate)",
                r, engineRate);
        }
    }

    g_mic.audioEngine->injectMicSamples(
        reinterpret_cast<const float*>(p + byteOff), n);
    return JS_UNDEFINED;
}

}  // namespace

void installMicBindings(JSContext* ctx, broaudio::Engine* audioEngine) {
    g_mic.audioEngine = audioEngine;
    g_mic.ctx         = ctx;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        JS_FreeValue(ctx, broObj);
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue mic = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, mic, "start",
        JS_NewCFunction(ctx, js_start, "start", 1));
    JS_SetPropertyStr(ctx, mic, "stop",
        JS_NewCFunction(ctx, js_stop, "stop", 0));
    JS_SetPropertyStr(ctx, mic, "isActive",
        JS_NewCFunction(ctx, js_isActive, "isActive", 0));
    JS_SetPropertyStr(ctx, mic, "engineRate",
        JS_NewCFunction(ctx, js_engineRate, "engineRate", 0));
    JS_SetPropertyStr(ctx, mic, "stats",
        JS_NewCFunction(ctx, js_stats, "stats", 0));
    JS_SetPropertyStr(ctx, mic, "levels",
        JS_NewCFunction(ctx, js_levels, "levels", 1));
    JS_SetPropertyStr(ctx, mic, "feed",
        JS_NewCFunction(ctx, js_feed, "feed", 1));
    JS_SetPropertyStr(ctx, broObj, "mic", mic);

    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

void tickMic(JSContext* ctx) {
    if (!g_mic.active) return;
    const uint64_t w = g_mic.writeCount.load(std::memory_order_acquire);
    if (w == g_mic.lastFired) return;
    if (JS_IsUndefined(g_mic.onChunk)) { g_mic.lastFired = w; return; }

    uint64_t start = g_mic.lastFired;
    if (w - start > static_cast<uint64_t>(kMicRing)) {
        g_mic.dropped.fetch_add(w - start - kMicRing, std::memory_order_relaxed);
        start = w - kMicRing;
    }
    for (uint64_t i = start; i < w; ++i) {
        const int slot = static_cast<int>(i % kMicRing);
        const int pk  = g_mic.peakRingX10000[slot].load(std::memory_order_relaxed);
        const int rms = g_mic.rmsRingX10000[slot].load(std::memory_order_relaxed);
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "index", JS_NewInt64(ctx, static_cast<int64_t>(i)));
        JS_SetPropertyStr(ctx, o, "peak",  JS_NewFloat64(ctx, pk / 10000.0));
        JS_SetPropertyStr(ctx, o, "rms",   JS_NewFloat64(ctx, rms / 10000.0));
        JSValue argv[1] = { o };
        JSValue r = JS_Call(ctx, g_mic.onChunk, JS_UNDEFINED, 1, argv);
        if (JS_IsException(r)) {
            JSValue exc = JS_GetException(ctx);
            const char* s = JS_ToCString(ctx, exc);
            std::fprintf(stderr, "[ERROR] [mic] onChunk threw: %s\n", s ? s : "?");
            if (s) JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, exc);
        }
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, o);
    }
    g_mic.lastFired = w;
}

void cleanupMicBindings(JSContext* /*ctx*/) {
    shutdownActiveMic();
    g_mic.audioEngine = nullptr;
    g_mic.ctx         = nullptr;
}

}  // namespace bro::js
