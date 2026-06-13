// bro.listen — JS surface over the shared listen host's stream retention.
// Thin wrapper over the listenHost* retention API (listen_host.h); all state and
// threading live there. See docs/listen-api.js.

#include "js/listen_bindings.h"

#include "js/listen_host.h"

#include <qjsbind/qjsbind.h>
#include <quickjs.h>

#include <cstdint>
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

}  // namespace

void installListenBindings(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        JS_FreeValue(ctx, broObj);
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue listen = JS_NewObject(ctx);
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
