#include "js/audio_bindings.h"
#include "js/runtime.h"
#include "audio/audio_engine.h"
#include "util/log.h"

#include <algorithm>
#include <string>
#include <cstring>
#include <cmath>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace bro::js {

// ---------------------------------------------------------------------------
// Class IDs
// ---------------------------------------------------------------------------

static JSClassID js_audioctx_class_id = 0;
static JSClassID js_oscnode_class_id = 0;
static JSClassID js_gainnode_class_id = 0;
static JSClassID js_destnode_class_id = 0;
static JSClassID js_audioparam_class_id = 0;
static JSClassID js_analysernode_class_id = 0;
static JSClassID js_micstream_class_id = 0;
static JSClassID js_micsource_class_id = 0;
static JSClassID js_biquadfilter_class_id = 0;

// ---------------------------------------------------------------------------
// AudioParam — wraps a float value that updates the engine
// ---------------------------------------------------------------------------

struct AudioParamData {
    audio::AudioEngine* engine;
    int voiceId;         // voice ID for voice params, or filter slot for filter params
    enum class Target {
        Frequency, Gain, Attack, Decay, SustainLevel, Release,
        FilterFrequency, FilterQ, FilterGain
    } target;
    float value;
};

static void js_audioparam_finalizer(JSRuntime*, JSValue val) {
    delete static_cast<AudioParamData*>(JS_GetOpaque(val, js_audioparam_class_id));
}

static JSClassDef js_audioparam_class = {
    "AudioParam", js_audioparam_finalizer, nullptr, nullptr, nullptr
};

static JSValue js_audioparam_get_value(JSContext* ctx, JSValueConst this_val) {
    auto* p = static_cast<AudioParamData*>(JS_GetOpaque(this_val, js_audioparam_class_id));
    return p ? JS_NewFloat64(ctx, p->value) : JS_UNDEFINED;
}

static JSValue js_audioparam_set_value(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* p = static_cast<AudioParamData*>(JS_GetOpaque(this_val, js_audioparam_class_id));
    if (!p) return JS_UNDEFINED;
    double v; JS_ToFloat64(ctx, &v, val);
    p->value = static_cast<float>(v);
    switch (p->target) {
        case AudioParamData::Target::Frequency:
            p->engine->setFrequency(p->voiceId, p->value); break;
        case AudioParamData::Target::Gain:
            p->engine->setGain(p->voiceId, p->value); break;
        case AudioParamData::Target::Attack:
            p->engine->setAttackTime(p->voiceId, p->value); break;
        case AudioParamData::Target::Decay:
            p->engine->setDecayTime(p->voiceId, p->value); break;
        case AudioParamData::Target::SustainLevel:
            p->engine->setSustainLevel(p->voiceId, p->value); break;
        case AudioParamData::Target::Release:
            p->engine->setReleaseTime(p->voiceId, p->value); break;
        case AudioParamData::Target::FilterFrequency:
            p->engine->setFilterFrequency(p->voiceId, p->value); break;
        case AudioParamData::Target::FilterQ:
            p->engine->setFilterQ(p->voiceId, p->value); break;
        case AudioParamData::Target::FilterGain:
            p->engine->setFilterGain(p->voiceId, p->value); break;
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_audioparam_proto_funcs[] = {
    JS_CGETSET_DEF("value", js_audioparam_get_value, js_audioparam_set_value),
};

static JSValue createAudioParam(JSContext* ctx, audio::AudioEngine* engine,
                                int voiceId, AudioParamData::Target target, float initial) {
    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_audioparam_class_id));
    auto* data = new AudioParamData{engine, voiceId, target, initial};
    JS_SetOpaque(obj, data);
    return obj;
}

// ---------------------------------------------------------------------------
// AudioDestinationNode — just a marker for connect() target
// ---------------------------------------------------------------------------

static JSClassDef js_destnode_class = {
    "AudioDestinationNode", nullptr, nullptr, nullptr, nullptr
};

// ---------------------------------------------------------------------------
// AnalyserNode — FFT analysis of audio data
// ---------------------------------------------------------------------------

struct AnalyserNodeData {
    audio::AudioEngine* engine;
    int fftSize = 2048;
    float minDecibels = -100.0f;
    float maxDecibels = -30.0f;
    float smoothingTimeConstant = 0.8f;
    // Which source to analyse: 0 = output mix, 1 = mic input, 2 = blend both
    int source = 0;
    // Smoothed magnitude data (previous frame)
    std::vector<float> smoothedMagnitudes;
};

static void js_analysernode_finalizer(JSRuntime*, JSValue val) {
    delete static_cast<AnalyserNodeData*>(JS_GetOpaque(val, js_analysernode_class_id));
}

static JSClassDef js_analysernode_class = {
    "AnalyserNode", js_analysernode_finalizer, nullptr, nullptr, nullptr
};

static JSValue js_analyser_get_fftSize(JSContext* ctx, JSValueConst this_val) {
    auto* d = static_cast<AnalyserNodeData*>(JS_GetOpaque(this_val, js_analysernode_class_id));
    return d ? JS_NewInt32(ctx, d->fftSize) : JS_UNDEFINED;
}

static JSValue js_analyser_set_fftSize(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* d = static_cast<AnalyserNodeData*>(JS_GetOpaque(this_val, js_analysernode_class_id));
    if (!d) return JS_UNDEFINED;
    int v; JS_ToInt32(ctx, &v, val);
    // Must be power of 2, between 32 and 32768
    if (v >= 32 && v <= 32768 && (v & (v - 1)) == 0) {
        d->fftSize = v;
        d->smoothedMagnitudes.clear();
    }
    return JS_UNDEFINED;
}

static JSValue js_analyser_get_frequencyBinCount(JSContext* ctx, JSValueConst this_val) {
    auto* d = static_cast<AnalyserNodeData*>(JS_GetOpaque(this_val, js_analysernode_class_id));
    return d ? JS_NewInt32(ctx, d->fftSize / 2) : JS_UNDEFINED;
}

static JSValue js_analyser_get_minDecibels(JSContext* ctx, JSValueConst this_val) {
    auto* d = static_cast<AnalyserNodeData*>(JS_GetOpaque(this_val, js_analysernode_class_id));
    return d ? JS_NewFloat64(ctx, d->minDecibels) : JS_UNDEFINED;
}

