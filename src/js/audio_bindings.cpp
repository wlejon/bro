#include "js/audio_bindings.h"
#include "js/runtime.h"
#include "audio/audio_engine.h"

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

// ---------------------------------------------------------------------------
// AudioParam — wraps a float value that updates the engine
// ---------------------------------------------------------------------------

struct AudioParamData {
    audio::AudioEngine* engine;
    int voiceId;
    enum class Target { Frequency, Gain } target;
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
    if (p->target == AudioParamData::Target::Frequency)
        p->engine->setFrequency(p->voiceId, p->value);
    else
        p->engine->setGain(p->voiceId, p->value);
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
    // Which source to analyse: 0 = output mix, 1 = mic input
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
    auto& buf = (d->source == 1) ? d->engine->micBuffer() : d->engine->outputBuffer();
    buf.readLatest(real.data(), n);

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

static JSValue js_analyser_getFloatFrequencyData(JSContext* ctx, JSValueConst this_val,
                                                   int argc, JSValueConst* argv) {
    auto* d = static_cast<AnalyserNodeData*>(JS_GetOpaque(this_val, js_analysernode_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;

    std::vector<float> magnitudes;
    analyserComputeFFT(d, magnitudes);

    // Write into Float32Array argument
    size_t byteLen = 0;
    size_t byteOff = 0;
    uint8_t* buf = JS_GetArrayBuffer(ctx, &byteLen,
        JS_GetTypedArrayBuffer(ctx, argv[0], &byteOff, &byteLen, nullptr));
    if (buf) {
        float* dst = reinterpret_cast<float*>(buf + byteOff);
        int count = std::min(static_cast<int>(byteLen / sizeof(float)),
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

    size_t byteLen = 0;
    size_t byteOff = 0;
    uint8_t* buf = JS_GetArrayBuffer(ctx, &byteLen,
        JS_GetTypedArrayBuffer(ctx, argv[0], &byteOff, &byteLen, nullptr));
    if (buf) {
        uint8_t* dst = buf + byteOff;
        int count = std::min(static_cast<int>(byteLen),
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

    size_t byteLen = 0;
    size_t byteOff = 0;
    uint8_t* buf = JS_GetArrayBuffer(ctx, &byteLen,
        JS_GetTypedArrayBuffer(ctx, argv[0], &byteOff, &byteLen, nullptr));
    if (buf) {
        float* dst = reinterpret_cast<float*>(buf + byteOff);
        int count = std::min(static_cast<int>(byteLen / sizeof(float)), d->fftSize);
        auto& ringBuf = (d->source == 1) ? d->engine->micBuffer() : d->engine->outputBuffer();
        ringBuf.readLatest(dst, count);
    }
    return JS_UNDEFINED;
}

static JSValue js_analyser_getByteTimeDomainData(JSContext* ctx, JSValueConst this_val,
                                                   int argc, JSValueConst* argv) {
    auto* d = static_cast<AnalyserNodeData*>(JS_GetOpaque(this_val, js_analysernode_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;

    std::vector<float> samples(d->fftSize);
    auto& ringBuf = (d->source == 1) ? d->engine->micBuffer() : d->engine->outputBuffer();
    ringBuf.readLatest(samples.data(), d->fftSize);

    size_t byteLen = 0;
    size_t byteOff = 0;
    uint8_t* buf = JS_GetArrayBuffer(ctx, &byteLen,
        JS_GetTypedArrayBuffer(ctx, argv[0], &byteOff, &byteLen, nullptr));
    if (buf) {
        uint8_t* dst = buf + byteOff;
        int count = std::min(static_cast<int>(byteLen), d->fftSize);
        for (int i = 0; i < count; i++) {
            dst[i] = static_cast<uint8_t>(std::clamp((samples[i] + 1.0f) * 128.0f, 0.0f, 255.0f));
        }
    }
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

    // Create the frequency AudioParam
    JSValue freqParam = createAudioParam(ctx, d->engine, voiceId,
                                         AudioParamData::Target::Frequency, 440.0f);
    JS_SetPropertyStr(ctx, obj, "frequency", freqParam);

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

static const JSCFunctionListEntry js_audioctx_proto_funcs[] = {
    JS_CGETSET_DEF("currentTime", js_audioctx_get_currentTime, nullptr),
    JS_CGETSET_DEF("sampleRate", js_audioctx_get_sampleRate, nullptr),
    JS_CFUNC_DEF("createOscillator", 0, js_audioctx_createOscillator),
    JS_CFUNC_DEF("createGain", 0, js_audioctx_createGain),
    JS_CFUNC_DEF("createAnalyser", 0, js_audioctx_createAnalyser),
    JS_CFUNC_DEF("createMediaStreamSource", 1, js_audioctx_createMediaStreamSource),
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

    // Use JS eval to set up navigator.mediaDevices.getUserMedia
    // This works regardless of how brokit configured the navigator object
    const char* shim =
        "if (typeof navigator !== 'undefined') {"
        "  if (!navigator.mediaDevices) navigator.mediaDevices = {};"
        "  navigator.mediaDevices.getUserMedia = __nativeGetUserMedia;"
        "}";
    JS_Eval(ctx, shim, strlen(shim), "<audio-shim>", JS_EVAL_TYPE_GLOBAL);
}

void AudioBindings::cleanup(JSContext*)
{
    s_audioEngine = nullptr;
}

} // namespace bro::js
