#if BRO_WITH_SOUNDML

#include "js/sense_bindings.h"
#include "audio_inference/audio_inference.h"
#include "js/listen_host.h"
#include <broaudio/engine.h>
#include <broaudio/mic_tap.h>
#include <brosoundml/sensor_hub.h>
#include <brotensor/runtime.h>
#include <quickjs.h>
#include <qjsbind/qjsbind.h>
#include <cstdint>
#include <cstdio>
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

// u2500u2500u2500 One stream's sensor tenant u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500
struct SenseTenant {
    StreamId                               streamId = kInvalidStream;
    // The hub. Owned by the tenant while active; the stream's task closure holds
    // a second strong ref so a late pump after stop() is harmless.
    std::shared_ptr<brosoundml::SensorHub> hub;
    bool                                   active = false;
};

struct SenseNamespace {
    broaudio::Engine* audioEngine = nullptr;
    AudioInference*   inference   = nullptr;
    JSContext*        ctx         = nullptr;
    std::unordered_map<StreamId, std::unique_ptr<SenseTenant>> tenants;
};

SenseNamespace g_sense;

// A per-stream view object: `stream.sense`. Carries only the stream id.
struct SenseView {
    StreamId streamId = kInvalidStream;
};

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

// Overlay sensor-policy keys present on `obj` onto `cfg` (flat keys u2014 the
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
    JS_SetPropertyStr(ctx, o, "centroid",    JS_NewFloat64(ctx, s.centroid));
    return o;
}

// u2500u2500u2500 Tenant registry u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500

SenseTenant* findTenant(StreamId id) {
    auto it = g_sense.tenants.find(id);
    return it == g_sense.tenants.end() ? nullptr : it->second.get();
}

void dropTenant(StreamId id) {
    auto it = g_sense.tenants.find(id);
    if (it == g_sense.tenants.end()) return;
    if (it->second->active)
        listenStreamSetHub(id, nullptr);   // detach (no-op if the stream is gone)
    g_sense.tenants.erase(it);
}

// The stream `this` addresses: a SenseView u2192 its stream; bro.sense u2192 default
// mic. Returns kInvalidStream (and prunes the tenant) if a view's stream has
// closed.
StreamId streamOf(JSContext* ctx, JSValueConst this_val) {
    if (SenseView* v = qjsbind::unwrap<SenseView>(ctx, this_val)) {
        if (!listenHostValid(v->streamId)) { dropTenant(v->streamId); return kInvalidStream; }
        return v->streamId;
    }
    return listenHostDefaultMicId();
}

SenseTenant* ensureTenant(StreamId id) {
    if (id == kInvalidStream) return nullptr;
    if (SenseTenant* t = findTenant(id)) return t;
    auto t = std::make_unique<SenseTenant>();
    t->streamId = id;
    SenseTenant* p = t.get();
    g_sense.tenants[id] = std::move(t);
    return p;
}

void stopSensing(SenseTenant* t) {
    if (!t->active) return;
    // Detach from the listen host. The host replaces (or tears down) the
    // stream's task; any other member (bro.kws) keeps rolling.
    listenStreamSetHub(t->streamId, nullptr);
    t->hub.reset();
    t->active = false;
}

// u2500u2500u2500 JS-callable functions u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500

// start(opts?) u2014 build the hub and go live on THIS stream.
//   opts (all optional): vadFloorDb, vadSnrDb, vadRiseDbps, vadHangFrames,
//   onsetRatio, onsetAbs, onsetEma, onsetRefractoryFrames,
//   tonalMinPeriodicity, tonalFminHz, tonalFmaxHz.
JSValue js_start(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (!g_sense.audioEngine)
        return JS_ThrowInternalError(ctx, "bro.sense.start: audio engine not available");
    if (!g_sense.inference)
        return JS_ThrowInternalError(ctx,
            "bro.sense.start: audio-inference subsystem not available");
    const StreamId sid = streamOf(ctx, this_val);
    if (sid == kInvalidStream)
        return JS_ThrowInternalError(ctx, "bro.sense.start: stream is closed");
    SenseTenant* t = ensureTenant(sid);
    if (t->active)
        return JS_ThrowInternalError(ctx,
            "bro.sense.start: this stream is already sensing (stop() first)");

    // The hub is pure CPU DSP, but its mel front-end runs on brotensor ops u2014
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

        // Join the listen host on this stream: one source + ring + task drive
        // the hub (alongside bro.kws's spotter, if live) off ONE mel pass. The
        // hub's snapshot IS the delivery u2014 no per-frame callback.
        listenStreamSetHub(sid, hub);
        t->hub    = hub;
        t->active = true;

        std::fprintf(stderr,
            "[INFO] [sense] sensor bus active on stream %u (hub=%d Hz, hop=%d)\n",
            sid, hub->sample_rate(), cfg.mel.hop_length);
        return JS_UNDEFINED;
    } catch (const std::exception& e) {
        t->active = true;   // let stopSensing clear the partial state
        stopSensing(t);
        return JS_ThrowInternalError(ctx, "bro.sense.start: %s", e.what());
    }
}