static JSValue js_analyser_set_minDecibels(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* d = static_cast<AnalyserNodeData*>(JS_GetOpaque(this_val, js_analysernode_class_id));
    if (!d) return JS_UNDEFINED;
    double v; JS_ToFloat64(ctx, &v, val);
    d->minDecibels = static_cast<float>(v);
    return JS_UNDEFINED;
}

static JSValue js_analyser_get_maxDecibels(JSContext* ctx, JSValueConst this_val) {
    auto* d = static_cast<AnalyserNodeData*>(JS_GetOpaque(this_val, js_analysernode_class_id));
    return d ? JS_NewFloat64(ctx, d->maxDecibels) : JS_UNDEFINED;
}

static JSValue js_analyser_set_maxDecibels(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* d = static_cast<AnalyserNodeData*>(JS_GetOpaque(this_val, js_analysernode_class_id));
    if (!d) return JS_UNDEFINED;
    double v; JS_ToFloat64(ctx, &v, val);
    d->maxDecibels = static_cast<float>(v);
    return JS_UNDEFINED;
}

static JSValue js_analyser_get_smoothing(JSContext* ctx, JSValueConst this_val) {
    auto* d = static_cast<AnalyserNodeData*>(JS_GetOpaque(this_val, js_analysernode_class_id));
    return d ? JS_NewFloat64(ctx, d->smoothingTimeConstant) : JS_UNDEFINED;
}

static JSValue js_analyser_set_smoothing(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* d = static_cast<AnalyserNodeData*>(JS_GetOpaque(this_val, js_analysernode_class_id));
    if (!d) return JS_UNDEFINED;
    double v; JS_ToFloat64(ctx, &v, val);
    d->smoothingTimeConstant = static_cast<float>(std::clamp(v, 0.0, 1.0));
    return JS_UNDEFINED;
}

// Helper: run FFT on the analyser's source buffer, return magnitude in dB
static void analyserComputeFFT(AnalyserNodeData* d, std::vector<float>& magnitudes) {
    int n = d->fftSize;
    int halfN = n / 2;
    magnitudes.resize(halfN);

    std::vector<float> real(n), imag(n, 0.0f);

    // Read samples from the appropriate ring buffer
    if (d->source == 2) {
        // Blend: sum output + mic (respects mic mute)
        d->engine->outputBuffer().readLatest(real.data(), n);
        if (!d->engine->isMicMuted()) {
            std::vector<float> mic(n);
            d->engine->micBuffer().readLatest(mic.data(), n);
            for (int i = 0; i < n; i++) real[i] += mic[i];
        }
    } else {
        auto& buf = (d->source == 1) ? d->engine->micBuffer() : d->engine->outputBuffer();
        buf.readLatest(real.data(), n);
    }

    // Apply Blackman window
    for (int i = 0; i < n; i++) {
        float w = 0.42f - 0.5f * std::cos(2.0f * static_cast<float>(M_PI) * i / (n - 1))
                        + 0.08f * std::cos(4.0f * static_cast<float>(M_PI) * i / (n - 1));
        real[i] *= w;
    }

    audio::fft(real.data(), imag.data(), n);

    // Compute magnitudes in dB with smoothing
    if (d->smoothedMagnitudes.size() != static_cast<size_t>(halfN)) {
        d->smoothedMagnitudes.assign(halfN, -100.0f);
    }

    float smooth = d->smoothingTimeConstant;
    for (int i = 0; i < halfN; i++) {
        float mag = std::sqrt(real[i] * real[i] + imag[i] * imag[i]) / static_cast<float>(n);
        float db = (mag > 1e-20f) ? 20.0f * std::log10(mag) : -100.0f;
        d->smoothedMagnitudes[i] = smooth * d->smoothedMagnitudes[i] + (1.0f - smooth) * db;
        magnitudes[i] = d->smoothedMagnitudes[i];
    }
}

// Helper: get raw pointer + usable byte length from a TypedArray argument
static uint8_t* getTypedArrayPtr(JSContext* ctx, JSValueConst arr, size_t& outLen) {
    size_t byteOff = 0, viewLen = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, arr, &byteOff, &viewLen, nullptr);
    if (JS_IsException(abuf)) return nullptr;
    size_t abufLen = 0;
    uint8_t* ptr = JS_GetArrayBuffer(ctx, &abufLen, abuf);
    JS_FreeValue(ctx, abuf);
    if (!ptr) return nullptr;
    outLen = viewLen;
    return ptr + byteOff;
}

