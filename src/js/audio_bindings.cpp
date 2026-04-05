#include "js/audio_bindings.h"
#include "js/runtime.h"
#include "util/log.h"
#include <broaudio/dsp/fft.h>
#include <broaudio/synth/voice_allocator.h>
#include <broaudio/synth/modulation.h>
#include <broaudio/synth/wavetable.h>
#include <broaudio/midi/midi_input.h>

#include <algorithm>
#include <string>
#include <cstring>
#include <cmath>
#include <vector>
#include <unordered_map>
#include <memory>

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
static JSClassID js_voiceallocator_class_id = 0;
static JSClassID js_modmatrix_class_id = 0;
static JSClassID js_midiinput_class_id = 0;

// ---------------------------------------------------------------------------
// Global engine pointer + wavetable registry
// ---------------------------------------------------------------------------

static broaudio::Engine* s_audioEngine = nullptr;
static std::unordered_map<int, std::shared_ptr<broaudio::WavetableBank>> s_wavetables;
static int s_nextWavetableId = 1;

// ---------------------------------------------------------------------------
// Helper: parse biquad filter type string
// ---------------------------------------------------------------------------

static broaudio::BiquadFilter::Type parseFilterType(const char* str) {
    if (strcmp(str, "highpass") == 0) return broaudio::BiquadFilter::Type::Highpass;
    if (strcmp(str, "bandpass") == 0) return broaudio::BiquadFilter::Type::Bandpass;
    if (strcmp(str, "notch") == 0) return broaudio::BiquadFilter::Type::Notch;
    if (strcmp(str, "allpass") == 0) return broaudio::BiquadFilter::Type::Allpass;
    if (strcmp(str, "peaking") == 0) return broaudio::BiquadFilter::Type::Peaking;
    if (strcmp(str, "lowshelf") == 0) return broaudio::BiquadFilter::Type::Lowshelf;
    if (strcmp(str, "highshelf") == 0) return broaudio::BiquadFilter::Type::Highshelf;
    return broaudio::BiquadFilter::Type::Lowpass;
}

