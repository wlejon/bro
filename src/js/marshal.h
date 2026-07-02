#pragma once
// Shared JS↔C++ marshalling for the audio/ML bindings. One home for the
// readers every model binding needs, instead of a private copy per file
// (readAudioBuffer used to exist verbatim in both stt and diar). Generic
// typed-array/option-object helpers live one level down in qjsbind; this
// header is only for bro-domain shapes like brosoundml::AudioBuffer.

#include <qjsbind/qjsbind.h>
#include <brosoundml/audio.h>

#include <string>

namespace bro::js {

// Read a JS audio argument — either { samples: Float32Array|number[],
// sampleRate }, or a bare Float32Array (assumed 16 kHz) — into a
// brosoundml::AudioBuffer. Returns false and fills `err` on shape errors.
inline bool readAudioBuffer(JSContext* ctx, JSValueConst v,
                            brosoundml::AudioBuffer& out, std::string& err) {
    out.sample_rate = 16000;
    size_t cnt = 0;
    if (const float* p = qjsbind::read_float32_view(ctx, v, cnt)) {
        out.samples.assign(p, p + cnt);           // bare Float32Array
        return true;
    }
    if (!JS_IsObject(v)) {
        err = "audio must be a Float32Array or { samples, sampleRate } object";
        return false;
    }
    JSValue sr = JS_GetPropertyStr(ctx, v, "sampleRate");
    if (JS_IsNumber(sr)) {
        int32_t t = out.sample_rate;
        JS_ToInt32(ctx, &t, sr);
        out.sample_rate = t;
    }
    JS_FreeValue(ctx, sr);

    JSValue s = JS_GetPropertyStr(ctx, v, "samples");
    bool ok = false;
    size_t fc = 0;
    if (const float* p = qjsbind::read_float32_view(ctx, s, fc)) {
        out.samples.assign(p, p + fc);
        ok = true;
    } else if (JS_IsArray(s)) {
        out.samples = qjsbind::read_float32_array(ctx, s);
        ok = true;
    } else {
        err = "audio.samples must be a Float32Array or number[]";
    }
    JS_FreeValue(ctx, s);
    return ok;
}

} // namespace bro::js