static JSValue js_analyser_getFloatFrequencyData(JSContext* ctx, JSValueConst this_val,
                                                   int argc, JSValueConst* argv) {
    auto* d = static_cast<AnalyserNodeData*>(JS_GetOpaque(this_val, js_analysernode_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;

    std::vector<float> magnitudes;
    analyserComputeFFT(d, magnitudes);

    size_t len = 0;
    uint8_t* raw = getTypedArrayPtr(ctx, argv[0], len);
    if (raw) {
        float* dst = reinterpret_cast<float*>(raw);
        int count = std::min(static_cast<int>(len / sizeof(float)),
                             static_cast<int>(magnitudes.size()));
        for (int i = 0; i < count; i++) dst[i] = magnitudes[i];
    }
    return JS_UNDEFINED;
}

static JSValue js_analyser_getByteFrequencyData(JSContext* ctx, JSValueConst this_val,
                                                  int argc, JSValueConst* argv) {
    auto* d = static_cast<AnalyserNodeData*>(JS_GetOpaque(this_val, js_analysernode_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;

    std::vector<float> magnitudes;
    analyserComputeFFT(d, magnitudes);

    size_t len = 0;
    uint8_t* dst = getTypedArrayPtr(ctx, argv[0], len);
    if (dst) {
        int count = std::min(static_cast<int>(len),
                             static_cast<int>(magnitudes.size()));
        float range = d->maxDecibels - d->minDecibels;
        for (int i = 0; i < count; i++) {
            float scaled = (magnitudes[i] - d->minDecibels) / range;
            scaled = std::clamp(scaled, 0.0f, 1.0f);
            dst[i] = static_cast<uint8_t>(scaled * 255.0f);
        }
    }
    return JS_UNDEFINED;
}

static JSValue js_analyser_getFloatTimeDomainData(JSContext* ctx, JSValueConst this_val,
                                                    int argc, JSValueConst* argv) {
    auto* d = static_cast<AnalyserNodeData*>(JS_GetOpaque(this_val, js_analysernode_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;

    size_t len = 0;
    uint8_t* raw = getTypedArrayPtr(ctx, argv[0], len);
    if (raw) {
        float* dst = reinterpret_cast<float*>(raw);
        int count = std::min(static_cast<int>(len / sizeof(float)), d->fftSize);
        if (d->source == 2) {
            d->engine->outputBuffer().readLatest(dst, count);
            if (!d->engine->isMicMuted()) {
                std::vector<float> mic(count);
                d->engine->micBuffer().readLatest(mic.data(), count);
                for (int i = 0; i < count; i++) dst[i] += mic[i];
            }
        } else {
            auto& ringBuf = (d->source == 1) ? d->engine->micBuffer() : d->engine->outputBuffer();
            ringBuf.readLatest(dst, count);
        }
    }
    return JS_UNDEFINED;
}

static JSValue js_analyser_getByteTimeDomainData(JSContext* ctx, JSValueConst this_val,
                                                   int argc, JSValueConst* argv) {
    auto* d = static_cast<AnalyserNodeData*>(JS_GetOpaque(this_val, js_analysernode_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;

    std::vector<float> samples(d->fftSize);
    if (d->source == 2) {
        d->engine->outputBuffer().readLatest(samples.data(), d->fftSize);
        if (!d->engine->isMicMuted()) {
            std::vector<float> mic(d->fftSize);
            d->engine->micBuffer().readLatest(mic.data(), d->fftSize);
            for (int i = 0; i < d->fftSize; i++) samples[i] += mic[i];
        }
    } else {
        auto& ringBuf = (d->source == 1) ? d->engine->micBuffer() : d->engine->outputBuffer();
        ringBuf.readLatest(samples.data(), d->fftSize);
    }

    size_t len = 0;
    uint8_t* dst = getTypedArrayPtr(ctx, argv[0], len);
    if (dst) {
        int count = std::min(static_cast<int>(len), d->fftSize);
        for (int i = 0; i < count; i++) {
            dst[i] = static_cast<uint8_t>(std::clamp((samples[i] + 1.0f) * 128.0f, 0.0f, 255.0f));
        }
    }
    return JS_UNDEFINED;
}

// source property: 0 = output mix, 1 = mic input
static JSValue js_analyser_get_source(JSContext* ctx, JSValueConst this_val) {
    auto* d = static_cast<AnalyserNodeData*>(JS_GetOpaque(this_val, js_analysernode_class_id));
    return d ? JS_NewInt32(ctx, d->source) : JS_UNDEFINED;
}

static JSValue js_analyser_set_source(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* d = static_cast<AnalyserNodeData*>(JS_GetOpaque(this_val, js_analysernode_class_id));
    if (!d) return JS_UNDEFINED;
    int v; JS_ToInt32(ctx, &v, val);
    d->source = (v == 2) ? 2 : (v == 1) ? 1 : 0;
    return JS_UNDEFINED;
}

static JSValue js_analyser_connect(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_analyser_disconnect(JSContext*, JSValueConst, int, JSValueConst*) {
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_analysernode_proto_funcs[] = {
    JS_CGETSET_DEF("fftSize", js_analyser_get_fftSize, js_analyser_set_fftSize),
    JS_CGETSET_DEF("frequencyBinCount", js_analyser_get_frequencyBinCount, nullptr),
    JS_CGETSET_DEF("minDecibels", js_analyser_get_minDecibels, js_analyser_set_minDecibels),
    JS_CGETSET_DEF("maxDecibels", js_analyser_get_maxDecibels, js_analyser_set_maxDecibels),
    JS_CGETSET_DEF("smoothingTimeConstant", js_analyser_get_smoothing, js_analyser_set_smoothing),
    JS_CGETSET_DEF("source", js_analyser_get_source, js_analyser_set_source),
    JS_CFUNC_DEF("getFloatFrequencyData", 1, js_analyser_getFloatFrequencyData),
    JS_CFUNC_DEF("getByteFrequencyData", 1, js_analyser_getByteFrequencyData),
    JS_CFUNC_DEF("getFloatTimeDomainData", 1, js_analyser_getFloatTimeDomainData),
    JS_CFUNC_DEF("getByteTimeDomainData", 1, js_analyser_getByteTimeDomainData),
    JS_CFUNC_DEF("connect", 1, js_analyser_connect),
    JS_CFUNC_DEF("disconnect", 0, js_analyser_disconnect),
};

// ---------------------------------------------------------------------------
// MediaStream / MediaStreamAudioSourceNode (for mic input)
// ---------------------------------------------------------------------------

struct MicStreamData {
    audio::AudioEngine* engine;
};

static void js_micstream_finalizer(JSRuntime*, JSValue val) {
    delete static_cast<MicStreamData*>(JS_GetOpaque(val, js_micstream_class_id));
}

static JSClassDef js_micstream_class = {
    "MediaStream", js_micstream_finalizer, nullptr, nullptr, nullptr
};

struct MicSourceData {
    audio::AudioEngine* engine;
};

static void js_micsource_finalizer(JSRuntime*, JSValue val) {
    delete static_cast<MicSourceData*>(JS_GetOpaque(val, js_micsource_class_id));
}

static JSClassDef js_micsource_class = {
    "MediaStreamAudioSourceNode", js_micsource_finalizer, nullptr, nullptr, nullptr
};

// MediaStreamAudioSourceNode.connect(analyser) — sets analyser to mic source
static JSValue js_micsource_connect(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;

    // If connecting to an AnalyserNode, switch it to mic source
    auto* ad = static_cast<AnalyserNodeData*>(JS_GetOpaque(argv[0], js_analysernode_class_id));
    if (ad) {
        ad->source = 1; // mic
    }

    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_micsource_disconnect(JSContext*, JSValueConst, int, JSValueConst*) {
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_micsource_proto_funcs[] = {
    JS_CFUNC_DEF("connect", 1, js_micsource_connect),
    JS_CFUNC_DEF("disconnect", 0, js_micsource_disconnect),
};

// Forward declaration (defined below, needed by osc_connect)
struct GainNodeData;

// ---------------------------------------------------------------------------
// OscillatorNode — wraps a voice in the AudioEngine
// ---------------------------------------------------------------------------

struct OscNodeData {
    audio::AudioEngine* engine;
    int voiceId;
    std::string type = "sine";
};

static void js_oscnode_finalizer(JSRuntime*, JSValue val) {
    auto* d = static_cast<OscNodeData*>(JS_GetOpaque(val, js_oscnode_class_id));
    if (d) {
        delete d;
    }
}

static JSClassDef js_oscnode_class = {
    "OscillatorNode", js_oscnode_finalizer, nullptr, nullptr, nullptr
};

static JSValue js_osc_get_type(JSContext* ctx, JSValueConst this_val) {
    auto* d = static_cast<OscNodeData*>(JS_GetOpaque(this_val, js_oscnode_class_id));
    return d ? JS_NewString(ctx, d->type.c_str()) : JS_UNDEFINED;
}

static JSValue js_osc_set_type(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* d = static_cast<OscNodeData*>(JS_GetOpaque(this_val, js_oscnode_class_id));
    if (!d) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, val);
    if (!s) return JS_UNDEFINED;
    d->type = s;
    audio::Waveform wf = audio::Waveform::Sine;
    if (d->type == "square") wf = audio::Waveform::Square;
    else if (d->type == "sawtooth") wf = audio::Waveform::Sawtooth;
    else if (d->type == "triangle") wf = audio::Waveform::Triangle;
    d->engine->setWaveform(d->voiceId, wf);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue js_osc_connect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    auto* d = static_cast<OscNodeData*>(JS_GetOpaque(this_val, js_oscnode_class_id));
    if (!d) return JS_DupValue(ctx, argv[0]);

    // If connecting to a GainNode, store reference
    auto* gd = static_cast<GainNodeData*>(JS_GetOpaque(argv[0], js_gainnode_class_id));
    if (gd) {
        JS_SetPropertyStr(ctx, this_val, "__gainNode", JS_DupValue(ctx, argv[0]));
    }

    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_osc_start(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<OscNodeData*>(JS_GetOpaque(this_val, js_oscnode_class_id));
    if (!d) return JS_UNDEFINED;

    // Read gain from connected GainNode if any
    JSValue gnVal = JS_GetPropertyStr(ctx, this_val, "__gainNode");
    if (!JS_IsUndefined(gnVal) && !JS_IsNull(gnVal)) {
        JSValue gainParam = JS_GetPropertyStr(ctx, gnVal, "gain");
        if (!JS_IsUndefined(gainParam)) {
            JSValue gainVal = JS_GetPropertyStr(ctx, gainParam, "value");
            double g = 1.0;
            JS_ToFloat64(ctx, &g, gainVal);
            d->engine->setGain(d->voiceId, static_cast<float>(g));
            JS_FreeValue(ctx, gainVal);
        }
        JS_FreeValue(ctx, gainParam);
    }
    JS_FreeValue(ctx, gnVal);

    double when = d->engine->currentTime();
    if (argc >= 1) JS_ToFloat64(ctx, &when, argv[0]);
    d->engine->startVoice(d->voiceId, when);
    return JS_UNDEFINED;
}

static JSValue js_osc_stop(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<OscNodeData*>(JS_GetOpaque(this_val, js_oscnode_class_id));
    if (!d) return JS_UNDEFINED;
    double when = d->engine->currentTime();
    if (argc >= 1) JS_ToFloat64(ctx, &when, argv[0]);
    d->engine->stopVoice(d->voiceId, when);
    return JS_UNDEFINED;
}

static JSValue js_osc_disconnect(JSContext*, JSValueConst, int, JSValueConst*) {
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// GainNode
// ---------------------------------------------------------------------------

struct GainNodeData {
    audio::AudioEngine* engine;
};

static void js_gainnode_finalizer(JSRuntime*, JSValue val) {
    delete static_cast<GainNodeData*>(JS_GetOpaque(val, js_gainnode_class_id));
}

static JSClassDef js_gainnode_class = {
    "GainNode", js_gainnode_finalizer, nullptr, nullptr, nullptr
};

static JSValue js_gain_connect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_gain_disconnect(JSContext*, JSValueConst, int, JSValueConst*) {
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_gainnode_proto_funcs[] = {
    JS_CFUNC_DEF("connect", 1, js_gain_connect),
    JS_CFUNC_DEF("disconnect", 0, js_gain_disconnect),
};

// ---------------------------------------------------------------------------
// BiquadFilterNode
// ---------------------------------------------------------------------------

struct BiquadFilterNodeData {
    audio::AudioEngine* engine;
    int slot;
};

static void js_biquadfilter_finalizer(JSRuntime*, JSValue val) {
    auto* d = static_cast<BiquadFilterNodeData*>(JS_GetOpaque(val, js_biquadfilter_class_id));
    if (d) {
        d->engine->releaseFilterSlot(d->slot);
        delete d;
    }
}

static JSClassDef js_biquadfilter_class = {
    "BiquadFilterNode", js_biquadfilter_finalizer, nullptr, nullptr, nullptr
};

static JSValue js_biquadfilter_get_type(JSContext* ctx, JSValueConst this_val) {
    auto* d = static_cast<BiquadFilterNodeData*>(JS_GetOpaque(this_val, js_biquadfilter_class_id));
    if (!d) return JS_UNDEFINED;
    JSValue v = JS_GetPropertyStr(ctx, this_val, "__type");
    return v;
}

static JSValue js_biquadfilter_set_type(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* d = static_cast<BiquadFilterNodeData*>(JS_GetOpaque(this_val, js_biquadfilter_class_id));
    if (!d) return JS_UNDEFINED;

    const char* str = JS_ToCString(ctx, val);
    if (!str) return JS_UNDEFINED;

    audio::BiquadFilter::Type type = audio::BiquadFilter::Type::Lowpass;
    if (strcmp(str, "lowpass") == 0) type = audio::BiquadFilter::Type::Lowpass;
    else if (strcmp(str, "highpass") == 0) type = audio::BiquadFilter::Type::Highpass;
    else if (strcmp(str, "bandpass") == 0) type = audio::BiquadFilter::Type::Bandpass;
    else if (strcmp(str, "notch") == 0) type = audio::BiquadFilter::Type::Notch;
    else if (strcmp(str, "allpass") == 0) type = audio::BiquadFilter::Type::Allpass;
    else if (strcmp(str, "peaking") == 0) type = audio::BiquadFilter::Type::Peaking;
    else if (strcmp(str, "lowshelf") == 0) type = audio::BiquadFilter::Type::Lowshelf;
    else if (strcmp(str, "highshelf") == 0) type = audio::BiquadFilter::Type::Highshelf;

    JS_FreeCString(ctx, str);

    d->engine->setFilterType(d->slot, type);
    JS_SetPropertyStr(ctx, this_val, "__type", JS_DupValue(ctx, val));
    return JS_UNDEFINED;
}

static JSValue js_biquadfilter_connect(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv) {
    auto* d = static_cast<BiquadFilterNodeData*>(JS_GetOpaque(this_val, js_biquadfilter_class_id));
    if (d) d->engine->setFilterEnabled(d->slot, true);
    if (argc < 1) return JS_UNDEFINED;
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_biquadfilter_disconnect(JSContext*, JSValueConst this_val,
                                           int, JSValueConst*) {
    auto* d = static_cast<BiquadFilterNodeData*>(JS_GetOpaque(this_val, js_biquadfilter_class_id));
    if (d) d->engine->setFilterEnabled(d->slot, false);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_biquadfilter_proto_funcs[] = {
    JS_CGETSET_DEF("type", js_biquadfilter_get_type, js_biquadfilter_set_type),
    JS_CFUNC_DEF("connect", 1, js_biquadfilter_connect),
    JS_CFUNC_DEF("disconnect", 0, js_biquadfilter_disconnect),
};

// ---------------------------------------------------------------------------
// AudioContext
// ---------------------------------------------------------------------------

struct AudioCtxData {
    audio::AudioEngine* engine;
};

static void js_audioctx_finalizer(JSRuntime*, JSValue val) {
    delete static_cast<AudioCtxData*>(JS_GetOpaque(val, js_audioctx_class_id));
}

static JSClassDef js_audioctx_class = {
    "AudioContext", js_audioctx_finalizer, nullptr, nullptr, nullptr
};

static JSValue js_audioctx_get_currentTime(JSContext* ctx, JSValueConst this_val) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    return d ? JS_NewFloat64(ctx, d->engine->currentTime()) : JS_UNDEFINED;
}

static JSValue js_audioctx_get_sampleRate(JSContext* ctx, JSValueConst this_val) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    return d ? JS_NewInt32(ctx, d->engine->sampleRate()) : JS_UNDEFINED;
}

static JSValue js_audioctx_createOscillator(JSContext* ctx, JSValueConst this_val,
                                             int, JSValueConst*) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d) return JS_UNDEFINED;

    int voiceId = d->engine->createVoice();

    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_oscnode_class_id));
    auto* oscData = new OscNodeData{d->engine, voiceId, "sine"};
    JS_SetOpaque(obj, oscData);

    // Create AudioParams for oscillator properties
    JS_SetPropertyStr(ctx, obj, "frequency",
        createAudioParam(ctx, d->engine, voiceId, AudioParamData::Target::Frequency, 440.0f));
    JS_SetPropertyStr(ctx, obj, "attack",
        createAudioParam(ctx, d->engine, voiceId, AudioParamData::Target::Attack, 0.01f));
    JS_SetPropertyStr(ctx, obj, "decay",
        createAudioParam(ctx, d->engine, voiceId, AudioParamData::Target::Decay, 0.1f));
    JS_SetPropertyStr(ctx, obj, "sustain",
        createAudioParam(ctx, d->engine, voiceId, AudioParamData::Target::SustainLevel, 1.0f));
    JS_SetPropertyStr(ctx, obj, "release",
        createAudioParam(ctx, d->engine, voiceId, AudioParamData::Target::Release, 0.04f));

    return obj;
}

static JSValue js_audioctx_createGain(JSContext* ctx, JSValueConst this_val,
                                       int, JSValueConst*) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d) return JS_UNDEFINED;

    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_gainnode_class_id));
    auto* gainData = new GainNodeData{d->engine};
    JS_SetOpaque(obj, gainData);

    JSValue gainParam = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, gainParam, "value", JS_NewFloat64(ctx, 1.0));
    JS_SetPropertyStr(ctx, obj, "gain", gainParam);

    return obj;
}

static JSValue js_audioctx_createAnalyser(JSContext* ctx, JSValueConst this_val,
                                           int, JSValueConst*) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d) return JS_UNDEFINED;

    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_analysernode_class_id));
    auto* data = new AnalyserNodeData{d->engine};
    JS_SetOpaque(obj, data);
    return obj;
}

static JSValue js_audioctx_createBiquadFilter(JSContext* ctx, JSValueConst this_val,
                                               int, JSValueConst*) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d) return JS_UNDEFINED;

    int slot = d->engine->allocateFilterSlot();
    if (slot < 0) return JS_ThrowInternalError(ctx, "No filter slots available");

    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_biquadfilter_class_id));
    auto* filterData = new BiquadFilterNodeData{d->engine, slot};
    JS_SetOpaque(obj, filterData);

    // Set default type property
    JS_SetPropertyStr(ctx, obj, "__type", JS_NewString(ctx, "lowpass"));

    // Create AudioParams for filter properties (voiceId field holds the slot index)
    JS_SetPropertyStr(ctx, obj, "frequency",
        createAudioParam(ctx, d->engine, slot, AudioParamData::Target::FilterFrequency, 1000.0f));
    JS_SetPropertyStr(ctx, obj, "Q",
        createAudioParam(ctx, d->engine, slot, AudioParamData::Target::FilterQ, 1.0f));
    JS_SetPropertyStr(ctx, obj, "gain",
        createAudioParam(ctx, d->engine, slot, AudioParamData::Target::FilterGain, 0.0f));

    return obj;
}

