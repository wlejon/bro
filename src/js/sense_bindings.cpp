// JS bindings for brosoundml::SensorHub — the tier-0 acoustic sensor bus.
//
// Installed onto bro.sense.* by installSenseBindings(). One hub at a time per
// JS context. See sense_bindings.h for the thread split; the short version:
// the hub's feed() runs on the inference thread (headless: inline), and
// snapshot() is a lock-free seqlock read the main thread polls — there is no
// per-frame tick and no callback registry, because every sensor pairs its
// momentary boolean with a monotonic counter, so a poller can never miss an
// event, only observe it one poll late.

#include "js/sense_bindings.h"

#include "audio_inference/audio_inference.h"
#include "js/listen_host.h"

#include <broaudio/engine.h>
#include <broaudio/mic_tap.h>
#include <brosoundml/sensor_hub.h>
#include <brotensor/runtime.h>

#include <quickjs.h>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace bro::js {

namespace {

using engine::AudioInference;

struct SenseState {
    broaudio::Engine* audioEngine = nullptr;
    AudioInference*   inference   = nullptr;
    JSContext*        ctx         = nullptr;

    // The hub. Owned by the binding while active; the listen host's task
    // closure holds a second strong ref so a late pump after stop() is
    // harmless. The audio plumbing (tap, ring, inference task, mel front-end)
    // is the SHARED listen host's — bro.sense is just a member.
    std::shared_ptr<brosoundml::SensorHub> hub;

    bool active = false;
};

SenseState g_sense;

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

// Overlay sensor-policy keys present on `obj` onto `cfg` (flat keys — the
// config is small enough that nesting would just be ceremony).
void readConfig(JSContext* ctx, JSValueConst obj,
                brosoundml::SensorHubConfig& cfg) {
    if (!JS_IsObject(obj)) return;
    double d;
    if (getNum(ctx, obj, "vadFloorDb", d))  cfg.vad_abs_floor_db    = (float)d;
    if (getNum(ctx, obj, "vadSnrDb", d))    cfg.vad_snr_db          = (float)d;
    if (getNum(ctx, obj, "vadRiseDbps", d)) cfg.vad_floor_rise_dbps = (float)d;
    getInt(ctx, obj, "vadHangFrames", cfg.vad_hang_frames);
    if (getNum(ctx, obj, "onsetRatio", d))  cfg.onset_ratio = (float)d;
    if (getNum(ctx, obj, "onsetAbs", d))    cfg.onset_abs   = (float)d;
    if (getNum(ctx, obj, "onsetEma", d))    cfg.onset_ema   = (float)d;
    getInt(ctx, obj, "onsetRefractoryFrames", cfg.onset_refractory_frames);
    if (getNum(ctx, obj, "tonalMinPeriodicity", d))
        cfg.tonal_min_periodicity = (float)d;
    if (getNum(ctx, obj, "tonalFminHz", d)) cfg.tonal_fmin_hz = (float)d;
    if (getNum(ctx, obj, "tonalFmaxHz", d)) cfg.tonal_fmax_hz = (float)d;
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

JSValue makeSnapshot(JSContext* ctx, const brosoundml::SensorSnapshot& s) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "frames", JS_NewInt64(ctx, s.frames));
    JS_SetPropertyStr(ctx, o, "t",      JS_NewFloat64(ctx, s.t));
    JS_SetPropertyStr(ctx, o, "rms",  JS_NewFloat64(ctx, s.rms));
    JS_SetPropertyStr(ctx, o, "peak", JS_NewFloat64(ctx, s.peak));
    JS_SetPropertyStr(ctx, o, "db",   JS_NewFloat64(ctx, s.db));
    JS_SetPropertyStr(ctx, o, "voice",        JS_NewBool(ctx, s.voice));
    JS_SetPropertyStr(ctx, o, "noiseFloorDb", JS_NewFloat64(ctx, s.noise_floor_db));
    JS_SetPropertyStr(ctx, o, "snrDb",        JS_NewFloat64(ctx, s.snr_db));
    JS_SetPropertyStr(ctx, o, "voiceFrames",  JS_NewInt64(ctx, s.voice_frames));
    JS_SetPropertyStr(ctx, o, "voiceEvents",  JS_NewInt64(ctx, s.voice_events));
    JS_SetPropertyStr(ctx, o, "lastVoiceFrame", JS_NewInt64(ctx, s.last_voice_frame));
    JS_SetPropertyStr(ctx, o, "flux",   JS_NewFloat64(ctx, s.flux));
    JS_SetPropertyStr(ctx, o, "onset",  JS_NewBool(ctx, s.onset));
    JS_SetPropertyStr(ctx, o, "onsets", JS_NewInt64(ctx, s.onsets));
    JS_SetPropertyStr(ctx, o, "lastOnsetFrame", JS_NewInt64(ctx, s.last_onset_frame));
    JS_SetPropertyStr(ctx, o, "periodicity", JS_NewFloat64(ctx, s.periodicity));
    JS_SetPropertyStr(ctx, o, "dominantHz",  JS_NewFloat64(ctx, s.dominant_hz));
    JS_SetPropertyStr(ctx, o, "tonal",       JS_NewBool(ctx, s.tonal));
    JS_SetPropertyStr(ctx, o, "tonalFrames", JS_NewInt64(ctx, s.tonal_frames));
    JS_SetPropertyStr(ctx, o, "tonalEvents", JS_NewInt64(ctx, s.tonal_events));
    JS_SetPropertyStr(ctx, o, "lastTonalFrame", JS_NewInt64(ctx, s.last_tonal_frame));
    return o;
}

