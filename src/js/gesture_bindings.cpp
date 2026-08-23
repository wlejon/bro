#if BRO_WITH_SOUNDML
#include "js/gesture_bindings.h"
#include "audio_inference/audio_inference.h"
#include "js/listen_host.h"
#include <broaudio/engine.h>
#include <brosoundml/gesture_spotter.h>
#include <quickjs.h>
#include <qjsbind/qjsbind.h>
#include <atomic>
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

constexpr std::uint64_t kEventSlots = 64;

// u2500u2500u2500 One stream's gesture tenant u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500
//
// Address-stable (held by unique_ptr in g_gesture.tenants), so the inference-
// thread onGestures closure can capture a raw GestureTenant* and publish into it.
struct GestureTenant {
    StreamId streamId = kInvalidStream;

    std::shared_ptr<brosoundml::GestureSpotter> spotter;

    std::vector<std::string> names;   // listen()-time snapshot; index i = idx i
    JSValue onGesture = JS_UNDEFINED;

    int                        eventIdx[kEventSlots]   = {};
    float                      eventConf[kEventSlots]  = {};
    std::uint8_t               eventTone[kEventSlots]  = {};   // 1 = tone, 0 = rhythm
    std::int64_t               eventStart[kEventSlots] = {};   // matched-span frames
    std::int64_t               eventEnd[kEventSlots]   = {};   //   (SensorHub frames axis)
    std::atomic<std::uint64_t> produced{0};
    std::atomic<std::uint64_t> drained{0};

    bool listening = false;
};

struct GestureNamespace {
    broaudio::Engine* audioEngine = nullptr;
    AudioInference*   inference   = nullptr;
    JSContext*        ctx         = nullptr;
    std::unordered_map<StreamId, std::unique_ptr<GestureTenant>> tenants;
};

GestureNamespace g_gesture;

// A per-stream view object: `stream.gesture`. Carries only the stream id.
struct GestureView {
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
    if (JS_IsNumber(v)) { int32_t t = dst; JS_ToInt32(ctx, &t, v); dst = t; ok = true; }
    JS_FreeValue(ctx, v);
    return ok;
}

// Overlay gesture-policy keys: tempoTol, pitchTol, pitchStabilityTol, shapeTol,
// refractoryFrames, minOnsets, minToneFrames, onsetSigFrames.
void readPolicy(JSContext* ctx, JSValueConst obj, brosoundml::GestureConfig& cfg) {
    if (!JS_IsObject(obj)) return;
    double d;
    if (getNum(ctx, obj, "tempoTol", d)) cfg.tempo_tol = (float)d;
    if (getNum(ctx, obj, "pitchTol", d)) cfg.pitch_tol = (float)d;
    if (getNum(ctx, obj, "pitchStabilityTol", d)) cfg.pitch_stability_tol = (float)d;
    if (getNum(ctx, obj, "shapeTol", d)) cfg.shape_tol = (float)d;
    getInt(ctx, obj, "refractoryFrames", cfg.refractory_frames);
    getInt(ctx, obj, "minOnsets",        cfg.min_onsets);
    getInt(ctx, obj, "minToneFrames",    cfg.min_tone_frames);
    getInt(ctx, obj, "onsetSigFrames",   cfg.onset_sig_frames);
}

const float* readFloats(JSContext* ctx, JSValueConst v, int& count) {
    count = 0;
    size_t byteOff = 0, viewLen = 0, bpe = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, v, &byteOff, &viewLen, &bpe);
    if (JS_IsException(abuf)) { JS_FreeValue(ctx, JS_GetException(ctx)); return nullptr; }
    size_t abufLen = 0;
    std::uint8_t* p = JS_GetArrayBuffer(ctx, &abufLen, abuf);
    JS_FreeValue(ctx, abuf);
    if (!p || bpe != sizeof(float)) return nullptr;
    count = static_cast<int>(viewLen / sizeof(float));
    return reinterpret_cast<const float*>(p + byteOff);
}

void publishEvent(GestureTenant* t, int nameIdx, float confidence, bool isTone,
                  std::int64_t startFrame, std::int64_t endFrame) {
    const std::uint64_t p = t->produced.load(std::memory_order_relaxed);
    if (p - t->drained.load(std::memory_order_acquire) >= kEventSlots) return;
    t->eventIdx[p % kEventSlots]   = nameIdx;
    t->eventConf[p % kEventSlots]  = confidence;
    t->eventTone[p % kEventSlots]  = isTone ? 1u : 0u;
    t->eventStart[p % kEventSlots] = startFrame;
    t->eventEnd[p % kEventSlots]   = endFrame;
    t->produced.store(p + 1, std::memory_order_release);
}