// Delay control methods on AudioContext
static JSValue js_audioctx_setDelayEnabled(JSContext* ctx, JSValueConst this_val,
                                            int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    d->engine->setDelayEnabled(JS_ToBool(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setDelayTime(JSContext* ctx, JSValueConst this_val,
                                         int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    double v; JS_ToFloat64(ctx, &v, argv[0]);
    d->engine->setDelayTime(static_cast<float>(v));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setDelayFeedback(JSContext* ctx, JSValueConst this_val,
                                             int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    double v; JS_ToFloat64(ctx, &v, argv[0]);
    d->engine->setDelayFeedback(static_cast<float>(v));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setDelayMix(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    double v; JS_ToFloat64(ctx, &v, argv[0]);
    d->engine->setDelayMix(static_cast<float>(v));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_createMediaStreamSource(JSContext* ctx, JSValueConst this_val,
                                                    int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;

    // Verify it's a MediaStream
    auto* ms = static_cast<MicStreamData*>(JS_GetOpaque(argv[0], js_micstream_class_id));
    if (!ms) return JS_ThrowTypeError(ctx, "Expected MediaStream argument");

    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_micsource_class_id));
    auto* data = new MicSourceData{d->engine};
    JS_SetOpaque(obj, data);
    return obj;
}

// Mic mute/gain properties on AudioContext (bro extension)
static JSValue js_audioctx_get_micMuted(JSContext* ctx, JSValueConst this_val) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    return d ? JS_NewBool(ctx, d->engine->isMicMuted()) : JS_UNDEFINED;
}

static JSValue js_audioctx_set_micMuted(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (d) d->engine->setMicMuted(JS_ToBool(ctx, val));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_get_micMonitorGain(JSContext* ctx, JSValueConst this_val) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    return d ? JS_NewFloat64(ctx, d->engine->micMonitorGain()) : JS_UNDEFINED;
}

static JSValue js_audioctx_set_micMonitorGain(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (d) {
        double v; JS_ToFloat64(ctx, &v, val);
        d->engine->setMicMonitorGain(static_cast<float>(std::clamp(v, 0.0, 1.0)));
    }
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Recording + Loop Station bindings
// ---------------------------------------------------------------------------

static JSValue js_audioctx_get_recording(JSContext* ctx, JSValueConst this_val) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    return d ? JS_NewBool(ctx, d->engine->isRecording()) : JS_UNDEFINED;
}

static JSValue js_audioctx_startRecording(JSContext* ctx, JSValueConst this_val,
                                           int, JSValueConst*) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (d) d->engine->startRecording();
    return JS_UNDEFINED;
}

static JSValue js_audioctx_stopRecording(JSContext* ctx, JSValueConst this_val,
                                          int, JSValueConst*) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d) return JS_UNDEFINED;

    d->engine->stopRecording();
    const auto& buf = d->engine->getRecordBuffer();
    if (buf.empty()) return JS_NULL;

    // Create Float32Array with copy of recorded data
    int count = static_cast<int>(buf.size());
    JSValue lenVal = JS_NewInt32(ctx, count);
    JSValue arr = JS_NewTypedArray(ctx, 1, &lenVal, JS_TYPED_ARRAY_FLOAT32);
    JS_FreeValue(ctx, lenVal);

    if (JS_IsException(arr)) return arr;

    size_t byteOff = 0, viewLen = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, arr, &byteOff, &viewLen, nullptr);
    if (!JS_IsException(abuf)) {
        size_t abufLen = 0;
        uint8_t* ptr = JS_GetArrayBuffer(ctx, &abufLen, abuf);
        if (ptr) {
            std::memcpy(ptr + byteOff, buf.data(), count * sizeof(float));
        }
        JS_FreeValue(ctx, abuf);
    }
    return arr;
}