void stopSensing() {
    if (!g_sense.active) return;
    // Detach from the shared listen host. The host replaces (or tears down)
    // the inference task; if bro.kws is still a member, its stream keeps
    // rolling untouched.
    listenHostSetHub(nullptr);
    g_sense.hub.reset();
    g_sense.active = false;
}

// ─── JS-callable functions ─────────────────────────────────────────────────

// bro.sense.start(opts?) — build the hub and go live on the mic.
//   opts (all optional): vadFloorDb, vadSnrDb, vadRiseDbps, vadHangFrames,
//   onsetRatio, onsetAbs, onsetEma, onsetRefractoryFrames,
//   tonalMinPeriodicity, tonalFminHz, tonalFmaxHz.
JSValue js_start(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!g_sense.audioEngine)
        return JS_ThrowInternalError(ctx, "bro.sense.start: audio engine not available");
    if (!g_sense.inference)
        return JS_ThrowInternalError(ctx,
            "bro.sense.start: audio-inference subsystem not available");
    if (g_sense.active)
        return JS_ThrowInternalError(ctx,
            "bro.sense.start: already active (bro.sense.stop() first)");

    // The hub is pure CPU DSP, but its mel front-end runs on brotensor ops —
    // init() is idempotent and cheap.
    try {
        brotensor::init();
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "bro.sense.start: %s", e.what());
    }

    try {
        brosoundml::SensorHubConfig cfg;
        if (argc >= 1) readConfig(ctx, argv[0], cfg);
        auto hub = std::make_shared<brosoundml::SensorHub>(cfg);

        // Join the shared listen host: one raw tap + ring + task drive the
        // hub (alongside bro.kws's spotter, if live) off ONE mel pass. The
        // hub's snapshot IS the delivery — no per-frame callback.
        listenHostSetHub(hub);
        g_sense.hub    = hub;
        g_sense.active = true;

        std::fprintf(stderr,
            "[INFO] [sense] sensor bus active (mic=%d Hz, hub=%d Hz, hop=%d)\n",
            g_sense.audioEngine->sampleRate(), hub->sample_rate(),
            cfg.mel.hop_length);
        return JS_UNDEFINED;
    } catch (const std::exception& e) {
        g_sense.active = true;   // let stopSensing clear the partial state
        stopSensing();
        return JS_ThrowInternalError(ctx, "bro.sense.start: %s", e.what());
    }
}

JSValue js_stop(JSContext*, JSValueConst, int, JSValueConst*) {
    stopSensing();
    return JS_UNDEFINED;
}

JSValue js_isActive(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewBool(ctx, g_sense.active);
}

// bro.sense.snapshot() -> {frames, t, rms, peak, db, voice, ..., tonal, ...}
// Lock-free seqlock read of the latest coherent sensor frame; null when
// inactive. Counters (onsets, voiceEvents, tonalEvents) are monotonic, so a
// poller detects events as deltas even when the boolean has already cleared.
JSValue js_snapshot(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (!g_sense.hub) return JS_NULL;
    return makeSnapshot(ctx, g_sense.hub->snapshot());
}