JSValue js_stop(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    if (SenseTenant* t = findTenant(streamOf(ctx, this_val))) stopSensing(t);
    return JS_UNDEFINED;
}

JSValue js_isActive(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    SenseTenant* t = findTenant(streamOf(ctx, this_val));
    return JS_NewBool(ctx, t && t->active);
}

// snapshot() -> {frames, t, rms, peak, db, voice, ..., tonal, ...}
// Lock-free seqlock read of THIS stream's latest coherent sensor frame; null
// when inactive. Counters (onsets, voiceEvents, tonalEvents) are monotonic, so a
// poller detects events as deltas even when the boolean has already cleared.
JSValue js_snapshot(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    SenseTenant* t = findTenant(streamOf(ctx, this_val));
    if (!t || !t->hub) return JS_NULL;
    return makeSnapshot(ctx, t->hub->snapshot());
}

JSValue js_sampleRate(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    SenseTenant* t = findTenant(streamOf(ctx, this_val));
    return JS_NewInt32(ctx, (t && t->hub) ? t->hub->sample_rate() : 0);
}

// Diagnostic surface over THIS stream's mic tap (cf. bro.kws.stats). Null for a
// non-mic loopback stream.
JSValue js_stats(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    SenseTenant* t = findTenant(streamOf(ctx, this_val));
    const broaudio::MicTapId tap =
        t ? listenStreamTapId(t->streamId) : broaudio::kInvalidMicTapId;
    if (!t || !t->active || !g_sense.audioEngine ||
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

// Manual feed for tests / scripted scenarios on THIS stream. Samples must
// already be at the hub's rate. Mode split mirrors bro.kws.feed:
//   - Headless (no inference worker): the stream's bus runs synchronously and
//     the post-feed snapshot comes back.
//   - Threaded: samples go into the stream's ring; poll snapshot() as usual.
// Refuses to run while live MIC capture is active (two-producer race).
JSValue js_feed(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    SenseTenant* t = findTenant(streamOf(ctx, this_val));
    if (!t || !t->active || !t->hub)
        return JS_ThrowInternalError(ctx, "bro.sense.feed: this stream is not sensing");
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
        listenStreamWriteRing(t->streamId, p, n);
        return JS_UNDEFINED;
    }

    try {
        listenStreamFeedInline(t->streamId, p, n);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "bro.sense.feed: %s", e.what());
    }
    return makeSnapshot(ctx, t->hub->snapshot());
}

