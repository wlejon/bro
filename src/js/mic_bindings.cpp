#include "js/mic_bindings.h"
#include <broaudio/engine.h>
#include <broaudio/mic_tap.h>
#include <qjsbind/qjsbind.h>
#include <quickjs.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace bro::js {

namespace {

constexpr int kMicRing = 4096;

struct MicState {
    broaudio::Engine* audioEngine = nullptr;
    JSContext*        ctx         = nullptr;

    JSValue            onChunk = JS_UNDEFINED;
    broaudio::MicTapId tapId   = broaudio::kInvalidMicTapId;
    int                chunkFrames = 0;

    std::atomic<int>      peakRingX10000[kMicRing];
    std::atomic<int>      rmsRingX10000[kMicRing];
    std::atomic<uint64_t> writeCount{0};
    std::atomic<uint64_t> dropped{0};

    bool               wantSamples = false;
    std::vector<float> sampleRing;

    uint64_t lastFired = 0;

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
    g_mic.wantSamples = false;
    g_mic.sampleRing.clear();
    g_mic.active = false;
}

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

} // namespace

static JSValue js_mic_start(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!g_mic.audioEngine) {
        return JS_ThrowInternalError(ctx, "bro.mic.start: audio engine not available");
    }
    JSValueConst opts = (argc >= 1 && JS_IsObject(argv[0])) ? argv[0] : JS_UNDEFINED;
    int  chunkFrames = 160;
    int  targetRate  = 16000;
    bool agc         = false;
    bool live        = true;
    bool samples     = false;
    broaudio::AgcConfig agcCfg;
    JSValue onChunkVal = JS_UNDEFINED;
    if (!JS_IsUndefined(opts)) {
        getInt(ctx, opts, "chunkFrames", chunkFrames);
        getInt(ctx, opts, "targetRate", targetRate);
        getBool(ctx, opts, "agc", agc);
        getBool(ctx, opts, "live", live);
        getBool(ctx, opts, "samples", samples);
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
        return JS_ThrowRangeError(ctx, "bro.mic.start: chunkFrames and targetRate must be >= 0");
    }
    if (samples && chunkFrames <= 0) {
        JS_FreeValue(ctx, onChunkVal);
        return JS_ThrowRangeError(ctx, "bro.mic.start: opts.samples needs a fixed chunkFrames (> 0)");
    }
    shutdownActiveMic();
    broaudio::MicTapConfig cfg;
    cfg.targetRate  = targetRate;
    cfg.chunkFrames = chunkFrames;
    cfg.agc         = agc;
    cfg.agcCfg      = agcCfg;
    g_mic.ctx         = ctx;
    g_mic.chunkFrames = chunkFrames;
    g_mic.onChunk     = JS_IsFunction(ctx, onChunkVal) ? JS_DupValue(ctx, onChunkVal) : JS_UNDEFINED;
    JS_FreeValue(ctx, onChunkVal);
    g_mic.wantSamples = samples;
    if (samples) {
        g_mic.sampleRing.assign(static_cast<size_t>(kMicRing) * static_cast<size_t>(chunkFrames), 0.0f);
    }
    g_mic.writeCount.store(0, std::memory_order_relaxed);
    g_mic.dropped.store(0, std::memory_order_relaxed);
    g_mic.lastFired = 0;
    g_mic.tapId = g_mic.audioEngine->addMicTap(cfg, [](const float* samples, int n) {
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
        g_mic.peakRingX10000[slot].store(static_cast<int>(peak * 10000.0f), std::memory_order_relaxed);
        g_mic.rmsRingX10000[slot].store(static_cast<int>(rms * 10000.0f), std::memory_order_relaxed);
        if (g_mic.wantSamples) {
            const int cf = g_mic.chunkFrames;
            const int m  = n < cf ? n : cf;
            float* dst = g_mic.sampleRing.data() + static_cast<size_t>(slot) * static_cast<size_t>(cf);
            std::memcpy(dst, samples, static_cast<size_t>(m) * sizeof(float));
            if (m < cf) std::memset(dst + m, 0, static_cast<size_t>(cf - m) * sizeof(float));
        }
        g_mic.writeCount.store(idx + 1, std::memory_order_release);
    });
    if (g_mic.tapId == broaudio::kInvalidMicTapId) {
        shutdownActiveMic();
        return JS_ThrowInternalError(ctx, "bro.mic.start: addMicTap failed");
    }
    if (live && !g_mic.audioEngine->isMicCapturing()) {
        g_mic.audioEngine->startMicCapture();
    }
    g_mic.active = true;
    return JS_UNDEFINED;
}

static JSValue js_mic_stop(JSContext*, JSValueConst, int, JSValueConst*) {
    shutdownActiveMic();
    return JS_UNDEFINED;
}

static JSValue js_mic_is_active(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewBool(ctx, g_mic.active);
}

static JSValue js_mic_engine_rate(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewInt32(ctx, g_mic.audioEngine ? g_mic.audioEngine->sampleRate() : 0);
}

