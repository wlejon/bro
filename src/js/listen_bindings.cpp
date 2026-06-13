// bro.listen — JS surface over the shared listen host's stream retention.
// Thin wrapper over the listenHost* retention API (listen_host.h); all state and
// threading live there. See docs/listen-api.js.

#include "js/listen_bindings.h"

#include "js/kws_bindings.h"
#include "js/listen_host.h"
#include "js/wake_bindings.h"

#include <broaudio/loopback_capture.h>
#include <brosoundml/listen_bus.h>

#include <qjsbind/qjsbind.h>
#include <quickjs.h>

#include <cstdint>
#include <string>
#include <vector>

namespace bro::js {

namespace {

// bro.listen.retain(seconds) — enable/resize raw-audio retention to `seconds`
// of the shared stream (0 disables and frees it). Source-agnostic: captures
// whatever drives the host (mic, scripted feed, future loopback). Takes effect
// immediately when the stream is live, else on the next start.
JSValue js_retain(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    int seconds = 0;
    if (argc >= 1 && JS_IsNumber(argv[0])) {
        int32_t s = 0;
        JS_ToInt32(ctx, &s, argv[0]);
        seconds = s;
    }
    listenHostSetRetention(seconds);
    return JS_UNDEFINED;
}

// bro.listen.audio(startFrame, endFrame) -> Float32Array | null.
// The retained raw PCM for the inclusive frame range (frames axis == samples /
// hop, same as bro.sense.snapshot().frames). null when retention is off or the
// range fell outside the held window (too old / future).
JSValue js_audio(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2 || !JS_IsNumber(argv[0]) || !JS_IsNumber(argv[1]))
        return JS_ThrowTypeError(ctx,
            "bro.listen.audio(startFrame, endFrame): two frame indices required");
    int64_t a = 0, b = 0;
    JS_ToInt64(ctx, &a, argv[0]);
    JS_ToInt64(ctx, &b, argv[1]);
    std::vector<float> out;
    const int n = listenHostReadAudio(static_cast<std::int64_t>(a),
                                      static_cast<std::int64_t>(b), out);
    if (n <= 0) return JS_NULL;
    return qjsbind::make_float32_array(ctx, out.data(),
                                       static_cast<std::size_t>(n));
}

// bro.listen.frame() -> current stream frame (total samples consumed / hop).
JSValue js_frame(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewInt64(ctx, listenHostStreamFrame());
}

// bro.listen.info() -> { active, seconds, rate, hop, frameRate, streamFrame,
// heldFrames, heldSeconds } — retention status for a UI / scrubber.
JSValue js_info(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    const ListenRetentionInfo r = listenHostRetentionInfo();
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "active", JS_NewBool(ctx, r.active));
    JS_SetPropertyStr(ctx, o, "seconds", JS_NewInt32(ctx, r.seconds));
    JS_SetPropertyStr(ctx, o, "rate", JS_NewInt32(ctx, r.rate));
    JS_SetPropertyStr(ctx, o, "hop", JS_NewInt32(ctx, r.hop));
    JS_SetPropertyStr(ctx, o, "frameRate",
                      JS_NewFloat64(ctx, r.hop > 0 ? (double)r.rate / r.hop : 0.0));
    JS_SetPropertyStr(ctx, o, "streamFrame", JS_NewInt64(ctx, r.streamFrame));
    JS_SetPropertyStr(ctx, o, "heldFrames", JS_NewInt64(ctx, r.heldFrames));
    JS_SetPropertyStr(ctx, o, "heldSeconds",
                      JS_NewFloat64(ctx, r.rate > 0
                          ? (double)(r.heldFrames * r.hop) / r.rate : 0.0));
    return o;
}

// ─── ListenStream handle — one independent listening pipeline ─────────────────
//
// `bro.listen.open(source)` returns one of these. It wraps a StreamId and
// delegates to the per-stream listen-host API (listen_host.h); the host owns all
// audio infra + member state, so the handle holds nothing but the id. Closing
// (explicitly via .close() or when the handle is GC'd) frees the stream's source
// + infra. StreamIds are monotonic (never reused), so a stale handle can never
// address a different stream — its methods just no-op once invalid.

