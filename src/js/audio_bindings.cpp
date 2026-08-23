#include "js/audio_bindings.h"
#include "js/asset_path.h"
#include "js/async_job.h"
#include "js/runtime.h"
#include "util/log.h"
#include <qjsbind/qjsbind.h>
#include <broaudio/dsp/fft.h>
#include <broaudio/synth/voice_allocator.h>
#include <broaudio/synth/modulation.h>
#include <broaudio/synth/wavetable.h>
#include <broaudio/midi/midi_input.h>
#include <broaudio/sequencer/sequence.h>
#include <broaudio/io/audio_file.h>
#include <broaudio/dsp/resampler.h>
#include <broaudio/node/audio_node.h>
#include <algorithm>
#include <optional>
#include <string>
#include <cstring>
#include <cmath>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cassert>
#include <thread>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace bro::js {

static JSValue make_float32_array(JSContext* ctx, const float* data, size_t count)
{
    size_t byte_len = count * sizeof(float);
    JSValue ab = JS_NewArrayBufferCopy(ctx, reinterpret_cast<const uint8_t*>(data), byte_len);
    if (JS_IsException(ab)) return ab;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, "Float32Array");
    JSValue result = JS_CallConstructor(ctx, ctor, 1, &ab);
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, ab);
    return result;
}

static bool resolve_f32(JSContext* ctx, JSValueConst v, const char* name,
                        float** out, size_t* count)
{
    size_t byte_offset = 0, byte_len = 0, bpe = 0;
    JSValue buf = JS_GetTypedArrayBuffer(ctx, v, &byte_offset, &byte_len, &bpe);
    if (JS_IsException(buf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_ThrowTypeError(ctx, "%s must be a Float32Array", name);
        return false;
    }
    if (bpe != sizeof(float)) {
        JS_FreeValue(ctx, buf);
        JS_ThrowTypeError(ctx, "%s must be a Float32Array", name);
        return false;
    }
    size_t ab_len = 0;
    uint8_t* ab_ptr = JS_GetArrayBuffer(ctx, &ab_len, buf);
    JS_FreeValue(ctx, buf);
    if (!ab_ptr) {
        JS_ThrowTypeError(ctx, "%s has a detached or invalid buffer", name);
        return false;
    }
    *out   = reinterpret_cast<float*>(ab_ptr + byte_offset);
    *count = byte_len / sizeof(float);
    return true;
}

using ::std::optional;
using ::std::string;
using ::std::clamp;
using ::std::min;

// ---------------------------------------------------------------------------
// Global engine pointer + wavetable registry
// ---------------------------------------------------------------------------

static broaudio::Engine* s_audioEngine = nullptr;
static ::std::unordered_map<int, ::std::shared_ptr<broaudio::WavetableBank>> s_wavetables;
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

// Helper: parse LFO shape
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

static broaudio::DistanceModel parseDistanceModel(const char* s) {
    if (strcmp(s, "linear") == 0) return broaudio::DistanceModel::Linear;
    if (strcmp(s, "exponential") == 0) return broaudio::DistanceModel::Exponential;
    return broaudio::DistanceModel::Inverse;
}

// ---------------------------------------------------------------------------
// Wrapper structs u2014 one per JS class (for unique qjsbind::class_id<T>)
// ---------------------------------------------------------------------------

struct AudioParamData {
    broaudio::Engine* engine;
    int voiceId;
    enum class Target {
        Frequency, Gain, Pan, Attack, Decay, SustainLevel, Release,
        FilterFrequency, FilterQ, FilterGain, PitchBend
    } target;
    float value;
};

struct AudioDestNodeData {
    // Marker only u2014 no data needed
};

struct AnalyserNodeData {
    broaudio::Engine* engine;
    int fftSize = 2048;
    float minDecibels = -100.0f;
    float maxDecibels = -30.0f;
    float smoothingTimeConstant = 0.8f;
    int source = 0;
    ::std::vector<float> smoothedMagnitudes;
};

struct MicStreamData {
    broaudio::Engine* engine;
};

struct MicSourceData {
    broaudio::Engine* engine;
};

struct OscNodeData {
    broaudio::Engine* engine;
    int voiceId;
    ::std::string type = "sine";
    broaudio::OscillatorNode node;

    ~OscNodeData() {
        engine->stopVoice(voiceId, engine->currentTime());
    }
};

struct GainNodeData {
    broaudio::Engine* engine;
    broaudio::GainNode node;
};

struct BiquadFilterNodeData {
    broaudio::Engine* engine;
    int slot;

    ~BiquadFilterNodeData() {
        engine->releaseFilterSlot(slot);
    }
};

struct VoiceAllocatorData {
    broaudio::Engine* engine;
    ::std::unique_ptr<broaudio::VoiceAllocator> allocator;
    JSContext* ctx;
    JSValue voiceSetupCallback = JS_UNDEFINED;
    JSValue lambdaCbRef = JS_UNDEFINED;

    ~VoiceAllocatorData() {
        allocator->setVoiceSetup(nullptr);
        if (ctx) {
            JSRuntime* rt = JS_GetRuntime(ctx);
            if (!JS_IsUndefined(voiceSetupCallback)) JS_FreeValueRT(rt, voiceSetupCallback);
            if (!JS_IsUndefined(lambdaCbRef))         JS_FreeValueRT(rt, lambdaCbRef);
        }
    }
};

struct ModMatrixData {
    broaudio::Engine* engine;
    broaudio::ModMatrix* modMatrix;
};

struct MidiInputData {
    broaudio::Engine* engine;
    ::std::unique_ptr<broaudio::MidiInput> midi;
    JSContext* ctx;
    JSValue pitchBendCallback = JS_UNDEFINED;
    JSValue rawCallback = JS_UNDEFINED;
    // One slot per CC number. The broaudio callback for a CC borrows the ref
    // stored here rather than taking its own u2014 every ref this struct owns must
    // be reachable from a field, or it can neither be freed nor GC-marked.
    // A default member initializer, not a constructor: the struct is created
    // with aggregate init below, and a user-declared ctor would break that.
    ::std::vector<JSValue> ccCallbacks = ::std::vector<JSValue>(128, JS_UNDEFINED);

    ~MidiInputData() {
        // Close first: the broaudio callbacks below capture `this` and read the
        // JSValues we are about to free, so they must stop firing before we do.
        if (midi) midi->close();
        if (ctx) {
            JSRuntime* rt = JS_GetRuntime(ctx);
            JS_FreeValueRT(rt, pitchBendCallback);
            JS_FreeValueRT(rt, rawCallback);
            for (JSValue& v : ccCallbacks) JS_FreeValueRT(rt, v);
        }
    }
};

struct SequenceData {
    ::std::unique_ptr<broaudio::Sequence> seq;
    JSContext* ctx = nullptr;
    ::std::vector<JSValue> automationCallbacks;

    ~SequenceData() {
        seq->clearAutomationLanes();
        if (ctx) {
            JSRuntime* rt = JS_GetRuntime(ctx);
            for (JSValue v : automationCallbacks) {
                if (!JS_IsUndefined(v)) JS_FreeValueRT(rt, v);
            }
            automationCallbacks.clear();
        }
    }
};

struct AudioCtxData {
    broaudio::Engine* engine;
};



static void assertMainAudioThread() {
#ifndef NDEBUG
    static const ::std::thread::id s_mainThreadId = ::std::this_thread::get_id();
    assert(::std::this_thread::get_id() == s_mainThreadId && "Main-thread affinity violated");
#endif
}

// ---------------------------------------------------------------------------
// AnalyserNode FFT helper
// ---------------------------------------------------------------------------

static void analyserComputeFFT(AnalyserNodeData* d, ::std::vector<float>& magnitudes) {
    assertMainAudioThread();
    int n = d->fftSize;
    int halfN = n / 2;
    magnitudes.resize(halfN);

    ::std::vector<float> real(n), imag(n, 0.0f);

    if (d->source == 2) {
        d->engine->outputBuffer().readLatest(real.data(), n);
        if (!d->engine->isMicMuted()) {
            ::std::vector<float> mic(n);
            d->engine->micBuffer().readLatest(mic.data(), n);
            for (int i = 0; i < n; i++) real[i] += mic[i];
        }
    } else {
        auto& buf = (d->source == 1) ? d->engine->micBuffer() : d->engine->outputBuffer();
        buf.readLatest(real.data(), n);
    }

    for (int i = 0; i < n; i++) {
        float w = 0.42f - 0.5f * ::std::cos(2.0f * static_cast<float>(M_PI) * i / (n - 1))
                        + 0.08f * ::std::cos(4.0f * static_cast<float>(M_PI) * i / (n - 1));
        real[i] *= w;
    }

    broaudio::fft(real.data(), imag.data(), n);

    if (d->smoothedMagnitudes.size() != static_cast<size_t>(halfN)) {
        d->smoothedMagnitudes.assign(halfN, -100.0f);
    }

    float smooth = d->smoothingTimeConstant;
    for (int i = 0; i < halfN; i++) {
        float mag = ::std::sqrt(real[i] * real[i] + imag[i] * imag[i]) / static_cast<float>(n);
        float db = (mag > 1e-20f) ? 20.0f * ::std::log10(mag) : -100.0f;
        d->smoothedMagnitudes[i] = smooth * d->smoothedMagnitudes[i] + (1.0f - smooth) * db;
        magnitudes[i] = d->smoothedMagnitudes[i];
    }
}

// ---------------------------------------------------------------------------
// Raw method functions (complex arg parsing that can't be lambdas)
// ---------------------------------------------------------------------------

// --- AnalyserNode raw methods ---

static JSValue js_analyser_getFloatFrequencyData(JSContext* ctx, JSValueConst this_val,
                                                   int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<AnalyserNodeData>(ctx, this_val);
    if (!d || argc < 1) return JS_UNDEFINED;

    ::std::vector<float> magnitudes;
    analyserComputeFFT(d, magnitudes);

    size_t len = 0;
    uint8_t* raw = getTypedArrayPtr(ctx, argv[0], len);
    if (raw) {
        float* dst = reinterpret_cast<float*>(raw);
        int count = ::std::min(static_cast<int>(len / sizeof(float)),
                             static_cast<int>(magnitudes.size()));
        for (int i = 0; i < count; i++) dst[i] = magnitudes[i];
    }
    return JS_UNDEFINED;
}

static JSValue js_analyser_getByteFrequencyData(JSContext* ctx, JSValueConst this_val,
                                                  int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<AnalyserNodeData>(ctx, this_val);
    if (!d || argc < 1) return JS_UNDEFINED;

    ::std::vector<float> magnitudes;
    analyserComputeFFT(d, magnitudes);

    size_t len = 0;
    uint8_t* dst = getTypedArrayPtr(ctx, argv[0], len);
    if (dst) {
        int count = ::std::min(static_cast<int>(len),
                             static_cast<int>(magnitudes.size()));
        float range = d->maxDecibels - d->minDecibels;
        for (int i = 0; i < count; i++) {
            float scaled = (magnitudes[i] - d->minDecibels) / range;
            scaled = ::std::clamp(scaled, 0.0f, 1.0f);
            dst[i] = static_cast<uint8_t>(scaled * 255.0f);
        }
    }
    return JS_UNDEFINED;
}

static JSValue js_analyser_getFloatTimeDomainData(JSContext* ctx, JSValueConst this_val,
                                                    int argc, JSValueConst* argv) {
    assertMainAudioThread();
    auto* d = qjsbind::unwrap<AnalyserNodeData>(ctx, this_val);
    if (!d || argc < 1) return JS_UNDEFINED;

    size_t len = 0;
    uint8_t* raw = getTypedArrayPtr(ctx, argv[0], len);
    if (raw) {
        float* dst = reinterpret_cast<float*>(raw);
        int count = ::std::min(static_cast<int>(len / sizeof(float)), d->fftSize);
        if (d->source == 2) {
            d->engine->outputBuffer().readLatest(dst, count);
            if (!d->engine->isMicMuted()) {
                ::std::vector<float> mic(count);
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
    assertMainAudioThread();
    auto* d = qjsbind::unwrap<AnalyserNodeData>(ctx, this_val);
    if (!d || argc < 1) return JS_UNDEFINED;

    ::std::vector<float> samples(d->fftSize);
    if (d->source == 2) {
        d->engine->outputBuffer().readLatest(samples.data(), d->fftSize);
        if (!d->engine->isMicMuted()) {
            ::std::vector<float> mic(d->fftSize);
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
        int count = ::std::min(static_cast<int>(len), d->fftSize);
        for (int i = 0; i < count; i++) {
            dst[i] = static_cast<uint8_t>(::std::clamp((samples[i] + 1.0f) * 128.0f, 0.0f, 255.0f));
        }
    }
    return JS_UNDEFINED;
}

// --- MicSource connect (cross-references AnalyserNodeData) ---

static JSValue js_micsource_connect(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    auto* ad = qjsbind::unwrap<AnalyserNodeData>(ctx, argv[0]);
    if (ad) {
        ad->source = 1;
    }
    return JS_DupValue(ctx, argv[0]);
}

// --- OscillatorNode raw methods (cross-reference GainNodeData) ---

static JSValue js_osc_connect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    auto* d = qjsbind::unwrap<OscNodeData>(ctx, this_val);
    if (!d) return JS_DupValue(ctx, argv[0]);

    auto* gd = qjsbind::unwrap<GainNodeData>(ctx, argv[0]);
    if (gd) {
        d->node.connect(&gd->node);
    }

    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_osc_start(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<OscNodeData>(ctx, this_val);
    if (!d) return JS_UNDEFINED;

    if (auto* gNode = d->node.connectedGainNode()) {
        float g = gNode->gain().value();
        d->engine->setGain(d->voiceId, g);
    }

    double when = d->engine->currentTime();
    if (argc >= 1) JS_ToFloat64(ctx, &when, argv[0]);
    d->engine->startVoice(d->voiceId, when);
    return JS_UNDEFINED;
}

static JSValue js_osc_stop(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<OscNodeData>(ctx, this_val);
    if (!d) return JS_UNDEFINED;
    double when = d->engine->currentTime();
    if (argc >= 1) JS_ToFloat64(ctx, &when, argv[0]);
    d->engine->stopVoice(d->voiceId, when);
    return JS_UNDEFINED;
}

// --- BiquadFilterNode raw methods (need this_val for __type property) ---

static JSValue js_biquadfilter_get_type(JSContext* ctx, JSValueConst this_val,
                                         int, JSValueConst*) {
    auto* d = qjsbind::unwrap<BiquadFilterNodeData>(ctx, this_val);
    if (!d) return JS_UNDEFINED;
    return JS_GetPropertyStr(ctx, this_val, "__type");
}

static JSValue js_biquadfilter_set_type(JSContext* ctx, JSValueConst this_val,
                                         int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<BiquadFilterNodeData>(ctx, this_val);
    if (!d || argc < 1) return JS_UNDEFINED;
    const char* str = JS_ToCString(ctx, argv[0]);
    if (!str) return JS_UNDEFINED;
    d->engine->setFilterType(d->slot, parseFilterType(str));
    JS_FreeCString(ctx, str);
    JS_SetPropertyStr(ctx, this_val, "__type", JS_DupValue(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_biquadfilter_connect(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<BiquadFilterNodeData>(ctx, this_val);
    if (d) d->engine->setFilterEnabled(d->slot, true);
    if (argc < 1) return JS_UNDEFINED;
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_biquadfilter_disconnect(JSContext* ctx, JSValueConst this_val,
                                           int, JSValueConst*) {
    auto* d = qjsbind::unwrap<BiquadFilterNodeData>(ctx, this_val);
    if (d) d->engine->setFilterEnabled(d->slot, false);
    return JS_UNDEFINED;
}

// --- VoiceAllocator raw methods ---

static JSValue js_va_noteOn(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<VoiceAllocatorData>(ctx, this_val);
    if (!d || argc < 2) return JS_UNDEFINED;
    int note; JS_ToInt32(ctx, &note, argv[0]);
    double velocity; JS_ToFloat64(ctx, &velocity, argv[1]);
    double when = d->engine->currentTime();
    if (argc >= 3) JS_ToFloat64(ctx, &when, argv[2]);
    int voiceId = d->allocator->noteOn(note, static_cast<float>(velocity), when);
    return JS_NewInt32(ctx, voiceId);
}

static JSValue js_va_noteOff(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<VoiceAllocatorData>(ctx, this_val);
    if (!d || argc < 1) return JS_UNDEFINED;
    int note; JS_ToInt32(ctx, &note, argv[0]);
    double when = d->engine->currentTime();
    if (argc >= 2) JS_ToFloat64(ctx, &when, argv[1]);
    d->allocator->noteOff(note, when);
    return JS_UNDEFINED;
}

static JSValue js_va_allNotesOff(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<VoiceAllocatorData>(ctx, this_val);
    if (!d) return JS_UNDEFINED;
    double when = d->engine->currentTime();
    if (argc >= 1) JS_ToFloat64(ctx, &when, argv[0]);
    d->allocator->allNotesOff(when);
    return JS_UNDEFINED;
}

static JSValue js_va_setVoiceSetup(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<VoiceAllocatorData>(ctx, this_val);
    if (!d || argc < 1) return JS_UNDEFINED;

    if (!JS_IsUndefined(d->voiceSetupCallback))
        JS_FreeValue(ctx, d->voiceSetupCallback);
    if (!JS_IsUndefined(d->lambdaCbRef))
        JS_FreeValue(ctx, d->lambdaCbRef);

    d->voiceSetupCallback = JS_DupValue(ctx, argv[0]);
    d->lambdaCbRef = JS_DupValue(ctx, argv[0]);

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

// --- ModMatrix raw methods ---

static JSValue js_mod_addRoute(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<ModMatrixData>(ctx, this_val);
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

// --- MidiInput raw methods ---

static JSValue js_midi_availablePorts(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* d = qjsbind::unwrap<MidiInputData>(ctx, this_val);
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

static JSValue js_midi_connectToAllocator(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<MidiInputData>(ctx, this_val);
    if (!d || argc < 1) return JS_UNDEFINED;
    auto* va = qjsbind::unwrap<VoiceAllocatorData>(ctx, argv[0]);
    if (va) d->midi->connectToAllocator(va->allocator.get());
    return JS_UNDEFINED;
}

static JSValue js_midi_onControlChange(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<MidiInputData>(ctx, this_val);
    if (!d || argc < 2) return JS_UNDEFINED;
    int cc; JS_ToInt32(ctx, &cc, argv[0]);
    if (cc < 0 || cc > 127) return JS_UNDEFINED;
    JS_FreeValue(ctx, d->ccCallbacks[cc]);          // replace any prior handler
    d->ccCallbacks[cc] = JS_DupValue(ctx, argv[1]);
    JSContext* jsCtx = ctx;
    d->midi->onControlChange(static_cast<uint8_t>(cc),
        [jsCtx, d, cc](uint8_t channel, uint8_t ccNum, uint8_t value) {
            JSValue args[3] = {
                JS_NewInt32(jsCtx, channel),
                JS_NewInt32(jsCtx, ccNum),
                JS_NewInt32(jsCtx, value)
            };
            JSValue ret = JS_Call(jsCtx, d->ccCallbacks[cc], JS_UNDEFINED, 3, args);
            for (int i = 0; i < 3; i++) JS_FreeValue(jsCtx, args[i]);
            JS_FreeValue(jsCtx, ret);
        });
    return JS_UNDEFINED;
}

static JSValue js_midi_onPitchBend(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<MidiInputData>(ctx, this_val);
    if (!d || argc < 1) return JS_UNDEFINED;
    JS_FreeValue(ctx, d->pitchBendCallback);
    d->pitchBendCallback = JS_DupValue(ctx, argv[0]);
    JSContext* jsCtx = ctx;
    d->midi->onPitchBend([jsCtx, d](uint8_t channel, int16_t value) {
        JSValue args[2] = { JS_NewInt32(jsCtx, channel), JS_NewInt32(jsCtx, value) };
        JSValue ret = JS_Call(jsCtx, d->pitchBendCallback, JS_UNDEFINED, 2, args);
        JS_FreeValue(jsCtx, args[0]); JS_FreeValue(jsCtx, args[1]);
        JS_FreeValue(jsCtx, ret);
    });
    return JS_UNDEFINED;
}

// Map a MidiEvent::Type to its string label (matches the typed event types in broaudio/midi_input.h).
static const char* midiEventTypeName(broaudio::MidiEvent::Type t) {
    switch (t) {
        case broaudio::MidiEvent::Type::NoteOn:          return "noteon";
        case broaudio::MidiEvent::Type::NoteOff:         return "noteoff";
        case broaudio::MidiEvent::Type::ControlChange:   return "controlchange";
        case broaudio::MidiEvent::Type::PitchBend:       return "pitchbend";
        case broaudio::MidiEvent::Type::ProgramChange:   return "programchange";
        case broaudio::MidiEvent::Type::Aftertouch:      return "aftertouch";
        case broaudio::MidiEvent::Type::ChannelPressure: return "channelpressure";
    }
    return "unknown";
}

static JSValue js_midi_onRawEvent(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<MidiInputData>(ctx, this_val);
    if (!d || argc < 1) return JS_UNDEFINED;
    JS_FreeValue(ctx, d->rawCallback);
    d->rawCallback = JS_DupValue(ctx, argv[0]);
    JSContext* jsCtx = ctx;
    d->midi->onRawEvent([jsCtx, d](const broaudio::MidiEvent& ev) {
        JSValue obj = JS_NewObject(jsCtx);
        JS_SetPropertyStr(jsCtx, obj, "type", JS_NewString(jsCtx, midiEventTypeName(ev.type)));
        JS_SetPropertyStr(jsCtx, obj, "channel", JS_NewInt32(jsCtx, ev.channel));
        JS_SetPropertyStr(jsCtx, obj, "data1", JS_NewInt32(jsCtx, ev.data1));
        JS_SetPropertyStr(jsCtx, obj, "data2", JS_NewInt32(jsCtx, ev.data2));
        JS_SetPropertyStr(jsCtx, obj, "pitchBend", JS_NewInt32(jsCtx, ev.pitchBend));
        JS_SetPropertyStr(jsCtx, obj, "timestamp", JS_NewFloat64(jsCtx, ev.timestamp));
        JSValue ret = JS_Call(jsCtx, d->rawCallback, JS_UNDEFINED, 1, &obj);
        JS_FreeValue(jsCtx, obj);
        JS_FreeValue(jsCtx, ret);
    });
    return JS_UNDEFINED;
}

// --- Sequence raw methods ---

static JSValue js_seq_addAutomationLane(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<SequenceData>(ctx, this_val);
    if (!d || argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_UNDEFINED;

    JSValue cbRef = JS_DupValue(ctx, argv[0]);
    d->automationCallbacks.push_back(cbRef);

    JSContext* jsCtx = d->ctx;
    int laneIdx = d->seq->addAutomationLane([jsCtx, cbRef](float value) {
        JSValue arg = JS_NewFloat64(jsCtx, value);
        JSValue ret = JS_Call(jsCtx, cbRef, JS_UNDEFINED, 1, &arg);
        JS_FreeValue(jsCtx, arg);
        JS_FreeValue(jsCtx, ret);
    });
    return JS_NewInt32(ctx, laneIdx);
}

static JSValue js_seq_removeAutomationLane(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<SequenceData>(ctx, this_val);
    if (!d || argc < 1) return JS_UNDEFINED;
    int idx; JS_ToInt32(ctx, &idx, argv[0]);
    if (idx >= 0 && idx < static_cast<int>(d->automationCallbacks.size())) {
        JS_FreeValue(ctx, d->automationCallbacks[idx]);
        d->automationCallbacks.erase(d->automationCallbacks.begin() + idx);
    }
    d->seq->removeAutomationLane(idx);
    return JS_UNDEFINED;
}

static JSValue js_seq_clearAutomationLanes(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* d = qjsbind::unwrap<SequenceData>(ctx, this_val);
    if (!d) return JS_UNDEFINED;
    d->seq->clearAutomationLanes();
    for (auto& cb : d->automationCallbacks)
        JS_FreeValue(ctx, cb);
    d->automationCallbacks.clear();
    return JS_UNDEFINED;
}

// --- AudioContext raw factory methods (create objects of other types) ---

static JSValue js_audioctx_createOscillator(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d) return JS_UNDEFINED;

    int voiceId = d->engine->createVoice();
    auto* oscData = new OscNodeData{d->engine, voiceId, "sine"};
    JSValue obj = qjsbind::wrap<OscNodeData>(ctx, oscData);

    // Helper to create AudioParam
    auto makeParam = [&](AudioParamData::Target target, float initial) {
        return qjsbind::wrap<AudioParamData>(ctx, new AudioParamData{d->engine, voiceId, target, initial});
    };

    JS_SetPropertyStr(ctx, obj, "frequency", makeParam(AudioParamData::Target::Frequency, 440.0f));
    JS_SetPropertyStr(ctx, obj, "pan", makeParam(AudioParamData::Target::Pan, 0.0f));
    JS_SetPropertyStr(ctx, obj, "attack", makeParam(AudioParamData::Target::Attack, 0.01f));
    JS_SetPropertyStr(ctx, obj, "decay", makeParam(AudioParamData::Target::Decay, 0.1f));
    JS_SetPropertyStr(ctx, obj, "sustain", makeParam(AudioParamData::Target::SustainLevel, 1.0f));
    JS_SetPropertyStr(ctx, obj, "release", makeParam(AudioParamData::Target::Release, 0.04f));
    JS_SetPropertyStr(ctx, obj, "pitchBend", makeParam(AudioParamData::Target::PitchBend, 0.0f));

    return obj;
}

static JSValue js_audioctx_createGain(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d) return JS_UNDEFINED;

    JSValue obj = qjsbind::wrap<GainNodeData>(ctx, new GainNodeData{d->engine});

    JSValue gainParam = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, gainParam, "value", JS_NewFloat64(ctx, 1.0));
    JS_SetPropertyStr(ctx, obj, "gain", gainParam);

    return obj;
}

static JSValue js_audioctx_createAnalyser(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d) return JS_UNDEFINED;
    return qjsbind::wrap<AnalyserNodeData>(ctx, new AnalyserNodeData{d->engine});
}

static JSValue js_audioctx_createBiquadFilter(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d) return JS_UNDEFINED;

    int slot = d->engine->allocateFilterSlot();
    if (slot < 0) return JS_ThrowInternalError(ctx, "No filter slots available");

    JSValue obj = qjsbind::wrap<BiquadFilterNodeData>(ctx, new BiquadFilterNodeData{d->engine, slot});

    auto makeParam = [&](AudioParamData::Target target, float initial) {
        return qjsbind::wrap<AudioParamData>(ctx, new AudioParamData{d->engine, slot, target, initial});
    };

    JS_SetPropertyStr(ctx, obj, "__type", JS_NewString(ctx, "lowpass"));
    JS_SetPropertyStr(ctx, obj, "frequency", makeParam(AudioParamData::Target::FilterFrequency, 1000.0f));
    JS_SetPropertyStr(ctx, obj, "Q", makeParam(AudioParamData::Target::FilterQ, 1.0f));
    JS_SetPropertyStr(ctx, obj, "gain", makeParam(AudioParamData::Target::FilterGain, 0.0f));

    return obj;
}

static JSValue js_audioctx_createVoiceAllocator(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d) return JS_UNDEFINED;
    int maxVoices = 16;
    if (argc >= 1) JS_ToInt32(ctx, &maxVoices, argv[0]);

    auto* va = new VoiceAllocatorData{
        d->engine,
        ::std::make_unique<broaudio::VoiceAllocator>(*d->engine, maxVoices),
        ctx,
        JS_UNDEFINED,
        JS_UNDEFINED
    };
    return qjsbind::wrap<VoiceAllocatorData>(ctx, va);
}

static JSValue js_audioctx_getModMatrix(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d) return JS_UNDEFINED;
    return qjsbind::wrap<ModMatrixData>(ctx, new ModMatrixData{d->engine, &d->engine->modMatrix()});
}

static JSValue js_audioctx_createMidiInput(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d) return JS_UNDEFINED;

    auto* mi = new MidiInputData{
        d->engine,
        ::std::make_unique<broaudio::MidiInput>(*d->engine),
        ctx,
        JS_UNDEFINED,
        JS_UNDEFINED
    };
    return qjsbind::wrap<MidiInputData>(ctx, mi);
}

static JSValue js_audioctx_createSequence(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d || argc < 1) return JS_UNDEFINED;

    auto* va = qjsbind::unwrap<VoiceAllocatorData>(ctx, argv[0]);
    if (!va) return JS_ThrowTypeError(ctx, "Expected VoiceAllocator argument");

    auto* sd = new SequenceData{
        ::std::make_unique<broaudio::Sequence>(*va->allocator),
        ctx,
        {}
    };
    return qjsbind::wrap<SequenceData>(ctx, sd);
}

static JSValue js_audioctx_createMediaStreamSource(JSContext* ctx, JSValueConst this_val,
                                                    int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d || argc < 1) return JS_UNDEFINED;

    auto* ms = qjsbind::unwrap<MicStreamData>(ctx, argv[0]);
    if (!ms) return JS_ThrowTypeError(ctx, "Expected MediaStream argument");

    return qjsbind::wrap<MicSourceData>(ctx, new MicSourceData{d->engine});
}

// --- AudioContext complex raw methods ---

static JSValue js_audioctx_setBusEffectOrder(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d || argc < 2) return JS_UNDEFINED;
    int busId; JS_ToInt32(ctx, &busId, argv[0]);

    JSValue lenVal = JS_GetPropertyStr(ctx, argv[1], "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, lenVal);
    JS_FreeValue(ctx, lenVal);

    if (len <= 0 || len > 7) return JS_UNDEFINED;

    broaudio::EffectSlot order[7];
    for (int32_t i = 0; i < len; i++) {
        JSValue elem = JS_GetPropertyUint32(ctx, argv[1], i);
        const char* str = JS_ToCString(ctx, elem);
        if (str) {
            if (strcmp(str, "filter") == 0) order[i] = broaudio::EffectSlot::Filter;
            else if (strcmp(str, "delay") == 0) order[i] = broaudio::EffectSlot::Delay;
            else if (strcmp(str, "compressor") == 0) order[i] = broaudio::EffectSlot::Compressor;
            else if (strcmp(str, "chorus") == 0) order[i] = broaudio::EffectSlot::Chorus;
            else if (strcmp(str, "reverb") == 0) order[i] = broaudio::EffectSlot::Reverb;
            else if (strcmp(str, "equalizer") == 0) order[i] = broaudio::EffectSlot::Equalizer;
            else if (strcmp(str, "distortion") == 0) order[i] = broaudio::EffectSlot::Distortion;
            else order[i] = static_cast<broaudio::EffectSlot>(i);
            JS_FreeCString(ctx, str);
        }
        JS_FreeValue(ctx, elem);
    }

    d->engine->setBusEffectOrder(busId, order, len);
    return JS_UNDEFINED;
}

static JSValue js_audioctx_createWavetable(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d || argc < 1) return JS_UNDEFINED;
    const char* type = JS_ToCString(ctx, argv[0]);
    if (!type) return JS_UNDEFINED;

    ::std::shared_ptr<broaudio::WavetableBank> bank;
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
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
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

static JSValue js_audioctx_setVoiceWavetable(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d || argc < 2) return JS_UNDEFINED;
    int voiceId, wtId; JS_ToInt32(ctx, &voiceId, argv[0]); JS_ToInt32(ctx, &wtId, argv[1]);
    auto it = s_wavetables.find(wtId);
    if (it != s_wavetables.end()) {
        d->engine->setVoiceWavetable(voiceId, it->second);
        d->engine->setWaveform(voiceId, broaudio::Waveform::Wavetable);
    }
    return JS_UNDEFINED;
}

static JSValue js_audioctx_getSpectrum(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
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
            auto spectrum = d->engine->getSpectrum(numBins);
            if (!spectrum.empty()) {
                ::std::memcpy(ptr + byteOff, spectrum.data(), ::std::min(spectrum.size() * sizeof(float), viewLen));
            }
        }
        JS_FreeValue(ctx, abuf);
    }
    return arr;
}