JSValue js_sampleRate(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewInt32(ctx, g_sense.hub ? g_sense.hub->sample_rate() : 0);
}

// Diagnostic surface over the SHARED listen-host mic tap (cf. bro.kws.stats).
JSValue js_stats(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    const broaudio::MicTapId tap = listenHostTapId();
    if (!g_sense.active || !g_sense.audioEngine ||
        tap == broaudio::kInvalidMicTapId) {
        return JS_NULL;
    }
    auto s = g_sense.audioEngine->getMicTapStats(tap);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "framesDelivered",
        JS_NewInt64(ctx, static_cast<int64_t>(s.framesDelivered)));
    JS_SetPropertyStr(ctx, o, "samplesDelivered",
        JS_NewInt64(ctx, static_cast<int64_t>(s.samplesDelivered)));
    JS_SetPropertyStr(ctx, o, "rollingPeak", JS_NewFloat64(ctx, s.rollingPeak));
    return o;
}

// Manual feed for tests / scripted scenarios. Samples must already be at the
// hub's rate. Mirrors bro.kws.feed's mode split:
//   - Headless (no inference worker): the shared bus runs synchronously on
//     this (the inference) thread — it is ONE stream, so the feed advances
//     every attached tenant (bro.kws included) — and the post-feed snapshot
//     comes back.
//   - Threaded: samples go into the live shared ring; poll snapshot() as
//     usual. Returns undefined.
// Refuses to run while live capture is active (two-producer race on the ring).
JSValue js_feed(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!g_sense.active || !g_sense.hub)
        return JS_ThrowInternalError(ctx, "bro.sense.feed: not active");
    if (g_sense.audioEngine && g_sense.audioEngine->isMicCapturing()) {
        return JS_ThrowInternalError(ctx,
            "bro.sense.feed: cannot feed while live mic capture is active "
            "(feed is for headless/offline use; the live tap already writes "
            "the ring)");
    }
    int n = 0;
    const float* p = (argc >= 1) ? readFloats(ctx, argv[0], n) : nullptr;
    if (!p)
        return JS_ThrowTypeError(ctx, "bro.sense.feed(Float32Array)");

    if (g_sense.inference && g_sense.inference->threaded()) {
        listenHostWriteRing(p, n);
        return JS_UNDEFINED;
    }

    try {
        listenHostFeedInline(p, n);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "bro.sense.feed: %s", e.what());
    }
    return makeSnapshot(ctx, g_sense.hub->snapshot());
}

}  // namespace

void installSenseBindings(JSContext* ctx, broaudio::Engine* audioEngine,
                          engine::AudioInference* inference) {
    g_sense.audioEngine = audioEngine;
    g_sense.inference   = inference;
    g_sense.ctx         = ctx;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        JS_FreeValue(ctx, broObj);
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue sense = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, sense, "start",
        JS_NewCFunction(ctx, js_start, "start", 1));
    JS_SetPropertyStr(ctx, sense, "stop",
        JS_NewCFunction(ctx, js_stop, "stop", 0));
    JS_SetPropertyStr(ctx, sense, "isActive",
        JS_NewCFunction(ctx, js_isActive, "isActive", 0));
    JS_SetPropertyStr(ctx, sense, "snapshot",
        JS_NewCFunction(ctx, js_snapshot, "snapshot", 0));
    JS_SetPropertyStr(ctx, sense, "sampleRate",
        JS_NewCFunction(ctx, js_sampleRate, "sampleRate", 0));
    JS_SetPropertyStr(ctx, sense, "stats",
        JS_NewCFunction(ctx, js_stats, "stats", 0));
    JS_SetPropertyStr(ctx, sense, "feed",
        JS_NewCFunction(ctx, js_feed, "feed", 1));
    JS_SetPropertyStr(ctx, broObj, "sense", sense);

    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

void cleanupSenseBindings(JSContext* /*ctx*/) {
    stopSensing();
    g_sense.audioEngine = nullptr;
    g_sense.inference   = nullptr;
    g_sense.ctx         = nullptr;
}

}  // namespace bro::js