static JSValue js_mic_stats(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (!g_mic.active || !g_mic.audioEngine || g_mic.tapId == broaudio::kInvalidMicTapId) return JS_NULL;
    auto s = g_mic.audioEngine->getMicTapStats(g_mic.tapId);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "framesDelivered", JS_NewInt64(ctx, static_cast<int64_t>(s.framesDelivered)));
    JS_SetPropertyStr(ctx, o, "samplesDelivered", JS_NewInt64(ctx, static_cast<int64_t>(s.samplesDelivered)));
    JS_SetPropertyStr(ctx, o, "rollingPeak", JS_NewFloat64(ctx, s.rollingPeak));
    JS_SetPropertyStr(ctx, o, "chunkCount", JS_NewInt64(ctx, static_cast<int64_t>(g_mic.writeCount.load(std::memory_order_acquire))));
    JS_SetPropertyStr(ctx, o, "dropped", JS_NewInt64(ctx, static_cast<int64_t>(g_mic.dropped.load(std::memory_order_relaxed))));
    JS_SetPropertyStr(ctx, o, "chunkFrames", JS_NewInt32(ctx, g_mic.chunkFrames));
    return o;
}

static JSValue js_mic_levels(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    int maxCount = kMicRing;
    if (argc >= 1 && JS_IsNumber(argv[0])) {
        int32_t r = 0; JS_ToInt32(ctx, &r, argv[0]);
        if (r >= 0 && r < maxCount) maxCount = r;
    }
    const uint64_t w = g_mic.writeCount.load(std::memory_order_acquire);
    int avail = static_cast<int>(w < static_cast<uint64_t>(kMicRing) ? w : static_cast<uint64_t>(kMicRing));
    int count = avail < maxCount ? avail : maxCount;
    JSValue arr = JS_NewArray(ctx);
    for (int k = 0; k < count; ++k) {
        const uint64_t i = w - static_cast<uint64_t>(count) + static_cast<uint64_t>(k);
        const int pk = g_mic.peakRingX10000[i % kMicRing].load(std::memory_order_relaxed);
        JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(k), JS_NewFloat64(ctx, pk / 10000.0));
    }
    return arr;
}

static JSValue js_mic_feed(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!g_mic.active) return JS_ThrowInternalError(ctx, "bro.mic.feed: not started");
    if (!g_mic.audioEngine) return JS_ThrowInternalError(ctx, "bro.mic.feed: audio engine not available");
    if (g_mic.audioEngine->isMicCapturing()) return JS_ThrowInternalError(ctx, "bro.mic.feed: cannot feed while live capture is active");
    if (argc < 1) return JS_ThrowTypeError(ctx, "bro.mic.feed(Float32Array, sampleRate?)");
    size_t byteOff = 0, viewLen = 0, bpe = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, argv[0], &byteOff, &viewLen, &bpe);
    if (JS_IsException(abuf)) return JS_EXCEPTION;
    size_t abufLen = 0;
    uint8_t* p = JS_GetArrayBuffer(ctx, &abufLen, abuf);
    JS_FreeValue(ctx, abuf);
    if (!p || bpe != sizeof(float)) return JS_ThrowTypeError(ctx, "bro.mic.feed: argument must be a Float32Array");
    const int n = static_cast<int>(viewLen / sizeof(float));
    const int engineRate = g_mic.audioEngine->sampleRate();
    if (argc >= 2 && JS_IsNumber(argv[1])) {
        int32_t r = 0; JS_ToInt32(ctx, &r, argv[1]);
        if (r > 0 && r != engineRate) return JS_ThrowTypeError(ctx, "bro.mic.feed: sampleRate=%d must equal engine capture rate=%d", r, engineRate);
    }
    g_mic.audioEngine->injectMicSamples(reinterpret_cast<const float*>(p + byteOff), n);
    return JS_UNDEFINED;
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
        if (g_mic.wantSamples) {
            const float* src = g_mic.sampleRing.data()
                + static_cast<size_t>(slot) * static_cast<size_t>(g_mic.chunkFrames);
            JS_SetPropertyStr(ctx, o, "samples",
                qjsbind::make_float32_array(ctx, src,
                    static_cast<size_t>(g_mic.chunkFrames)));
        }
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

void installMicBindings(JSContext* ctx, broaudio::Engine* audioEngine) {
    g_mic.audioEngine = audioEngine;
    g_mic.ctx         = ctx;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue micObj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, micObj, "start",
        JS_NewCFunction(ctx, js_mic_start, "start", 1));
    JS_SetPropertyStr(ctx, micObj, "stop",
        JS_NewCFunction(ctx, js_mic_stop, "stop", 0));
    JS_SetPropertyStr(ctx, micObj, "isActive",
        JS_NewCFunction(ctx, js_mic_is_active, "isActive", 0));
    JS_SetPropertyStr(ctx, micObj, "engineRate",
        JS_NewCFunction(ctx, js_mic_engine_rate, "engineRate", 0));
    JS_SetPropertyStr(ctx, micObj, "stats",
        JS_NewCFunction(ctx, js_mic_stats, "stats", 0));
    JS_SetPropertyStr(ctx, micObj, "levels",
        JS_NewCFunction(ctx, js_mic_levels, "levels", 1));
    JS_SetPropertyStr(ctx, micObj, "feed",
        JS_NewCFunction(ctx, js_mic_feed, "feed", 2));

    JS_SetPropertyStr(ctx, broObj, "mic", micObj);
    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

} // namespace bro::js