struct StreamHandle {
    StreamId           id   = kInvalidStream;
    ListenSource::Kind kind = ListenSource::Kind::Mic;
    ~StreamHandle() {
        if (id != kInvalidStream) listenHostClose(id);
    }
};

const char* kindName(ListenSource::Kind k) {
    switch (k) {
        case ListenSource::Kind::Mic:             return "mic";
        case ListenSource::Kind::SystemLoopback:  return "system";
        case ListenSource::Kind::ProcessLoopback: return "process";
    }
    return "?";
}

StreamHandle* streamSelf(JSContext* ctx, JSValueConst this_val) {
    return qjsbind::unwrap<StreamHandle>(ctx, this_val);
}

// stream.retain(seconds) — see bro.listen.retain (per-stream).
JSValue js_stream_retain(JSContext* ctx, JSValueConst this_val,
                         int argc, JSValueConst* argv) {
    StreamHandle* h = streamSelf(ctx, this_val);
    if (!h) return JS_ThrowTypeError(ctx, "retain: not a ListenStream");
    int seconds = 0;
    if (argc >= 1 && JS_IsNumber(argv[0])) {
        int32_t s = 0;
        JS_ToInt32(ctx, &s, argv[0]);
        seconds = s;
    }
    listenStreamSetRetention(h->id, seconds);
    return JS_UNDEFINED;
}

// stream.audio(startFrame, endFrame) -> Float32Array | null (per-stream).
JSValue js_stream_audio(JSContext* ctx, JSValueConst this_val,
                        int argc, JSValueConst* argv) {
    StreamHandle* h = streamSelf(ctx, this_val);
    if (!h) return JS_ThrowTypeError(ctx, "audio: not a ListenStream");
    if (argc < 2 || !JS_IsNumber(argv[0]) || !JS_IsNumber(argv[1]))
        return JS_ThrowTypeError(ctx,
            "stream.audio(startFrame, endFrame): two frame indices required");
    int64_t a = 0, b = 0;
    JS_ToInt64(ctx, &a, argv[0]);
    JS_ToInt64(ctx, &b, argv[1]);
    std::vector<float> out;
    const int n = listenStreamReadAudio(h->id, static_cast<std::int64_t>(a),
                                        static_cast<std::int64_t>(b), out);
    if (n <= 0) return JS_NULL;
    return qjsbind::make_float32_array(ctx, out.data(),
                                       static_cast<std::size_t>(n));
}

// stream.frame() -> current stream frame.
JSValue js_stream_frame(JSContext* ctx, JSValueConst this_val,
                        int, JSValueConst*) {
    StreamHandle* h = streamSelf(ctx, this_val);
    if (!h) return JS_ThrowTypeError(ctx, "frame: not a ListenStream");
    return JS_NewInt64(ctx, listenStreamFrame(h->id));
}

// stream.info() -> retention status object (per-stream; same shape as
// bro.listen.info()).
JSValue js_stream_info(JSContext* ctx, JSValueConst this_val,
                       int, JSValueConst*) {
    StreamHandle* h = streamSelf(ctx, this_val);
    if (!h) return JS_ThrowTypeError(ctx, "info: not a ListenStream");
    const ListenRetentionInfo r = listenStreamRetentionInfo(h->id);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "active", JS_NewBool(ctx, r.active));
    JS_SetPropertyStr(ctx, o, "seconds", JS_NewInt32(ctx, r.seconds));
    JS_SetPropertyStr(ctx, o, "rate", JS_NewInt32(ctx, r.rate));
    JS_SetPropertyStr(ctx, o, "hop", JS_NewInt32(ctx, r.hop));
    JS_SetPropertyStr(ctx, o, "frameRate",
                      JS_NewFloat64(ctx, r.hop > 0 ? (double)r.rate / r.hop : 0.0));
    JS_SetPropertyStr(ctx, o, "streamFrame", JS_NewInt64(ctx, r.streamFrame));
    JS_SetPropertyStr(ctx, o, "heldFrames", JS_NewInt64(ctx, r.heldFrames));
    JS_SetPropertyStr(ctx, o, "heldSeconds",
                      JS_NewFloat64(ctx, r.rate > 0
                          ? (double)(r.heldFrames * r.hop) / r.rate : 0.0));
    return o;
}