// Helper: parse waveform string (including noise types)
static broaudio::Waveform parseWaveform(const char* str) {
    if (strcmp(str, "square") == 0) return broaudio::Waveform::Square;
    if (strcmp(str, "sawtooth") == 0) return broaudio::Waveform::Sawtooth;
    if (strcmp(str, "triangle") == 0) return broaudio::Waveform::Triangle;
    if (strcmp(str, "wavetable") == 0) return broaudio::Waveform::Wavetable;
    if (strcmp(str, "whitenoise") == 0) return broaudio::Waveform::WhiteNoise;
    if (strcmp(str, "pinknoise") == 0) return broaudio::Waveform::PinkNoise;
    if (strcmp(str, "brownnoise") == 0) return broaudio::Waveform::BrownNoise;
    return broaudio::Waveform::Sine;
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

// ---------------------------------------------------------------------------
// AudioParam — wraps a float value that updates the engine
// ---------------------------------------------------------------------------

struct AudioParamData {
    broaudio::Engine* engine;
    int voiceId;         // voice ID for voice params, or filter slot for filter params
    enum class Target {
        Frequency, Gain, Pan, Attack, Decay, SustainLevel, Release,
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
        case AudioParamData::Target::Pan:
            p->engine->setVoicePan(p->voiceId, p->value); break;
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

static JSValue createAudioParam(JSContext* ctx, broaudio::Engine* engine,
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
    broaudio::Engine* engine;
    int fftSize = 2048;
    float minDecibels = -100.0f;
    float maxDecibels = -30.0f;
    float smoothingTimeConstant = 0.8f;
    int source = 0;
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

    if (d->source == 2) {
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

    for (int i = 0; i < n; i++) {
        float w = 0.42f - 0.5f * std::cos(2.0f * static_cast<float>(M_PI) * i / (n - 1))
                        + 0.08f * std::cos(4.0f * static_cast<float>(M_PI) * i / (n - 1));
        real[i] *= w;
    }

    broaudio::fft(real.data(), imag.data(), n);

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
    broaudio::Engine* engine;
};

static void js_micstream_finalizer(JSRuntime*, JSValue val) {
    delete static_cast<MicStreamData*>(JS_GetOpaque(val, js_micstream_class_id));
}

static JSClassDef js_micstream_class = {
    "MediaStream", js_micstream_finalizer, nullptr, nullptr, nullptr
};

struct MicSourceData {
    broaudio::Engine* engine;
};

static void js_micsource_finalizer(JSRuntime*, JSValue val) {
    delete static_cast<MicSourceData*>(JS_GetOpaque(val, js_micsource_class_id));
}

static JSClassDef js_micsource_class = {
    "MediaStreamAudioSourceNode", js_micsource_finalizer, nullptr, nullptr, nullptr
};

static JSValue js_micsource_connect(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    auto* ad = static_cast<AnalyserNodeData*>(JS_GetOpaque(argv[0], js_analysernode_class_id));
    if (ad) {
        ad->source = 1;
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
    broaudio::Engine* engine;
    int voiceId;
    std::string type = "sine";
};

static void js_oscnode_finalizer(JSRuntime*, JSValue val) {
    auto* d = static_cast<OscNodeData*>(JS_GetOpaque(val, js_oscnode_class_id));
    if (d) {
        d->engine->stopVoice(d->voiceId, d->engine->currentTime());
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
    d->engine->setWaveform(d->voiceId, parseWaveform(s));
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue js_osc_connect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    auto* d = static_cast<OscNodeData*>(JS_GetOpaque(this_val, js_oscnode_class_id));
    if (!d) return JS_DupValue(ctx, argv[0]);

    auto* gd = static_cast<GainNodeData*>(JS_GetOpaque(argv[0], js_gainnode_class_id));
    if (gd) {
        JS_SetPropertyStr(ctx, this_val, "__gainNode", JS_DupValue(ctx, argv[0]));
    }

    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_osc_start(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<OscNodeData*>(JS_GetOpaque(this_val, js_oscnode_class_id));
    if (!d) return JS_UNDEFINED;

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
    broaudio::Engine* engine;
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
    broaudio::Engine* engine;
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
    return JS_GetPropertyStr(ctx, this_val, "__type");
}

static JSValue js_biquadfilter_set_type(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* d = static_cast<BiquadFilterNodeData*>(JS_GetOpaque(this_val, js_biquadfilter_class_id));
    if (!d) return JS_UNDEFINED;
    const char* str = JS_ToCString(ctx, val);
    if (!str) return JS_UNDEFINED;
    d->engine->setFilterType(d->slot, parseFilterType(str));
    JS_FreeCString(ctx, str);
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
// VoiceAllocator — polyphonic voice management
// ---------------------------------------------------------------------------

struct VoiceAllocatorData {
    broaudio::Engine* engine;
    std::unique_ptr<broaudio::VoiceAllocator> allocator;
    JSContext* ctx;
    JSValue voiceSetupCallback;  // prevents GC of the JS function
    JSValue lambdaCbRef;         // the DupValue captured by the C++ lambda
};

static void js_voiceallocator_finalizer(JSRuntime* rt, JSValue val) {
    auto* d = static_cast<VoiceAllocatorData*>(JS_GetOpaque(val, js_voiceallocator_class_id));
    if (d) {
        // Clear the C++ callback first so it doesn't reference freed JS values
        d->allocator->setVoiceSetup(nullptr);
        if (!JS_IsUndefined(d->voiceSetupCallback))
            JS_FreeValueRT(rt, d->voiceSetupCallback);
        if (!JS_IsUndefined(d->lambdaCbRef))
            JS_FreeValueRT(rt, d->lambdaCbRef);
        delete d;
    }
}

static JSClassDef js_voiceallocator_class = {
    "VoiceAllocator", js_voiceallocator_finalizer, nullptr, nullptr, nullptr
};

static JSValue js_va_noteOn(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<VoiceAllocatorData*>(JS_GetOpaque(this_val, js_voiceallocator_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;
    int note; JS_ToInt32(ctx, &note, argv[0]);
    double velocity; JS_ToFloat64(ctx, &velocity, argv[1]);
    double when = d->engine->currentTime();
    if (argc >= 3) JS_ToFloat64(ctx, &when, argv[2]);
    int voiceId = d->allocator->noteOn(note, static_cast<float>(velocity), when);
    return JS_NewInt32(ctx, voiceId);
}

static JSValue js_va_noteOff(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<VoiceAllocatorData*>(JS_GetOpaque(this_val, js_voiceallocator_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    int note; JS_ToInt32(ctx, &note, argv[0]);
    double when = d->engine->currentTime();
    if (argc >= 2) JS_ToFloat64(ctx, &when, argv[1]);
    d->allocator->noteOff(note, when);
    return JS_UNDEFINED;
}

static JSValue js_va_allNotesOff(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<VoiceAllocatorData*>(JS_GetOpaque(this_val, js_voiceallocator_class_id));
    if (!d) return JS_UNDEFINED;
    double when = d->engine->currentTime();
    if (argc >= 1) JS_ToFloat64(ctx, &when, argv[0]);
    d->allocator->allNotesOff(when);
    return JS_UNDEFINED;
}

static JSValue js_va_setStealPolicy(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<VoiceAllocatorData*>(JS_GetOpaque(this_val, js_voiceallocator_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_UNDEFINED;
    broaudio::StealPolicy policy = broaudio::StealPolicy::Oldest;
    if (strcmp(s, "quietest") == 0) policy = broaudio::StealPolicy::Quietest;
    else if (strcmp(s, "samenote") == 0) policy = broaudio::StealPolicy::SameNote;
    else if (strcmp(s, "none") == 0) policy = broaudio::StealPolicy::None;
    JS_FreeCString(ctx, s);
    d->allocator->setStealPolicy(policy);
    return JS_UNDEFINED;
}

static JSValue js_va_setMaxVoices(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<VoiceAllocatorData*>(JS_GetOpaque(this_val, js_voiceallocator_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    int count; JS_ToInt32(ctx, &count, argv[0]);
    d->allocator->setMaxVoices(count);
    return JS_UNDEFINED;
}

static JSValue js_va_setVoiceSetup(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<VoiceAllocatorData*>(JS_GetOpaque(this_val, js_voiceallocator_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;

    // Free previous references
    if (!JS_IsUndefined(d->voiceSetupCallback))
        JS_FreeValue(ctx, d->voiceSetupCallback);
    if (!JS_IsUndefined(d->lambdaCbRef))
        JS_FreeValue(ctx, d->lambdaCbRef);

    d->voiceSetupCallback = JS_DupValue(ctx, argv[0]);
    d->lambdaCbRef = JS_DupValue(ctx, argv[0]);

    // Set the C++ callback that calls into JS
    JSValue cbRef = d->lambdaCbRef;
    JSContext* jsCtx = ctx;
    d->allocator->setVoiceSetup([jsCtx, cbRef](int voiceId, int note, float velocity) {
        JSValue args[3] = {
            JS_NewInt32(jsCtx, voiceId),
            JS_NewInt32(jsCtx, note),
            JS_NewFloat64(jsCtx, velocity)
        };
        JSValue ret = JS_Call(jsCtx, cbRef, JS_UNDEFINED, 3, args);
        JS_FreeValue(jsCtx, args[0]);
        JS_FreeValue(jsCtx, args[1]);
        JS_FreeValue(jsCtx, args[2]);
        if (JS_IsException(ret)) {
            JSValue exc = JS_GetException(jsCtx);
            const char* msg = JS_ToCString(jsCtx, exc);
            if (msg) { LOG_ERROR("voiceSetup callback error: %s", msg); JS_FreeCString(jsCtx, msg); }
            JS_FreeValue(jsCtx, exc);
        }
        JS_FreeValue(jsCtx, ret);
    });

    return JS_UNDEFINED;
}

static JSValue js_va_get_activeVoiceCount(JSContext* ctx, JSValueConst this_val) {
    auto* d = static_cast<VoiceAllocatorData*>(JS_GetOpaque(this_val, js_voiceallocator_class_id));
    return d ? JS_NewInt32(ctx, d->allocator->activeVoiceCount()) : JS_UNDEFINED;
}

static JSValue js_va_voiceForNote(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<VoiceAllocatorData*>(JS_GetOpaque(this_val, js_voiceallocator_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    int note; JS_ToInt32(ctx, &note, argv[0]);
    return JS_NewInt32(ctx, d->allocator->voiceForNote(note));
}

static const JSCFunctionListEntry js_voiceallocator_proto_funcs[] = {
    JS_CFUNC_DEF("noteOn", 2, js_va_noteOn),
    JS_CFUNC_DEF("noteOff", 1, js_va_noteOff),
    JS_CFUNC_DEF("allNotesOff", 0, js_va_allNotesOff),
    JS_CFUNC_DEF("setStealPolicy", 1, js_va_setStealPolicy),
    JS_CFUNC_DEF("setMaxVoices", 1, js_va_setMaxVoices),
    JS_CFUNC_DEF("setVoiceSetup", 1, js_va_setVoiceSetup),
    JS_CFUNC_DEF("voiceForNote", 1, js_va_voiceForNote),
    JS_CGETSET_DEF("activeVoiceCount", js_va_get_activeVoiceCount, nullptr),
};

// ---------------------------------------------------------------------------
// ModMatrix — modulation routing
// ---------------------------------------------------------------------------

struct ModMatrixData {
    broaudio::Engine* engine;
    broaudio::ModMatrix* modMatrix;
};

static JSClassDef js_modmatrix_class = {
    "ModMatrix", nullptr, nullptr, nullptr, nullptr
};

static broaudio::LfoShape parseLfoShape(const char* s) {
    if (strcmp(s, "triangle") == 0) return broaudio::LfoShape::Triangle;
    if (strcmp(s, "square") == 0) return broaudio::LfoShape::Square;
    if (strcmp(s, "sawup") == 0) return broaudio::LfoShape::SawUp;
    if (strcmp(s, "sawdown") == 0) return broaudio::LfoShape::SawDown;
    if (strcmp(s, "sampleandhold") == 0) return broaudio::LfoShape::SampleAndHold;
    return broaudio::LfoShape::Sine;
}

static broaudio::ModSource parseModSource(const char* s) {
    if (strcmp(s, "lfo2") == 0) return broaudio::ModSource::Lfo2;
    if (strcmp(s, "lfo3") == 0) return broaudio::ModSource::Lfo3;
    if (strcmp(s, "lfo4") == 0) return broaudio::ModSource::Lfo4;
    if (strcmp(s, "envelope") == 0) return broaudio::ModSource::Envelope;
    if (strcmp(s, "velocity") == 0) return broaudio::ModSource::Velocity;
    if (strcmp(s, "keytracking") == 0) return broaudio::ModSource::KeyTracking;
    if (strcmp(s, "modwheel") == 0) return broaudio::ModSource::ModWheel;
    if (strcmp(s, "aftertouch") == 0) return broaudio::ModSource::Aftertouch;
    return broaudio::ModSource::Lfo1;
}

static broaudio::ModDest parseModDest(const char* s) {
    if (strcmp(s, "gain") == 0) return broaudio::ModDest::Gain;
    if (strcmp(s, "pan") == 0) return broaudio::ModDest::Pan;
    if (strcmp(s, "filterfreq") == 0) return broaudio::ModDest::FilterFreq;
    if (strcmp(s, "filterq") == 0) return broaudio::ModDest::FilterQ;
    if (strcmp(s, "pulsewidth") == 0) return broaudio::ModDest::PulseWidth;
    if (strcmp(s, "delaysend") == 0) return broaudio::ModDest::DelaySend;
    return broaudio::ModDest::Pitch;
}

// LFO control
static JSValue js_mod_setLfoShape(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<ModMatrixData*>(JS_GetOpaque(this_val, js_modmatrix_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;
    int idx; JS_ToInt32(ctx, &idx, argv[0]);
    const char* s = JS_ToCString(ctx, argv[1]);
    if (s) { d->modMatrix->setLfoShape(idx, parseLfoShape(s)); JS_FreeCString(ctx, s); }
    return JS_UNDEFINED;
}

static JSValue js_mod_setLfoRate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<ModMatrixData*>(JS_GetOpaque(this_val, js_modmatrix_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;
    int idx; JS_ToInt32(ctx, &idx, argv[0]);
    double hz; JS_ToFloat64(ctx, &hz, argv[1]);
    d->modMatrix->setLfoRate(idx, static_cast<float>(hz));
    return JS_UNDEFINED;
}

static JSValue js_mod_setLfoDepth(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<ModMatrixData*>(JS_GetOpaque(this_val, js_modmatrix_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;
    int idx; JS_ToInt32(ctx, &idx, argv[0]);
    double v; JS_ToFloat64(ctx, &v, argv[1]);
    d->modMatrix->setLfoDepth(idx, static_cast<float>(v));
    return JS_UNDEFINED;
}

static JSValue js_mod_setLfoOffset(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<ModMatrixData*>(JS_GetOpaque(this_val, js_modmatrix_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;
    int idx; JS_ToInt32(ctx, &idx, argv[0]);
    double v; JS_ToFloat64(ctx, &v, argv[1]);
    d->modMatrix->setLfoOffset(idx, static_cast<float>(v));
    return JS_UNDEFINED;
}

static JSValue js_mod_setLfoBipolar(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<ModMatrixData*>(JS_GetOpaque(this_val, js_modmatrix_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;
    int idx; JS_ToInt32(ctx, &idx, argv[0]);
    d->modMatrix->setLfoBipolar(idx, JS_ToBool(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue js_mod_setLfoSync(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<ModMatrixData*>(JS_GetOpaque(this_val, js_modmatrix_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;
    int idx; JS_ToInt32(ctx, &idx, argv[0]);
    d->modMatrix->setLfoSync(idx, JS_ToBool(ctx, argv[1]));
    return JS_UNDEFINED;
}

// Route management
static JSValue js_mod_addRoute(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<ModMatrixData*>(JS_GetOpaque(this_val, js_modmatrix_class_id));
    if (!d || argc < 3) return JS_UNDEFINED;
    const char* srcStr = JS_ToCString(ctx, argv[0]);
    const char* dstStr = JS_ToCString(ctx, argv[1]);
    double amount; JS_ToFloat64(ctx, &amount, argv[2]);
    int idx = -1;
    if (srcStr && dstStr) {
        idx = d->modMatrix->addRoute(parseModSource(srcStr), parseModDest(dstStr), static_cast<float>(amount));
    }
    if (srcStr) JS_FreeCString(ctx, srcStr);
    if (dstStr) JS_FreeCString(ctx, dstStr);
    return JS_NewInt32(ctx, idx);
}

static JSValue js_mod_removeRoute(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<ModMatrixData*>(JS_GetOpaque(this_val, js_modmatrix_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    int idx; JS_ToInt32(ctx, &idx, argv[0]);
    d->modMatrix->removeRoute(idx);
    return JS_UNDEFINED;
}

static JSValue js_mod_setRouteAmount(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<ModMatrixData*>(JS_GetOpaque(this_val, js_modmatrix_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;
    int idx; JS_ToInt32(ctx, &idx, argv[0]);
    double v; JS_ToFloat64(ctx, &v, argv[1]);
    d->modMatrix->setRouteAmount(idx, static_cast<float>(v));
    return JS_UNDEFINED;
}

static JSValue js_mod_setRouteEnabled(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<ModMatrixData*>(JS_GetOpaque(this_val, js_modmatrix_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;
    int idx; JS_ToInt32(ctx, &idx, argv[0]);
    d->modMatrix->setRouteEnabled(idx, JS_ToBool(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue js_mod_clearAllRoutes(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* d = static_cast<ModMatrixData*>(JS_GetOpaque(this_val, js_modmatrix_class_id));
    if (d) d->modMatrix->clearAllRoutes();
    return JS_UNDEFINED;
}

static JSValue js_mod_get_routeCount(JSContext* ctx, JSValueConst this_val) {
    auto* d = static_cast<ModMatrixData*>(JS_GetOpaque(this_val, js_modmatrix_class_id));
    return d ? JS_NewInt32(ctx, d->modMatrix->routeCount()) : JS_UNDEFINED;
}

static JSValue js_mod_setModWheel(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<ModMatrixData*>(JS_GetOpaque(this_val, js_modmatrix_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    double v; JS_ToFloat64(ctx, &v, argv[0]);
    d->modMatrix->setModWheel(static_cast<float>(v));
    return JS_UNDEFINED;
}

static JSValue js_mod_setAftertouch(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<ModMatrixData*>(JS_GetOpaque(this_val, js_modmatrix_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    double v; JS_ToFloat64(ctx, &v, argv[0]);
    d->modMatrix->setAftertouch(static_cast<float>(v));
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_modmatrix_proto_funcs[] = {
    JS_CFUNC_DEF("setLfoShape", 2, js_mod_setLfoShape),
    JS_CFUNC_DEF("setLfoRate", 2, js_mod_setLfoRate),
    JS_CFUNC_DEF("setLfoDepth", 2, js_mod_setLfoDepth),
    JS_CFUNC_DEF("setLfoOffset", 2, js_mod_setLfoOffset),
    JS_CFUNC_DEF("setLfoBipolar", 2, js_mod_setLfoBipolar),
    JS_CFUNC_DEF("setLfoSync", 2, js_mod_setLfoSync),
    JS_CFUNC_DEF("addRoute", 3, js_mod_addRoute),
    JS_CFUNC_DEF("removeRoute", 1, js_mod_removeRoute),
    JS_CFUNC_DEF("setRouteAmount", 2, js_mod_setRouteAmount),
    JS_CFUNC_DEF("setRouteEnabled", 2, js_mod_setRouteEnabled),
    JS_CFUNC_DEF("clearAllRoutes", 0, js_mod_clearAllRoutes),
    JS_CFUNC_DEF("setModWheel", 1, js_mod_setModWheel),
    JS_CFUNC_DEF("setAftertouch", 1, js_mod_setAftertouch),
    JS_CGETSET_DEF("routeCount", js_mod_get_routeCount, nullptr),
};

// ---------------------------------------------------------------------------
// MidiInput — hardware MIDI controller support
// ---------------------------------------------------------------------------

struct MidiInputData {
    broaudio::Engine* engine;
    std::unique_ptr<broaudio::MidiInput> midi;
    JSContext* ctx;
    JSValue pitchBendCallback;
    JSValue rawCallback;
};

static void js_midiinput_finalizer(JSRuntime* rt, JSValue val) {
    auto* d = static_cast<MidiInputData*>(JS_GetOpaque(val, js_midiinput_class_id));
    if (d) {
        if (d->midi) d->midi->close();
        if (!JS_IsUndefined(d->pitchBendCallback)) JS_FreeValueRT(rt, d->pitchBendCallback);
        if (!JS_IsUndefined(d->rawCallback)) JS_FreeValueRT(rt, d->rawCallback);
        delete d;
    }
}

static JSClassDef js_midiinput_class = {
    "MidiInput", js_midiinput_finalizer, nullptr, nullptr, nullptr
};

static JSValue js_midi_availablePorts(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* d = static_cast<MidiInputData*>(JS_GetOpaque(this_val, js_midiinput_class_id));
    if (!d) return JS_UNDEFINED;
    auto ports = d->midi->availablePorts();
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < ports.size(); i++) {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "index", JS_NewInt32(ctx, ports[i].index));
        JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, ports[i].name.c_str()));
        JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), obj);
    }
    return arr;
}

static JSValue js_midi_open(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<MidiInputData*>(JS_GetOpaque(this_val, js_midiinput_class_id));
    if (!d || argc < 1) return JS_FALSE;
    int port; JS_ToInt32(ctx, &port, argv[0]);
    return JS_NewBool(ctx, d->midi->open(port));
}

static JSValue js_midi_close(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* d = static_cast<MidiInputData*>(JS_GetOpaque(this_val, js_midiinput_class_id));
    if (d) d->midi->close();
    return JS_UNDEFINED;
}

static JSValue js_midi_get_isOpen(JSContext* ctx, JSValueConst this_val) {
    auto* d = static_cast<MidiInputData*>(JS_GetOpaque(this_val, js_midiinput_class_id));
    return d ? JS_NewBool(ctx, d->midi->isOpen()) : JS_FALSE;
}

static JSValue js_midi_connectToAllocator(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<MidiInputData*>(JS_GetOpaque(this_val, js_midiinput_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    auto* va = static_cast<VoiceAllocatorData*>(JS_GetOpaque(argv[0], js_voiceallocator_class_id));
    if (va) d->midi->connectToAllocator(va->allocator.get());
    return JS_UNDEFINED;
}

static JSValue js_midi_onControlChange(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<MidiInputData*>(JS_GetOpaque(this_val, js_midiinput_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;
    int cc; JS_ToInt32(ctx, &cc, argv[0]);
    if (cc < 0 || cc > 127) return JS_UNDEFINED;
    JSValue cbRef = JS_DupValue(ctx, argv[1]);
    JSContext* jsCtx = ctx;
    d->midi->onControlChange(static_cast<uint8_t>(cc),
        [jsCtx, cbRef](uint8_t channel, uint8_t ccNum, uint8_t value) {
            JSValue args[3] = {
                JS_NewInt32(jsCtx, channel),
                JS_NewInt32(jsCtx, ccNum),
                JS_NewInt32(jsCtx, value)
            };
            JSValue ret = JS_Call(jsCtx, cbRef, JS_UNDEFINED, 3, args);
            for (int i = 0; i < 3; i++) JS_FreeValue(jsCtx, args[i]);
            JS_FreeValue(jsCtx, ret);
        });
    return JS_UNDEFINED;
}

static JSValue js_midi_onPitchBend(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<MidiInputData*>(JS_GetOpaque(this_val, js_midiinput_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    if (!JS_IsUndefined(d->pitchBendCallback)) JS_FreeValue(ctx, d->pitchBendCallback);
    d->pitchBendCallback = JS_DupValue(ctx, argv[0]);
    JSValue cbRef = JS_DupValue(ctx, argv[0]);
    JSContext* jsCtx = ctx;
    d->midi->onPitchBend([jsCtx, cbRef](uint8_t channel, int16_t value) {
        JSValue args[2] = { JS_NewInt32(jsCtx, channel), JS_NewInt32(jsCtx, value) };
        JSValue ret = JS_Call(jsCtx, cbRef, JS_UNDEFINED, 2, args);
        JS_FreeValue(jsCtx, args[0]); JS_FreeValue(jsCtx, args[1]);
        JS_FreeValue(jsCtx, ret);
    });
    return JS_UNDEFINED;
}

static JSValue js_midi_processEvents(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* d = static_cast<MidiInputData*>(JS_GetOpaque(this_val, js_midiinput_class_id));
    if (d) d->midi->processEvents();
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_midiinput_proto_funcs[] = {
    JS_CFUNC_DEF("availablePorts", 0, js_midi_availablePorts),
    JS_CFUNC_DEF("open", 1, js_midi_open),
    JS_CFUNC_DEF("close", 0, js_midi_close),
    JS_CFUNC_DEF("connectToAllocator", 1, js_midi_connectToAllocator),
    JS_CFUNC_DEF("onControlChange", 2, js_midi_onControlChange),
    JS_CFUNC_DEF("onPitchBend", 1, js_midi_onPitchBend),
    JS_CFUNC_DEF("processEvents", 0, js_midi_processEvents),
    JS_CGETSET_DEF("isOpen", js_midi_get_isOpen, nullptr),
};

// ---------------------------------------------------------------------------
// AudioContext
// ---------------------------------------------------------------------------

struct AudioCtxData {
    broaudio::Engine* engine;
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

static JSValue js_audioctx_createOscillator(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d) return JS_UNDEFINED;

    int voiceId = d->engine->createVoice();

    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_oscnode_class_id));
    auto* oscData = new OscNodeData{d->engine, voiceId, "sine"};
    JS_SetOpaque(obj, oscData);

    JS_SetPropertyStr(ctx, obj, "frequency",
        createAudioParam(ctx, d->engine, voiceId, AudioParamData::Target::Frequency, 440.0f));
    JS_SetPropertyStr(ctx, obj, "pan",
        createAudioParam(ctx, d->engine, voiceId, AudioParamData::Target::Pan, 0.0f));
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

static JSValue js_audioctx_createGain(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
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

static JSValue js_audioctx_createAnalyser(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d) return JS_UNDEFINED;

    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_analysernode_class_id));
    auto* data = new AnalyserNodeData{d->engine};
    JS_SetOpaque(obj, data);
    return obj;
}

static JSValue js_audioctx_createBiquadFilter(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d) return JS_UNDEFINED;

    int slot = d->engine->allocateFilterSlot();
    if (slot < 0) return JS_ThrowInternalError(ctx, "No filter slots available");

    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_biquadfilter_class_id));
    auto* filterData = new BiquadFilterNodeData{d->engine, slot};
    JS_SetOpaque(obj, filterData);

    JS_SetPropertyStr(ctx, obj, "__type", JS_NewString(ctx, "lowpass"));
    JS_SetPropertyStr(ctx, obj, "frequency",
        createAudioParam(ctx, d->engine, slot, AudioParamData::Target::FilterFrequency, 1000.0f));
    JS_SetPropertyStr(ctx, obj, "Q",
        createAudioParam(ctx, d->engine, slot, AudioParamData::Target::FilterQ, 1.0f));
    JS_SetPropertyStr(ctx, obj, "gain",
        createAudioParam(ctx, d->engine, slot, AudioParamData::Target::FilterGain, 0.0f));

    return obj;
}

// --- Delay (master bus shortcuts) ---

static JSValue js_audioctx_setDelayEnabled(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    d->engine->setDelayEnabled(JS_ToBool(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setDelayTime(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    double v; JS_ToFloat64(ctx, &v, argv[0]);
    d->engine->setDelayTime(static_cast<float>(v));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setDelayFeedback(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    double v; JS_ToFloat64(ctx, &v, argv[0]);
    d->engine->setDelayFeedback(static_cast<float>(v));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setDelayMix(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    double v; JS_ToFloat64(ctx, &v, argv[0]);
    d->engine->setDelayMix(static_cast<float>(v));
    return JS_UNDEFINED;
}

// --- Reverb (master bus shortcuts) ---

static JSValue js_audioctx_setReverbEnabled(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    d->engine->setBusReverbEnabled(0, JS_ToBool(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setReverbRoomSize(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    double v; JS_ToFloat64(ctx, &v, argv[0]);
    d->engine->setBusReverbRoomSize(0, static_cast<float>(v));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setReverbDamping(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    double v; JS_ToFloat64(ctx, &v, argv[0]);
    d->engine->setBusReverbDamping(0, static_cast<float>(v));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setReverbMix(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    double v; JS_ToFloat64(ctx, &v, argv[0]);
    d->engine->setBusReverbMix(0, static_cast<float>(v));
    return JS_UNDEFINED;
}

// --- Chorus (master bus shortcuts) ---

static JSValue js_audioctx_setChorusEnabled(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    d->engine->setBusChorusEnabled(0, JS_ToBool(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setChorusRate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    double v; JS_ToFloat64(ctx, &v, argv[0]);
    d->engine->setBusChorusRate(0, static_cast<float>(v));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setChorusDepth(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    double v; JS_ToFloat64(ctx, &v, argv[0]);
    d->engine->setBusChorusDepth(0, static_cast<float>(v));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setChorusMix(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    double v; JS_ToFloat64(ctx, &v, argv[0]);
    d->engine->setBusChorusMix(0, static_cast<float>(v));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setChorusFeedback(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    double v; JS_ToFloat64(ctx, &v, argv[0]);
    d->engine->setBusChorusFeedback(0, static_cast<float>(v));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setChorusBaseDelay(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    double v; JS_ToFloat64(ctx, &v, argv[0]);
    d->engine->setBusChorusBaseDelay(0, static_cast<float>(v));
    return JS_UNDEFINED;
}

// --- Compressor (master bus shortcuts) ---

static JSValue js_audioctx_setCompressorEnabled(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    d->engine->setBusCompressorEnabled(0, JS_ToBool(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setCompressorThreshold(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    double v; JS_ToFloat64(ctx, &v, argv[0]);
    d->engine->setBusCompressorThreshold(0, static_cast<float>(v));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setCompressorRatio(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    double v; JS_ToFloat64(ctx, &v, argv[0]);
    d->engine->setBusCompressorRatio(0, static_cast<float>(v));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setCompressorAttack(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    double v; JS_ToFloat64(ctx, &v, argv[0]);
    d->engine->setBusCompressorAttack(0, static_cast<float>(v));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setCompressorRelease(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    double v; JS_ToFloat64(ctx, &v, argv[0]);
    d->engine->setBusCompressorRelease(0, static_cast<float>(v));
    return JS_UNDEFINED;
}

// --- Mix bus API ---

static JSValue js_audioctx_createBus(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d) return JS_UNDEFINED;
    return JS_NewInt32(ctx, d->engine->createBus());
}

static JSValue js_audioctx_deleteBus(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    int id; JS_ToInt32(ctx, &id, argv[0]);
    d->engine->deleteBus(id);
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setBusGain(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;
    int id; JS_ToInt32(ctx, &id, argv[0]);
    double v; JS_ToFloat64(ctx, &v, argv[1]);
    d->engine->setBusGain(id, static_cast<float>(v));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setBusPan(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;
    int id; JS_ToInt32(ctx, &id, argv[0]);
    double v; JS_ToFloat64(ctx, &v, argv[1]);
    d->engine->setBusPan(id, static_cast<float>(v));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setBusMuted(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;
    int id; JS_ToInt32(ctx, &id, argv[0]);
    d->engine->setBusMuted(id, JS_ToBool(ctx, argv[1]));
    return JS_UNDEFINED;
}

// Per-bus filter control
static JSValue js_audioctx_allocateBusFilterSlot(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    int busId; JS_ToInt32(ctx, &busId, argv[0]);
    return JS_NewInt32(ctx, d->engine->allocateBusFilterSlot(busId));
}

static JSValue js_audioctx_releaseBusFilterSlot(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;
    int busId, slot; JS_ToInt32(ctx, &busId, argv[0]); JS_ToInt32(ctx, &slot, argv[1]);
    d->engine->releaseBusFilterSlot(busId, slot);
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setBusFilterEnabled(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 3) return JS_UNDEFINED;
    int busId, slot; JS_ToInt32(ctx, &busId, argv[0]); JS_ToInt32(ctx, &slot, argv[1]);
    d->engine->setBusFilterEnabled(busId, slot, JS_ToBool(ctx, argv[2]));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setBusFilterType(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 3) return JS_UNDEFINED;
    int busId, slot; JS_ToInt32(ctx, &busId, argv[0]); JS_ToInt32(ctx, &slot, argv[1]);
    const char* s = JS_ToCString(ctx, argv[2]);
    if (s) { d->engine->setBusFilterType(busId, slot, parseFilterType(s)); JS_FreeCString(ctx, s); }
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setBusFilterFrequency(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 3) return JS_UNDEFINED;
    int busId, slot; JS_ToInt32(ctx, &busId, argv[0]); JS_ToInt32(ctx, &slot, argv[1]);
    double v; JS_ToFloat64(ctx, &v, argv[2]);
    d->engine->setBusFilterFrequency(busId, slot, static_cast<float>(v));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setBusFilterQ(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 3) return JS_UNDEFINED;
    int busId, slot; JS_ToInt32(ctx, &busId, argv[0]); JS_ToInt32(ctx, &slot, argv[1]);
    double v; JS_ToFloat64(ctx, &v, argv[2]);
    d->engine->setBusFilterQ(busId, slot, static_cast<float>(v));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setBusFilterGain(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 3) return JS_UNDEFINED;
    int busId, slot; JS_ToInt32(ctx, &busId, argv[0]); JS_ToInt32(ctx, &slot, argv[1]);
    double v; JS_ToFloat64(ctx, &v, argv[2]);
    d->engine->setBusFilterGain(busId, slot, static_cast<float>(v));
    return JS_UNDEFINED;
}

// Per-bus effects (delay, compressor, reverb, chorus)
#define BUS_EFFECT_BOOL(name, method) \
static JSValue js_audioctx_##name(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) { \
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id)); \
    if (!d || argc < 2) return JS_UNDEFINED; \
    int busId; JS_ToInt32(ctx, &busId, argv[0]); \
    d->engine->method(busId, JS_ToBool(ctx, argv[1])); \
    return JS_UNDEFINED; \
}

#define BUS_EFFECT_FLOAT(name, method) \
static JSValue js_audioctx_##name(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) { \
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id)); \
    if (!d || argc < 2) return JS_UNDEFINED; \
    int busId; JS_ToInt32(ctx, &busId, argv[0]); \
    double v; JS_ToFloat64(ctx, &v, argv[1]); \
    d->engine->method(busId, static_cast<float>(v)); \
    return JS_UNDEFINED; \
}

BUS_EFFECT_BOOL(setBusDelayEnabled, setBusDelayEnabled)
BUS_EFFECT_FLOAT(setBusDelayTime, setBusDelayTime)
BUS_EFFECT_FLOAT(setBusDelayFeedback, setBusDelayFeedback)
BUS_EFFECT_FLOAT(setBusDelayMix, setBusDelayMix)

BUS_EFFECT_BOOL(setBusCompressorEnabled, setBusCompressorEnabled)
BUS_EFFECT_FLOAT(setBusCompressorThreshold, setBusCompressorThreshold)
BUS_EFFECT_FLOAT(setBusCompressorRatio, setBusCompressorRatio)
BUS_EFFECT_FLOAT(setBusCompressorAttack, setBusCompressorAttack)
BUS_EFFECT_FLOAT(setBusCompressorRelease, setBusCompressorRelease)

BUS_EFFECT_BOOL(setBusReverbEnabled, setBusReverbEnabled)
BUS_EFFECT_FLOAT(setBusReverbRoomSize, setBusReverbRoomSize)
BUS_EFFECT_FLOAT(setBusReverbDamping, setBusReverbDamping)
BUS_EFFECT_FLOAT(setBusReverbMix, setBusReverbMix)

BUS_EFFECT_BOOL(setBusChorusEnabled, setBusChorusEnabled)
BUS_EFFECT_FLOAT(setBusChorusRate, setBusChorusRate)
BUS_EFFECT_FLOAT(setBusChorusDepth, setBusChorusDepth)
BUS_EFFECT_FLOAT(setBusChorusMix, setBusChorusMix)
BUS_EFFECT_FLOAT(setBusChorusFeedback, setBusChorusFeedback)
BUS_EFFECT_FLOAT(setBusChorusBaseDelay, setBusChorusBaseDelay)

#undef BUS_EFFECT_BOOL
#undef BUS_EFFECT_FLOAT

// Voice/clip bus routing
static JSValue js_audioctx_setVoiceBus(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;
    int voiceId, busId; JS_ToInt32(ctx, &voiceId, argv[0]); JS_ToInt32(ctx, &busId, argv[1]);
    d->engine->setVoiceBus(voiceId, busId);
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setPlaybackBus(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;
    int id, busId; JS_ToInt32(ctx, &id, argv[0]); JS_ToInt32(ctx, &busId, argv[1]);
    d->engine->setPlaybackBus(id, busId);
    return JS_UNDEFINED;
}

// Offline effect processing
static JSValue js_audioctx_processEffectsOffline(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;

    int busId; JS_ToInt32(ctx, &busId, argv[0]);

    // Get input Float32Array
    size_t byteOffset, byteLen;
    JSValue typedBuf = JS_GetTypedArrayBuffer(ctx, argv[1], &byteOffset, &byteLen, nullptr);
    if (JS_IsException(typedBuf)) return JS_EXCEPTION;
    size_t totalSize;
    auto* rawBuf = JS_GetArrayBuffer(ctx, &totalSize, typedBuf);
    JS_FreeValue(ctx, typedBuf);
    if (!rawBuf) return JS_UNDEFINED;

    auto* inputData = reinterpret_cast<const float*>(rawBuf + byteOffset);
    int numSamples = static_cast<int>(byteLen / sizeof(float));

    auto result = d->engine->processEffectsOffline(busId, inputData, numSamples);
    if (result.empty()) return JS_UNDEFINED;

    // Return as Float32Array (same pattern as stopRecording)
    int count = static_cast<int>(result.size());
    JSValue lenVal = JS_NewInt32(ctx, count);
    JSValue arr = JS_NewTypedArray(ctx, 1, &lenVal, JS_TYPED_ARRAY_FLOAT32);
    JS_FreeValue(ctx, lenVal);
    if (JS_IsException(arr)) return arr;

    size_t byteOff = 0, viewLen = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, arr, &byteOff, &viewLen, nullptr);
    if (!JS_IsException(abuf)) {
        size_t abufLen = 0;
        uint8_t* ptr = JS_GetArrayBuffer(ctx, &abufLen, abuf);
        if (ptr && abufLen >= result.size() * sizeof(float)) {
            std::memcpy(ptr + byteOff, result.data(), result.size() * sizeof(float));
        }
        JS_FreeValue(ctx, abuf);
    }
    return arr;
}

// --- Voice note context ---

static JSValue js_audioctx_setVoiceNote(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 3) return JS_UNDEFINED;
    int voiceId, note; JS_ToInt32(ctx, &voiceId, argv[0]); JS_ToInt32(ctx, &note, argv[1]);
    double vel; JS_ToFloat64(ctx, &vel, argv[2]);
    d->engine->setVoiceNote(voiceId, note, static_cast<float>(vel));
    return JS_UNDEFINED;
}

// --- Direct voice parameter control (for VoiceAllocator voiceSetup callbacks) ---

static JSValue js_audioctx_setVoiceWaveform(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;
    int voiceId; JS_ToInt32(ctx, &voiceId, argv[0]);
    const char* s = JS_ToCString(ctx, argv[1]);
    if (s) { d->engine->setWaveform(voiceId, parseWaveform(s)); JS_FreeCString(ctx, s); }
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setVoiceFrequency(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;
    int voiceId; JS_ToInt32(ctx, &voiceId, argv[0]);
    double v; JS_ToFloat64(ctx, &v, argv[1]);
    d->engine->setFrequency(voiceId, static_cast<float>(v));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setVoiceGain(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;
    int voiceId; JS_ToInt32(ctx, &voiceId, argv[0]);
    double v; JS_ToFloat64(ctx, &v, argv[1]);
    d->engine->setGain(voiceId, static_cast<float>(v));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setVoicePan(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;
    int voiceId; JS_ToInt32(ctx, &voiceId, argv[0]);
    double v; JS_ToFloat64(ctx, &v, argv[1]);
    d->engine->setVoicePan(voiceId, static_cast<float>(v));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setVoiceAttack(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;
    int voiceId; JS_ToInt32(ctx, &voiceId, argv[0]);
    double v; JS_ToFloat64(ctx, &v, argv[1]);
    d->engine->setAttackTime(voiceId, static_cast<float>(v));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setVoiceDecay(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;
    int voiceId; JS_ToInt32(ctx, &voiceId, argv[0]);
    double v; JS_ToFloat64(ctx, &v, argv[1]);
    d->engine->setDecayTime(voiceId, static_cast<float>(v));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setVoiceSustain(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;
    int voiceId; JS_ToInt32(ctx, &voiceId, argv[0]);
    double v; JS_ToFloat64(ctx, &v, argv[1]);
    d->engine->setSustainLevel(voiceId, static_cast<float>(v));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setVoiceRelease(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;
    int voiceId; JS_ToInt32(ctx, &voiceId, argv[0]);
    double v; JS_ToFloat64(ctx, &v, argv[1]);
    d->engine->setReleaseTime(voiceId, static_cast<float>(v));
    return JS_UNDEFINED;
}

// --- Wavetable synthesis ---

static JSValue js_audioctx_createWavetable(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    const char* type = JS_ToCString(ctx, argv[0]);
    if (!type) return JS_UNDEFINED;

    std::shared_ptr<broaudio::WavetableBank> bank;
    int sr = d->engine->sampleRate();
    if (strcmp(type, "saw") == 0) bank = broaudio::WavetableBank::createSaw(sr);
    else if (strcmp(type, "square") == 0) bank = broaudio::WavetableBank::createSquare(sr);
    else if (strcmp(type, "triangle") == 0) bank = broaudio::WavetableBank::createTriangle(sr);
    JS_FreeCString(ctx, type);

    if (!bank) return JS_UNDEFINED;
    int id = s_nextWavetableId++;
    s_wavetables[id] = bank;
    return JS_NewInt32(ctx, id);
}

static JSValue js_audioctx_createWavetableFromWaveform(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;

    size_t len = 0;
    uint8_t* raw = getTypedArrayPtr(ctx, argv[0], len);
    if (!raw) return JS_ThrowTypeError(ctx, "Expected Float32Array");

    int numSamples = static_cast<int>(len / sizeof(float));
    auto bank = broaudio::WavetableBank::createFromWaveform(
        reinterpret_cast<float*>(raw), numSamples, d->engine->sampleRate());
    if (!bank) return JS_UNDEFINED;

    int id = s_nextWavetableId++;
    s_wavetables[id] = bank;
    return JS_NewInt32(ctx, id);
}

static JSValue js_audioctx_deleteWavetable(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    int id; JS_ToInt32(ctx, &id, argv[0]);
    s_wavetables.erase(id);
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setVoiceWavetable(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;
    int voiceId, wtId; JS_ToInt32(ctx, &voiceId, argv[0]); JS_ToInt32(ctx, &wtId, argv[1]);
    auto it = s_wavetables.find(wtId);
    if (it != s_wavetables.end()) {
        d->engine->setVoiceWavetable(voiceId, it->second);
        d->engine->setWaveform(voiceId, broaudio::Waveform::Wavetable);
    }
    return JS_UNDEFINED;
}

// --- Spectrum API ---

static JSValue js_audioctx_getSpectrum(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;

    int numBins; JS_ToInt32(ctx, &numBins, argv[0]);
    if (numBins <= 0 || numBins > 8192) return JS_UNDEFINED;

    JSValue lenVal = JS_NewInt32(ctx, numBins);
    JSValue arr = JS_NewTypedArray(ctx, 1, &lenVal, JS_TYPED_ARRAY_FLOAT32);
    JS_FreeValue(ctx, lenVal);
    if (JS_IsException(arr)) return arr;

    size_t byteOff = 0, viewLen = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, arr, &byteOff, &viewLen, nullptr);
    if (!JS_IsException(abuf)) {
        size_t abufLen = 0;
        uint8_t* ptr = JS_GetArrayBuffer(ctx, &abufLen, abuf);
        if (ptr) {
            d->engine->getSpectrum(reinterpret_cast<float*>(ptr + byteOff), numBins);
        }
        JS_FreeValue(ctx, abuf);
    }
    return arr;
}

// --- Spatial audio ---

static JSValue js_audioctx_setListenerPosition(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 3) return JS_UNDEFINED;
    double x, y, z;
    JS_ToFloat64(ctx, &x, argv[0]); JS_ToFloat64(ctx, &y, argv[1]); JS_ToFloat64(ctx, &z, argv[2]);
    d->engine->setListenerPosition(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setListenerOrientation(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 6) return JS_UNDEFINED;
    double fx, fy, fz, ux, uy, uz;
    JS_ToFloat64(ctx, &fx, argv[0]); JS_ToFloat64(ctx, &fy, argv[1]); JS_ToFloat64(ctx, &fz, argv[2]);
    JS_ToFloat64(ctx, &ux, argv[3]); JS_ToFloat64(ctx, &uy, argv[4]); JS_ToFloat64(ctx, &uz, argv[5]);
    d->engine->setListenerOrientation(
        static_cast<float>(fx), static_cast<float>(fy), static_cast<float>(fz),
        static_cast<float>(ux), static_cast<float>(uy), static_cast<float>(uz));
    return JS_UNDEFINED;
}

// --- Factory methods for VoiceAllocator, ModMatrix, MidiInput ---

static JSValue js_audioctx_createVoiceAllocator(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d) return JS_UNDEFINED;
    int maxVoices = 16;
    if (argc >= 1) JS_ToInt32(ctx, &maxVoices, argv[0]);

    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_voiceallocator_class_id));
    auto* va = new VoiceAllocatorData{
        d->engine,
        std::make_unique<broaudio::VoiceAllocator>(*d->engine, maxVoices),
        ctx,
        JS_UNDEFINED,
        JS_UNDEFINED
    };
    JS_SetOpaque(obj, va);
    return obj;
}

static JSValue js_audioctx_getModMatrix(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d) return JS_UNDEFINED;

    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_modmatrix_class_id));
    auto* mm = new ModMatrixData{d->engine, &d->engine->modMatrix()};
    JS_SetOpaque(obj, mm);
    return obj;
}

static JSValue js_audioctx_createMidiInput(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d) return JS_UNDEFINED;

    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_midiinput_class_id));
    auto* mi = new MidiInputData{
        d->engine,
        std::make_unique<broaudio::MidiInput>(*d->engine),
        ctx,
        JS_UNDEFINED,
        JS_UNDEFINED
    };
    JS_SetOpaque(obj, mi);
    return obj;
}

// --- Mic ---

static JSValue js_audioctx_createMediaStreamSource(JSContext* ctx, JSValueConst this_val,
                                                    int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;

    auto* ms = static_cast<MicStreamData*>(JS_GetOpaque(argv[0], js_micstream_class_id));
    if (!ms) return JS_ThrowTypeError(ctx, "Expected MediaStream argument");

    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_micsource_class_id));
    auto* data = new MicSourceData{d->engine};
    JS_SetOpaque(obj, data);
    return obj;
}

// Mic mute/gain properties
static JSValue js_audioctx_get_masterGain(JSContext* ctx, JSValueConst this_val) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    return d ? JS_NewFloat64(ctx, d->engine->masterGain()) : JS_UNDEFINED;
}

static JSValue js_audioctx_set_masterGain(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (d) {
        double v; JS_ToFloat64(ctx, &v, val);
        d->engine->setMasterGain(static_cast<float>(v));
    }
    return JS_UNDEFINED;
}

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

static JSValue js_audioctx_get_micBus(JSContext* ctx, JSValueConst this_val) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    return d ? JS_NewInt32(ctx, d->engine->micBus()) : JS_UNDEFINED;
}

static JSValue js_audioctx_set_micBus(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (d) {
        int v; JS_ToInt32(ctx, &v, val);
        d->engine->setMicBus(v);
    }
    return JS_UNDEFINED;
}

// --- Recording ---

static JSValue js_audioctx_get_recording(JSContext* ctx, JSValueConst this_val) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    return d ? JS_NewBool(ctx, d->engine->isRecording()) : JS_UNDEFINED;
}

static JSValue js_audioctx_startRecording(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (d) d->engine->startRecording();
    return JS_UNDEFINED;
}

static JSValue js_audioctx_stopRecording(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d) return JS_UNDEFINED;

    d->engine->stopRecording();
    const auto& buf = d->engine->getRecordBuffer();
    if (buf.empty()) return JS_NULL;

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

static JSValue js_audioctx_createClip(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;

    size_t len = 0;
    uint8_t* raw = getTypedArrayPtr(ctx, argv[0], len);
    if (!raw) return JS_ThrowTypeError(ctx, "Expected Float32Array argument");

    int numSamples = static_cast<int>(len / sizeof(float));
    int id = d->engine->createClip(reinterpret_cast<float*>(raw), numSamples);
    return JS_NewInt32(ctx, id);
}

static JSValue js_audioctx_deleteClip(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    int id; JS_ToInt32(ctx, &id, argv[0]);
    d->engine->deleteClip(id);
    return JS_UNDEFINED;
}

static JSValue js_audioctx_getClipSampleCount(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    int id; JS_ToInt32(ctx, &id, argv[0]);
    return JS_NewInt32(ctx, d->engine->getClipSampleCount(id));
}

static JSValue js_audioctx_getClipWaveform(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
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

// --- Clip Playback ---

static JSValue js_audioctx_playClip(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    int clipId; JS_ToInt32(ctx, &clipId, argv[0]);
    float gain = 1.0f;
    bool loop = false;
    if (argc >= 2) { double v; JS_ToFloat64(ctx, &v, argv[1]); gain = static_cast<float>(v); }
    if (argc >= 3) { loop = JS_ToBool(ctx, argv[2]); }
    return JS_NewInt32(ctx, d->engine->playClip(clipId, gain, loop));
}

static JSValue js_audioctx_stopPlayback(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    int id; JS_ToInt32(ctx, &id, argv[0]);
    d->engine->stopPlayback(id);
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setPlaybackGain(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;
    int id; JS_ToInt32(ctx, &id, argv[0]);
    double v; JS_ToFloat64(ctx, &v, argv[1]);
    d->engine->setPlaybackGain(id, static_cast<float>(v));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setPlaybackLoop(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;
    int id; JS_ToInt32(ctx, &id, argv[0]);
    d->engine->setPlaybackLoop(id, JS_ToBool(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setPlaybackPlaying(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;
    int id; JS_ToInt32(ctx, &id, argv[0]);
    d->engine->setPlaybackPlaying(id, JS_ToBool(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setPlaybackRegion(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 3) return JS_UNDEFINED;
    int id, start, end;
    JS_ToInt32(ctx, &id, argv[0]);
    JS_ToInt32(ctx, &start, argv[1]);
    JS_ToInt32(ctx, &end, argv[2]);
    d->engine->setPlaybackRegion(id, start, end);
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setPlaybackRate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;
    int id; JS_ToInt32(ctx, &id, argv[0]);
    double v; JS_ToFloat64(ctx, &v, argv[1]);
    d->engine->setPlaybackRate(id, static_cast<float>(v));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setPlaybackPan(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 2) return JS_UNDEFINED;
    int id; JS_ToInt32(ctx, &id, argv[0]);
    double v; JS_ToFloat64(ctx, &v, argv[1]);
    d->engine->setPlaybackPan(id, static_cast<float>(v));
    return JS_UNDEFINED;
}

static JSValue js_audioctx_getPlaybackPosition(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d || argc < 1) return JS_UNDEFINED;
    int id; JS_ToInt32(ctx, &id, argv[0]);
    return JS_NewFloat64(ctx, d->engine->getPlaybackPosition(id));
}

// ---------------------------------------------------------------------------
// AudioContext prototype function list
// ---------------------------------------------------------------------------

static const JSCFunctionListEntry js_audioctx_proto_funcs[] = {
    // Properties
    JS_CGETSET_DEF("currentTime", js_audioctx_get_currentTime, nullptr),
    JS_CGETSET_DEF("sampleRate", js_audioctx_get_sampleRate, nullptr),
    JS_CGETSET_DEF("masterGain", js_audioctx_get_masterGain, js_audioctx_set_masterGain),
    JS_CGETSET_DEF("micMuted", js_audioctx_get_micMuted, js_audioctx_set_micMuted),
    JS_CGETSET_DEF("micMonitorGain", js_audioctx_get_micMonitorGain, js_audioctx_set_micMonitorGain),
    JS_CGETSET_DEF("micBus", js_audioctx_get_micBus, js_audioctx_set_micBus),
    JS_CGETSET_DEF("recording", js_audioctx_get_recording, nullptr),

    // Node creation
    JS_CFUNC_DEF("createOscillator", 0, js_audioctx_createOscillator),
    JS_CFUNC_DEF("createGain", 0, js_audioctx_createGain),
    JS_CFUNC_DEF("createAnalyser", 0, js_audioctx_createAnalyser),
    JS_CFUNC_DEF("createBiquadFilter", 0, js_audioctx_createBiquadFilter),
    JS_CFUNC_DEF("createMediaStreamSource", 1, js_audioctx_createMediaStreamSource),

    // Master delay
    JS_CFUNC_DEF("setDelayEnabled", 1, js_audioctx_setDelayEnabled),
    JS_CFUNC_DEF("setDelayTime", 1, js_audioctx_setDelayTime),
    JS_CFUNC_DEF("setDelayFeedback", 1, js_audioctx_setDelayFeedback),
    JS_CFUNC_DEF("setDelayMix", 1, js_audioctx_setDelayMix),

    // Master reverb
    JS_CFUNC_DEF("setReverbEnabled", 1, js_audioctx_setReverbEnabled),
    JS_CFUNC_DEF("setReverbRoomSize", 1, js_audioctx_setReverbRoomSize),
    JS_CFUNC_DEF("setReverbDamping", 1, js_audioctx_setReverbDamping),
    JS_CFUNC_DEF("setReverbMix", 1, js_audioctx_setReverbMix),

    // Master chorus
    JS_CFUNC_DEF("setChorusEnabled", 1, js_audioctx_setChorusEnabled),
    JS_CFUNC_DEF("setChorusRate", 1, js_audioctx_setChorusRate),
    JS_CFUNC_DEF("setChorusDepth", 1, js_audioctx_setChorusDepth),
    JS_CFUNC_DEF("setChorusMix", 1, js_audioctx_setChorusMix),
    JS_CFUNC_DEF("setChorusFeedback", 1, js_audioctx_setChorusFeedback),
    JS_CFUNC_DEF("setChorusBaseDelay", 1, js_audioctx_setChorusBaseDelay),

    // Master compressor
    JS_CFUNC_DEF("setCompressorEnabled", 1, js_audioctx_setCompressorEnabled),
    JS_CFUNC_DEF("setCompressorThreshold", 1, js_audioctx_setCompressorThreshold),
    JS_CFUNC_DEF("setCompressorRatio", 1, js_audioctx_setCompressorRatio),
    JS_CFUNC_DEF("setCompressorAttack", 1, js_audioctx_setCompressorAttack),
    JS_CFUNC_DEF("setCompressorRelease", 1, js_audioctx_setCompressorRelease),

    // Mix bus API
    JS_CFUNC_DEF("createBus", 0, js_audioctx_createBus),
    JS_CFUNC_DEF("deleteBus", 1, js_audioctx_deleteBus),
    JS_CFUNC_DEF("setBusGain", 2, js_audioctx_setBusGain),
    JS_CFUNC_DEF("setBusPan", 2, js_audioctx_setBusPan),
    JS_CFUNC_DEF("setBusMuted", 2, js_audioctx_setBusMuted),
    JS_CFUNC_DEF("allocateBusFilterSlot", 1, js_audioctx_allocateBusFilterSlot),
    JS_CFUNC_DEF("releaseBusFilterSlot", 2, js_audioctx_releaseBusFilterSlot),
    JS_CFUNC_DEF("setBusFilterEnabled", 3, js_audioctx_setBusFilterEnabled),
    JS_CFUNC_DEF("setBusFilterType", 3, js_audioctx_setBusFilterType),
    JS_CFUNC_DEF("setBusFilterFrequency", 3, js_audioctx_setBusFilterFrequency),
    JS_CFUNC_DEF("setBusFilterQ", 3, js_audioctx_setBusFilterQ),
    JS_CFUNC_DEF("setBusFilterGain", 3, js_audioctx_setBusFilterGain),
    JS_CFUNC_DEF("setBusDelayEnabled", 2, js_audioctx_setBusDelayEnabled),
    JS_CFUNC_DEF("setBusDelayTime", 2, js_audioctx_setBusDelayTime),
    JS_CFUNC_DEF("setBusDelayFeedback", 2, js_audioctx_setBusDelayFeedback),
    JS_CFUNC_DEF("setBusDelayMix", 2, js_audioctx_setBusDelayMix),
    JS_CFUNC_DEF("setBusCompressorEnabled", 2, js_audioctx_setBusCompressorEnabled),
    JS_CFUNC_DEF("setBusCompressorThreshold", 2, js_audioctx_setBusCompressorThreshold),
    JS_CFUNC_DEF("setBusCompressorRatio", 2, js_audioctx_setBusCompressorRatio),
    JS_CFUNC_DEF("setBusCompressorAttack", 2, js_audioctx_setBusCompressorAttack),
    JS_CFUNC_DEF("setBusCompressorRelease", 2, js_audioctx_setBusCompressorRelease),
    JS_CFUNC_DEF("setBusReverbEnabled", 2, js_audioctx_setBusReverbEnabled),
    JS_CFUNC_DEF("setBusReverbRoomSize", 2, js_audioctx_setBusReverbRoomSize),
    JS_CFUNC_DEF("setBusReverbDamping", 2, js_audioctx_setBusReverbDamping),
    JS_CFUNC_DEF("setBusReverbMix", 2, js_audioctx_setBusReverbMix),
    JS_CFUNC_DEF("setBusChorusEnabled", 2, js_audioctx_setBusChorusEnabled),
    JS_CFUNC_DEF("setBusChorusRate", 2, js_audioctx_setBusChorusRate),
    JS_CFUNC_DEF("setBusChorusDepth", 2, js_audioctx_setBusChorusDepth),
    JS_CFUNC_DEF("setBusChorusMix", 2, js_audioctx_setBusChorusMix),
    JS_CFUNC_DEF("setBusChorusFeedback", 2, js_audioctx_setBusChorusFeedback),
    JS_CFUNC_DEF("setBusChorusBaseDelay", 2, js_audioctx_setBusChorusBaseDelay),

    // Voice/clip bus routing
    JS_CFUNC_DEF("setVoiceBus", 2, js_audioctx_setVoiceBus),
    JS_CFUNC_DEF("setPlaybackBus", 2, js_audioctx_setPlaybackBus),

    // Offline effect processing
    JS_CFUNC_DEF("processEffectsOffline", 2, js_audioctx_processEffectsOffline),

    // Direct voice parameter control
    JS_CFUNC_DEF("setVoiceNote", 3, js_audioctx_setVoiceNote),
    JS_CFUNC_DEF("setVoiceWaveform", 2, js_audioctx_setVoiceWaveform),
    JS_CFUNC_DEF("setVoiceFrequency", 2, js_audioctx_setVoiceFrequency),
    JS_CFUNC_DEF("setVoiceGain", 2, js_audioctx_setVoiceGain),
    JS_CFUNC_DEF("setVoicePan", 2, js_audioctx_setVoicePan),
    JS_CFUNC_DEF("setVoiceAttack", 2, js_audioctx_setVoiceAttack),
    JS_CFUNC_DEF("setVoiceDecay", 2, js_audioctx_setVoiceDecay),
    JS_CFUNC_DEF("setVoiceSustain", 2, js_audioctx_setVoiceSustain),
    JS_CFUNC_DEF("setVoiceRelease", 2, js_audioctx_setVoiceRelease),

    // Wavetable
    JS_CFUNC_DEF("createWavetable", 1, js_audioctx_createWavetable),
    JS_CFUNC_DEF("createWavetableFromWaveform", 1, js_audioctx_createWavetableFromWaveform),
    JS_CFUNC_DEF("deleteWavetable", 1, js_audioctx_deleteWavetable),
    JS_CFUNC_DEF("setVoiceWavetable", 2, js_audioctx_setVoiceWavetable),

    // Spectrum
    JS_CFUNC_DEF("getSpectrum", 1, js_audioctx_getSpectrum),

    // Spatial audio
    JS_CFUNC_DEF("setListenerPosition", 3, js_audioctx_setListenerPosition),
    JS_CFUNC_DEF("setListenerOrientation", 6, js_audioctx_setListenerOrientation),

    // Factory methods
    JS_CFUNC_DEF("createVoiceAllocator", 1, js_audioctx_createVoiceAllocator),
    JS_CFUNC_DEF("getModMatrix", 0, js_audioctx_getModMatrix),
    JS_CFUNC_DEF("createMidiInput", 0, js_audioctx_createMidiInput),

    // Recording
    JS_CFUNC_DEF("startRecording", 0, js_audioctx_startRecording),
    JS_CFUNC_DEF("stopRecording", 0, js_audioctx_stopRecording),

    // Clips
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
    JS_CFUNC_DEF("setPlaybackRate", 2, js_audioctx_setPlaybackRate),
    JS_CFUNC_DEF("setPlaybackPan", 2, js_audioctx_setPlaybackPan),
    JS_CFUNC_DEF("getPlaybackPosition", 1, js_audioctx_getPlaybackPosition),
};

// ---------------------------------------------------------------------------
// Constructor for `new AudioContext()`
// ---------------------------------------------------------------------------

static JSValue js_audioctx_constructor(JSContext* ctx, JSValueConst new_target,
                                        int, JSValueConst*) {
    if (!s_audioEngine) return JS_ThrowInternalError(ctx, "Audio not initialized");

    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_audioctx_class_id));
    if (JS_IsException(obj)) return obj;

    auto* data = new AudioCtxData{s_audioEngine};
    JS_SetOpaque(obj, data);

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

    if (!s_audioEngine->startMicCapture()) {
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

    JSValue stream = JS_NewObjectClass(ctx, static_cast<int>(js_micstream_class_id));
    auto* data = new MicStreamData{s_audioEngine};
    JS_SetOpaque(stream, data);

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

void AudioBindings::install(JSContext* ctx, broaudio::Engine* engine)
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

    // VoiceAllocator
    JS_NewClassID(rt, &js_voiceallocator_class_id);
    JS_NewClass(rt, js_voiceallocator_class_id, &js_voiceallocator_class);
    {
        JSValue vaProto = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, vaProto, js_voiceallocator_proto_funcs,
                                   sizeof(js_voiceallocator_proto_funcs)/sizeof(js_voiceallocator_proto_funcs[0]));
        JS_SetClassProto(ctx, js_voiceallocator_class_id, vaProto);
    }

    // ModMatrix
    JS_NewClassID(rt, &js_modmatrix_class_id);
    JS_NewClass(rt, js_modmatrix_class_id, &js_modmatrix_class);
    {
        JSValue mmProto = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, mmProto, js_modmatrix_proto_funcs,
                                   sizeof(js_modmatrix_proto_funcs)/sizeof(js_modmatrix_proto_funcs[0]));
        JS_SetClassProto(ctx, js_modmatrix_class_id, mmProto);
    }

    // MidiInput
    JS_NewClassID(rt, &js_midiinput_class_id);
    JS_NewClass(rt, js_midiinput_class_id, &js_midiinput_class);
    {
        JSValue miProto = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, miProto, js_midiinput_proto_funcs,
                                   sizeof(js_midiinput_proto_funcs)/sizeof(js_midiinput_proto_funcs[0]));
        JS_SetClassProto(ctx, js_midiinput_class_id, miProto);
    }

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
    s_wavetables.clear();
    s_nextWavetableId = 1;
}

} // namespace bro::js