int nameIndexOf(const std::vector<std::string>& names, const std::string& n) {
    for (std::size_t i = 0; i < names.size(); ++i)
        if (names[i] == n) return static_cast<int>(i);
    return -1;
}

const char* kindName(brosoundml::GestureKind k) {
    return k == brosoundml::GestureKind::Tone ? "tone" : "rhythm";
}

// u2500u2500u2500 Tenant registry u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500

GestureTenant* findTenant(StreamId id) {
    auto it = g_gesture.tenants.find(id);
    return it == g_gesture.tenants.end() ? nullptr : it->second.get();
}

GestureTenant* ensureTenant(StreamId id) {
    if (id == kInvalidStream) return nullptr;
    if (GestureTenant* t = findTenant(id)) return t;
    auto t = std::make_unique<GestureTenant>();
    t->streamId = id;
    GestureTenant* p = t.get();
    g_gesture.tenants[id] = std::move(t);
    return p;
}

// Lazily create the tenant's spotter on first enroll.
brosoundml::GestureSpotter& ensureSpotter(GestureTenant* t) {
    if (!t->spotter)
        t->spotter = std::make_shared<brosoundml::GestureSpotter>();
    return *t->spotter;
}

void stopListening(GestureTenant* t) {
    if (!t->listening) return;
    listenStreamSetGesture(t->streamId, nullptr, nullptr);
    if (g_gesture.ctx && !JS_IsUndefined(t->onGesture)) {
        JS_FreeValue(g_gesture.ctx, t->onGesture);
        t->onGesture = JS_UNDEFINED;
    }
    t->produced.store(0, std::memory_order_relaxed);
    t->drained.store(0, std::memory_order_relaxed);
    t->names.clear();
    t->listening = false;
}

JSValue throwIfListening(JSContext* ctx, const GestureTenant* t, const char* what) {
    if (!t || !t->listening) return JS_UNDEFINED;
    return JS_ThrowInternalError(ctx,
        "bro.gesture.%s: not allowed while this stream is listening "
        "(enroll/remove/clear/reset share the matcher's feed thread u2014 stop() "
        "first)", what);
}

// u2500u2500u2500 Tenant resolution (the dual-home seam) u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500

StreamId streamOf(JSContext* ctx, JSValueConst this_val) {
    if (GestureView* v = qjsbind::unwrap<GestureView>(ctx, this_val)) return v->streamId;
    return listenHostDefaultMicId();
}

// u2500u2500u2500 JS-callable functions u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500

// enrollFromAudio(name, samples, policy?) -> beats
//   samples: Float32Array mono PCM at sampleRate(). Runs the clip through a
//   private SensorHub and stores the extracted rhythm/tone template on THIS
//   stream's spotter.
JSValue js_enrollFromAudio(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    GestureTenant* t = ensureTenant(streamOf(ctx, this_val));
    if (!t)
        return JS_ThrowInternalError(ctx, "bro.gesture.enrollFromAudio: no stream");
    JSValue guard = throwIfListening(ctx, t, "enrollFromAudio");
    if (JS_IsException(guard)) return guard;
    if (argc < 2 || !JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx,
            "bro.gesture.enrollFromAudio(name, samples, policy?): name and "
            "samples required");
    }
    const char* s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_EXCEPTION;
    std::string name = s;
    JS_FreeCString(ctx, s);

    int n = 0;
    const float* p = readFloats(ctx, argv[1], n);
    if (!p || n <= 0) {
        return JS_ThrowTypeError(ctx,
            "bro.gesture.enrollFromAudio: samples must be a non-empty Float32Array");
    }
    try {
        brosoundml::GestureSpotter& g = ensureSpotter(t);
        brosoundml::GestureConfig pol = g.config();
        const bool hasPol = (argc >= 3) && JS_IsObject(argv[2]);
        if (hasPol) readPolicy(ctx, argv[2], pol);
        const int beats = g.enroll_from_audio(name, p, n, hasPol ? &pol : nullptr);
        return JS_NewInt32(ctx, beats);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "bro.gesture.enrollFromAudio: %s", e.what());
    }
}