// stream.feed(Float32Array) — scripted/headless feed at the stream's rate. The
// host picks the path (threaded ring write vs inline bus run). For driving
// retention + attached models in tests; live capture writes the ring itself.
JSValue js_stream_feed(JSContext* ctx, JSValueConst this_val,
                       int argc, JSValueConst* argv) {
    StreamHandle* h = streamSelf(ctx, this_val);
    if (!h) return JS_ThrowTypeError(ctx, "feed: not a ListenStream");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "stream.feed(Float32Array): samples required");
    std::vector<float> samples = qjsbind::read_float32_array(ctx, argv[0]);
    if (samples.empty())
        return JS_ThrowTypeError(ctx,
            "stream.feed: samples must be a non-empty Float32Array");
    listenStreamFeed(h->id, samples.data(), static_cast<int>(samples.size()));
    return JS_UNDEFINED;
}

// stream.close() — detach members, stop the source, free infra. Idempotent.
JSValue js_stream_close(JSContext* ctx, JSValueConst this_val,
                        int, JSValueConst*) {
    StreamHandle* h = streamSelf(ctx, this_val);
    if (!h) return JS_ThrowTypeError(ctx, "close: not a ListenStream");
    if (h->id != kInvalidStream) {
        listenHostClose(h->id);
        h->id = kInvalidStream;
    }
    return JS_UNDEFINED;
}

void registerListenStreamClass(JSContext* ctx) {
    qjsbind::Class<StreamHandle>(ctx, "ListenStream", qjsbind::NoGlobal)
        .get("id",    [](StreamHandle* h) { return static_cast<int>(h->id); })
        .get("kind",  [](StreamHandle* h) { return std::string(kindName(h->kind)); })
        .get("valid", [](StreamHandle* h) { return listenHostValid(h->id); })
        .method_raw("retain", js_stream_retain, 1)
        .method_raw("audio",  js_stream_audio, 2)
        .method_raw("frame",  js_stream_frame, 0)
        .method_raw("info",   js_stream_info, 0)
        .method_raw("feed",   js_stream_feed, 1)
        .method_raw("close",  js_stream_close, 0);
}

// Parse a bro.listen.open() source argument into a ListenSource.
//   undefined | "mic" | {mic:true}                         -> Mic
//   "system" | {system:true}                               -> SystemLoopback
//   {process: pid} | {pid: n, exclude?: bool}              -> ProcessLoopback
//   any loopback source may carry { channel: n } (-1 downmix; >=0 pick one).
bool parseSource(JSContext* ctx, JSValueConst v, ListenSource& out,
                 std::string& err) {
    out = ListenSource{};
    if (JS_IsUndefined(v) || JS_IsNull(v)) return true;  // default mic
    if (JS_IsString(v)) {
        const char* s = JS_ToCString(ctx, v);
        std::string sv = s ? s : "";
        if (s) JS_FreeCString(ctx, s);
        if (sv == "mic")    { out.kind = ListenSource::Kind::Mic; return true; }
        if (sv == "system") { out.kind = ListenSource::Kind::SystemLoopback; return true; }
        err = "source string must be 'mic' or 'system' (use {process:pid} for an app)";
        return false;
    }
    if (!JS_IsObject(v)) { err = "source must be a string or an object"; return false; }

    auto boolProp = [&](const char* k) -> bool {
        JSValue p = JS_GetPropertyStr(ctx, v, k);
        bool b = JS_ToBool(ctx, p) > 0;
        JS_FreeValue(ctx, p);
        return b;
    };
    auto numProp = [&](const char* k, bool& present) -> double {
        JSValue p = JS_GetPropertyStr(ctx, v, k);
        present = JS_IsNumber(p) > 0;
        double d = 0;
        if (present) JS_ToFloat64(ctx, &d, p);
        JS_FreeValue(ctx, p);
        return d;
    };

    bool hasProcess = false, hasPid = false;
    const double procPid = numProp("process", hasProcess);
    const double pidVal  = numProp("pid", hasPid);
    if (hasProcess || hasPid) {
        out.kind    = ListenSource::Kind::ProcessLoopback;
        out.pid     = static_cast<std::uint32_t>(hasProcess ? procPid : pidVal);
        out.exclude = boolProp("exclude");
    } else if (boolProp("system")) {
        out.kind = ListenSource::Kind::SystemLoopback;
    } else {
        out.kind = ListenSource::Kind::Mic;  // {} or {mic:true}
    }
    bool hasChannel = false;
    const double ch = numProp("channel", hasChannel);
    if (hasChannel) out.channel = static_cast<int>(ch);
    return true;
}