// --- Audio Clip management ---

static JSValue js_audioctx_createClip(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;

    size_t len = 0;
    uint8_t* raw = getTypedArrayPtr(ctx, argv[0], len);
    if (!raw) return JS_ThrowTypeError(ctx, "Expected Float32Array argument");

    int numSamples = static_cast<int>(len / sizeof(float));
    int id = d->engine->createClip(reinterpret_cast<float*>(raw), numSamples);
    return JS_NewInt32(ctx, id);
}

static JSValue js_audioctx_deleteClip(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    int id; JS_ToInt32(ctx, &id, argv[0]);
    d->engine->deleteClip(id);
    return JS_UNDEFINED;
}

static JSValue js_audioctx_getClipSampleCount(JSContext* ctx, JSValueConst this_val,
                                               int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    int id; JS_ToInt32(ctx, &id, argv[0]);
    return JS_NewInt32(ctx, d->engine->getClipSampleCount(id));
}

static JSValue js_audioctx_getClipWaveform(JSContext* ctx, JSValueConst this_val,
                                            int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;

    int clipId, numBins;
    JS_ToInt32(ctx, &clipId, argv[0]);
    JS_ToInt32(ctx, &numBins, argv[1]);
    if (numBins <= 0 || numBins > 1024) return JS_UNDEFINED;

    int floatCount = numBins * 2;
    JSValue lenVal = JS_NewInt32(ctx, floatCount);
    JSValue arr = JS_NewTypedArray(ctx, 1, &lenVal, JS_TYPED_ARRAY_FLOAT32);
    JS_FreeValue(ctx, lenVal);

    if (JS_IsException(arr)) return arr;

    size_t byteOff = 0, viewLen = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, arr, &byteOff, &viewLen, nullptr);
    if (!JS_IsException(abuf)) {
        size_t abufLen = 0;
        uint8_t* ptr = JS_GetArrayBuffer(ctx, &abufLen, abuf);
        if (ptr) {
            d->engine->getClipWaveform(clipId, reinterpret_cast<float*>(ptr + byteOff), numBins);
        }
        JS_FreeValue(ctx, abuf);
    }
    return arr;
}