static JSValue js_audioctx_processEffectsOffline(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d || argc < 2) return JS_UNDEFINED;

    int busId; JS_ToInt32(ctx, &busId, argv[0]);

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
            ::std::memcpy(ptr + byteOff, result.data(), result.size() * sizeof(float));
        }
        JS_FreeValue(ctx, abuf);
    }
    return arr;
}

static JSValue js_audioctx_setVoiceWaveform(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d || argc < 2) return JS_UNDEFINED;
    int voiceId; JS_ToInt32(ctx, &voiceId, argv[0]);
    const char* s = JS_ToCString(ctx, argv[1]);
    if (s) { d->engine->setWaveform(voiceId, parseWaveform(s)); JS_FreeCString(ctx, s); }
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setVoiceFilterType(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d || argc < 2) return JS_UNDEFINED;
    int voiceId; JS_ToInt32(ctx, &voiceId, argv[0]);
    const char* s = JS_ToCString(ctx, argv[1]);
    if (s) { d->engine->setVoiceFilterType(voiceId, parseFilterType(s)); JS_FreeCString(ctx, s); }
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setVoiceSpatialDistanceModel(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d || argc < 2) return JS_UNDEFINED;
    int voiceId; JS_ToInt32(ctx, &voiceId, argv[0]);
    const char* s = JS_ToCString(ctx, argv[1]);
    if (s) { d->engine->setVoiceSpatialDistanceModel(voiceId, parseDistanceModel(s)); JS_FreeCString(ctx, s); }
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setPlaybackSpatialDistanceModel(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d || argc < 2) return JS_UNDEFINED;
    int id; JS_ToInt32(ctx, &id, argv[0]);
    const char* s = JS_ToCString(ctx, argv[1]);
    if (s) { d->engine->setPlaybackSpatialDistanceModel(id, parseDistanceModel(s)); JS_FreeCString(ctx, s); }
    return JS_UNDEFINED;
}

static JSValue js_audioctx_setBusFilterType(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d || argc < 3) return JS_UNDEFINED;
    int busId, slot; JS_ToInt32(ctx, &busId, argv[0]); JS_ToInt32(ctx, &slot, argv[1]);
    const char* s = JS_ToCString(ctx, argv[2]);
    if (s) { d->engine->setBusFilterType(busId, slot, parseFilterType(s)); JS_FreeCString(ctx, s); }
    return JS_UNDEFINED;
}