// bro.sense.analyze(samples, opts?) -> per-frame sensor timeline for a clip.
// Runs a PRIVATE SensorHub over the clip offline u2014 no live tap, no stream, no
// effect on any bus, callable any time u2014 and returns columnar arrays. Namespace
// op (not stream-scoped). flags packs bit0=voice, bit1=tonal, bit2=onset.
JSValue js_analyze(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    int n = 0;
    const float* p = (argc >= 1) ? readFloats(ctx, argv[0], n) : nullptr;
    if (!p || n <= 0)
        return JS_ThrowTypeError(ctx, "bro.sense.analyze(Float32Array, opts?)");
    try {
        brotensor::init();
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "bro.sense.analyze: %s", e.what());
    }

    brosoundml::SensorHubConfig cfg;
    if (argc >= 2) readConfig(ctx, argv[1], cfg);

    brosoundml::SensorHub hub(cfg);
    const int win = cfg.mel.win_length, hop = cfg.mel.hop_length;
    std::vector<float>   db, hz, per, cen;
    std::vector<int32_t> flags;
    auto take = [&](const brosoundml::SensorSnapshot& s) {
        db.push_back(s.db);
        hz.push_back(s.dominant_hz);
        per.push_back(s.periodicity);
        cen.push_back(s.centroid);
        int32_t f = 0;
        if (s.voice) f |= 1;
        if (s.tonal) f |= 2;
        if (s.onset) f |= 4;
        flags.push_back(f);
    };
    // Prime one window, then advance one hop per frame u2014 identical framing to
    // the enroll path, so frame indices line up with what a gesture captures.
    if (n >= win) {
        hub.feed(p, win);
        take(hub.snapshot());
        int pos = win;
        while (pos + hop <= n) {
            hub.feed(p + pos, hop);
            take(hub.snapshot());
            pos += hop;
        }
    }

    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "frames", JS_NewInt32(ctx, static_cast<int>(db.size())));
    JS_SetPropertyStr(ctx, o, "hop",  JS_NewInt32(ctx, hop));
    JS_SetPropertyStr(ctx, o, "win",  JS_NewInt32(ctx, win));
    JS_SetPropertyStr(ctx, o, "rate", JS_NewInt32(ctx, cfg.mel.sample_rate));
    JS_SetPropertyStr(ctx, o, "frameMs",
        JS_NewFloat64(ctx, 1000.0 * static_cast<double>(hop) /
                               static_cast<double>(cfg.mel.sample_rate)));
    JS_SetPropertyStr(ctx, o, "db",          qjsbind::make_float32_array(ctx, db));
    JS_SetPropertyStr(ctx, o, "dominantHz",  qjsbind::make_float32_array(ctx, hz));
    JS_SetPropertyStr(ctx, o, "periodicity", qjsbind::make_float32_array(ctx, per));
    JS_SetPropertyStr(ctx, o, "centroid",    qjsbind::make_float32_array(ctx, cen));
    JS_SetPropertyStr(ctx, o, "flags",       qjsbind::make_int32_array(ctx, flags));
    return o;
}

// u2500u2500u2500 View class registration u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500

void registerSenseViewClass(JSContext* ctx) {
    qjsbind::Class<SenseView>(ctx, "SenseStreamView", qjsbind::NoGlobal)
        .get("active", [](SenseView* v) {
            SenseTenant* t = findTenant(v->streamId);
            return t && t->active;
        })
        .method_raw("start",      js_start, 1)
        .method_raw("stop",       js_stop, 0)
        .method_raw("isActive",   js_isActive, 0)
        .method_raw("snapshot",   js_snapshot, 0)
        .method_raw("sampleRate", js_sampleRate, 0)
        .method_raw("stats",      js_stats, 0)
        .method_raw("feed",       js_feed, 1)
        .method_raw("analyze",    js_analyze, 2);   // offline; ignores the stream
}

}  // namespace

JSValue senseViewFor(JSContext* ctx, std::uint32_t id) {
    return qjsbind::wrap<SenseView>(ctx, new SenseView{static_cast<StreamId>(id)});
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void installSenseBindings(JSContext* ctx, broaudio::Engine* audioEngine, engine::AudioInference* inference) {
    g_sense.audioEngine = audioEngine;
        g_sense.inference   = inference;
        g_sense.ctx         = ctx;
    
        registerSenseViewClass(ctx);
    
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
        JS_SetPropertyStr(ctx, sense, "analyze",
            JS_NewCFunction(ctx, js_analyze, "analyze", 2));
        JS_SetPropertyStr(ctx, broObj, "sense", sense);
    
        JS_FreeValue(ctx, broObj);
        JS_FreeValue(ctx, global);
}

void cleanupSenseBindings(JSContext* /*ctx*/) {
    for (auto& kv : g_sense.tenants) stopSensing(kv.second.get());
    g_sense.tenants.clear();
    g_sense.audioEngine = nullptr;
    g_sense.inference   = nullptr;
    g_sense.ctx         = nullptr;
}


} // namespace bro::js

#endif // BRO_WITH_SOUNDML