// --- Clip Playback instances ---

static JSValue js_audioctx_playClip(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    int clipId; JS_ToInt32(ctx, &clipId, argv[0]);
    float gain = 1.0f;
    bool loop = false;
    if (argc >= 2) { double v; JS_ToFloat64(ctx, &v, argv[1]); gain = static_cast<float>(v); }
    if (argc >= 3) { loop = JS_ToBool(ctx, argv[2]); }
    return JS_NewInt32(ctx, d->engine->playClip(clipId, gain, loop));
}

static JSValue js_audioctx_stopPlayback(JSContext* ctx, JSValueConst this_val,
                                         int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    int id; JS_ToInt32(ctx, &id, argv[0]);
    d->engine->stopPlayback(id);
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setPlaybackGain(JSContext* ctx, JSValueConst this_val,
                                            int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;
    int id; JS_ToInt32(ctx, &id, argv[0]);
    double v; JS_ToFloat64(ctx, &v, argv[1]);
    d->engine->setPlaybackGain(id, static_cast<float>(v));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setPlaybackLoop(JSContext* ctx, JSValueConst this_val,
                                            int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;
    int id; JS_ToInt32(ctx, &id, argv[0]);
    d->engine->setPlaybackLoop(id, JS_ToBool(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setPlaybackPlaying(JSContext* ctx, JSValueConst this_val,
                                               int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;
    int id; JS_ToInt32(ctx, &id, argv[0]);
    d->engine->setPlaybackPlaying(id, JS_ToBool(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setPlaybackRegion(JSContext* ctx, JSValueConst this_val,
                                              int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 3) return JS_UNDEFINED;
    int id, start, end;
    JS_ToInt32(ctx, &id, argv[0]);
    JS_ToInt32(ctx, &start, argv[1]);
    JS_ToInt32(ctx, &end, argv[2]);
    d->engine->setPlaybackRegion(id, start, end);
    return JS_UNDEFINED;
}

static JSValue js_audioctx_getPlaybackPosition(JSContext* ctx, JSValueConst this_val,
                                                int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    int id; JS_ToInt32(ctx, &id, argv[0]);
    return JS_NewFloat64(ctx, d->engine->getPlaybackPosition(id));
}

static const JSCFunctionListEntry js_audioctx_proto_funcs[] = {
    JS_CGETSET_DEF("currentTime", js_audioctx_get_currentTime, nullptr),
    JS_CGETSET_DEF("sampleRate", js_audioctx_get_sampleRate, nullptr),
    JS_CGETSET_DEF("micMuted", js_audioctx_get_micMuted, js_audioctx_set_micMuted),
    JS_CGETSET_DEF("micMonitorGain", js_audioctx_get_micMonitorGain, js_audioctx_set_micMonitorGain),
    JS_CGETSET_DEF("recording", js_audioctx_get_recording, nullptr),
    JS_CFUNC_DEF("createOscillator", 0, js_audioctx_createOscillator),
    JS_CFUNC_DEF("createGain", 0, js_audioctx_createGain),
    JS_CFUNC_DEF("createAnalyser", 0, js_audioctx_createAnalyser),
    JS_CFUNC_DEF("createBiquadFilter", 0, js_audioctx_createBiquadFilter),
    JS_CFUNC_DEF("createMediaStreamSource", 1, js_audioctx_createMediaStreamSource),
    JS_CFUNC_DEF("setDelayEnabled", 1, js_audioctx_setDelayEnabled),
    JS_CFUNC_DEF("setDelayTime", 1, js_audioctx_setDelayTime),
    JS_CFUNC_DEF("setDelayFeedback", 1, js_audioctx_setDelayFeedback),
    JS_CFUNC_DEF("setDelayMix", 1, js_audioctx_setDelayMix),
    JS_CFUNC_DEF("startRecording", 0, js_audioctx_startRecording),
    JS_CFUNC_DEF("stopRecording", 0, js_audioctx_stopRecording),
    JS_CFUNC_DEF("createClip", 1, js_audioctx_createClip),
    JS_CFUNC_DEF("deleteClip", 1, js_audioctx_deleteClip),
    JS_CFUNC_DEF("getClipSampleCount", 1, js_audioctx_getClipSampleCount),
    JS_CFUNC_DEF("getClipWaveform", 2, js_audioctx_getClipWaveform),
    JS_CFUNC_DEF("playClip", 3, js_audioctx_playClip),
    JS_CFUNC_DEF("stopPlayback", 1, js_audioctx_stopPlayback),
    JS_CFUNC_DEF("setPlaybackGain", 2, js_audioctx_setPlaybackGain),
    JS_CFUNC_DEF("setPlaybackLoop", 2, js_audioctx_setPlaybackLoop),
    JS_CFUNC_DEF("setPlaybackPlaying", 2, js_audioctx_setPlaybackPlaying),
    JS_CFUNC_DEF("setPlaybackRegion", 3, js_audioctx_setPlaybackRegion),
    JS_CFUNC_DEF("getPlaybackPosition", 1, js_audioctx_getPlaybackPosition),
};

// ---------------------------------------------------------------------------
// Constructor for `new AudioContext()`
// ---------------------------------------------------------------------------

static audio::AudioEngine* s_audioEngine = nullptr;

static JSValue js_audioctx_constructor(JSContext* ctx, JSValueConst new_target,
                                        int, JSValueConst*) {
    if (!s_audioEngine) return JS_ThrowInternalError(ctx, "Audio not initialized");

    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_audioctx_class_id));
    if (JS_IsException(obj)) return obj;

    auto* data = new AudioCtxData{s_audioEngine};
    JS_SetOpaque(obj, data);

    // Create destination node
    JSValue dest = JS_NewObjectClass(ctx, static_cast<int>(js_destnode_class_id));
    JS_SetPropertyStr(ctx, obj, "destination", dest);

    return obj;
}

// ---------------------------------------------------------------------------
// navigator.mediaDevices.getUserMedia()
// ---------------------------------------------------------------------------

static JSValue js_getUserMedia(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv) {
    if (!s_audioEngine) return JS_ThrowInternalError(ctx, "Audio not initialized");

    // Start mic capture
    if (!s_audioEngine->startMicCapture()) {
        // Return rejected promise
        JSValue err = JS_NewError(ctx);
        JS_SetPropertyStr(ctx, err, "message",
                          JS_NewString(ctx, "Failed to access microphone"));
        JSValue resolving[2];
        JSValue promise = JS_NewPromiseCapability(ctx, resolving);
        JS_Call(ctx, resolving[1], JS_UNDEFINED, 1, &err);
        JS_FreeValue(ctx, resolving[0]);
        JS_FreeValue(ctx, resolving[1]);
        JS_FreeValue(ctx, err);
        return promise;
    }

    // Create MediaStream object
    JSValue stream = JS_NewObjectClass(ctx, static_cast<int>(js_micstream_class_id));
    auto* data = new MicStreamData{s_audioEngine};
    JS_SetOpaque(stream, data);

    // Return resolved promise with the stream
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    JS_Call(ctx, resolving[0], JS_UNDEFINED, 1, &stream);
    JS_FreeValue(ctx, resolving[0]);
    JS_FreeValue(ctx, resolving[1]);
    JS_FreeValue(ctx, stream);

    return promise;
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void AudioBindings::install(JSContext* ctx, audio::AudioEngine* engine)
{
    s_audioEngine = engine;
    JSRuntime* rt = JS_GetRuntime(ctx);

    // AudioParam
    JS_NewClassID(rt, &js_audioparam_class_id);
    JS_NewClass(rt, js_audioparam_class_id, &js_audioparam_class);
    JSValue apProto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, apProto, js_audioparam_proto_funcs,
                               sizeof(js_audioparam_proto_funcs)/sizeof(js_audioparam_proto_funcs[0]));
    JS_SetClassProto(ctx, js_audioparam_class_id, apProto);

    // AudioDestinationNode
    JS_NewClassID(rt, &js_destnode_class_id);
    JS_NewClass(rt, js_destnode_class_id, &js_destnode_class);

    // AnalyserNode
    JS_NewClassID(rt, &js_analysernode_class_id);
    JS_NewClass(rt, js_analysernode_class_id, &js_analysernode_class);
    JSValue anProto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, anProto, js_analysernode_proto_funcs,
                               sizeof(js_analysernode_proto_funcs)/sizeof(js_analysernode_proto_funcs[0]));
    JS_SetClassProto(ctx, js_analysernode_class_id, anProto);

    // MediaStream
    JS_NewClassID(rt, &js_micstream_class_id);
    JS_NewClass(rt, js_micstream_class_id, &js_micstream_class);

    // MediaStreamAudioSourceNode
    JS_NewClassID(rt, &js_micsource_class_id);
    JS_NewClass(rt, js_micsource_class_id, &js_micsource_class);
    JSValue msProto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, msProto, js_micsource_proto_funcs,
                               sizeof(js_micsource_proto_funcs)/sizeof(js_micsource_proto_funcs[0]));
    JS_SetClassProto(ctx, js_micsource_class_id, msProto);

    // BiquadFilterNode
    JS_NewClassID(rt, &js_biquadfilter_class_id);
    JS_NewClass(rt, js_biquadfilter_class_id, &js_biquadfilter_class);
    {
        JSValue bfProto = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, bfProto, js_biquadfilter_proto_funcs,
                                   sizeof(js_biquadfilter_proto_funcs)/sizeof(js_biquadfilter_proto_funcs[0]));
        JS_SetClassProto(ctx, js_biquadfilter_class_id, bfProto);
    }

    // OscillatorNode
    JS_NewClassID(rt, &js_oscnode_class_id);
    JS_NewClass(rt, js_oscnode_class_id, &js_oscnode_class);
    {
        static const JSCFunctionListEntry osc_funcs[] = {
            JS_CGETSET_DEF("type", js_osc_get_type, js_osc_set_type),
            JS_CFUNC_DEF("connect", 1, js_osc_connect),
            JS_CFUNC_DEF("disconnect", 0, js_osc_disconnect),
            JS_CFUNC_DEF("start", 1, js_osc_start),
            JS_CFUNC_DEF("stop", 1, js_osc_stop),
        };
        JSValue oscProto = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, oscProto, osc_funcs,
                                   sizeof(osc_funcs) / sizeof(osc_funcs[0]));
        JS_SetClassProto(ctx, js_oscnode_class_id, oscProto);
    }

    // GainNode
    JS_NewClassID(rt, &js_gainnode_class_id);
    JS_NewClass(rt, js_gainnode_class_id, &js_gainnode_class);
    JSValue gainProto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, gainProto, js_gainnode_proto_funcs,
                               sizeof(js_gainnode_proto_funcs)/sizeof(js_gainnode_proto_funcs[0]));
    JS_SetClassProto(ctx, js_gainnode_class_id, gainProto);

    // AudioContext
    JS_NewClassID(rt, &js_audioctx_class_id);
    JS_NewClass(rt, js_audioctx_class_id, &js_audioctx_class);
    JSValue ctxProto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, ctxProto, js_audioctx_proto_funcs,
                               sizeof(js_audioctx_proto_funcs)/sizeof(js_audioctx_proto_funcs[0]));
    JS_SetClassProto(ctx, js_audioctx_class_id, ctxProto);

    // Register AudioContext constructor on global
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_NewCFunction2(ctx, js_audioctx_constructor, "AudioContext", 0,
                                     JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, global, "AudioContext", ctor);

    // Install native getUserMedia as a global, then wire into navigator via JS
    JS_SetPropertyStr(ctx, global, "__nativeGetUserMedia",
                      JS_NewCFunction(ctx, js_getUserMedia, "__nativeGetUserMedia", 1));
    JS_FreeValue(ctx, global);

    // Wire native getUserMedia into navigator.mediaDevices via JS eval.
    // Must run after brokit::api::installAll() which creates the navigator object.
    const char* shim =
        "(function() {"
        "  if (typeof navigator === 'undefined') return;"
        "  if (!navigator.mediaDevices) navigator.mediaDevices = {};"
        "  navigator.mediaDevices.getUserMedia = globalThis.__nativeGetUserMedia;"
        "})();";
    JSValue shimResult = JS_Eval(ctx, shim, strlen(shim), "<audio-shim>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(shimResult)) {
        JSValue exc = JS_GetException(ctx);
        const char* msg = JS_ToCString(ctx, exc);
        if (msg) {
            LOG_ERROR("Audio shim failed: %s", msg);
            JS_FreeCString(ctx, msg);
        }
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, shimResult);
}

void AudioBindings::cleanup(JSContext*)
{
    s_audioEngine = nullptr;
}

} // namespace bro::js