JSValue js_remove(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    GestureTenant* t = findTenant(streamOf(ctx, this_val));
    if (!t || !t->spotter)
        return JS_ThrowInternalError(ctx, "bro.gesture.remove: nothing enrolled");
    JSValue guard = throwIfListening(ctx, t, "remove");
    if (JS_IsException(guard)) return guard;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "bro.gesture.remove(name): name required");
    const char* s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_EXCEPTION;
    const bool removed = t->spotter->remove(s);
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, removed);
}

JSValue js_clear(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    GestureTenant* t = findTenant(streamOf(ctx, this_val));
    if (!t || !t->spotter) return JS_UNDEFINED;
    JSValue guard = throwIfListening(ctx, t, "clear");
    if (JS_IsException(guard)) return guard;
    t->spotter->clear();
    return JS_UNDEFINED;
}

JSValue js_templates(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    JSValue arr = JS_NewArray(ctx);
    GestureTenant* t = findTenant(streamOf(ctx, this_val));
    if (!t) return arr;
    const std::vector<std::string> names =
        t->listening ? t->names
        : (t->spotter ? t->spotter->templates() : std::vector<std::string>{});
    for (std::uint32_t i = 0; i < names.size(); ++i)
        JS_SetPropertyUint32(ctx, arr, i, JS_NewString(ctx, names[i].c_str()));
    return arr;
}

// inspect(name) -> { name, kind, frameMs, intervalsMs:[...], onsets:[...],
//   toneHz, toneMs, toneSpread } or null. The legible view of an enrolled
//   gesture on THIS stream.
JSValue js_inspect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    GestureTenant* t = findTenant(streamOf(ctx, this_val));
    if (!t || !t->spotter) return JS_NULL;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "bro.gesture.inspect(name): name required");
    const char* s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_EXCEPTION;
    std::string name = s;
    JS_FreeCString(ctx, s);

    brosoundml::GestureView v;
    if (!t->spotter->inspect(name, v)) return JS_NULL;

    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, v.name.c_str()));
    JS_SetPropertyStr(ctx, o, "kind", JS_NewString(ctx, kindName(v.kind)));
    JS_SetPropertyStr(ctx, o, "frameMs", JS_NewFloat64(ctx, v.frame_ms));
    JSValue arr = JS_NewArray(ctx);
    for (std::uint32_t i = 0; i < v.intervals.size(); ++i)
        JS_SetPropertyUint32(ctx, arr, i,
            JS_NewFloat64(ctx, v.intervals[i] * v.frame_ms));
    JS_SetPropertyStr(ctx, o, "intervalsMs", arr);
    JSValue onsets = JS_NewArray(ctx);
    for (std::uint32_t i = 0; i < v.onsets.size(); ++i) {
        JSValue b = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, b, "voiced", JS_NewFloat64(ctx, v.onsets[i].voiced));
        JS_SetPropertyStr(ctx, b, "pitchHz", JS_NewFloat64(ctx, v.onsets[i].pitch));
        JS_SetPropertyStr(ctx, b, "bright", JS_NewFloat64(ctx, v.onsets[i].bright));
        JS_SetPropertyUint32(ctx, onsets, i, b);
    }
    JS_SetPropertyStr(ctx, o, "onsets", onsets);
    JS_SetPropertyStr(ctx, o, "toneHz", JS_NewFloat64(ctx, v.tone_hz));
    JS_SetPropertyStr(ctx, o, "toneMs",
        JS_NewFloat64(ctx, v.tone_frames * v.frame_ms));
    JS_SetPropertyStr(ctx, o, "toneSpread", JS_NewFloat64(ctx, v.tone_spread));
    return o;
}

JSValue js_reset(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    GestureTenant* t = findTenant(streamOf(ctx, this_val));
    if (!t || !t->spotter) return JS_UNDEFINED;
    JSValue guard = throwIfListening(ctx, t, "reset");
    if (JS_IsException(guard)) return guard;
    t->spotter->reset();
    return JS_UNDEFINED;
}