static JSValue js_audioctx_stopRecording(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
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
            ::std::memcpy(ptr + byteOff, buf.data(), count * sizeof(float));
        }
        JS_FreeValue(ctx, abuf);
    }
    return arr;
}

static JSValue js_audioctx_createClip(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d || argc < 1) return JS_UNDEFINED;

    size_t len = 0;
    uint8_t* raw = getTypedArrayPtr(ctx, argv[0], len);
    if (!raw) return JS_ThrowTypeError(ctx, "Expected Float32Array argument");

    int numSamples = static_cast<int>(len / sizeof(float));
    int channels = 1;
    if (argc >= 2) JS_ToInt32(ctx, &channels, argv[1]);

    // Optional 3rd arg: the PCM's source sample rate. When it differs from the
    // engine rate, resample so the clip plays at the right pitch/speed (mirrors
    // decodeAudioData). Without it, the samples are assumed to be at engine rate.
    const float* samples = reinterpret_cast<float*>(raw);
    ::std::vector<float> resampled;
    if (argc >= 3 && JS_IsNumber(argv[2])) {
        int srcRate = 0; JS_ToInt32(ctx, &srcRate, argv[2]);
        const int engRate = d->engine->sampleRate();
        if (srcRate > 0 && srcRate != engRate && channels > 0) {
            resampled = broaudio::resample(samples, numSamples / channels,
                                           channels, srcRate, engRate);
            if (!resampled.empty()) {
                samples = resampled.data();
                numSamples = static_cast<int>(resampled.size());
            }
        }
    }
    int id = d->engine->createClip(samples, numSamples, channels);
    return JS_NewInt32(ctx, id);
}

static JSValue js_audioctx_getClipWaveform(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
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
            auto wf = d->engine->getClipWaveform(clipId, numBins);
            if (!wf.empty()) {
                ::std::memcpy(ptr + byteOff, wf.data(), ::std::min(wf.size() * sizeof(float), viewLen));
            }
        JS_FreeValue(ctx, abuf);
    }
    return arr;
}

static JSValue js_audioctx_createClipFromFile(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d || argc < 1) return JS_UNDEFINED;
    const char* path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_UNDEFINED;
    ::std::string resolved = resolveAssetPath(path);
    JS_FreeCString(ctx, path);
    int id = d->engine->createClipFromFile(resolved.c_str());
    return JS_NewInt32(ctx, id);
}

// createClipFromFileAsync(path) -> Promise<clipId>. Decode + resample run on
// a background thread (async-job runner); the promise resolves with the clip
// id or rejects with an Error carrying the actionable decode message from
// broaudio (corrupt stream, size cap, unsupported codec).
static JSValue js_audioctx_createClipFromFileAsync(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d || argc < 1)
        return JS_ThrowTypeError(ctx, "createClipFromFileAsync: file path required");
    const char* cpath = JS_ToCString(ctx, argv[0]);
    if (!cpath) return JS_EXCEPTION;
    // Resolve on the JS thread u2014 the decode runs on a worker that has no
    // notion of the app directory or the mount table.
    ::std::string pathCopy = resolveAssetPath(cpath);
    JS_FreeCString(ctx, cpath);

    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise)) return promise;

    struct ClipLoadState {
        broaudio::Engine* engine = nullptr;
        ::std::string path;
        ::std::string error;   // written by the work thread, read in done()
        int clipId = -1;
        JSValue resolve = JS_UNDEFINED;
        JSValue reject = JS_UNDEFINED;
    };
    auto st = ::std::make_shared<ClipLoadState>();
    st->engine  = d->engine;
    st->path    = ::std::move(pathCopy);
    st->resolve = resolving[0];
    st->reject  = resolving[1];

    auto work = [st](const ::std::atomic<bool>&) {
        // createClip locks the control-plane media mutex u2014 safe off-thread.
        st->clipId = st->engine->createClipFromFileEx(st->path.c_str(), &st->error);
    };
    auto done = [st](JSContext* c, bool cancelled, const ::std::string& jobError) {
        if (st->clipId >= 0 && !cancelled && jobError.empty()) {
            JSValue v = JS_NewInt32(c, st->clipId);
            JSValue r = JS_Call(c, st->resolve, JS_UNDEFINED, 1, &v);
            if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
            JS_FreeValue(c, r);
            JS_FreeValue(c, v);
        } else {
            ::std::string msg = !jobError.empty() ? jobError
                            : !st->error.empty() ? st->error
                            : cancelled          ? "load cancelled"
                                                 : "failed to load audio file";
            msg = st->path + ": " + msg;
            JSValue err = JS_NewError(c);
            JS_DefinePropertyValueStr(c, err, "message", JS_NewString(c, msg.c_str()),
                                      JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
            JSValue r = JS_Call(c, st->reject, JS_UNDEFINED, 1, &err);
            if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
            JS_FreeValue(c, r);
            JS_FreeValue(c, err);
        }
        JS_FreeValue(c, st->resolve);
        JS_FreeValue(c, st->reject);
    };
    JSValue handle = launchAsyncJob(ctx, ::std::move(work), nullptr, ::std::move(done));
    JS_FreeValue(ctx, handle);  // the promise is the JS-facing handle
    return promise;
}

// createStreamFromFile(path, options?) -> playbackId. Disk-streamed playback:
// the file decodes incrementally on a broaudio worker thread into a ring the
// mixer consumes u2014 large files never sit in RAM. Throws with the decode error
// on failure. options: { ringFrames, prebufferFrames, loop, gain }.
static JSValue js_audioctx_createStreamFromFile(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d || argc < 1)
        return JS_ThrowTypeError(ctx, "createStreamFromFile: file path required");
    const char* path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;
    ::std::string resolvedPath = resolveAssetPath(path);
    JS_FreeCString(ctx, path);

    broaudio::FileStreamOptions opts;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue v;
        int32_t i32 = 0;
        double f64 = 0.0;
        v = JS_GetPropertyStr(ctx, argv[1], "ringFrames");
        if (!JS_IsUndefined(v) && JS_ToInt32(ctx, &i32, v) == 0) opts.ringFrames = i32;
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "prebufferFrames");
        if (!JS_IsUndefined(v) && JS_ToInt32(ctx, &i32, v) == 0) opts.prebufferFrames = i32;
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "loop");
        if (!JS_IsUndefined(v)) opts.loop = JS_ToBool(ctx, v) > 0;
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "gain");
        if (!JS_IsUndefined(v) && JS_ToFloat64(ctx, &f64, v) == 0)
            opts.gain = static_cast<float>(f64);
        JS_FreeValue(ctx, v);
    }

    ::std::string err;
    int id = d->engine->createStreamFromFile(resolvedPath.c_str(), opts, &err);
    if (id < 0)
        return JS_ThrowInternalError(ctx, "createStreamFromFile: %s",
                                     err.empty() ? "failed to open stream" : err.c_str());
    return JS_NewInt32(ctx, id);
}

// getStreamStats(playbackId) -> { decodedFrames, playedFrames, bufferedFrames,
// underrunFrames, finished } or null when the id is not a streaming playback.
static JSValue js_audioctx_getStreamStats(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d || argc < 1) return JS_NULL;
    int32_t id = -1;
    if (JS_ToInt32(ctx, &id, argv[0]) != 0) return JS_EXCEPTION;

    broaudio::StreamStats s = d->engine->getStreamStats(id);
    if (!s.valid) return JS_NULL;

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "decodedFrames",
                      JS_NewFloat64(ctx, static_cast<double>(s.decodedFrames)));
    JS_SetPropertyStr(ctx, obj, "playedFrames",
                      JS_NewFloat64(ctx, static_cast<double>(s.playedFrames)));
    JS_SetPropertyStr(ctx, obj, "bufferedFrames",
                      JS_NewFloat64(ctx, static_cast<double>(s.bufferedFrames)));
    JS_SetPropertyStr(ctx, obj, "underrunFrames",
                      JS_NewFloat64(ctx, static_cast<double>(s.underrunFrames)));
    JS_SetPropertyStr(ctx, obj, "finished", JS_NewBool(ctx, s.finished));
    return obj;
}

static JSValue js_audioctx_decodeAudioData(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d || argc < 1) return JS_NULL;

    uint8_t* inputPtr = nullptr;
    size_t inputLen = 0;

    inputPtr = getTypedArrayPtr(ctx, argv[0], inputLen);
    if (!inputPtr) {
        inputPtr = JS_GetArrayBuffer(ctx, &inputLen, argv[0]);
    }
    if (!inputPtr || inputLen == 0) return JS_NULL;

    broaudio::AudioFileData data = broaudio::loadAudioFileFromMemory(inputPtr, inputLen);
    if (!data.valid()) {
        if (!data.error.empty())
            LOG_ERROR("decodeAudioData: %s", data.error.c_str());
        return JS_NULL;
    }

    const float* outSamples = data.samples.data();
    int outFrames = data.numFrames;
    int outChannels = data.channels;
    int outRate = data.sampleRate;
    ::std::vector<float> resampled;

    if (data.sampleRate != d->engine->sampleRate()) {
        resampled = broaudio::resample(data.samples.data(), data.numFrames,
                                       data.channels, data.sampleRate, d->engine->sampleRate());
        if (!resampled.empty()) {
            outSamples = resampled.data();
            outFrames = static_cast<int>(resampled.size()) / outChannels;
            outRate = d->engine->sampleRate();
        }
    }

    JSValue result = JS_NewObject(ctx);
    int totalFloats = outFrames * outChannels;

    JSValue lenVal = JS_NewInt32(ctx, totalFloats);
    JSValue arr = JS_NewTypedArray(ctx, 1, &lenVal, JS_TYPED_ARRAY_FLOAT32);
    JS_FreeValue(ctx, lenVal);

    if (!JS_IsException(arr)) {
        size_t byteOff = 0, viewLen = 0;
        JSValue abuf = JS_GetTypedArrayBuffer(ctx, arr, &byteOff, &viewLen, nullptr);
        if (!JS_IsException(abuf)) {
            size_t abufLen = 0;
            uint8_t* ptr = JS_GetArrayBuffer(ctx, &abufLen, abuf);
            if (ptr) {
                ::std::memcpy(ptr + byteOff, outSamples, totalFloats * sizeof(float));
            }
            JS_FreeValue(ctx, abuf);
        }
    }

    JS_SetPropertyStr(ctx, result, "samples", arr);
    JS_SetPropertyStr(ctx, result, "channels", JS_NewInt32(ctx, outChannels));
    JS_SetPropertyStr(ctx, result, "sampleRate", JS_NewInt32(ctx, outRate));
    JS_SetPropertyStr(ctx, result, "numFrames", JS_NewInt32(ctx, outFrames));

    return result;
}

static JSValue js_audioctx_decodeAudioFile(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d || argc < 1) return JS_NULL;

    const char* rawPath = JS_ToCString(ctx, argv[0]);
    if (!rawPath) return JS_NULL;
    ::std::string path = resolveAssetPath(rawPath);
    JS_FreeCString(ctx, rawPath);

    broaudio::AudioFileData data = broaudio::loadAudioFile(path.c_str());
    if (!data.valid()) {
        if (!data.error.empty())
            LOG_ERROR("decodeAudioFile: %s: %s", path.c_str(), data.error.c_str());
        return JS_NULL;
    }

    const float* outSamples = data.samples.data();
    int outFrames = data.numFrames;
    int outChannels = data.channels;
    int outRate = data.sampleRate;
    ::std::vector<float> resampled;

    if (data.sampleRate != d->engine->sampleRate()) {
        resampled = broaudio::resample(data.samples.data(), data.numFrames,
                                       data.channels, data.sampleRate, d->engine->sampleRate());
        if (!resampled.empty()) {
            outSamples = resampled.data();
            outFrames = static_cast<int>(resampled.size()) / outChannels;
            outRate = d->engine->sampleRate();
        }
    }

    JSValue result = JS_NewObject(ctx);
    int totalFloats = outFrames * outChannels;

    JSValue lenVal = JS_NewInt32(ctx, totalFloats);
    JSValue arr = JS_NewTypedArray(ctx, 1, &lenVal, JS_TYPED_ARRAY_FLOAT32);
    JS_FreeValue(ctx, lenVal);

    if (!JS_IsException(arr)) {
        size_t byteOff = 0, viewLen = 0;
        JSValue abuf = JS_GetTypedArrayBuffer(ctx, arr, &byteOff, &viewLen, nullptr);
        if (!JS_IsException(abuf)) {
            size_t abufLen = 0;
            uint8_t* ptr = JS_GetArrayBuffer(ctx, &abufLen, abuf);
            if (ptr) {
                ::std::memcpy(ptr + byteOff, outSamples, totalFloats * sizeof(float));
            }
            JS_FreeValue(ctx, abuf);
        }
    }

    JS_SetPropertyStr(ctx, result, "samples", arr);
    JS_SetPropertyStr(ctx, result, "channels", JS_NewInt32(ctx, outChannels));
    JS_SetPropertyStr(ctx, result, "sampleRate", JS_NewInt32(ctx, outRate));
    JS_SetPropertyStr(ctx, result, "numFrames", JS_NewInt32(ctx, outFrames));

    return result;
}

static JSValue js_audioctx_saveWav(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d || argc < 4) return JS_FALSE;

    const char* path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_FALSE;

    size_t len = 0;
    uint8_t* raw = getTypedArrayPtr(ctx, argv[1], len);
    if (!raw) {
        JS_FreeCString(ctx, path);
        return JS_ThrowTypeError(ctx, "Expected Float32Array as second argument");
    }

    int channels, sr;
    JS_ToInt32(ctx, &channels, argv[2]);
    JS_ToInt32(ctx, &sr, argv[3]);

    int numFrames = static_cast<int>(len / sizeof(float)) / ::std::max(channels, 1);
    ::std::string resolved = resolveAssetWritePath(path);
    bool ok = broaudio::saveWav(resolved.c_str(), reinterpret_cast<float*>(raw),
                                numFrames, channels, sr);
    JS_FreeCString(ctx, path);
    return JS_NewBool(ctx, ok);
}

static JSValue js_audioctx_playClip(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d || argc < 1) return JS_UNDEFINED;
    int clipId; JS_ToInt32(ctx, &clipId, argv[0]);
    float gain = 1.0f;
    bool loop = false;
    if (argc >= 2) { double v; JS_ToFloat64(ctx, &v, argv[1]); gain = static_cast<float>(v); }
    if (argc >= 3) { loop = JS_ToBool(ctx, argv[2]); }
    // Optional 4th arg: a sample-accurate start time (engine seconds, from
    // ctx.currentTime). Streaming uses it to queue chunks on the audio clock so
    // they join gaplessly u2014 no main-thread setTimeout jitter or clock drift. A
    // `when` at/before now plays immediately, same as the 3-arg form.
    if (argc >= 4 && JS_IsNumber(argv[3])) {
        double when; JS_ToFloat64(ctx, &when, argv[3]);
        return JS_NewInt32(ctx, d->engine->playClipAt(clipId, when, gain, loop));
    }
    return JS_NewInt32(ctx, d->engine->playClip(clipId, gain, loop));
}

// createStream(channels=1, ringFrames=0) -> playbackId for a live PCM source.
// Feed it with pushStreamSamples; spatialize with setPlaybackSpatial*; works with
// every setPlayback* method. Use for voice chat / network audio: a continuous
// stream plays click-free without per-frame clip churn.
static JSValue js_audioctx_createStream(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d) return JS_NewInt32(ctx, -1);
    int channels = 1;
    if (argc >= 1) JS_ToInt32(ctx, &channels, argv[0]);
    int ringFrames = 0;
    if (argc >= 2) JS_ToInt32(ctx, &ringFrames, argv[1]);
    return JS_NewInt32(ctx, d->engine->createStream(channels, ringFrames));
}

// pushStreamSamples(streamId, Float32Array) -> frames written. Samples must be
// interleaved at the engine sample rate (resample on the producer side, e.g.
// decode voice directly at ctx.sampleRate). Single-producer.
static JSValue js_audioctx_pushStreamSamples(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d || argc < 2) return JS_NewInt32(ctx, 0);
    int id; JS_ToInt32(ctx, &id, argv[0]);
    size_t len = 0;
    uint8_t* raw = getTypedArrayPtr(ctx, argv[1], len);
    if (!raw) return JS_ThrowTypeError(ctx, "Expected Float32Array samples");
    int numSamples = static_cast<int>(len / sizeof(float));
    return JS_NewInt32(ctx, d->engine->pushStreamSamples(id, reinterpret_cast<float*>(raw), numSamples));
}

static JSValue js_audioctx_closeStream(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d || argc < 1) return JS_UNDEFINED;
    int id; JS_ToInt32(ctx, &id, argv[0]);
    d->engine->closeStream(id);
    return JS_UNDEFINED;
}

// --- getUserMedia (standalone, no class) ---

