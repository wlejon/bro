// bro.media — the waveform and the filmstrip.
//
// A timeline has to show what is IN a file, not just play it. Neither the
// samples nor the frames exist anywhere the DOM can reach: they only appear
// inside a decoder, and only if someone decodes the whole file. So the engine
// hands them over, once, in the shape a timeline draws from.
//
// Compiled out with BRO_WITH_VIDEO, where it installs the usual
// `{ available: false }` stub.
#include "js/media_bindings.h"

#if BRO_WITH_VIDEO

#include "video/media_analysis.h"

#include <qjsbind/qjsbind.h>

#include <string>
#include <vector>

namespace bro::js {

namespace {

std::string s_basePath;

std::string resolveMediaPath(const std::string& src) {
    if (src.size() >= 2 && src[1] == ':') return src;              // C:\...
    if (!src.empty() && (src[0] == '/' || src[0] == '\\')) return src;
    if (s_basePath.empty()) return src;
    std::string p = s_basePath;
    if (p.back() != '/' && p.back() != '\\') p += '/';
    return p + src;
}

int intOption(JSContext* ctx, JSValueConst opts, const char* name, int fallback) {
    if (!JS_IsObject(opts)) return fallback;
    JSValue v = JS_GetPropertyStr(ctx, opts, name);
    int32_t out = fallback;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) JS_ToInt32(ctx, &out, v);
    JS_FreeValue(ctx, v);
    return out;
}

bool pathArg(JSContext* ctx, JSValueConst v, std::string& out) {
    const char* s = JS_ToCString(ctx, v);
    if (!s) return false;
    out = resolveMediaPath(s);
    JS_FreeCString(ctx, s);
    return true;
}

// bro.media.peaks(path, { buckets })
JSValue js_media_peaks(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "peaks(path, options)");
    std::string path;
    if (!pathArg(ctx, argv[0], path)) return JS_EXCEPTION;

    int buckets = argc >= 2 ? intOption(ctx, argv[1], "buckets", 2048) : 2048;
    if (buckets < 1) buckets = 1;
    if (buckets > (1 << 20)) buckets = 1 << 20;

    video::AudioPeaks peaks;
    if (!video::analyzeAudioPeaks(path, buckets, peaks)) return JS_NULL;

    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "sampleRate", JS_NewInt32(ctx, int(peaks.sampleRate)));
    JS_SetPropertyStr(ctx, out, "channels", JS_NewInt32(ctx, int(peaks.channels)));
    JS_SetPropertyStr(ctx, out, "duration", JS_NewFloat64(ctx, peaks.durationNs / 1e9));
    JS_SetPropertyStr(ctx, out, "buckets", JS_NewInt32(ctx, int(peaks.maxv.size())));
    JS_SetPropertyStr(ctx, out, "min", qjsbind::make_float32_array(ctx, peaks.minv));
    JS_SetPropertyStr(ctx, out, "max", qjsbind::make_float32_array(ctx, peaks.maxv));
    JS_SetPropertyStr(ctx, out, "rms", qjsbind::make_float32_array(ctx, peaks.rms));
    return out;
}

// bro.media.thumbnails(path, { count, height })
JSValue js_media_thumbnails(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "thumbnails(path, options)");
    std::string path;
    if (!pathArg(ctx, argv[0], path)) return JS_EXCEPTION;

    int count = argc >= 2 ? intOption(ctx, argv[1], "count", 24) : 24;
    int height = argc >= 2 ? intOption(ctx, argv[1], "height", 72) : 72;
    count = count < 1 ? 1 : (count > 4096 ? 4096 : count);
    height = height < 1 ? 1 : (height > 2048 ? 2048 : height);

    video::ThumbnailStrip strip;
    if (!video::grabThumbnails(path, count, height, strip)) return JS_NULL;

    JSValue abuf = JS_NewArrayBufferCopy(ctx, strip.rgba.data(), strip.rgba.size());
    JSValue args[3] = { abuf, JS_UNDEFINED, JS_UNDEFINED };
    JSValue data = JS_NewTypedArray(ctx, 1, args, JS_TYPED_ARRAY_UINT8C);
    JS_FreeValue(ctx, abuf);

    JSValue times = JS_NewArray(ctx);
    for (size_t i = 0; i < strip.times.size(); ++i)
        JS_SetPropertyUint32(ctx, times, uint32_t(i),
                             JS_NewFloat64(ctx, strip.times[i] / 1e9));

    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "width", JS_NewInt32(ctx, strip.width));
    JS_SetPropertyStr(ctx, out, "height", JS_NewInt32(ctx, strip.height));
    JS_SetPropertyStr(ctx, out, "count", JS_NewInt32(ctx, strip.count));
    // What was turned to get these upright, not what is left to do: the pixels
    // are already the right way up, and `width` is the turned width.
    JS_SetPropertyStr(ctx, out, "rotation", JS_NewInt32(ctx, strip.rotationDegrees));
    JS_SetPropertyStr(ctx, out, "times", times);
    JS_SetPropertyStr(ctx, out, "data", data);
    return out;
}

} // namespace

void installMediaBindings(JSContext* ctx, const std::string& basePath) {
    s_basePath = basePath;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (!JS_IsObject(broObj)) {
        JS_FreeValue(ctx, broObj);
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue media = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, media, "available", JS_TRUE);
    JS_SetPropertyStr(ctx, media, "peaks",
                      JS_NewCFunction(ctx, js_media_peaks, "peaks", 2));
    JS_SetPropertyStr(ctx, media, "thumbnails",
                      JS_NewCFunction(ctx, js_media_thumbnails, "thumbnails", 2));
    JS_SetPropertyStr(ctx, broObj, "media", media);

    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

} // namespace bro::js

#else  // !BRO_WITH_VIDEO

namespace bro::js {

void installMediaBindings(JSContext* ctx, const std::string&) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (!JS_IsObject(broObj)) {
        JS_FreeValue(ctx, broObj);
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }
    JSValue media = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, media, "available", JS_FALSE);
    JS_SetPropertyStr(ctx, broObj, "media", media);
    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

} // namespace bro::js

#endif // BRO_WITH_VIDEO