// listen({ onGesture }) u2014 start matching on THIS stream. Requires at least one
// enrolled gesture; needs the stream's bro.sense active to actually fire (the
// matcher reads the SensorHub snapshot).
JSValue js_listen(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (!g_gesture.audioEngine || !g_gesture.inference)
        return JS_ThrowInternalError(ctx,
            "bro.gesture.listen: audio subsystems not available");
    const StreamId sid = streamOf(ctx, this_val);
    GestureTenant* t = ensureTenant(sid);
    if (!t || !t->spotter)
        return JS_ThrowInternalError(ctx,
            "bro.gesture.listen: enroll a gesture first");
    if (t->listening)
        return JS_ThrowInternalError(ctx,
            "bro.gesture.listen: this stream is already listening (stop() first)");
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "bro.gesture.listen(opts): opts object required");

    JSValue cb = JS_GetPropertyStr(ctx, argv[0], "onGesture");
    if (!JS_IsFunction(ctx, cb)) {
        JS_FreeValue(ctx, cb);
        return JS_ThrowTypeError(ctx,
            "bro.gesture.listen: opts.onGesture (function) required");
    }
    const std::vector<std::string> names = t->spotter->templates();
    if (names.empty()) {
        JS_FreeValue(ctx, cb);
        return JS_ThrowInternalError(ctx,
            "bro.gesture.listen: no gestures enrolled on this stream "
            "(enrollFromAudio first)");
    }
    try {
        t->names     = names;
        t->onGesture = JS_DupValue(ctx, cb);
        t->produced.store(0, std::memory_order_relaxed);
        t->drained.store(0, std::memory_order_relaxed);
        JS_FreeValue(ctx, cb);

        listenStreamSetGesture(
            sid, t->spotter,
            [t, names](const std::vector<brosoundml::GestureEvent>& events) {
                for (const auto& ev : events) {
                    const int idx = nameIndexOf(names, ev.name);
                    if (idx >= 0)
                        publishEvent(t, idx, ev.confidence,
                                     ev.kind == brosoundml::GestureKind::Tone,
                                     ev.start_frame, ev.end_frame);
                }
            });
        t->listening = true;
        std::fprintf(stderr, "[INFO] [gesture] listening on stream %u (%zu gesture%s)\n",
                     sid, names.size(), names.size() == 1 ? "" : "s");
        return JS_UNDEFINED;
    } catch (const std::exception& e) {
        t->listening = true;
        stopListening(t);
        return JS_ThrowInternalError(ctx, "bro.gesture.listen: %s", e.what());
    }
}

JSValue js_stop(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    if (GestureTenant* t = findTenant(streamOf(ctx, this_val))) stopListening(t);
    return JS_UNDEFINED;
}

JSValue js_isActive(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    GestureTenant* t = findTenant(streamOf(ctx, this_val));
    return JS_NewBool(ctx, t && t->listening);
}

JSValue js_sampleRate(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    GestureTenant* t = findTenant(streamOf(ctx, this_val));
    return JS_NewInt32(ctx,
        (t && t->spotter) ? t->spotter->sample_rate() : 16000);
}

// Drain one tenant's gesture event ring into its onGesture callback (main thread).
void drainTenant(JSContext* ctx, GestureTenant* t) {
    if (!t->listening) return;
    const std::uint64_t produced = t->produced.load(std::memory_order_acquire);
    std::uint64_t drained = t->drained.load(std::memory_order_relaxed);
    if (drained >= produced || JS_IsUndefined(t->onGesture)) return;
    while (drained < produced) {
        const int   idx  = t->eventIdx[drained % kEventSlots];
        const float conf = t->eventConf[drained % kEventSlots];
        const bool  tone = t->eventTone[drained % kEventSlots] != 0u;
        const std::int64_t startF = t->eventStart[drained % kEventSlots];
        const std::int64_t endF   = t->eventEnd[drained % kEventSlots];
        drained++;
        t->drained.store(drained, std::memory_order_release);
        if (idx < 0 || idx >= (int)t->names.size()) continue;
        // 4th arg: the matched span on the SensorHub frames axis (align with
        // bro.sense.snapshot().frames). Backward-compatible with
        // onGesture(name, conf, kind) handlers.
        JSValue span = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, span, "startFrame", JS_NewInt64(ctx, startF));
        JS_SetPropertyStr(ctx, span, "endFrame", JS_NewInt64(ctx, endF));
        JS_SetPropertyStr(ctx, span, "matchedFrames",
                          JS_NewInt64(ctx, endF >= startF ? endF - startF + 1 : 0));
        JSValue args[4] = {
            JS_NewString(ctx, t->names[(std::size_t)idx].c_str()),
            JS_NewFloat64(ctx, conf),
            JS_NewString(ctx, tone ? "tone" : "rhythm"),
            span,
        };
        JSValue r = JS_Call(ctx, t->onGesture, JS_UNDEFINED, 4, args);
        if (JS_IsException(r)) {
            JSValue exc = JS_GetException(ctx);
            const char* s = JS_ToCString(ctx, exc);
            std::fprintf(stderr, "[ERROR] [gesture] onGesture threw: %s\n", s ? s : "?");
            if (s) JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, exc);
        }
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
        JS_FreeValue(ctx, args[2]);
        JS_FreeValue(ctx, args[3]);
    }
}