static JSValue js_getUserMedia(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv) {
    if (!s_audioEngine) return JS_ThrowInternalError(ctx, "Audio not initialized");

    if (!s_audioEngine->startMicCapture()) {
        JSValue err = JS_NewError(ctx);
        JS_SetPropertyStr(ctx, err, "message",
                          JS_NewString(ctx, "Failed to access microphone"));
        return qjsbind::make_rejected_promise(ctx, err);
    }

    JSValue stream = qjsbind::wrap<MicStreamData>(ctx, new MicStreamData{s_audioEngine});
    return qjsbind::make_resolved_promise(ctx, stream);
}

// --- Sequence raw methods that need string parsing ---

static JSValue js_seq_setAutomationInterpMode(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<SequenceData>(ctx, this_val);
    if (!d || argc < 2) return JS_UNDEFINED;
    int laneIdx; JS_ToInt32(ctx, &laneIdx, argv[0]);
    const char* s = JS_ToCString(ctx, argv[1]);
    if (!s) return JS_UNDEFINED;
    broaudio::InterpMode mode = broaudio::InterpMode::Linear;
    if (strcmp(s, "step") == 0) mode = broaudio::InterpMode::Step;
    else if (strcmp(s, "smooth") == 0) mode = broaudio::InterpMode::Smooth;
    JS_FreeCString(ctx, s);
    if (laneIdx >= 0 && laneIdx < d->seq->automationLaneCount())
        d->seq->automationLane(laneIdx).setInterpMode(mode);
    return JS_UNDEFINED;
}

// --- ModMatrix raw methods that need string parsing ---

static JSValue js_mod_setLfoShape(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<ModMatrixData>(ctx, this_val);
    if (!d || argc < 2) return JS_UNDEFINED;
    int idx; JS_ToInt32(ctx, &idx, argv[0]);
    const char* s = JS_ToCString(ctx, argv[1]);
    if (s) { d->modMatrix->setLfoShape(idx, parseLfoShape(s)); JS_FreeCString(ctx, s); }
    return JS_UNDEFINED;
}

// --- VoiceAllocator raw method for setStealPolicy (string parsing) ---

static JSValue js_va_setStealPolicy(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<VoiceAllocatorData>(ctx, this_val);
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

// ---------------------------------------------------------------------------
// Preset (de)serialization u2014 JS object <-> broaudio preset structs
// ---------------------------------------------------------------------------
//
// broaudio's toJson/fromJson operate on plain preset structs (VoicePreset,
// BusPreset, ModPreset, EnginePreset). The engine has no getter that reads its
// live state back into a preset, so the JS surface marshals a plain JS object
// into a preset struct (for toJson / applyXPreset) and a struct back into a JS
// object (for fromJson). Enums round-trip as the same lowercase strings the
// rest of this file already uses.

// --- JS object field readers (return default when absent) ---

static double jsObjNum(JSContext* ctx, JSValueConst o, const char* k, double def) {
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); return def; }
    double d = def; JS_ToFloat64(ctx, &d, v); JS_FreeValue(ctx, v); return d;
}
static bool jsObjBool(JSContext* ctx, JSValueConst o, const char* k, bool def) {
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); return def; }
    bool b = JS_ToBool(ctx, v) != 0; JS_FreeValue(ctx, v); return b;
}
static int jsObjInt(JSContext* ctx, JSValueConst o, const char* k, int def) {
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); return def; }
    int i = def; JS_ToInt32(ctx, &i, v); JS_FreeValue(ctx, v); return i;
}
static ::std::string jsObjStr(JSContext* ctx, JSValueConst o, const char* k, const char* def) {
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    ::std::string out = def;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        const char* s = JS_ToCString(ctx, v);
        if (s) { out = s; JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, v);
    return out;
}
static uint32_t jsArrayLen(JSContext* ctx, JSValueConst v) {
    JSValue l = JS_GetPropertyStr(ctx, v, "length");
    uint32_t n = 0; JS_ToUint32(ctx, &n, l); JS_FreeValue(ctx, l); return n;
}

// --- Enum <-> string (reverse of the parse* helpers above) ---

static const char* waveformToString(broaudio::Waveform w) {
    switch (w) {
        case broaudio::Waveform::Square:     return "square";
        case broaudio::Waveform::Sawtooth:   return "sawtooth";
        case broaudio::Waveform::Triangle:   return "triangle";
        case broaudio::Waveform::Wavetable:  return "wavetable";
        case broaudio::Waveform::WhiteNoise: return "whitenoise";
        case broaudio::Waveform::PinkNoise:  return "pinknoise";
        case broaudio::Waveform::BrownNoise: return "brownnoise";
        default:                             return "sine";
    }
}
static const char* filterTypeToString(broaudio::BiquadFilter::Type t) {
    switch (t) {
        case broaudio::BiquadFilter::Type::Highpass:  return "highpass";
        case broaudio::BiquadFilter::Type::Bandpass:  return "bandpass";
        case broaudio::BiquadFilter::Type::Notch:     return "notch";
        case broaudio::BiquadFilter::Type::Allpass:   return "allpass";
        case broaudio::BiquadFilter::Type::Peaking:   return "peaking";
        case broaudio::BiquadFilter::Type::Lowshelf:  return "lowshelf";
        case broaudio::BiquadFilter::Type::Highshelf: return "highshelf";
        default:                                      return "lowpass";
    }
}
static broaudio::DistortionMode parseDistortionMode(const ::std::string& m) {
    if (m == "hardclip") return broaudio::DistortionMode::HardClip;
    if (m == "foldback") return broaudio::DistortionMode::Foldback;
    if (m == "bitcrush") return broaudio::DistortionMode::Bitcrush;
    return broaudio::DistortionMode::SoftClip;
}
static const char* distortionModeToString(broaudio::DistortionMode m) {
    switch (m) {
        case broaudio::DistortionMode::HardClip: return "hardclip";
        case broaudio::DistortionMode::Foldback: return "foldback";
        case broaudio::DistortionMode::Bitcrush: return "bitcrush";
        default:                                 return "softclip";
    }
}
static broaudio::EffectSlot parseEffectSlot(const ::std::string& s, broaudio::EffectSlot def) {
    if (s == "filter")     return broaudio::EffectSlot::Filter;
    if (s == "delay")      return broaudio::EffectSlot::Delay;
    if (s == "compressor") return broaudio::EffectSlot::Compressor;
    if (s == "chorus")     return broaudio::EffectSlot::Chorus;
    if (s == "reverb")     return broaudio::EffectSlot::Reverb;
    if (s == "equalizer")  return broaudio::EffectSlot::Equalizer;
    if (s == "distortion") return broaudio::EffectSlot::Distortion;
    return def;
}
static const char* effectSlotToString(broaudio::EffectSlot s) {
    switch (s) {
        case broaudio::EffectSlot::Delay:      return "delay";
        case broaudio::EffectSlot::Compressor: return "compressor";
        case broaudio::EffectSlot::Chorus:     return "chorus";
        case broaudio::EffectSlot::Reverb:     return "reverb";
        case broaudio::EffectSlot::Equalizer:  return "equalizer";
        case broaudio::EffectSlot::Distortion: return "distortion";
        default:                               return "filter";
    }
}
static const char* lfoShapeToString(broaudio::LfoShape s) {
    switch (s) {
        case broaudio::LfoShape::Triangle:      return "triangle";
        case broaudio::LfoShape::Square:        return "square";
        case broaudio::LfoShape::SawUp:         return "sawup";
        case broaudio::LfoShape::SawDown:       return "sawdown";
        case broaudio::LfoShape::SampleAndHold: return "sampleandhold";
        default:                                return "sine";
    }
}
static const char* modSourceToString(broaudio::ModSource s) {
    switch (s) {
        case broaudio::ModSource::Lfo2:        return "lfo2";
        case broaudio::ModSource::Lfo3:        return "lfo3";
        case broaudio::ModSource::Lfo4:        return "lfo4";
        case broaudio::ModSource::Envelope:    return "envelope";
        case broaudio::ModSource::Velocity:    return "velocity";
        case broaudio::ModSource::KeyTracking: return "keytracking";
        case broaudio::ModSource::ModWheel:    return "modwheel";
        case broaudio::ModSource::Aftertouch:  return "aftertouch";
        default:                               return "lfo1";
    }
}
static const char* modDestToString(broaudio::ModDest d) {
    switch (d) {
        case broaudio::ModDest::Gain:       return "gain";
        case broaudio::ModDest::Pan:        return "pan";
        case broaudio::ModDest::FilterFreq: return "filterfreq";
        case broaudio::ModDest::FilterQ:    return "filterq";
        case broaudio::ModDest::PulseWidth: return "pulsewidth";
        case broaudio::ModDest::DelaySend:  return "delaysend";
        default:                            return "pitch";
    }
}

// --- JS object -> preset struct ---

static broaudio::VoicePreset jsToVoicePreset(JSContext* ctx, JSValueConst o) {
    broaudio::VoicePreset p;
    if (JS_IsUndefined(o) || JS_IsNull(o) || !JS_IsObject(o)) return p;
    p.waveform          = parseWaveform(jsObjStr(ctx, o, "waveform", "sine").c_str());
    p.frequency         = (float)jsObjNum(ctx, o, "frequency", p.frequency);
    p.gain              = (float)jsObjNum(ctx, o, "gain", p.gain);
    p.pan               = (float)jsObjNum(ctx, o, "pan", p.pan);
    p.pitchBend         = (float)jsObjNum(ctx, o, "pitchBend", p.pitchBend);
    p.attackTime        = (float)jsObjNum(ctx, o, "attackTime", p.attackTime);
    p.decayTime         = (float)jsObjNum(ctx, o, "decayTime", p.decayTime);
    p.sustainLevel      = (float)jsObjNum(ctx, o, "sustainLevel", p.sustainLevel);
    p.releaseTime       = (float)jsObjNum(ctx, o, "releaseTime", p.releaseTime);
    p.filterEnabled     = jsObjBool(ctx, o, "filterEnabled", p.filterEnabled);
    p.filterType        = parseFilterType(jsObjStr(ctx, o, "filterType", "lowpass").c_str());
    p.filterFreq        = (float)jsObjNum(ctx, o, "filterFreq", p.filterFreq);
    p.filterQ           = (float)jsObjNum(ctx, o, "filterQ", p.filterQ);
    p.unisonCount       = jsObjInt(ctx, o, "unisonCount", p.unisonCount);
    p.unisonDetune      = (float)jsObjNum(ctx, o, "unisonDetune", p.unisonDetune);
    p.unisonStereoWidth = (float)jsObjNum(ctx, o, "unisonStereoWidth", p.unisonStereoWidth);
    return p;
}
static broaudio::FilterPreset jsToFilterPreset(JSContext* ctx, JSValueConst o) {
    broaudio::FilterPreset p;
    if (!JS_IsObject(o)) return p;
    p.enabled   = jsObjBool(ctx, o, "enabled", p.enabled);
    p.type      = parseFilterType(jsObjStr(ctx, o, "type", "lowpass").c_str());
    p.frequency = (float)jsObjNum(ctx, o, "frequency", p.frequency);
    p.Q         = (float)jsObjNum(ctx, o, "Q", p.Q);
    p.gainDB    = (float)jsObjNum(ctx, o, "gainDB", p.gainDB);
    return p;
}
static broaudio::DelayPreset jsToDelayPreset(JSContext* ctx, JSValueConst o) {
    broaudio::DelayPreset p;
    if (!JS_IsObject(o)) return p;
    p.enabled  = jsObjBool(ctx, o, "enabled", p.enabled);
    p.time     = (float)jsObjNum(ctx, o, "time", p.time);
    p.feedback = (float)jsObjNum(ctx, o, "feedback", p.feedback);
    p.mix      = (float)jsObjNum(ctx, o, "mix", p.mix);
    return p;
}
static broaudio::CompressorPreset jsToCompressorPreset(JSContext* ctx, JSValueConst o) {
    broaudio::CompressorPreset p;
    if (!JS_IsObject(o)) return p;
    p.enabled   = jsObjBool(ctx, o, "enabled", p.enabled);
    p.threshold = (float)jsObjNum(ctx, o, "threshold", p.threshold);
    p.ratio     = (float)jsObjNum(ctx, o, "ratio", p.ratio);
    p.attackMs  = (float)jsObjNum(ctx, o, "attackMs", p.attackMs);
    p.releaseMs = (float)jsObjNum(ctx, o, "releaseMs", p.releaseMs);
    return p;
}
static broaudio::ReverbPreset jsToReverbPreset(JSContext* ctx, JSValueConst o) {
    broaudio::ReverbPreset p;
    if (!JS_IsObject(o)) return p;
    p.enabled  = jsObjBool(ctx, o, "enabled", p.enabled);
    p.roomSize = (float)jsObjNum(ctx, o, "roomSize", p.roomSize);
    p.damping  = (float)jsObjNum(ctx, o, "damping", p.damping);
    p.mix      = (float)jsObjNum(ctx, o, "mix", p.mix);
    return p;
}
static broaudio::ChorusPreset jsToChorusPreset(JSContext* ctx, JSValueConst o) {
    broaudio::ChorusPreset p;
    if (!JS_IsObject(o)) return p;
    p.enabled   = jsObjBool(ctx, o, "enabled", p.enabled);
    p.rate      = (float)jsObjNum(ctx, o, "rate", p.rate);
    p.depth     = (float)jsObjNum(ctx, o, "depth", p.depth);
    p.mix       = (float)jsObjNum(ctx, o, "mix", p.mix);
    p.feedback  = (float)jsObjNum(ctx, o, "feedback", p.feedback);
    p.baseDelay = (float)jsObjNum(ctx, o, "baseDelay", p.baseDelay);
    return p;
}
static broaudio::DistortionPreset jsToDistortionPreset(JSContext* ctx, JSValueConst o) {
    broaudio::DistortionPreset p;
    if (!JS_IsObject(o)) return p;
    p.enabled    = jsObjBool(ctx, o, "enabled", p.enabled);
    p.mode       = parseDistortionMode(jsObjStr(ctx, o, "mode", "softclip"));
    p.drive      = (float)jsObjNum(ctx, o, "drive", p.drive);
    p.mix        = (float)jsObjNum(ctx, o, "mix", p.mix);
    p.outputGain = (float)jsObjNum(ctx, o, "outputGain", p.outputGain);
    p.crushBits  = (float)jsObjNum(ctx, o, "crushBits", p.crushBits);
    p.crushRate  = (float)jsObjNum(ctx, o, "crushRate", p.crushRate);
    return p;
}
static broaudio::EqPreset jsToEqPreset(JSContext* ctx, JSValueConst o) {
    broaudio::EqPreset p;
    if (!JS_IsObject(o)) return p;
    p.enabled    = jsObjBool(ctx, o, "enabled", p.enabled);
    p.masterGain = (float)jsObjNum(ctx, o, "masterGain", p.masterGain);
    JSValue bg = JS_GetPropertyStr(ctx, o, "bandGains");
    if (JS_IsObject(bg)) {
        for (int i = 0; i < 7; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, bg, i);
            if (!JS_IsUndefined(e) && !JS_IsNull(e)) {
                double d = p.bandGains[i]; JS_ToFloat64(ctx, &d, e); p.bandGains[i] = (float)d;
            }
            JS_FreeValue(ctx, e);
        }
    }
    JS_FreeValue(ctx, bg);
    return p;
}
static broaudio::BusPreset jsToBusPreset(JSContext* ctx, JSValueConst o) {
    broaudio::BusPreset p;
    if (!JS_IsObject(o)) return p;
    p.gain = (float)jsObjNum(ctx, o, "gain", p.gain);
    p.pan  = (float)jsObjNum(ctx, o, "pan", p.pan);

    const int slotCount = static_cast<int>(broaudio::EffectSlot::Count);
    JSValue ord = JS_GetPropertyStr(ctx, o, "effectOrder");
    if (JS_IsObject(ord)) {
        for (int i = 0; i < slotCount; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, ord, i);
            const char* s = JS_ToCString(ctx, e);
            if (s) { p.effectOrder[i] = parseEffectSlot(s, p.effectOrder[i]); JS_FreeCString(ctx, s); }
            JS_FreeValue(ctx, e);
        }
    }
    JS_FreeValue(ctx, ord);

    JSValue filt = JS_GetPropertyStr(ctx, o, "filters");
    if (JS_IsObject(filt)) {
        for (int i = 0; i < broaudio::BusPreset::MAX_FILTERS; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, filt, i);
            if (JS_IsObject(e)) p.filters[i] = jsToFilterPreset(ctx, e);
            JS_FreeValue(ctx, e);
        }
    }
    JS_FreeValue(ctx, filt);

    JSValue v;
    v = JS_GetPropertyStr(ctx, o, "delay");      p.delay      = jsToDelayPreset(ctx, v);      JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, o, "compressor"); p.compressor = jsToCompressorPreset(ctx, v); JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, o, "reverb");     p.reverb     = jsToReverbPreset(ctx, v);     JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, o, "chorus");     p.chorus     = jsToChorusPreset(ctx, v);     JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, o, "distortion"); p.distortion = jsToDistortionPreset(ctx, v); JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, o, "eq");         p.eq         = jsToEqPreset(ctx, v);         JS_FreeValue(ctx, v);
    return p;
}
static broaudio::LfoPreset jsToLfoPreset(JSContext* ctx, JSValueConst o) {
    broaudio::LfoPreset p;
    if (!JS_IsObject(o)) return p;
    p.shape   = parseLfoShape(jsObjStr(ctx, o, "shape", "sine").c_str());
    p.rate    = (float)jsObjNum(ctx, o, "rate", p.rate);
    p.depth   = (float)jsObjNum(ctx, o, "depth", p.depth);
    p.offset  = (float)jsObjNum(ctx, o, "offset", p.offset);
    p.bipolar = jsObjBool(ctx, o, "bipolar", p.bipolar);
    p.sync    = jsObjBool(ctx, o, "sync", p.sync);
    return p;
}
static broaudio::RoutePreset jsToRoutePreset(JSContext* ctx, JSValueConst o) {
    broaudio::RoutePreset p;
    if (!JS_IsObject(o)) return p;
    p.source  = parseModSource(jsObjStr(ctx, o, "source", "lfo1").c_str());
    p.dest    = parseModDest(jsObjStr(ctx, o, "dest", "pitch").c_str());
    p.amount  = (float)jsObjNum(ctx, o, "amount", p.amount);
    p.enabled = jsObjBool(ctx, o, "enabled", p.enabled);
    return p;
}
static broaudio::ModPreset jsToModPreset(JSContext* ctx, JSValueConst o) {
    broaudio::ModPreset p;
    if (!JS_IsObject(o)) return p;
    JSValue lfos = JS_GetPropertyStr(ctx, o, "lfos");
    if (JS_IsObject(lfos)) {
        for (int i = 0; i < broaudio::ModPreset::MAX_LFOS; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, lfos, i);
            if (JS_IsObject(e)) p.lfos[i] = jsToLfoPreset(ctx, e);
            JS_FreeValue(ctx, e);
        }
    }
    JS_FreeValue(ctx, lfos);

    JSValue routes = JS_GetPropertyStr(ctx, o, "routes");
    if (JS_IsObject(routes)) {
        uint32_t n = jsArrayLen(ctx, routes);
        for (uint32_t i = 0; i < n; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, routes, i);
            if (JS_IsObject(e)) p.routes.push_back(jsToRoutePreset(ctx, e));
            JS_FreeValue(ctx, e);
        }
    }
    JS_FreeValue(ctx, routes);
    return p;
}
static broaudio::EnginePreset jsToEnginePreset(JSContext* ctx, JSValueConst o) {
    broaudio::EnginePreset p;
    if (!JS_IsObject(o)) return p;
    p.masterGain = (float)jsObjNum(ctx, o, "masterGain", p.masterGain);

    JSValue lim = JS_GetPropertyStr(ctx, o, "limiter");
    if (JS_IsObject(lim)) {
        p.limiter.enabled     = jsObjBool(ctx, lim, "enabled", p.limiter.enabled);
        p.limiter.thresholdDb = (float)jsObjNum(ctx, lim, "thresholdDb", p.limiter.thresholdDb);
        p.limiter.releaseMs   = (float)jsObjNum(ctx, lim, "releaseMs", p.limiter.releaseMs);
    }
    JS_FreeValue(ctx, lim);

    JSValue mb = JS_GetPropertyStr(ctx, o, "masterBus");
    if (JS_IsObject(mb)) p.masterBus = jsToBusPreset(ctx, mb);
    JS_FreeValue(ctx, mb);

    JSValue mod = JS_GetPropertyStr(ctx, o, "modulation");
    if (JS_IsObject(mod)) p.modulation = jsToModPreset(ctx, mod);
    JS_FreeValue(ctx, mod);

    JSValue buses = JS_GetPropertyStr(ctx, o, "buses");
    if (JS_IsObject(buses)) {
        uint32_t n = jsArrayLen(ctx, buses);
        for (uint32_t i = 0; i < n; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, buses, i);
            if (JS_IsObject(e)) p.buses.push_back(jsToBusPreset(ctx, e));
            JS_FreeValue(ctx, e);
        }
    }
    JS_FreeValue(ctx, buses);
    return p;
}