// bro.listen.open(source?) -> ListenStream | throws.
// Opens an independent listening pipeline on `source`. Loopback sources start
// capturing immediately. Throws if the source is unavailable (loopback
// unsupported on this build/OS, or the target process is gone).
JSValue js_open(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    ListenSource src;
    std::string err;
    if (!parseSource(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED, src, err))
        return JS_ThrowTypeError(ctx, "bro.listen.open: %s", err.c_str());

    if (src.kind != ListenSource::Kind::Mic && !listenHostLoopbackSupported())
        return JS_ThrowInternalError(ctx,
            "bro.listen.open: loopback/per-app capture not supported on this build/OS");

    const StreamId id = listenHostOpen(src);
    if (id == kInvalidStream)
        return JS_ThrowInternalError(ctx,
            "bro.listen.open: could not open the stream (source unavailable, "
            "or the audio subsystem is not ready)");

    auto* h = new StreamHandle{id, src.kind};
    JSValue wrapper = qjsbind::wrap<StreamHandle>(ctx, h);
    // Per-stream tenant views. Each carries this stream's id and exposes the
    // model bindings scoped to it (stream.kws.enroll/listen/…). The shared
    // weights load once via the namespace op (bro.kws.load); each stream
    // attaches its own spotter/session over them. (wake/sense/gesture join
    // here as their slices land.)
    JS_SetPropertyStr(ctx, wrapper, "kws",  kwsViewFor(ctx, id));
    JS_SetPropertyStr(ctx, wrapper, "wake", wakeViewFor(ctx, id));
    return wrapper;
}

// bro.listen.supported() -> bool. Is render-side (system / per-app) capture
// available on this build/OS? (Mic streams are always available.)
JSValue js_supported(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewBool(ctx, listenHostLoopbackSupported());
}

// bro.listen.apps() -> [{ pid, name }, ...]. Applications currently holding a
// render audio session — the candidates for {process: pid}. Empty when loopback
// is unsupported.
JSValue js_apps(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    const std::vector<broaudio::AudioProcess> apps = listenHostEnumerateApps();
    JSValue arr = JS_NewArray(ctx);
    for (std::uint32_t i = 0; i < apps.size(); ++i) {
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "pid", JS_NewInt64(ctx, apps[i].pid));
        JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, apps[i].name.c_str()));
        JS_SetPropertyUint32(ctx, arr, i, o);
    }
    return arr;
}

}  // namespace

void installListenBindings(JSContext* ctx) {
    registerListenStreamClass(ctx);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        JS_FreeValue(ctx, broObj);
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue listen = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, listen, "open",
        JS_NewCFunction(ctx, js_open, "open", 1));
    JS_SetPropertyStr(ctx, listen, "supported",
        JS_NewCFunction(ctx, js_supported, "supported", 0));
    JS_SetPropertyStr(ctx, listen, "apps",
        JS_NewCFunction(ctx, js_apps, "apps", 0));
    JS_SetPropertyStr(ctx, listen, "retain",
        JS_NewCFunction(ctx, js_retain, "retain", 1));
    JS_SetPropertyStr(ctx, listen, "audio",
        JS_NewCFunction(ctx, js_audio, "audio", 2));
    JS_SetPropertyStr(ctx, listen, "frame",
        JS_NewCFunction(ctx, js_frame, "frame", 0));
    JS_SetPropertyStr(ctx, listen, "info",
        JS_NewCFunction(ctx, js_info, "info", 0));
    JS_SetPropertyStr(ctx, broObj, "listen", listen);

    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

}  // namespace bro::js