// u2500u2500u2500 View class registration u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500

void registerGestureViewClass(JSContext* ctx) {
    qjsbind::Class<GestureView>(ctx, "GestureStreamView", qjsbind::NoGlobal)
        .get("active", [](GestureView* v) {
            GestureTenant* t = findTenant(v->streamId);
            return t && t->listening;
        })
        .method_raw("enrollFromAudio", js_enrollFromAudio, 3)
        .method_raw("remove",          js_remove, 1)
        .method_raw("clear",           js_clear, 0)
        .method_raw("templates",       js_templates, 0)
        .method_raw("inspect",         js_inspect, 1)
        .method_raw("reset",           js_reset, 0)
        .method_raw("listen",          js_listen, 1)
        .method_raw("stop",            js_stop, 0)
        .method_raw("isActive",        js_isActive, 0)
        .method_raw("sampleRate",      js_sampleRate, 0);
}

}  // namespace

JSValue gestureViewFor(JSContext* ctx, std::uint32_t id) {
    return qjsbind::wrap<GestureView>(ctx, new GestureView{static_cast<StreamId>(id)});
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void installGestureBindings(JSContext* ctx, broaudio::Engine* audioEngine, engine::AudioInference* inference) {
    g_gesture.audioEngine = audioEngine;
        g_gesture.inference   = inference;
        g_gesture.ctx         = ctx;
    
        registerGestureViewClass(ctx);
    
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
        if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
            JS_FreeValue(ctx, broObj);
            broObj = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
        }
    
        JSValue ges = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, ges, "enrollFromAudio",
            JS_NewCFunction(ctx, js_enrollFromAudio, "enrollFromAudio", 3));
        JS_SetPropertyStr(ctx, ges, "remove",
            JS_NewCFunction(ctx, js_remove, "remove", 1));
        JS_SetPropertyStr(ctx, ges, "clear",
            JS_NewCFunction(ctx, js_clear, "clear", 0));
        JS_SetPropertyStr(ctx, ges, "templates",
            JS_NewCFunction(ctx, js_templates, "templates", 0));
        JS_SetPropertyStr(ctx, ges, "inspect",
            JS_NewCFunction(ctx, js_inspect, "inspect", 1));
        JS_SetPropertyStr(ctx, ges, "reset",
            JS_NewCFunction(ctx, js_reset, "reset", 0));
        JS_SetPropertyStr(ctx, ges, "listen",
            JS_NewCFunction(ctx, js_listen, "listen", 1));
        JS_SetPropertyStr(ctx, ges, "stop",
            JS_NewCFunction(ctx, js_stop, "stop", 0));
        JS_SetPropertyStr(ctx, ges, "isActive",
            JS_NewCFunction(ctx, js_isActive, "isActive", 0));
        JS_SetPropertyStr(ctx, ges, "sampleRate",
            JS_NewCFunction(ctx, js_sampleRate, "sampleRate", 0));
        JS_SetPropertyStr(ctx, broObj, "gesture", ges);
    
        JS_FreeValue(ctx, broObj);
        JS_FreeValue(ctx, global);
}

void tickGesture(JSContext* ctx) {
    for (auto it = g_gesture.tenants.begin(); it != g_gesture.tenants.end(); ) {
        GestureTenant* t = it->second.get();
        // Prune a tenant whose stream has closed (handle .close()'d or GC'd).
        // The stream's teardown removed its inference task (a barrier), so the
        // onGestures closure can no longer run u2014 safe to drop. Default mic is
        // never invalid.
        if (!listenHostValid(t->streamId)) {
            stopListening(t);   // frees onGesture (detach is a no-op u2014 stream gone)
            it = g_gesture.tenants.erase(it);
            continue;
        }
        drainTenant(ctx, t);
        ++it;
    }
}

void cleanupGestureBindings(JSContext* /*ctx*/) {
    for (auto& kv : g_gesture.tenants) stopListening(kv.second.get());
    g_gesture.tenants.clear();
    g_gesture.audioEngine = nullptr;
    g_gesture.inference   = nullptr;
    g_gesture.ctx         = nullptr;
}


} // namespace bro::js

#endif  // BRO_WITH_SOUNDML