// --- preset struct -> JS object ---

static JSValue voicePresetToJs(JSContext* ctx, const broaudio::VoicePreset& p) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "waveform", JS_NewString(ctx, waveformToString(p.waveform)));
    JS_SetPropertyStr(ctx, o, "frequency", JS_NewFloat64(ctx, p.frequency));
    JS_SetPropertyStr(ctx, o, "gain", JS_NewFloat64(ctx, p.gain));
    JS_SetPropertyStr(ctx, o, "pan", JS_NewFloat64(ctx, p.pan));
    JS_SetPropertyStr(ctx, o, "pitchBend", JS_NewFloat64(ctx, p.pitchBend));
    JS_SetPropertyStr(ctx, o, "attackTime", JS_NewFloat64(ctx, p.attackTime));
    JS_SetPropertyStr(ctx, o, "decayTime", JS_NewFloat64(ctx, p.decayTime));
    JS_SetPropertyStr(ctx, o, "sustainLevel", JS_NewFloat64(ctx, p.sustainLevel));
    JS_SetPropertyStr(ctx, o, "releaseTime", JS_NewFloat64(ctx, p.releaseTime));
    JS_SetPropertyStr(ctx, o, "filterEnabled", JS_NewBool(ctx, p.filterEnabled));
    JS_SetPropertyStr(ctx, o, "filterType", JS_NewString(ctx, filterTypeToString(p.filterType)));
    JS_SetPropertyStr(ctx, o, "filterFreq", JS_NewFloat64(ctx, p.filterFreq));
    JS_SetPropertyStr(ctx, o, "filterQ", JS_NewFloat64(ctx, p.filterQ));
    JS_SetPropertyStr(ctx, o, "unisonCount", JS_NewInt32(ctx, p.unisonCount));
    JS_SetPropertyStr(ctx, o, "unisonDetune", JS_NewFloat64(ctx, p.unisonDetune));
    JS_SetPropertyStr(ctx, o, "unisonStereoWidth", JS_NewFloat64(ctx, p.unisonStereoWidth));
    return o;
}
static JSValue filterPresetToJs(JSContext* ctx, const broaudio::FilterPreset& p) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "enabled", JS_NewBool(ctx, p.enabled));
    JS_SetPropertyStr(ctx, o, "type", JS_NewString(ctx, filterTypeToString(p.type)));
    JS_SetPropertyStr(ctx, o, "frequency", JS_NewFloat64(ctx, p.frequency));
    JS_SetPropertyStr(ctx, o, "Q", JS_NewFloat64(ctx, p.Q));
    JS_SetPropertyStr(ctx, o, "gainDB", JS_NewFloat64(ctx, p.gainDB));
    return o;
}
static JSValue busPresetToJs(JSContext* ctx, const broaudio::BusPreset& p) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "gain", JS_NewFloat64(ctx, p.gain));
    JS_SetPropertyStr(ctx, o, "pan", JS_NewFloat64(ctx, p.pan));

    const int slotCount = static_cast<int>(broaudio::EffectSlot::Count);
    JSValue ord = JS_NewArray(ctx);
    for (int i = 0; i < slotCount; i++)
        JS_SetPropertyUint32(ctx, ord, i, JS_NewString(ctx, effectSlotToString(p.effectOrder[i])));
    JS_SetPropertyStr(ctx, o, "effectOrder", ord);

    JSValue filt = JS_NewArray(ctx);
    for (int i = 0; i < broaudio::BusPreset::MAX_FILTERS; i++)
        JS_SetPropertyUint32(ctx, filt, i, filterPresetToJs(ctx, p.filters[i]));
    JS_SetPropertyStr(ctx, o, "filters", filt);

    JSValue dl = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, dl, "enabled", JS_NewBool(ctx, p.delay.enabled));
    JS_SetPropertyStr(ctx, dl, "time", JS_NewFloat64(ctx, p.delay.time));
    JS_SetPropertyStr(ctx, dl, "feedback", JS_NewFloat64(ctx, p.delay.feedback));
    JS_SetPropertyStr(ctx, dl, "mix", JS_NewFloat64(ctx, p.delay.mix));
    JS_SetPropertyStr(ctx, o, "delay", dl);

    JSValue cp = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, cp, "enabled", JS_NewBool(ctx, p.compressor.enabled));
    JS_SetPropertyStr(ctx, cp, "threshold", JS_NewFloat64(ctx, p.compressor.threshold));
    JS_SetPropertyStr(ctx, cp, "ratio", JS_NewFloat64(ctx, p.compressor.ratio));
    JS_SetPropertyStr(ctx, cp, "attackMs", JS_NewFloat64(ctx, p.compressor.attackMs));
    JS_SetPropertyStr(ctx, cp, "releaseMs", JS_NewFloat64(ctx, p.compressor.releaseMs));
    JS_SetPropertyStr(ctx, o, "compressor", cp);

    JSValue rv = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, rv, "enabled", JS_NewBool(ctx, p.reverb.enabled));
    JS_SetPropertyStr(ctx, rv, "roomSize", JS_NewFloat64(ctx, p.reverb.roomSize));
    JS_SetPropertyStr(ctx, rv, "damping", JS_NewFloat64(ctx, p.reverb.damping));
    JS_SetPropertyStr(ctx, rv, "mix", JS_NewFloat64(ctx, p.reverb.mix));
    JS_SetPropertyStr(ctx, o, "reverb", rv);

    JSValue ch = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ch, "enabled", JS_NewBool(ctx, p.chorus.enabled));
    JS_SetPropertyStr(ctx, ch, "rate", JS_NewFloat64(ctx, p.chorus.rate));
    JS_SetPropertyStr(ctx, ch, "depth", JS_NewFloat64(ctx, p.chorus.depth));
    JS_SetPropertyStr(ctx, ch, "mix", JS_NewFloat64(ctx, p.chorus.mix));
    JS_SetPropertyStr(ctx, ch, "feedback", JS_NewFloat64(ctx, p.chorus.feedback));
    JS_SetPropertyStr(ctx, ch, "baseDelay", JS_NewFloat64(ctx, p.chorus.baseDelay));
    JS_SetPropertyStr(ctx, o, "chorus", ch);

    JSValue ds = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ds, "enabled", JS_NewBool(ctx, p.distortion.enabled));
    JS_SetPropertyStr(ctx, ds, "mode", JS_NewString(ctx, distortionModeToString(p.distortion.mode)));
    JS_SetPropertyStr(ctx, ds, "drive", JS_NewFloat64(ctx, p.distortion.drive));
    JS_SetPropertyStr(ctx, ds, "mix", JS_NewFloat64(ctx, p.distortion.mix));
    JS_SetPropertyStr(ctx, ds, "outputGain", JS_NewFloat64(ctx, p.distortion.outputGain));
    JS_SetPropertyStr(ctx, ds, "crushBits", JS_NewFloat64(ctx, p.distortion.crushBits));
    JS_SetPropertyStr(ctx, ds, "crushRate", JS_NewFloat64(ctx, p.distortion.crushRate));
    JS_SetPropertyStr(ctx, o, "distortion", ds);

    JSValue eq = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, eq, "enabled", JS_NewBool(ctx, p.eq.enabled));
    JS_SetPropertyStr(ctx, eq, "masterGain", JS_NewFloat64(ctx, p.eq.masterGain));
    JSValue bands = JS_NewArray(ctx);
    for (int i = 0; i < 7; i++) JS_SetPropertyUint32(ctx, bands, i, JS_NewFloat64(ctx, p.eq.bandGains[i]));
    JS_SetPropertyStr(ctx, eq, "bandGains", bands);
    JS_SetPropertyStr(ctx, o, "eq", eq);
    return o;
}
static JSValue modPresetToJs(JSContext* ctx, const broaudio::ModPreset& p) {
    JSValue o = JS_NewObject(ctx);
    JSValue lfos = JS_NewArray(ctx);
    for (int i = 0; i < broaudio::ModPreset::MAX_LFOS; i++) {
        const broaudio::LfoPreset& l = p.lfos[i];
        JSValue lo = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, lo, "shape", JS_NewString(ctx, lfoShapeToString(l.shape)));
        JS_SetPropertyStr(ctx, lo, "rate", JS_NewFloat64(ctx, l.rate));
        JS_SetPropertyStr(ctx, lo, "depth", JS_NewFloat64(ctx, l.depth));
        JS_SetPropertyStr(ctx, lo, "offset", JS_NewFloat64(ctx, l.offset));
        JS_SetPropertyStr(ctx, lo, "bipolar", JS_NewBool(ctx, l.bipolar));
        JS_SetPropertyStr(ctx, lo, "sync", JS_NewBool(ctx, l.sync));
        JS_SetPropertyUint32(ctx, lfos, i, lo);
    }
    JS_SetPropertyStr(ctx, o, "lfos", lfos);

    JSValue routes = JS_NewArray(ctx);
    for (size_t i = 0; i < p.routes.size(); i++) {
        const broaudio::RoutePreset& r = p.routes[i];
        JSValue ro = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, ro, "source", JS_NewString(ctx, modSourceToString(r.source)));
        JS_SetPropertyStr(ctx, ro, "dest", JS_NewString(ctx, modDestToString(r.dest)));
        JS_SetPropertyStr(ctx, ro, "amount", JS_NewFloat64(ctx, r.amount));
        JS_SetPropertyStr(ctx, ro, "enabled", JS_NewBool(ctx, r.enabled));
        JS_SetPropertyUint32(ctx, routes, (uint32_t)i, ro);
    }
    JS_SetPropertyStr(ctx, o, "routes", routes);
    return o;
}
static JSValue enginePresetToJs(JSContext* ctx, const broaudio::EnginePreset& p) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "masterGain", JS_NewFloat64(ctx, p.masterGain));

    JSValue lim = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, lim, "enabled", JS_NewBool(ctx, p.limiter.enabled));
    JS_SetPropertyStr(ctx, lim, "thresholdDb", JS_NewFloat64(ctx, p.limiter.thresholdDb));
    JS_SetPropertyStr(ctx, lim, "releaseMs", JS_NewFloat64(ctx, p.limiter.releaseMs));
    JS_SetPropertyStr(ctx, o, "limiter", lim);

    JS_SetPropertyStr(ctx, o, "masterBus", busPresetToJs(ctx, p.masterBus));
    JS_SetPropertyStr(ctx, o, "modulation", modPresetToJs(ctx, p.modulation));

    JSValue buses = JS_NewArray(ctx);
    for (size_t i = 0; i < p.buses.size(); i++)
        JS_SetPropertyUint32(ctx, buses, (uint32_t)i, busPresetToJs(ctx, p.buses[i]));
    JS_SetPropertyStr(ctx, o, "buses", buses);
    return o;
}

// --- AudioContext preset raw methods ---

static JSValue js_audioctx_voicePresetToJson(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NULL;
    ::std::string j = broaudio::toJson(jsToVoicePreset(ctx, argv[0]));
    return JS_NewStringLen(ctx, j.data(), j.size());
}
static JSValue js_audioctx_busPresetToJson(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NULL;
    ::std::string j = broaudio::toJson(jsToBusPreset(ctx, argv[0]));
    return JS_NewStringLen(ctx, j.data(), j.size());
}
static JSValue js_audioctx_modPresetToJson(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NULL;
    ::std::string j = broaudio::toJson(jsToModPreset(ctx, argv[0]));
    return JS_NewStringLen(ctx, j.data(), j.size());
}
static JSValue js_audioctx_enginePresetToJson(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NULL;
    ::std::string j = broaudio::toJson(jsToEnginePreset(ctx, argv[0]));
    return JS_NewStringLen(ctx, j.data(), j.size());
}
static JSValue js_audioctx_voicePresetFromJson(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NULL;
    const char* s = JS_ToCString(ctx, argv[0]); if (!s) return JS_NULL;
    broaudio::VoicePreset p = broaudio::voicePresetFromJson(s); JS_FreeCString(ctx, s);
    return voicePresetToJs(ctx, p);
}
static JSValue js_audioctx_busPresetFromJson(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NULL;
    const char* s = JS_ToCString(ctx, argv[0]); if (!s) return JS_NULL;
    broaudio::BusPreset p = broaudio::busPresetFromJson(s); JS_FreeCString(ctx, s);
    return busPresetToJs(ctx, p);
}
static JSValue js_audioctx_modPresetFromJson(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NULL;
    const char* s = JS_ToCString(ctx, argv[0]); if (!s) return JS_NULL;
    broaudio::ModPreset p = broaudio::modPresetFromJson(s); JS_FreeCString(ctx, s);
    return modPresetToJs(ctx, p);
}
static JSValue js_audioctx_enginePresetFromJson(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NULL;
    const char* s = JS_ToCString(ctx, argv[0]); if (!s) return JS_NULL;
    broaudio::EnginePreset p = broaudio::enginePresetFromJson(s); JS_FreeCString(ctx, s);
    return enginePresetToJs(ctx, p);
}
static JSValue js_audioctx_applyVoicePreset(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d || argc < 2) return JS_UNDEFINED;
    int voiceId; JS_ToInt32(ctx, &voiceId, argv[0]);
    d->engine->applyVoicePreset(voiceId, jsToVoicePreset(ctx, argv[1]));
    return JS_UNDEFINED;
}
static JSValue js_audioctx_applyBusPreset(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d || argc < 2) return JS_UNDEFINED;
    int busId; JS_ToInt32(ctx, &busId, argv[0]);
    d->engine->applyBusPreset(busId, jsToBusPreset(ctx, argv[1]));
    return JS_UNDEFINED;
}
static JSValue js_audioctx_applyModPreset(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d || argc < 1) return JS_UNDEFINED;
    d->engine->applyModPreset(jsToModPreset(ctx, argv[0]));
    return JS_UNDEFINED;
}
static JSValue js_audioctx_applyEnginePreset(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d || argc < 1) return JS_UNDEFINED;
    d->engine->applyEnginePreset(jsToEnginePreset(ctx, argv[0]));
    return JS_UNDEFINED;
}
static JSValue js_audioctx_savePreset(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_FALSE;
    const char* j = JS_ToCString(ctx, argv[0]);
    const char* path = JS_ToCString(ctx, argv[1]);
    bool ok = false;
    if (j && path) ok = broaudio::savePresetToFile(j, resolveAssetWritePath(path).c_str());
    if (j) JS_FreeCString(ctx, j);
    if (path) JS_FreeCString(ctx, path);
    return JS_NewBool(ctx, ok);
}
static JSValue js_audioctx_loadPreset(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NULL;
    const char* path = JS_ToCString(ctx, argv[0]); if (!path) return JS_NULL;
    ::std::string s = broaudio::loadPresetFromFile(resolveAssetPath(path).c_str());
    JS_FreeCString(ctx, path);
    if (s.empty()) return JS_NULL;
    return JS_NewStringLen(ctx, s.data(), s.size());
}

// --- Offline render (headless) ---

// Render numFrames through the full pipeline (no device) and return the latest
// mono mixdown as a Float32Array. If a Float32Array is passed as the 2nd arg it
// is filled in place (up to its length) and returned. Headless only u2014 driving
// the pipeline from the main thread while a live device callback runs would race.
static JSValue js_audioctx_renderBlock(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    assertMainAudioThread();
    auto* d = qjsbind::unwrap<AudioCtxData>(ctx, this_val);
    if (!d || argc < 1) return JS_UNDEFINED;
    int numFrames; JS_ToInt32(ctx, &numFrames, argv[0]);
    if (numFrames <= 0) return JS_UNDEFINED;

    d->engine->renderBlock(numFrames);

    int cap = d->engine->outputBuffer().capacity();
    int n = ::std::min(numFrames, cap);

    if (argc >= 2 && JS_IsObject(argv[1])) {
        size_t len = 0;
        uint8_t* raw = getTypedArrayPtr(ctx, argv[1], len);
        if (raw) {
            int rc = ::std::min(n, static_cast<int>(len / sizeof(float)));
            if (rc > 0) d->engine->outputBuffer().readLatest(reinterpret_cast<float*>(raw), rc);
            return JS_DupValue(ctx, argv[1]);
        }
    }

    JSValue lenVal = JS_NewInt32(ctx, n);
    JSValue arr = JS_NewTypedArray(ctx, 1, &lenVal, JS_TYPED_ARRAY_FLOAT32);
    JS_FreeValue(ctx, lenVal);
    if (JS_IsException(arr)) return arr;

    size_t byteOff = 0, viewLen = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, arr, &byteOff, &viewLen, nullptr);
    if (!JS_IsException(abuf)) {
        size_t abufLen = 0;
        uint8_t* ptr = JS_GetArrayBuffer(ctx, &abufLen, abuf);
        if (ptr) d->engine->outputBuffer().readLatest(reinterpret_cast<float*>(ptr + byteOff), n);
        JS_FreeValue(ctx, abuf);
    }
    return arr;
}

// --- Sequence read-back raw methods ---

static JSValue js_seq_note(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<SequenceData>(ctx, this_val);
    if (!d || argc < 1) return JS_NULL;
    int idx; JS_ToInt32(ctx, &idx, argv[0]);
    if (idx < 0 || idx >= d->seq->noteCount()) return JS_NULL;
    const broaudio::NoteEvent& n = d->seq->note(idx);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "beat", JS_NewFloat64(ctx, n.beatPosition));
    JS_SetPropertyStr(ctx, o, "note", JS_NewInt32(ctx, n.note));
    JS_SetPropertyStr(ctx, o, "velocity", JS_NewFloat64(ctx, n.velocity));
    JS_SetPropertyStr(ctx, o, "duration", JS_NewFloat64(ctx, n.duration));
    return o;
}
static JSValue js_seq_automationPoint(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<SequenceData>(ctx, this_val);
    if (!d || argc < 2) return JS_NULL;
    int lane, pt; JS_ToInt32(ctx, &lane, argv[0]); JS_ToInt32(ctx, &pt, argv[1]);
    if (lane < 0 || lane >= d->seq->automationLaneCount()) return JS_NULL;
    const broaudio::AutomationLane& L = d->seq->automationLane(lane);
    if (pt < 0 || pt >= L.pointCount()) return JS_NULL;
    const broaudio::AutomationPoint& p = L.point(pt);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "beat", JS_NewFloat64(ctx, p.beat));
    JS_SetPropertyStr(ctx, o, "value", JS_NewFloat64(ctx, p.value));
    return o;
}
static JSValue js_seq_automationInterpMode(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = qjsbind::unwrap<SequenceData>(ctx, this_val);
    if (!d || argc < 1) return JS_NULL;
    int lane; JS_ToInt32(ctx, &lane, argv[0]);
    if (lane < 0 || lane >= d->seq->automationLaneCount()) return JS_NULL;
    broaudio::InterpMode m = d->seq->automationLane(lane).interpMode();
    const char* s = (m == broaudio::InterpMode::Step) ? "step"
                  : (m == broaudio::InterpMode::Smooth) ? "smooth" : "linear";
    return JS_NewString(ctx, s);
}

void AudioBindings::cleanup(JSContext*)
{
    // No central JS-ref registry u2014 every wrapper (VoiceAllocatorData,
    // MidiInputData, SequenceData, etc.) frees its own JSValue callbacks in
    // its destructor when qjsbind finalizes it. We just drop the engine
    // pointer + wavetable bank cache here; the per-wrapper finalizers run as
    // the runtime tears down.
    s_audioEngine = nullptr;
    s_wavetables.clear();
    s_nextWavetableId = 1;
}

void AudioBindings::install(JSContext* ctx, broaudio::Engine* engine)
{
    s_audioEngine = engine;
    
        // --- AudioParam ---
        {
            qjsbind::Class<AudioParamData>(ctx, "AudioParam")
                .prop("value",
                    [](AudioParamData* p) -> double { return p->value; },
                    [](AudioParamData* p, double v) {
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
                            case AudioParamData::Target::PitchBend:
                                p->engine->setVoicePitchBend(p->voiceId, p->value); break;
                        }
                    });
        }
    
        // --- AudioDestinationNode ---
        {
            qjsbind::Class<AudioDestNodeData>(ctx, "AudioDestinationNode");
        }
    
        // --- AnalyserNode ---
        {
            qjsbind::Class<AnalyserNodeData>(ctx, "AnalyserNode")
                .prop("fftSize",
                    [](AnalyserNodeData* d) -> int { return d->fftSize; },
                    [](AnalyserNodeData* d, int v) {
                        if (v >= 32 && v <= 32768 && (v & (v - 1)) == 0) {
                            d->fftSize = v;
                            d->smoothedMagnitudes.clear();
                        }
                    })
                .get("frequencyBinCount",
                    [](AnalyserNodeData* d) -> int { return d->fftSize / 2; })
                .prop("minDecibels",
                    [](AnalyserNodeData* d) -> double { return d->minDecibels; },
                    [](AnalyserNodeData* d, double v) { d->minDecibels = static_cast<float>(v); })
                .prop("maxDecibels",
                    [](AnalyserNodeData* d) -> double { return d->maxDecibels; },
                    [](AnalyserNodeData* d, double v) { d->maxDecibels = static_cast<float>(v); })
                .prop("smoothingTimeConstant",
                    [](AnalyserNodeData* d) -> double { return d->smoothingTimeConstant; },
                    [](AnalyserNodeData* d, double v) { d->smoothingTimeConstant = static_cast<float>(::std::clamp(v, 0.0, 1.0)); })
                .prop("source",
                    [](AnalyserNodeData* d) -> int { return d->source; },
                    [](AnalyserNodeData* d, int v) { d->source = (v == 2) ? 2 : (v == 1) ? 1 : 0; })
                .method_raw("getFloatFrequencyData", js_analyser_getFloatFrequencyData, 1)
                .method_raw("getByteFrequencyData", js_analyser_getByteFrequencyData, 1)
                .method_raw("getFloatTimeDomainData", js_analyser_getFloatTimeDomainData, 1)
                .method_raw("getByteTimeDomainData", js_analyser_getByteTimeDomainData, 1)
                .method("connect",
                    [](AnalyserNodeData*, JSContext* ctx, JSValue arg) -> JSValue {
                        return JS_DupValue(ctx, arg);
                    })
                .method("disconnect",
                    [](AnalyserNodeData*) {});
        }
    
        // --- MediaStream ---
        {
            qjsbind::Class<MicStreamData>(ctx, "MediaStream");
        }
    
        // --- MediaStreamAudioSourceNode ---
        {
            qjsbind::Class<MicSourceData>(ctx, "MediaStreamAudioSourceNode")
                .method_raw("connect", js_micsource_connect, 1)
                .method("disconnect", [](MicSourceData*) {});
        }
    
        // --- OscillatorNode ---
        {
            qjsbind::Class<OscNodeData>(ctx, "OscillatorNode")
                .get("voiceId",
                    [](OscNodeData* d) -> int { return d->voiceId; })
                .prop("type",
                    [](OscNodeData* d, JSContext* ctx) -> JSValue {
                        return JS_NewString(ctx, d->type.c_str());
                    },
                    [](OscNodeData* d, JSContext* ctx, JSValue val) {
                        const char* s = JS_ToCString(ctx, val);
                        if (!s) return;
                        d->type = s;
                        d->engine->setWaveform(d->voiceId, parseWaveform(s));
                        JS_FreeCString(ctx, s);
                    })
                .method_raw("connect", js_osc_connect, 1)
                .method("disconnect", [](OscNodeData*) {})
                .method_raw("start", js_osc_start, 1)
                .method_raw("stop", js_osc_stop, 1);
        }
    
        // --- GainNode ---
        {
            qjsbind::Class<GainNodeData>(ctx, "GainNode")
                .method("connect",
                    [](GainNodeData*, JSContext* ctx, JSValue arg) -> JSValue {
                        return JS_DupValue(ctx, arg);
                    })
                .method("disconnect", [](GainNodeData*) {});
        }
    
        // --- BiquadFilterNode ---
        {
            qjsbind::Class<BiquadFilterNodeData>(ctx, "BiquadFilterNode")
                .method_raw("connect", js_biquadfilter_connect, 1)
                .method_raw("disconnect", js_biquadfilter_disconnect, 0);
    
            // type getter/setter needs this_val for __type property, use method_raw workaround
            // We register type as methods since prop() doesn't support this_val access
            // Actually, the original used JS_CGETSET_DEF which maps to prop. But
            // since the getter reads from __type on this_val and setter writes to it,
            // we can't use qjsbind prop(). Keep as raw methods that simulate getter/setter.
            // Note: BiquadFilterNode type property was registered via JS_CGETSET_DEF in the
            // original code, but since qjsbind prop lambdas don't have access to this_val
            // (only self pointer), we'll just use the two raw methods above + add the
            // type property via JS_DefinePropertyGetSet manually after class registration.
            // However, the class destructor has already run. Instead, we handle this by
            // making the type methods work on the biquad data directly, storing the type
            // string in the wrapper struct. Actually simpler: just use method_raw for get/set.
        }
    
        // --- VoiceAllocator ---
        {
            qjsbind::Class<VoiceAllocatorData>(ctx, "VoiceAllocator")
                .gc_mark([](VoiceAllocatorData* d, JSRuntime* rt, JS_MarkFunc* mark) {
                    JS_MarkValue(rt, d->voiceSetupCallback, mark);
                    JS_MarkValue(rt, d->lambdaCbRef, mark);
                })
                .method_raw("noteOn", js_va_noteOn, 2)
                .method_raw("noteOff", js_va_noteOff, 1)
                .method_raw("allNotesOff", js_va_allNotesOff, 0)
                .method_raw("setStealPolicy", js_va_setStealPolicy, 1)
                .method("setMaxVoices",
                    [](VoiceAllocatorData* d, int count) { d->allocator->setMaxVoices(count); })
                .method_raw("setVoiceSetup", js_va_setVoiceSetup, 1)
                .method("voiceForNote",
                    [](VoiceAllocatorData* d, int note) -> int { return d->allocator->voiceForNote(note); })
                .get("activeVoiceCount",
                    [](VoiceAllocatorData* d) -> int { return d->allocator->activeVoiceCount(); });
        }
    
        // --- ModMatrix ---
        {
            qjsbind::Class<ModMatrixData>(ctx, "ModMatrix")
                .method_raw("setLfoShape", js_mod_setLfoShape, 2)
                .method("setLfoRate",
                    [](ModMatrixData* d, int idx, double hz) { d->modMatrix->setLfoRate(idx, static_cast<float>(hz)); })
                .method("setLfoDepth",
                    [](ModMatrixData* d, int idx, double v) { d->modMatrix->setLfoDepth(idx, static_cast<float>(v)); })
                .method("setLfoOffset",
                    [](ModMatrixData* d, int idx, double v) { d->modMatrix->setLfoOffset(idx, static_cast<float>(v)); })
                .method("setLfoBipolar",
                    [](ModMatrixData* d, int idx, bool v) { d->modMatrix->setLfoBipolar(idx, v); })
                .method("setLfoSync",
                    [](ModMatrixData* d, int idx, bool v) { d->modMatrix->setLfoSync(idx, v); })
                .method_raw("addRoute", js_mod_addRoute, 3)
                .method("removeRoute",
                    [](ModMatrixData* d, int idx) { d->modMatrix->removeRoute(idx); })
                .method("setRouteAmount",
                    [](ModMatrixData* d, int idx, double v) { d->modMatrix->setRouteAmount(idx, static_cast<float>(v)); })
                .method("setRouteEnabled",
                    [](ModMatrixData* d, int idx, bool v) { d->modMatrix->setRouteEnabled(idx, v); })
                .method("clearAllRoutes",
                    [](ModMatrixData* d) { d->modMatrix->clearAllRoutes(); })
                .method("setModWheel",
                    [](ModMatrixData* d, double v) { d->modMatrix->setModWheel(static_cast<float>(v)); })
                .method("setAftertouch",
                    [](ModMatrixData* d, double v) { d->modMatrix->setAftertouch(static_cast<float>(v)); })
                .get("routeCount",
                    [](ModMatrixData* d) -> int { return d->modMatrix->routeCount(); });
        }
    
        // --- MidiInput ---
        {
            qjsbind::Class<MidiInputData>(ctx, "MidiInput")
                .gc_mark([](MidiInputData* d, JSRuntime* rt, JS_MarkFunc* mark) {
                    JS_MarkValue(rt, d->pitchBendCallback, mark);
                    JS_MarkValue(rt, d->rawCallback, mark);
                    for (JSValue& v : d->ccCallbacks) JS_MarkValue(rt, v, mark);
                })
                .method_raw("availablePorts", js_midi_availablePorts, 0)
                .method("open",
                    [](MidiInputData* d, int port) -> bool { return d->midi->open(port); })
                .method("close",
                    [](MidiInputData* d) { d->midi->close(); })
                .method_raw("connectToAllocator", js_midi_connectToAllocator, 1)
                .method_raw("onControlChange", js_midi_onControlChange, 2)
                .method_raw("onPitchBend", js_midi_onPitchBend, 1)
                .method_raw("onRawEvent", js_midi_onRawEvent, 1)
                .method("processEvents",
                    [](MidiInputData* d) { d->midi->processEvents(); })
                .get("isOpen",
                    [](MidiInputData* d) -> bool { return d->midi->isOpen(); });
        }
    
        // --- Sequence ---
        {
            qjsbind::Class<SequenceData>(ctx, "Sequence")
                .gc_mark([](SequenceData* d, JSRuntime* rt, JS_MarkFunc* mark) {
                    for (JSValue v : d->automationCallbacks) JS_MarkValue(rt, v, mark);
                })
                .method("setBPM",
                    [](SequenceData* d, double v) {
                        if (s_audioEngine)
                            d->seq->setBPM(v, s_audioEngine->currentTime());
                        else
                            d->seq->setBPM(v);
                    })
                .method("setTimeSignature",
                    [](SequenceData* d, int num, int den) { d->seq->setTimeSignature(num, den); })
                .method("addNote",
                    [](SequenceData* d, double beat, int note, double vel, double dur) {
                        broaudio::NoteEvent ev{beat, note, static_cast<float>(vel), dur};
                        d->seq->addNote(ev);
                    })
                .method("removeNote",
                    [](SequenceData* d, int idx) { d->seq->removeNote(idx); })
                .method("clearNotes",
                    [](SequenceData* d) { d->seq->clearNotes(); })
                .method("play",
                    [](SequenceData* d, optional<double> t) {
                        double when = t.value_or(s_audioEngine ? s_audioEngine->currentTime() : 0.0);
                        d->seq->play(when);
                    })
                .method("stop",
                    [](SequenceData* d) { d->seq->stop(); })
                .method("pause",
                    [](SequenceData* d, optional<double> t) {
                        double when = t.value_or(s_audioEngine ? s_audioEngine->currentTime() : 0.0);
                        d->seq->pause(when);
                    })
                .method("resume",
                    [](SequenceData* d, optional<double> t) {
                        double when = t.value_or(s_audioEngine ? s_audioEngine->currentTime() : 0.0);
                        d->seq->resume(when);
                    })
                .method("setLoopEnabled",
                    [](SequenceData* d, bool v) { d->seq->setLoopEnabled(v); })
                .method("setLoopRange",
                    [](SequenceData* d, double start, double end) { d->seq->setLoopRange(start, end); })
                .method("currentBeat",
                    [](SequenceData* d, optional<double> t) -> double {
                        double when = t.value_or(s_audioEngine ? s_audioEngine->currentTime() : 0.0);
                        return d->seq->currentBeat(when);
                    })
                .method("update",
                    [](SequenceData* d, optional<double> t) {
                        double when = t.value_or(s_audioEngine ? s_audioEngine->currentTime() : 0.0);
                        d->seq->update(when);
                    })
                .get("bpm",
                    [](SequenceData* d) -> double { return d->seq->bpm(); })
                .get("playing",
                    [](SequenceData* d) -> bool { return d->seq->isPlaying(); })
                .get("paused",
                    [](SequenceData* d) -> bool { return d->seq->isPaused(); })
                .get("loopEnabled",
                    [](SequenceData* d) -> bool { return d->seq->isLoopEnabled(); })
                .get("noteCount",
                    [](SequenceData* d) -> int { return d->seq->noteCount(); })
                .method_raw("note", js_seq_note, 1)
                .method_raw("addAutomationLane", js_seq_addAutomationLane, 1)
                .method_raw("removeAutomationLane", js_seq_removeAutomationLane, 1)
                .method_raw("clearAutomationLanes", js_seq_clearAutomationLanes, 0)
                .method("addAutomationPoint",
                    [](SequenceData* d, int laneIdx, double beat, double value) {
                        if (laneIdx >= 0 && laneIdx < d->seq->automationLaneCount())
                            d->seq->automationLane(laneIdx).addPoint(beat, static_cast<float>(value));
                    })
                .method("removeAutomationPoint",
                    [](SequenceData* d, int laneIdx, int ptIdx) {
                        if (laneIdx >= 0 && laneIdx < d->seq->automationLaneCount())
                            d->seq->automationLane(laneIdx).removePoint(ptIdx);
                    })
                .method("clearAutomationPoints",
                    [](SequenceData* d, int laneIdx) {
                        if (laneIdx >= 0 && laneIdx < d->seq->automationLaneCount())
                            d->seq->automationLane(laneIdx).clearPoints();
                    })
                .method_raw("setAutomationInterpMode", js_seq_setAutomationInterpMode, 2)
                .method("automationPointCount",
                    [](SequenceData* d, int lane) -> int {
                        if (lane < 0 || lane >= d->seq->automationLaneCount()) return 0;
                        return d->seq->automationLane(lane).pointCount();
                    })
                .method_raw("automationPoint", js_seq_automationPoint, 2)
                .method_raw("automationInterpMode", js_seq_automationInterpMode, 1)
                .get("automationLaneCount",
                    [](SequenceData* d) -> int { return d->seq->automationLaneCount(); });
        }
    
        // --- AudioContext ---
        {
            qjsbind::Class<AudioCtxData>(ctx, "AudioContext")
                .constructor([](JSContext* ctx, int, JSValueConst*) -> AudioCtxData* {
                    if (!s_audioEngine) {
                        JS_ThrowInternalError(ctx, "Audio not initialized");
                        return nullptr;
                    }
                    auto* data = new AudioCtxData{s_audioEngine};
                    // We need to set destination after wrap; handled below via __postCtor
                    return data;
                })
                // Properties
                .get("currentTime",
                    [](AudioCtxData* d) -> double { return d->engine->currentTime(); })
                .get("sampleRate",
                    [](AudioCtxData* d) -> int { return d->engine->sampleRate(); })
                .get("outputLatency",
                    [](AudioCtxData* d) -> double { return d->engine->outputLatencySeconds(); })
                .prop("masterGain",
                    [](AudioCtxData* d) -> double { return d->engine->masterGain(); },
                    [](AudioCtxData* d, double v) { d->engine->setMasterGain(static_cast<float>(v)); })
                .prop("micMuted",
                    [](AudioCtxData* d) -> bool { return d->engine->isMicMuted(); },
                    [](AudioCtxData* d, bool v) { d->engine->setMicMuted(v); })
                .prop("micMonitorGain",
                    [](AudioCtxData* d) -> double { return d->engine->micMonitorGain(); },
                    [](AudioCtxData* d, double v) { d->engine->setMicMonitorGain(static_cast<float>(clamp(v, 0.0, 1.0))); })
                .prop("micBus",
                    [](AudioCtxData* d) -> int { return d->engine->micBus(); },
                    [](AudioCtxData* d, int v) { d->engine->setMicBus(v); })
                .get("recording",
                    [](AudioCtxData* d) -> bool { return d->engine->isRecording(); })
    
                // Node creation (raw u2014 they create objects of other types)
                .method_raw("createOscillator", js_audioctx_createOscillator, 0)
                .method_raw("createGain", js_audioctx_createGain, 0)
                .method_raw("createAnalyser", js_audioctx_createAnalyser, 0)
                .method_raw("createBiquadFilter", js_audioctx_createBiquadFilter, 0)
                .method_raw("createMediaStreamSource", js_audioctx_createMediaStreamSource, 1)
    
                // Master delay
                .method("setDelayEnabled",
                    [](AudioCtxData* d, bool v) { d->engine->setDelayEnabled(v); })
                .method("setDelayTime",
                    [](AudioCtxData* d, double v) { d->engine->setDelayTime(static_cast<float>(v)); })
                .method("setDelayFeedback",
                    [](AudioCtxData* d, double v) { d->engine->setDelayFeedback(static_cast<float>(v)); })
                .method("setDelayMix",
                    [](AudioCtxData* d, double v) { d->engine->setDelayMix(static_cast<float>(v)); })
    
                // Master reverb
                .method("setReverbEnabled",
                    [](AudioCtxData* d, bool v) { d->engine->setBusReverbEnabled(0, v); })
                .method("setReverbRoomSize",
                    [](AudioCtxData* d, double v) { d->engine->setBusReverbRoomSize(0, static_cast<float>(v)); })
                .method("setReverbDamping",
                    [](AudioCtxData* d, double v) { d->engine->setBusReverbDamping(0, static_cast<float>(v)); })
                .method("setReverbMix",
                    [](AudioCtxData* d, double v) { d->engine->setBusReverbMix(0, static_cast<float>(v)); })
    
                // Master chorus
                .method("setChorusEnabled",
                    [](AudioCtxData* d, bool v) { d->engine->setBusChorusEnabled(0, v); })
                .method("setChorusRate",
                    [](AudioCtxData* d, double v) { d->engine->setBusChorusRate(0, static_cast<float>(v)); })
                .method("setChorusDepth",
                    [](AudioCtxData* d, double v) { d->engine->setBusChorusDepth(0, static_cast<float>(v)); })
                .method("setChorusMix",
                    [](AudioCtxData* d, double v) { d->engine->setBusChorusMix(0, static_cast<float>(v)); })
                .method("setChorusFeedback",
                    [](AudioCtxData* d, double v) { d->engine->setBusChorusFeedback(0, static_cast<float>(v)); })
                .method("setChorusBaseDelay",
                    [](AudioCtxData* d, double v) { d->engine->setBusChorusBaseDelay(0, static_cast<float>(v)); })
    
                // Master compressor
                .method("setCompressorEnabled",
                    [](AudioCtxData* d, bool v) { d->engine->setBusCompressorEnabled(0, v); })
                .method("setCompressorThreshold",
                    [](AudioCtxData* d, double v) { d->engine->setBusCompressorThreshold(0, static_cast<float>(v)); })
                .method("setCompressorRatio",
                    [](AudioCtxData* d, double v) { d->engine->setBusCompressorRatio(0, static_cast<float>(v)); })
                .method("setCompressorAttack",
                    [](AudioCtxData* d, double v) { d->engine->setBusCompressorAttack(0, static_cast<float>(v)); })
                .method("setCompressorRelease",
                    [](AudioCtxData* d, double v) { d->engine->setBusCompressorRelease(0, static_cast<float>(v)); })
    
                // Master limiter (lookahead peak limiter on the master bus)
                .method("setLimiterEnabled",
                    [](AudioCtxData* d, bool v) { d->engine->setLimiterEnabled(v); })
                .method("setLimiterThreshold",
                    [](AudioCtxData* d, double dB) { d->engine->setLimiterThreshold(static_cast<float>(dB)); })
                .method("setLimiterRelease",
                    [](AudioCtxData* d, double ms) { d->engine->setLimiterRelease(static_cast<float>(ms)); })
    
                // Mix bus API
                .method("createBus",
                    [](AudioCtxData* d) -> int { return d->engine->createBus(); })
                .method("deleteBus",
                    [](AudioCtxData* d, int id) { d->engine->deleteBus(id); })
                .method("setBusGain",
                    [](AudioCtxData* d, int id, double v) { d->engine->setBusGain(id, static_cast<float>(v)); })
                .method("setBusPan",
                    [](AudioCtxData* d, int id, double v) { d->engine->setBusPan(id, static_cast<float>(v)); })
                .method("setBusMuted",
                    [](AudioCtxData* d, int id, bool v) { d->engine->setBusMuted(id, v); })
                .method("setBusSolo",
                    [](AudioCtxData* d, int id, bool v) { d->engine->setBusSolo(id, v); })
                .method("getBusSolo",
                    [](AudioCtxData* d, int id) -> bool { return d->engine->getBusSolo(id); })
                .method("allocateBusFilterSlot",
                    [](AudioCtxData* d, int busId) -> int { return d->engine->allocateBusFilterSlot(busId); })
                .method("releaseBusFilterSlot",
                    [](AudioCtxData* d, int busId, int slot) { d->engine->releaseBusFilterSlot(busId, slot); })
                .method("setBusFilterEnabled",
                    [](AudioCtxData* d, int busId, int slot, bool v) { d->engine->setBusFilterEnabled(busId, slot, v); })
                .method_raw("setBusFilterType", js_audioctx_setBusFilterType, 3)
                .method("setBusFilterFrequency",
                    [](AudioCtxData* d, int busId, int slot, double v) { d->engine->setBusFilterFrequency(busId, slot, static_cast<float>(v)); })
                .method("setBusFilterQ",
                    [](AudioCtxData* d, int busId, int slot, double v) { d->engine->setBusFilterQ(busId, slot, static_cast<float>(v)); })
                .method("setBusFilterGain",
                    [](AudioCtxData* d, int busId, int slot, double v) { d->engine->setBusFilterGain(busId, slot, static_cast<float>(v)); })
    
                // Per-bus effects
                .method("setBusDelayEnabled",
                    [](AudioCtxData* d, int busId, bool v) { d->engine->setBusDelayEnabled(busId, v); })
                .method("setBusDelayTime",
                    [](AudioCtxData* d, int busId, double v) { d->engine->setBusDelayTime(busId, static_cast<float>(v)); })
                .method("setBusDelayFeedback",
                    [](AudioCtxData* d, int busId, double v) { d->engine->setBusDelayFeedback(busId, static_cast<float>(v)); })
                .method("setBusDelayMix",
                    [](AudioCtxData* d, int busId, double v) { d->engine->setBusDelayMix(busId, static_cast<float>(v)); })
                .method("setBusCompressorEnabled",
                    [](AudioCtxData* d, int busId, bool v) { d->engine->setBusCompressorEnabled(busId, v); })
                .method("setBusCompressorThreshold",
                    [](AudioCtxData* d, int busId, double v) { d->engine->setBusCompressorThreshold(busId, static_cast<float>(v)); })
                .method("setBusCompressorRatio",
                    [](AudioCtxData* d, int busId, double v) { d->engine->setBusCompressorRatio(busId, static_cast<float>(v)); })
                .method("setBusCompressorAttack",
                    [](AudioCtxData* d, int busId, double v) { d->engine->setBusCompressorAttack(busId, static_cast<float>(v)); })
                .method("setBusCompressorRelease",
                    [](AudioCtxData* d, int busId, double v) { d->engine->setBusCompressorRelease(busId, static_cast<float>(v)); })
                .method("setBusReverbEnabled",
                    [](AudioCtxData* d, int busId, bool v) { d->engine->setBusReverbEnabled(busId, v); })
                .method("setBusReverbRoomSize",
                    [](AudioCtxData* d, int busId, double v) { d->engine->setBusReverbRoomSize(busId, static_cast<float>(v)); })
                .method("setBusReverbDamping",
                    [](AudioCtxData* d, int busId, double v) { d->engine->setBusReverbDamping(busId, static_cast<float>(v)); })
                .method("setBusReverbMix",
                    [](AudioCtxData* d, int busId, double v) { d->engine->setBusReverbMix(busId, static_cast<float>(v)); })
                .method("setBusChorusEnabled",
                    [](AudioCtxData* d, int busId, bool v) { d->engine->setBusChorusEnabled(busId, v); })
                .method("setBusChorusRate",
                    [](AudioCtxData* d, int busId, double v) { d->engine->setBusChorusRate(busId, static_cast<float>(v)); })
                .method("setBusChorusDepth",
                    [](AudioCtxData* d, int busId, double v) { d->engine->setBusChorusDepth(busId, static_cast<float>(v)); })
                .method("setBusChorusMix",
                    [](AudioCtxData* d, int busId, double v) { d->engine->setBusChorusMix(busId, static_cast<float>(v)); })
                .method("setBusChorusFeedback",
                    [](AudioCtxData* d, int busId, double v) { d->engine->setBusChorusFeedback(busId, static_cast<float>(v)); })
                .method("setBusChorusBaseDelay",
                    [](AudioCtxData* d, int busId, double v) { d->engine->setBusChorusBaseDelay(busId, static_cast<float>(v)); })
                .method("setBusEqEnabled",
                    [](AudioCtxData* d, int busId, bool v) { d->engine->setBusEqEnabled(busId, v); })
                .method("setBusEqBandGain",
                    [](AudioCtxData* d, int busId, int band, double v) { d->engine->setBusEqBandGain(busId, band, static_cast<float>(v)); })
                .method("setBusEqMasterGain",
                    [](AudioCtxData* d, int busId, double v) { d->engine->setBusEqMasterGain(busId, static_cast<float>(v)); })
                .method("setBusDistortionEnabled",
                    [](AudioCtxData* d, int busId, bool v) { d->engine->setBusDistortionEnabled(busId, v); })
                .method("setBusDistortionMode",
                    [](AudioCtxData* d, int busId, string mode) {
                        broaudio::DistortionMode m = broaudio::DistortionMode::SoftClip;
                        if (mode == "softclip") m = broaudio::DistortionMode::SoftClip;
                        else if (mode == "hardclip") m = broaudio::DistortionMode::HardClip;
                        else if (mode == "foldback") m = broaudio::DistortionMode::Foldback;
                        else if (mode == "bitcrush") m = broaudio::DistortionMode::Bitcrush;
                        d->engine->setBusDistortionMode(busId, m);
                    })
                .method("setBusDistortionDrive",
                    [](AudioCtxData* d, int busId, double v) { d->engine->setBusDistortionDrive(busId, static_cast<float>(v)); })
                .method("setBusDistortionMix",
                    [](AudioCtxData* d, int busId, double v) { d->engine->setBusDistortionMix(busId, static_cast<float>(v)); })
                .method("setBusDistortionOutputGain",
                    [](AudioCtxData* d, int busId, double v) { d->engine->setBusDistortionOutputGain(busId, static_cast<float>(v)); })
                .method("setBusDistortionCrushBits",
                    [](AudioCtxData* d, int busId, double v) { d->engine->setBusDistortionCrushBits(busId, static_cast<float>(v)); })
                .method("setBusDistortionCrushRate",
                    [](AudioCtxData* d, int busId, double v) { d->engine->setBusDistortionCrushRate(busId, static_cast<float>(v)); })
                .method("setBusCompressorSidechain",
                    [](AudioCtxData* d, int busId, int scBusId) { d->engine->setBusCompressorSidechain(busId, scBusId); })
    
                // Bus metering
                .method("getBusPeakL",
                    [](AudioCtxData* d, int busId) -> double { return d->engine->getBusPeakL(busId); })
                .method("getBusPeakR",
                    [](AudioCtxData* d, int busId) -> double { return d->engine->getBusPeakR(busId); })
                .method("getBusRmsL",
                    [](AudioCtxData* d, int busId) -> double { return d->engine->getBusRmsL(busId); })
                .method("getBusRmsR",
                    [](AudioCtxData* d, int busId) -> double { return d->engine->getBusRmsR(busId); })
    
                // Sample-accurate scheduling
                .method("scheduleNoteOn",
                    [](AudioCtxData* d, int voiceId, double when) { d->engine->scheduleNoteOn(voiceId, when); })
                .method("scheduleNoteOff",
                    [](AudioCtxData* d, int voiceId, double when) { d->engine->scheduleNoteOff(voiceId, when); })
    
                // Voice/clip bus routing
                .method("setVoiceBus",
                    [](AudioCtxData* d, int voiceId, int busId) { d->engine->setVoiceBus(voiceId, busId); })
                .method("setPlaybackBus",
                    [](AudioCtxData* d, int id, int busId) { d->engine->setPlaybackBus(id, busId); })
    
                // Offline effect processing
                .method_raw("processEffectsOffline", js_audioctx_processEffectsOffline, 2)
    
                // Offline render (headless u2014 pump the full pipeline without a device)
                .method_raw("renderBlock", js_audioctx_renderBlock, 1)
    
                // Preset serialization (JS object <-> JSON) + apply to live engine
                .method_raw("voicePresetToJson", js_audioctx_voicePresetToJson, 1)
                .method_raw("busPresetToJson", js_audioctx_busPresetToJson, 1)
                .method_raw("modPresetToJson", js_audioctx_modPresetToJson, 1)
                .method_raw("enginePresetToJson", js_audioctx_enginePresetToJson, 1)
                .method_raw("voicePresetFromJson", js_audioctx_voicePresetFromJson, 1)
                .method_raw("busPresetFromJson", js_audioctx_busPresetFromJson, 1)
                .method_raw("modPresetFromJson", js_audioctx_modPresetFromJson, 1)
                .method_raw("enginePresetFromJson", js_audioctx_enginePresetFromJson, 1)
                .method_raw("applyVoicePreset", js_audioctx_applyVoicePreset, 2)
                .method_raw("applyBusPreset", js_audioctx_applyBusPreset, 2)
                .method_raw("applyModPreset", js_audioctx_applyModPreset, 1)
                .method_raw("applyEnginePreset", js_audioctx_applyEnginePreset, 1)
                .method_raw("savePreset", js_audioctx_savePreset, 2)
                .method_raw("loadPreset", js_audioctx_loadPreset, 1)
    
                // Voice lifecycle
                .method("createVoice",
                    [](AudioCtxData* d) -> int { return d->engine->createVoice(); })
                .method("removeVoice",
                    [](AudioCtxData* d, int voiceId) { d->engine->removeVoice(voiceId); })
                .method("startVoice",
                    [](AudioCtxData* d, int voiceId, double when) { d->engine->startVoice(voiceId, when); })
                .method("stopVoice",
                    [](AudioCtxData* d, int voiceId, double when) { d->engine->stopVoice(voiceId, when); })
                .method("setVoicePersistent",
                    [](AudioCtxData* d, int voiceId, bool v) { d->engine->setVoicePersistent(voiceId, v); })
    
                // Voice note context
                .method("setVoiceNote",
                    [](AudioCtxData* d, int voiceId, int note, double vel) {
                        d->engine->setVoiceNote(voiceId, note, static_cast<float>(vel));
                    })
    
                // Direct voice parameter control
                .method_raw("setVoiceWaveform", js_audioctx_setVoiceWaveform, 2)
                .method("setVoiceFrequency",
                    [](AudioCtxData* d, int voiceId, double v) { d->engine->setFrequency(voiceId, static_cast<float>(v)); })
                .method("setVoiceGain",
                    [](AudioCtxData* d, int voiceId, double v) { d->engine->setGain(voiceId, static_cast<float>(v)); })
                .method("setVoicePan",
                    [](AudioCtxData* d, int voiceId, double v) { d->engine->setVoicePan(voiceId, static_cast<float>(v)); })
                .method("setVoiceAttack",
                    [](AudioCtxData* d, int voiceId, double v) { d->engine->setAttackTime(voiceId, static_cast<float>(v)); })
                .method("setVoiceDecay",
                    [](AudioCtxData* d, int voiceId, double v) { d->engine->setDecayTime(voiceId, static_cast<float>(v)); })
                .method("setVoiceSustain",
                    [](AudioCtxData* d, int voiceId, double v) { d->engine->setSustainLevel(voiceId, static_cast<float>(v)); })
                .method("setVoiceRelease",
                    [](AudioCtxData* d, int voiceId, double v) { d->engine->setReleaseTime(voiceId, static_cast<float>(v)); })
                .method("setVoicePitchBend",
                    [](AudioCtxData* d, int voiceId, double semitones) { d->engine->setVoicePitchBend(voiceId, static_cast<float>(semitones)); })
    
                // Unison
                .method("setVoiceUnisonCount",
                    [](AudioCtxData* d, int voiceId, int count) { d->engine->setVoiceUnisonCount(voiceId, count); })
                .method("setVoiceUnisonDetune",
                    [](AudioCtxData* d, int voiceId, double v) { d->engine->setVoiceUnisonDetune(voiceId, static_cast<float>(v)); })
                .method("setVoiceUnisonStereoWidth",
                    [](AudioCtxData* d, int voiceId, double v) { d->engine->setVoiceUnisonStereoWidth(voiceId, static_cast<float>(v)); })
    
                // Per-voice filter
                .method("setVoiceFilterEnabled",
                    [](AudioCtxData* d, int voiceId, bool v) { d->engine->setVoiceFilterEnabled(voiceId, v); })
                .method_raw("setVoiceFilterType", js_audioctx_setVoiceFilterType, 2)
                .method("setVoiceFilterFrequency",
                    [](AudioCtxData* d, int voiceId, double v) { d->engine->setVoiceFilterFrequency(voiceId, static_cast<float>(v)); })
                .method("setVoiceFilterQ",
                    [](AudioCtxData* d, int voiceId, double v) { d->engine->setVoiceFilterQ(voiceId, static_cast<float>(v)); })
    
                // Bus effect order
                .method_raw("setBusEffectOrder", js_audioctx_setBusEffectOrder, 2)
    
                // Wavetable
                .method_raw("createWavetable", js_audioctx_createWavetable, 1)
                .method_raw("createWavetableFromWaveform", js_audioctx_createWavetableFromWaveform, 1)
                .method("deleteWavetable",
                    [](AudioCtxData*, int id) { s_wavetables.erase(id); })
                .method_raw("setVoiceWavetable", js_audioctx_setVoiceWavetable, 2)
    
                // Spectrum
                .method_raw("getSpectrum", js_audioctx_getSpectrum, 1)
    
                // Spatial audio u2014 listener
                .method("setListenerPosition",
                    [](AudioCtxData* d, double x, double y, double z) {
                        d->engine->setListenerPosition(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
                    })
                .method("setListenerOrientation",
                    [](AudioCtxData* d, double fx, double fy, double fz, double ux, double uy, double uz) {
                        d->engine->setListenerOrientation(
                            static_cast<float>(fx), static_cast<float>(fy), static_cast<float>(fz),
                            static_cast<float>(ux), static_cast<float>(uy), static_cast<float>(uz));
                    })
                .method("setListenerVelocity",
                    [](AudioCtxData* d, double x, double y, double z) {
                        d->engine->setListenerVelocity(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
                    })
    
                // Doppler
                .prop("dopplerFactor",
                    [](AudioCtxData* d) -> double { return d->engine->dopplerFactor(); },
                    [](AudioCtxData* d, double v) { d->engine->setDopplerFactor(static_cast<float>(v)); })
                .method("getPlaybackDopplerRatio",
                    [](AudioCtxData* d, int id) -> double { return d->engine->getPlaybackDopplerRatio(id); })
                .method("getVoiceDopplerRatio",
                    [](AudioCtxData* d, int id) -> double { return d->engine->getVoiceDopplerRatio(id); })
    
                // Spatial audio u2014 voice sources
                .method("setVoiceSpatialEnabled",
                    [](AudioCtxData* d, int voiceId, bool v) { d->engine->setVoiceSpatialEnabled(voiceId, v); })
                .method("setVoiceSpatialPosition",
                    [](AudioCtxData* d, int voiceId, double x, double y, double z) {
                        d->engine->setVoiceSpatialPosition(voiceId, static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
                    })
                .method("setVoiceSpatialVelocity",
                    [](AudioCtxData* d, int voiceId, double x, double y, double z) {
                        d->engine->setVoiceSpatialVelocity(voiceId, static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
                    })
                .method("setVoiceSpatialRefDistance",
                    [](AudioCtxData* d, int voiceId, double v) { d->engine->setVoiceSpatialRefDistance(voiceId, static_cast<float>(v)); })
                .method("setVoiceSpatialMaxDistance",
                    [](AudioCtxData* d, int voiceId, double v) { d->engine->setVoiceSpatialMaxDistance(voiceId, static_cast<float>(v)); })
                .method("setVoiceSpatialRolloff",
                    [](AudioCtxData* d, int voiceId, double v) { d->engine->setVoiceSpatialRolloff(voiceId, static_cast<float>(v)); })
                .method_raw("setVoiceSpatialDistanceModel", js_audioctx_setVoiceSpatialDistanceModel, 2)
    
                // Spatial audio u2014 playback sources
                .method("setPlaybackSpatialEnabled",
                    [](AudioCtxData* d, int id, bool v) { d->engine->setPlaybackSpatialEnabled(id, v); })
                .method("setPlaybackSpatialPosition",
                    [](AudioCtxData* d, int id, double x, double y, double z) {
                        d->engine->setPlaybackSpatialPosition(id, static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
                    })
                .method("setPlaybackSpatialVelocity",
                    [](AudioCtxData* d, int id, double x, double y, double z) {
                        d->engine->setPlaybackSpatialVelocity(id, static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
                    })
                .method("setPlaybackSpatialRefDistance",
                    [](AudioCtxData* d, int id, double v) { d->engine->setPlaybackSpatialRefDistance(id, static_cast<float>(v)); })
                .method("setPlaybackSpatialMaxDistance",
                    [](AudioCtxData* d, int id, double v) { d->engine->setPlaybackSpatialMaxDistance(id, static_cast<float>(v)); })
                .method("setPlaybackSpatialRolloff",
                    [](AudioCtxData* d, int id, double v) { d->engine->setPlaybackSpatialRolloff(id, static_cast<float>(v)); })
                .method_raw("setPlaybackSpatialDistanceModel", js_audioctx_setPlaybackSpatialDistanceModel, 2)
    
                // Head model
                .method("setHeadModelEnabled",
                    [](AudioCtxData* d, bool v) { d->engine->setHeadModelEnabled(v); })
                .method("setHeadModelIldStrength",
                    [](AudioCtxData* d, double v) { d->engine->setHeadModelIldStrength(static_cast<float>(v)); })
                .method("setHeadModelBehindAttenuation",
                    [](AudioCtxData* d, double v) { d->engine->setHeadModelBehindAttenuation(static_cast<float>(v)); })
                .method("setHeadModelNearCutoff",
                    [](AudioCtxData* d, double front, double behind) {
                        d->engine->setHeadModelNearCutoff(static_cast<float>(front), static_cast<float>(behind));
                    })
                .method("setHeadModelFarCutoffRatio",
                    [](AudioCtxData* d, double v) { d->engine->setHeadModelFarCutoffRatio(static_cast<float>(v)); })
                .method("setHeadModelElevation",
                    [](AudioCtxData* d, double nearHz, double farHz) {
                        d->engine->setHeadModelElevation(static_cast<float>(nearHz), static_cast<float>(farHz));
                    })
                .method("setHeadModelCutoffRange",
                    [](AudioCtxData* d, double minHz, double maxHz) {
                        d->engine->setHeadModelCutoffRange(static_cast<float>(minHz), static_cast<float>(maxHz));
                    })
    
                // Aux sends
                .method("setVoiceSend",
                    [](AudioCtxData* d, int voiceId, int sendBusId, double v) {
                        d->engine->setVoiceSend(voiceId, sendBusId, static_cast<float>(v));
                    })
                .method("setPlaybackSend",
                    [](AudioCtxData* d, int id, int sendBusId, double v) {
                        d->engine->setPlaybackSend(id, sendBusId, static_cast<float>(v));
                    })
                .method("setBusSend",
                    [](AudioCtxData* d, int busId, int sendBusId, double v) {
                        d->engine->setBusSend(busId, sendBusId, static_cast<float>(v));
                    })
    
                // Factory methods
                .method_raw("createVoiceAllocator", js_audioctx_createVoiceAllocator, 1)
                .method_raw("getModMatrix", js_audioctx_getModMatrix, 0)
                .method_raw("createMidiInput", js_audioctx_createMidiInput, 0)
                .method_raw("createSequence", js_audioctx_createSequence, 1)
    
                // Recording
                .method("startRecording",
                    [](AudioCtxData* d) { d->engine->startRecording(); })
                .method_raw("stopRecording", js_audioctx_stopRecording, 0)
    
                // Audio file I/O
                .method_raw("createClipFromFile", js_audioctx_createClipFromFile, 1)
                .method_raw("createClipFromFileAsync", js_audioctx_createClipFromFileAsync, 1)
                .method_raw("decodeAudioData", js_audioctx_decodeAudioData, 1)
                .method_raw("decodeAudioFile", js_audioctx_decodeAudioFile, 1)
                .method("exportRecordingToWav",
                    [](AudioCtxData* d, JSContext* ctx, JSValue pathVal) -> JSValue {
                        const char* path = JS_ToCString(ctx, pathVal);
                        if (!path) return JS_FALSE;
                        string resolved = resolveAssetWritePath(path);
                        bool ok = d->engine->exportRecordingToWav(resolved.c_str());
                        JS_FreeCString(ctx, path);
                        return JS_NewBool(ctx, ok);
                    })
                .method_raw("saveWav", js_audioctx_saveWav, 4)
    
                // Clips
                .method_raw("createClip", js_audioctx_createClip, 1)
                .method("deleteClip",
                    [](AudioCtxData* d, int id) { d->engine->deleteClip(id); })
                .method("getClipSampleCount",
                    [](AudioCtxData* d, int id) -> int { return d->engine->getClipSampleCount(id); })
                .method("getClipChannels",
                    [](AudioCtxData* d, int id) -> int { return d->engine->getClipChannels(id); })
                .method_raw("getClipWaveform", js_audioctx_getClipWaveform, 2)
                .method_raw("playClip", js_audioctx_playClip, 4)
                // Streaming PCM source (live voice / network audio)
                .method_raw("createStream", js_audioctx_createStream, 2)
                .method_raw("pushStreamSamples", js_audioctx_pushStreamSamples, 2)
                .method_raw("closeStream", js_audioctx_closeStream, 1)
                // Disk-streamed file playback
                .method_raw("createStreamFromFile", js_audioctx_createStreamFromFile, 2)
                .method_raw("getStreamStats", js_audioctx_getStreamStats, 1)
                .method("stopPlayback",
                    [](AudioCtxData* d, int id) { d->engine->stopPlayback(id); })
                .method("setPlaybackGain",
                    [](AudioCtxData* d, int id, double v) { d->engine->setPlaybackGain(id, static_cast<float>(v)); })
                .method("setPlaybackLoop",
                    [](AudioCtxData* d, int id, bool v) { d->engine->setPlaybackLoop(id, v); })
                .method("setPlaybackPlaying",
                    [](AudioCtxData* d, int id, bool v) { d->engine->setPlaybackPlaying(id, v); })
                .method("setPlaybackRegion",
                    [](AudioCtxData* d, int id, int start, int end) { d->engine->setPlaybackRegion(id, start, end); })
                .method("setPlaybackRate",
                    [](AudioCtxData* d, int id, double v) { d->engine->setPlaybackRate(id, static_cast<float>(v)); })
                .method("setPlaybackPan",
                    [](AudioCtxData* d, int id, double v) { d->engine->setPlaybackPan(id, static_cast<float>(v)); })
                .method("getPlaybackPosition",
                    [](AudioCtxData* d, int id) -> double { return d->engine->getPlaybackPosition(id); })
                .method("getPlaybackPositionSeconds",
                    [](AudioCtxData* d, int id) -> double { return d->engine->getPlaybackPositionSeconds(id); })
                .method("seekPlayback",
                    [](AudioCtxData* d, int id, double seconds) { d->engine->seekPlayback(id, seconds); });
        }
    
        // The AudioContext constructor created by qjsbind doesn't add the destination property.
        // We need to patch it so that `new AudioContext()` gets a destination property.
        // We do this by evaluating a small JS shim that wraps the constructor.
        {
            JSValue global = JS_GetGlobalObject(ctx);
    
            // Install native getUserMedia
            JS_SetPropertyStr(ctx, global, "__nativeGetUserMedia",
                              JS_NewCFunction(ctx, js_getUserMedia, "__nativeGetUserMedia", 1));
    
            JS_FreeValue(ctx, global);
        }
    
        // Wire destination and getUserMedia via JS shim
        const char* shim =
            "(function() {"
            "  var _OrigAudioContext = AudioContext;"
            "  globalThis.AudioContext = function() {"
            "    var ctx = new _OrigAudioContext();"
            "    ctx.destination = {};"
            "    return ctx;"
            "  };"
            "  globalThis.AudioContext.prototype = _OrigAudioContext.prototype;"
            "  if (typeof navigator !== 'undefined') {"
            "    if (!navigator.mediaDevices) navigator.mediaDevices = {};"
            "    navigator.mediaDevices.getUserMedia = globalThis.__nativeGetUserMedia;"
            "  }"
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

} // namespace bro::js
