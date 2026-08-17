// Web Audio & Sound Engine Integration for the bronze host.
//
// Bridges Web Audio (AudioContext, AudioNode, AudioParam, GainNode,
// OscillatorNode, BiquadFilterNode, AnalyserNode, AudioBuffer,
// AudioBufferSourceNode, etc.) to the broaudio engine (`hostEngine()->audioEngine()`).
//
// Follows bronze GC rules strictly:
// - Payload structs are plain host memory, freed by handle finalizers.
// - Finalizers never touch the embed API / never own Persistents.
// - Persistents and child objects live on JS properties.
// - Heap pointers from typedArrayInfo/arrayBufferInfo are consumed before any allocation.

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "engine/engine.h"
#include "util/log.h"

#include <broaudio/dsp/fft.h>
#include <broaudio/synth/wavetable.h>
#include <broaudio/dsp/resampler.h>
#include <broaudio/io/audio_file.h>
#include <broaudio/node/audio_node.h>
#include <broaudio/engine.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace bro::bronze_host {

namespace {

// ---------------------------------------------------------------------------
// Helpers: parse filter types and waveforms
// ---------------------------------------------------------------------------

static broaudio::BiquadFilter::Type parseFilterType(const std::string& str) {
    if (str == "highpass") return broaudio::BiquadFilter::Type::Highpass;
    if (str == "bandpass") return broaudio::BiquadFilter::Type::Bandpass;
    if (str == "notch") return broaudio::BiquadFilter::Type::Notch;
    if (str == "allpass") return broaudio::BiquadFilter::Type::Allpass;
    if (str == "peaking") return broaudio::BiquadFilter::Type::Peaking;
    if (str == "lowshelf") return broaudio::BiquadFilter::Type::Lowshelf;
    if (str == "highshelf") return broaudio::BiquadFilter::Type::Highshelf;
    return broaudio::BiquadFilter::Type::Lowpass;
}

static const char* filterTypeToString(broaudio::BiquadFilter::Type type) {
    switch (type) {
        case broaudio::BiquadFilter::Type::Highpass: return "highpass";
        case broaudio::BiquadFilter::Type::Bandpass: return "bandpass";
        case broaudio::BiquadFilter::Type::Notch: return "notch";
        case broaudio::BiquadFilter::Type::Allpass: return "allpass";
        case broaudio::BiquadFilter::Type::Peaking: return "peaking";
        case broaudio::BiquadFilter::Type::Lowshelf: return "lowshelf";
        case broaudio::BiquadFilter::Type::Highshelf: return "highshelf";
        case broaudio::BiquadFilter::Type::Lowpass:
        default:
            return "lowpass";
    }
}

static broaudio::Waveform parseWaveform(const std::string& str) {
    if (str == "square") return broaudio::Waveform::Square;
    if (str == "sawtooth") return broaudio::Waveform::Sawtooth;
    if (str == "triangle") return broaudio::Waveform::Triangle;
    if (str == "wavetable") return broaudio::Waveform::Wavetable;
    if (str == "whitenoise") return broaudio::Waveform::WhiteNoise;
    if (str == "pinknoise") return broaudio::Waveform::PinkNoise;
    if (str == "brownnoise") return broaudio::Waveform::BrownNoise;
    return broaudio::Waveform::Sine;
}

static const char* waveformToString(broaudio::Waveform wf) {
    switch (wf) {
        case broaudio::Waveform::Square: return "square";
        case broaudio::Waveform::Sawtooth: return "sawtooth";
        case broaudio::Waveform::Triangle: return "triangle";
        case broaudio::Waveform::Wavetable: return "wavetable";
        case broaudio::Waveform::WhiteNoise: return "whitenoise";
        case broaudio::Waveform::PinkNoise: return "pinknoise";
        case broaudio::Waveform::BrownNoise: return "brownnoise";
        case broaudio::Waveform::Sine:
        default:
            return "sine";
    }
}

// ---------------------------------------------------------------------------
// Structs & handle cells
// ---------------------------------------------------------------------------

struct HostAudioContext {
    uint32_t tag = kHostAudioContextTag;
};

enum class AudioNodeType : uint8_t {
    Destination = 0,
    Gain,
    Oscillator,
    BiquadFilter,
    Analyser,
    BufferSource,
    Generic,
};

struct HostAudioNode {
    uint32_t tag = kHostAudioNodeTag;
    AudioNodeType nodeType = AudioNodeType::Generic;
};

enum class AudioParamTarget : uint8_t {
    Generic = 0,
    Gain,
    VoiceFrequency,
    VoiceDetune,
    VoicePan,
    FilterFrequency,
    FilterQ,
    FilterGain,
    PlaybackRate,
    PlaybackDetune,
};

struct HostAudioParam {
    uint32_t tag = kHostAudioParamTag;
    AudioParamTarget target = AudioParamTarget::Generic;
    int targetId = -1;
    float value = 1.0f;
    float defaultValue = 1.0f;
    float minValue = -3.402823466e+38f;
    float maxValue = 3.402823466e+38f;
};

struct HostGainNode {
    HostAudioNode base;
};

struct HostOscillatorNode {
    HostAudioNode base;
    int voiceId = -1;
    std::string type = "sine";
    bool started = false;
    bool stopped = false;
};

struct HostBiquadFilterNode {
    HostAudioNode base;
    int slot = -1;
    std::string type = "lowpass";
};

struct HostAnalyserNode {
    HostAudioNode base;
    int fftSize = 2048;
    float minDecibels = -100.0f;
    float maxDecibels = -30.0f;
    float smoothingTimeConstant = 0.8f;
    std::vector<float> smoothedMagnitudes;
};

struct HostAudioBuffer {
    uint32_t tag = kHostAudioBufferTag;
    int numberOfChannels = 1;
    int length = 0;
    int sampleRate = 44100;
    std::vector<std::vector<float>> channels;
};

struct HostAudioBufferSourceNode {
    HostAudioNode base;
    HostAudioBuffer* buffer = nullptr;
    bool loop = false;
    double loopStart = 0.0;
    double loopEnd = 0.0;
    int clipId = -1;
    int playbackId = -1;
    bool started = false;
    bool stopped = false;
};

// ---------------------------------------------------------------------------
// Destructors (Finalizers) — Plain C++ only, no embed calls
// ---------------------------------------------------------------------------

void hostAudioContextDtor(void* p) {
    delete static_cast<HostAudioContext*>(p);
}

void hostAudioNodeDtor(void* p) {
    delete static_cast<HostAudioNode*>(p);
}

void hostAudioParamDtor(void* p) {
    delete static_cast<HostAudioParam*>(p);
}

void hostAudioBufferDtor(void* p) {
    delete static_cast<HostAudioBuffer*>(p);
}

void hostGainDtor(void* p) {
    delete static_cast<HostGainNode*>(p);
}

void hostOscillatorDtor(void* p) {
    auto* osc = static_cast<HostOscillatorNode*>(p);
    if (osc) {
        if (osc->voiceId >= 0) {
            auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
            if (e) {
                e->stopVoice(osc->voiceId, e->currentTime());
                e->removeVoice(osc->voiceId);
            }
        }
        delete osc;
    }
}

void hostBiquadFilterDtor(void* p) {
    auto* filter = static_cast<HostBiquadFilterNode*>(p);
    if (filter) {
        if (filter->slot >= 0) {
            auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
            if (e) {
                e->releaseFilterSlot(filter->slot);
            }
        }
        delete filter;
    }
}

void hostAnalyserDtor(void* p) {
    delete static_cast<HostAnalyserNode*>(p);
}

void hostAudioBufferSourceDtor(void* p) {
    auto* src = static_cast<HostAudioBufferSourceNode*>(p);
    if (src) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) {
            if (src->playbackId >= 0) e->stopPlayback(src->playbackId);
            if (src->clipId >= 0) e->deleteClip(src->clipId);
        }
        delete src;
    }
}

// ---------------------------------------------------------------------------
// Unwrap helpers
// ---------------------------------------------------------------------------

HostAudioContext* hostAudioContextOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* p = static_cast<HostAudioContext*>(ev::handleData(v));
    if (!p || p->tag != kHostAudioContextTag) return nullptr;
    return p;
}

HostAudioNode* hostAudioNodeOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* p = static_cast<HostAudioNode*>(ev::handleData(v));
    if (!p || p->tag != kHostAudioNodeTag) return nullptr;
    return p;
}

HostAudioParam* hostAudioParamOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* p = static_cast<HostAudioParam*>(ev::handleData(v));
    if (!p || p->tag != kHostAudioParamTag) return nullptr;
    return p;
}

HostAudioBuffer* hostAudioBufferOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* p = static_cast<HostAudioBuffer*>(ev::handleData(v));
    if (!p || p->tag != kHostAudioBufferTag) return nullptr;
    return p;
}

// ---------------------------------------------------------------------------
// AudioParam parameter sync
// ---------------------------------------------------------------------------

void syncAudioParamValue(HostAudioParam* p, float val) {
    p->value = std::clamp(val, p->minValue, p->maxValue);
    auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
    if (!e || p->targetId < 0) return;
    switch (p->target) {
        case AudioParamTarget::VoiceFrequency:
            e->setFrequency(p->targetId, p->value);
            break;
        case AudioParamTarget::VoicePan:
            e->setVoicePan(p->targetId, p->value);
            break;
        case AudioParamTarget::FilterFrequency:
            e->setFilterFrequency(p->targetId, p->value);
            break;
        case AudioParamTarget::FilterQ:
            e->setFilterQ(p->targetId, p->value);
            break;
        case AudioParamTarget::FilterGain:
            e->setFilterGain(p->targetId, p->value);
            break;
        case AudioParamTarget::PlaybackRate:
            e->setPlaybackRate(p->targetId, p->value);
            break;
        case AudioParamTarget::VoiceDetune:
        case AudioParamTarget::PlaybackDetune:
        case AudioParamTarget::Gain:
        case AudioParamTarget::Generic:
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// AudioNode base surface
// ---------------------------------------------------------------------------

void installAudioNodeCore(ObjectBuilder& b, HostAudioNode* node) {
    b.def("connect", 3, [](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::throwTypeError("AudioNode.connect: destination argument required");
        return a[0];
    });

    b.def("disconnect", 1, [](Value, std::span<const Value>) -> Value {
        return ev::undefined();
    });

    b.accessor("numberOfInputs", [node](Value, std::span<const Value>) {
        if (node->nodeType == AudioNodeType::Oscillator || node->nodeType == AudioNodeType::BufferSource) {
            return ev::fromDouble(0.0);
        }
        return ev::fromDouble(1.0);
    }, nullptr);

    b.accessor("numberOfOutputs", [node](Value, std::span<const Value>) {
        if (node->nodeType == AudioNodeType::Destination) {
            return ev::fromDouble(0.0);
        }
        return ev::fromDouble(1.0);
    }, nullptr);

    b.accessor("channelCount", [](Value, std::span<const Value>) {
        return ev::fromDouble(2.0);
    }, [](Value, std::span<const Value>) {
        return ev::undefined();
    });

    b.set("channelCountMode", ev::fromUtf8("max"));
    b.set("channelInterpretation", ev::fromUtf8("speakers"));
}

// ---------------------------------------------------------------------------
// AudioParam
// ---------------------------------------------------------------------------

Value makeAudioParamValue(AudioParamTarget target, int targetId,
                          float initialVal, float minVal, float maxVal, float defaultVal) {
    auto* p = new HostAudioParam();
    p->target = target;
    p->targetId = targetId;
    p->value = initialVal;
    p->defaultValue = defaultVal;
    p->minValue = minVal;
    p->maxValue = maxVal;

    ObjectBuilder b(ev::makeHandle(p, hostAudioParamDtor));

    b.accessor("value",
               [p](Value, std::span<const Value>) { return ev::fromDouble(p->value); },
               [p](Value, std::span<const Value> a) {
                   syncAudioParamValue(p, static_cast<float>(numAt(a, 0)));
                   return ev::undefined();
               });

    b.accessor("defaultValue", [p](Value, std::span<const Value>) {
        return ev::fromDouble(p->defaultValue);
    }, nullptr);

    b.accessor("minValue", [p](Value, std::span<const Value>) {
        return ev::fromDouble(p->minValue);
    }, nullptr);

    b.accessor("maxValue", [p](Value, std::span<const Value>) {
        return ev::fromDouble(p->maxValue);
    }, nullptr);

    // Automation methods (all return `this` AudioParam)
    b.def("setValueAtTime", 2, [p](Value thisValue, std::span<const Value> a) -> Value {
        syncAudioParamValue(p, static_cast<float>(numAt(a, 0)));
        return thisValue;
    });

    b.def("linearRampToValueAtTime", 2, [p](Value thisValue, std::span<const Value> a) -> Value {
        syncAudioParamValue(p, static_cast<float>(numAt(a, 0)));
        return thisValue;
    });

    b.def("exponentialRampToValueAtTime", 2, [p](Value thisValue, std::span<const Value> a) -> Value {
        syncAudioParamValue(p, static_cast<float>(numAt(a, 0)));
        return thisValue;
    });

    b.def("setTargetAtTime", 3, [p](Value thisValue, std::span<const Value> a) -> Value {
        syncAudioParamValue(p, static_cast<float>(numAt(a, 0)));
        return thisValue;
    });

    b.def("cancelScheduledValues", 1, [](Value thisValue, std::span<const Value>) -> Value {
        return thisValue;
    });

    b.def("setValueCurveAtTime", 3, [](Value thisValue, std::span<const Value>) -> Value {
        return thisValue;
    });

    return b.get();
}

// ---------------------------------------------------------------------------
// GainNode
// ---------------------------------------------------------------------------

Value makeGainNodeValue() {
    auto* gain = new HostGainNode();
    gain->base.nodeType = AudioNodeType::Gain;

    ObjectBuilder b(ev::makeHandle(gain, hostGainDtor));
    installAudioNodeCore(b, &gain->base);

    Value gainParam = makeAudioParamValue(AudioParamTarget::Gain, -1, 1.0f, -3.402823466e+38f, 3.402823466e+38f, 1.0f);
    b.set("gain", gainParam);

    return b.get();
}

// ---------------------------------------------------------------------------
// OscillatorNode
// ---------------------------------------------------------------------------

Value makeOscillatorNodeValue() {
    auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
    int voiceId = e ? e->createVoice() : -1;

    auto* osc = new HostOscillatorNode();
    osc->base.nodeType = AudioNodeType::Oscillator;
    osc->voiceId = voiceId;

    ObjectBuilder b(ev::makeHandle(osc, hostOscillatorDtor));
    installAudioNodeCore(b, &osc->base);

    b.accessor("type",
               [osc](Value, std::span<const Value>) {
                   return ev::fromUtf8(osc->type);
               },
               [osc](Value, std::span<const Value> a) {
                   if (a.empty() || ev::isObject(a[0]) || ev::isUndefined(a[0])) return ev::undefined();
                   std::string t = ev::toUtf8(a[0]);
                   osc->type = t;
                   auto* eng = hostEngine() ? hostEngine()->audioEngine() : nullptr;
                   if (eng && osc->voiceId >= 0) {
                       eng->setWaveform(osc->voiceId, parseWaveform(t));
                   }
                   return ev::undefined();
               });

    b.set("frequency", makeAudioParamValue(AudioParamTarget::VoiceFrequency, voiceId, 440.0f, 0.0f, 24000.0f, 440.0f));
    b.set("detune", makeAudioParamValue(AudioParamTarget::VoiceDetune, voiceId, 0.0f, -153600.0f, 153600.0f, 0.0f));

    b.def("start", 1, [osc](Value, std::span<const Value> a) -> Value {
        if (osc->started) return ev::throwError("OscillatorNode cannot be started more than once");
        osc->started = true;
        auto* eng = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (eng && osc->voiceId >= 0) {
            eng->startVoice(osc->voiceId, numAt(a, 0));
        }
        return ev::undefined();
    });

    b.def("stop", 1, [osc](Value, std::span<const Value> a) -> Value {
        osc->stopped = true;
        auto* eng = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (eng && osc->voiceId >= 0) {
            eng->stopVoice(osc->voiceId, numAt(a, 0));
        }
        return ev::undefined();
    });

    return b.get();
}

// ---------------------------------------------------------------------------
// BiquadFilterNode
// ---------------------------------------------------------------------------

Value makeBiquadFilterNodeValue() {
    auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
    int slot = e ? e->allocateFilterSlot() : -1;

    auto* filter = new HostBiquadFilterNode();
    filter->base.nodeType = AudioNodeType::BiquadFilter;
    filter->slot = slot;

    ObjectBuilder b(ev::makeHandle(filter, hostBiquadFilterDtor));
    installAudioNodeCore(b, &filter->base);

    b.accessor("type",
               [filter](Value, std::span<const Value>) {
                   return ev::fromUtf8(filter->type);
               },
               [filter](Value, std::span<const Value> a) {
                   if (a.empty() || ev::isObject(a[0]) || ev::isUndefined(a[0])) return ev::undefined();
                   std::string t = ev::toUtf8(a[0]);
                   filter->type = t;
                   auto* eng = hostEngine() ? hostEngine()->audioEngine() : nullptr;
                   if (eng && filter->slot >= 0) {
                       eng->setFilterType(filter->slot, parseFilterType(t));
                   }
                   return ev::undefined();
               });

    b.set("frequency", makeAudioParamValue(AudioParamTarget::FilterFrequency, slot, 350.0f, 10.0f, 24000.0f, 350.0f));
    b.set("Q", makeAudioParamValue(AudioParamTarget::FilterQ, slot, 1.0f, 0.0001f, 1000.0f, 1.0f));
    b.set("gain", makeAudioParamValue(AudioParamTarget::FilterGain, slot, 0.0f, -100.0f, 100.0f, 0.0f));

    return b.get();
}

// ---------------------------------------------------------------------------
// AnalyserNode
// ---------------------------------------------------------------------------

Value makeAnalyserNodeValue() {
    auto* analyser = new HostAnalyserNode();
    analyser->base.nodeType = AudioNodeType::Analyser;

    ObjectBuilder b(ev::makeHandle(analyser, hostAnalyserDtor));
    installAudioNodeCore(b, &analyser->base);

    b.accessor("fftSize",
               [analyser](Value, std::span<const Value>) {
                   return ev::fromDouble(analyser->fftSize);
               },
               [analyser](Value, std::span<const Value> a) {
                   int v = i32At(a, 0);
                   if (v >= 32 && v <= 32768 && (v & (v - 1)) == 0) {
                       analyser->fftSize = v;
                       analyser->smoothedMagnitudes.clear();
                   }
                   return ev::undefined();
               });

    b.accessor("frequencyBinCount", [analyser](Value, std::span<const Value>) {
        return ev::fromDouble(analyser->fftSize / 2);
    }, nullptr);

    b.accessor("minDecibels",
               [analyser](Value, std::span<const Value>) {
                   return ev::fromDouble(analyser->minDecibels);
               },
               [analyser](Value, std::span<const Value> a) {
                   analyser->minDecibels = static_cast<float>(numAt(a, 0));
                   return ev::undefined();
               });

    b.accessor("maxDecibels",
               [analyser](Value, std::span<const Value>) {
                   return ev::fromDouble(analyser->maxDecibels);
               },
               [analyser](Value, std::span<const Value> a) {
                   analyser->maxDecibels = static_cast<float>(numAt(a, 0));
                   return ev::undefined();
               });

    b.accessor("smoothingTimeConstant",
               [analyser](Value, std::span<const Value>) {
                   return ev::fromDouble(analyser->smoothingTimeConstant);
               },
               [analyser](Value, std::span<const Value> a) {
                   analyser->smoothingTimeConstant = static_cast<float>(std::clamp(numAt(a, 0), 0.0, 1.0));
                   return ev::undefined();
               });

    b.def("getFloatFrequencyData", 1, [analyser](Value, std::span<const Value> a) -> Value {
        Value arr = argAt(a, 0);
        if (!ev::isTypedArray(arr)) return ev::undefined();
        ev::TypedArrayInfo info = ev::typedArrayInfo(arr);
        if (!info || !info.data) return ev::undefined();

        int n = analyser->fftSize;
        int halfN = n / 2;
        std::vector<float> real(n, 0.0f), imag(n, 0.0f);
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) {
            e->outputBuffer().readLatest(real.data(), n);
        }

        // Apply Blackman window
        for (int i = 0; i < n; i++) {
            float w = 0.42f - 0.5f * std::cos(2.0f * static_cast<float>(M_PI) * i / (n - 1))
                            + 0.08f * std::cos(4.0f * static_cast<float>(M_PI) * i / (n - 1));
            real[i] *= w;
        }

        broaudio::fft(real.data(), imag.data(), n);

        if (analyser->smoothedMagnitudes.size() != static_cast<size_t>(halfN)) {
            analyser->smoothedMagnitudes.assign(halfN, -100.0f);
        }

        float sm = std::clamp(analyser->smoothingTimeConstant, 0.0f, 1.0f);
        std::vector<float> outData(halfN);
        for (int i = 0; i < halfN; i++) {
            float mag = std::sqrt(real[i] * real[i] + imag[i] * imag[i]) / (n / 2.0f);
            float db = (mag > 1e-6f) ? 20.0f * std::log10(mag) : -100.0f;
            analyser->smoothedMagnitudes[i] = sm * analyser->smoothedMagnitudes[i] + (1.0f - sm) * db;
            outData[i] = analyser->smoothedMagnitudes[i];
        }

        size_t count = std::min(static_cast<size_t>(info.elementCount), static_cast<size_t>(halfN));
        std::memcpy(info.data, outData.data(), count * sizeof(float));
        return ev::undefined();
    });

    b.def("getByteFrequencyData", 1, [analyser](Value, std::span<const Value> a) -> Value {
        Value arr = argAt(a, 0);
        if (!ev::isTypedArray(arr)) return ev::undefined();
        ev::TypedArrayInfo info = ev::typedArrayInfo(arr);
        if (!info || !info.data) return ev::undefined();

        int n = analyser->fftSize;
        int halfN = n / 2;
        std::vector<float> real(n, 0.0f), imag(n, 0.0f);
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) {
            e->outputBuffer().readLatest(real.data(), n);
        }

        for (int i = 0; i < n; i++) {
            float w = 0.42f - 0.5f * std::cos(2.0f * static_cast<float>(M_PI) * i / (n - 1))
                            + 0.08f * std::cos(4.0f * static_cast<float>(M_PI) * i / (n - 1));
            real[i] *= w;
        }

        broaudio::fft(real.data(), imag.data(), n);

        if (analyser->smoothedMagnitudes.size() != static_cast<size_t>(halfN)) {
            analyser->smoothedMagnitudes.assign(halfN, -100.0f);
        }

        float sm = std::clamp(analyser->smoothingTimeConstant, 0.0f, 1.0f);
        float minDb = analyser->minDecibels;
        float maxDb = analyser->maxDecibels;
        float range = (maxDb > minDb) ? (maxDb - minDb) : 1.0f;

        std::vector<uint8_t> outData(halfN);
        for (int i = 0; i < halfN; i++) {
            float mag = std::sqrt(real[i] * real[i] + imag[i] * imag[i]) / (n / 2.0f);
            float db = (mag > 1e-6f) ? 20.0f * std::log10(mag) : -100.0f;
            analyser->smoothedMagnitudes[i] = sm * analyser->smoothedMagnitudes[i] + (1.0f - sm) * db;
            float norm = (analyser->smoothedMagnitudes[i] - minDb) / range;
            norm = std::clamp(norm, 0.0f, 1.0f);
            outData[i] = static_cast<uint8_t>(norm * 255.0f);
        }

        size_t count = std::min(static_cast<size_t>(info.elementCount), static_cast<size_t>(halfN));
        std::memcpy(info.data, outData.data(), count);
        return ev::undefined();
    });

    b.def("getFloatTimeDomainData", 1, [analyser](Value, std::span<const Value> a) -> Value {
        Value arr = argAt(a, 0);
        if (!ev::isTypedArray(arr)) return ev::undefined();
        ev::TypedArrayInfo info = ev::typedArrayInfo(arr);
        if (!info || !info.data) return ev::undefined();

        int n = analyser->fftSize;
        std::vector<float> real(n, 0.0f);
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) {
            e->outputBuffer().readLatest(real.data(), n);
        }

        size_t count = std::min(static_cast<size_t>(info.elementCount), static_cast<size_t>(n));
        std::memcpy(info.data, real.data(), count * sizeof(float));
        return ev::undefined();
    });

    b.def("getByteTimeDomainData", 1, [analyser](Value, std::span<const Value> a) -> Value {
        Value arr = argAt(a, 0);
        if (!ev::isTypedArray(arr)) return ev::undefined();
        ev::TypedArrayInfo info = ev::typedArrayInfo(arr);
        if (!info || !info.data) return ev::undefined();

        int n = analyser->fftSize;
        std::vector<float> real(n, 0.0f);
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) {
            e->outputBuffer().readLatest(real.data(), n);
        }

        std::vector<uint8_t> byteData(n);
        for (int i = 0; i < n; i++) {
            float s = std::clamp(real[i], -1.0f, 1.0f);
            byteData[i] = static_cast<uint8_t>(std::clamp(static_cast<int>(s * 128.0f + 128.0f), 0, 255));
        }

        size_t count = std::min(static_cast<size_t>(info.elementCount), static_cast<size_t>(n));
        std::memcpy(info.data, byteData.data(), count);
        return ev::undefined();
    });

    return b.get();
}

// ---------------------------------------------------------------------------
// AudioBuffer
// ---------------------------------------------------------------------------

Value makeAudioBufferValue(int channels, int length, int sampleRate) {
    if (channels <= 0) channels = 1;
    if (length < 0) length = 0;
    if (sampleRate <= 0) sampleRate = 44100;

    auto* buf = new HostAudioBuffer();
    buf->numberOfChannels = channels;
    buf->length = length;
    buf->sampleRate = sampleRate;
    buf->channels.resize(channels, std::vector<float>(length, 0.0f));

    ObjectBuilder b(ev::makeHandle(buf, hostAudioBufferDtor));

    for (int c = 0; c < channels; ++c) {
        Value arr = ev::createTypedArray(ev::elements::Float32, length);
        b.set(("_ch" + std::to_string(c)).c_str(), arr);
    }

    b.accessor("numberOfChannels", [buf](Value, std::span<const Value>) {
        return ev::fromDouble(buf->numberOfChannels);
    }, nullptr);

    b.accessor("length", [buf](Value, std::span<const Value>) {
        return ev::fromDouble(buf->length);
    }, nullptr);

    b.accessor("sampleRate", [buf](Value, std::span<const Value>) {
        return ev::fromDouble(buf->sampleRate);
    }, nullptr);

    b.accessor("duration", [buf](Value, std::span<const Value>) {
        return ev::fromDouble(buf->sampleRate > 0 ? static_cast<double>(buf->length) / static_cast<double>(buf->sampleRate) : 0.0);
    }, nullptr);

    b.def("getChannelData", 1, [buf](Value thisValue, std::span<const Value> a) -> Value {
        int ch = i32At(a, 0);
        if (ch < 0 || ch >= buf->numberOfChannels) {
            return ev::throwRangeError("AudioBuffer.getChannelData: channel index out of range");
        }
        std::string key = "_ch" + std::to_string(ch);
        Value arr = ev::getProperty(thisValue, key);
        if (ev::isTypedArray(arr)) return arr;

        Value newArr = ev::createTypedArray(ev::elements::Float32, buf->length);
        if (ch < static_cast<int>(buf->channels.size()) && !buf->channels[ch].empty()) {
            ev::fillTypedArray(newArr, std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(buf->channels[ch].data()),
                buf->channels[ch].size() * sizeof(float)));
        }
        ev::Persistent root(thisValue);
        ev::Persistent arrRoot(newArr);
        ev::setProperty(root.get(), key, arrRoot.get());
        return arrRoot.get();
    });

    b.def("copyFromChannel", 3, [buf](Value thisValue, std::span<const Value> a) -> Value {
        Value dst = argAt(a, 0);
        int ch = i32At(a, 1);
        int startInChannel = a.size() >= 3 ? i32At(a, 2) : 0;
        if (ch < 0 || ch >= buf->numberOfChannels || startInChannel < 0 || startInChannel >= buf->length) {
            return ev::undefined();
        }
        if (!ev::isTypedArray(dst)) return ev::undefined();
        ev::TypedArrayInfo dstInfo = ev::typedArrayInfo(dst);
        if (!dstInfo || !dstInfo.data) return ev::undefined();

        std::string key = "_ch" + std::to_string(ch);
        Value srcArr = ev::getProperty(thisValue, key);
        if (ev::isTypedArray(srcArr)) {
            ev::TypedArrayInfo srcInfo = ev::typedArrayInfo(srcArr);
            if (srcInfo && srcInfo.data) {
                size_t toCopy = std::min(static_cast<size_t>(dstInfo.elementCount),
                                         static_cast<size_t>(buf->length - startInChannel));
                std::memcpy(dstInfo.data,
                            reinterpret_cast<const float*>(srcInfo.data) + startInChannel,
                            toCopy * sizeof(float));
            }
        }
        return ev::undefined();
    });

    b.def("copyToChannel", 3, [buf](Value thisValue, std::span<const Value> a) -> Value {
        Value src = argAt(a, 0);
        int ch = i32At(a, 1);
        int startInChannel = a.size() >= 3 ? i32At(a, 2) : 0;
        if (ch < 0 || ch >= buf->numberOfChannels || startInChannel < 0 || startInChannel >= buf->length) {
            return ev::undefined();
        }
        if (!ev::isTypedArray(src)) return ev::undefined();
        ev::TypedArrayInfo srcInfo = ev::typedArrayInfo(src);
        if (!srcInfo || !srcInfo.data) return ev::undefined();

        std::string key = "_ch" + std::to_string(ch);
        Value dstArr = ev::getProperty(thisValue, key);
        if (ev::isTypedArray(dstArr)) {
            ev::TypedArrayInfo dstInfo = ev::typedArrayInfo(dstArr);
            if (dstInfo && dstInfo.data) {
                size_t toCopy = std::min(static_cast<size_t>(srcInfo.elementCount),
                                         static_cast<size_t>(buf->length - startInChannel));
                std::memcpy(reinterpret_cast<float*>(dstInfo.data) + startInChannel,
                            srcInfo.data,
                            toCopy * sizeof(float));
            }
        }
        return ev::undefined();
    });

    return b.get();
}

// ---------------------------------------------------------------------------
// AudioBufferSourceNode
// ---------------------------------------------------------------------------

Value makeAudioBufferSourceNodeValue() {
    auto* src = new HostAudioBufferSourceNode();
    src->base.nodeType = AudioNodeType::BufferSource;

    ObjectBuilder b(ev::makeHandle(src, hostAudioBufferSourceDtor));
    installAudioNodeCore(b, &src->base);

    b.accessor("buffer",
               [](Value thisValue, std::span<const Value>) {
                   Value buf = ev::getProperty(thisValue, "_buffer");
                   return ev::isObject(buf) ? buf : ev::null();
               },
               [src](Value thisValue, std::span<const Value> a) {
                   Value v = argAt(a, 0);
                   ev::Persistent self(thisValue);
                   ev::Persistent bufVal(v);
                   if (auto* b = hostAudioBufferOf(v)) {
                       src->buffer = b;
                       ev::setProperty(self.get(), "_buffer", bufVal.get());
                   } else {
                       src->buffer = nullptr;
                       ev::setProperty(self.get(), "_buffer", ev::null());
                   }
                   return ev::undefined();
               });

    b.accessor("loop",
               [src](Value, std::span<const Value>) {
                   return ev::fromBool(src->loop);
               },
               [src](Value, std::span<const Value> a) {
                   src->loop = boolAt(a, 0);
                   auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
                   if (e && src->playbackId >= 0) {
                       e->setPlaybackLoop(src->playbackId, src->loop);
                   }
                   return ev::undefined();
               });

    b.accessor("loopStart",
               [src](Value, std::span<const Value>) { return ev::fromDouble(src->loopStart); },
               [src](Value, std::span<const Value> a) { src->loopStart = numAt(a, 0); return ev::undefined(); });

    b.accessor("loopEnd",
               [src](Value, std::span<const Value>) { return ev::fromDouble(src->loopEnd); },
               [src](Value, std::span<const Value> a) { src->loopEnd = numAt(a, 0); return ev::undefined(); });

    b.set("playbackRate", makeAudioParamValue(AudioParamTarget::PlaybackRate, -1, 1.0f, 0.0f, 1024.0f, 1.0f));
    b.set("detune", makeAudioParamValue(AudioParamTarget::PlaybackDetune, -1, 0.0f, -153600.0f, 153600.0f, 0.0f));

    b.def("start", 3, [src](Value thisValue, std::span<const Value> a) -> Value {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (!e) return ev::undefined();
        if (src->started) return ev::throwError("AudioBufferSourceNode cannot be started more than once");
        src->started = true;

        Value bufVal = ev::getProperty(thisValue, "_buffer");
        HostAudioBuffer* hostBuf = hostAudioBufferOf(bufVal);
        if (hostBuf && hostBuf->length > 0 && hostBuf->numberOfChannels > 0) {
            int channels = hostBuf->numberOfChannels;
            int frames = hostBuf->length;
            std::vector<std::vector<float>> chData(channels);
            for (int c = 0; c < channels; ++c) {
                chData[c].resize(frames, 0.0f);
                std::string key = "_ch" + std::to_string(c);
                Value arr = ev::getProperty(bufVal, key);
                if (ev::isTypedArray(arr)) {
                    ev::TypedArrayInfo info = ev::typedArrayInfo(arr);
                    if (info && info.data) {
                        size_t count = std::min(static_cast<size_t>(frames), static_cast<size_t>(info.elementCount));
                        std::memcpy(chData[c].data(), info.data, count * sizeof(float));
                    }
                } else if (c < static_cast<int>(hostBuf->channels.size())) {
                    chData[c] = hostBuf->channels[c];
                }
            }

            std::vector<float> interleaved(frames * channels);
            for (int f = 0; f < frames; ++f) {
                for (int c = 0; c < channels; ++c) {
                    interleaved[f * channels + c] = chData[c][f];
                }
            }

            src->clipId = e->createClip(interleaved.data(), frames * channels, channels);
            double when = numAt(a, 0);
            if (when > 0.0) {
                src->playbackId = e->playClipAt(src->clipId, when, 1.0f, src->loop);
            } else {
                src->playbackId = e->playClip(src->clipId, 1.0f, src->loop);
            }

            Value rateVal = ev::getProperty(thisValue, "playbackRate");
            if (auto* rateParam = hostAudioParamOf(rateVal)) {
                if (rateParam->value != 1.0f && src->playbackId >= 0) {
                    e->setPlaybackRate(src->playbackId, rateParam->value);
                }
            }

            double offset = numAt(a, 1);
            if (offset > 0.0 && src->playbackId >= 0) {
                e->seekPlayback(src->playbackId, offset);
            }
        }
        return ev::undefined();
    });

    b.def("stop", 1, [src](Value, std::span<const Value>) -> Value {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && src->playbackId >= 0) {
            e->stopPlayback(src->playbackId);
            src->playbackId = -1;
        }
        src->stopped = true;
        return ev::undefined();
    });

    return b.get();
}

// ---------------------------------------------------------------------------
// Destination Node & AudioListener
// ---------------------------------------------------------------------------

Value makeDestinationNodeValue() {
    auto* dest = new HostAudioNode();
    dest->nodeType = AudioNodeType::Destination;

    ObjectBuilder b(ev::makeHandle(dest, hostAudioNodeDtor));
    installAudioNodeCore(b, dest);
    b.accessor("maxChannelCount", [](Value, std::span<const Value>) {
        return ev::fromDouble(2.0);
    }, nullptr);

    return b.get();
}

Value makeListenerValue() {
    ObjectBuilder b;

    b.def("setPosition", 3, [](Value, std::span<const Value> a) -> Value {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) {
            e->setListenerPosition(static_cast<float>(numAt(a, 0)),
                                   static_cast<float>(numAt(a, 1)),
                                   static_cast<float>(numAt(a, 2)));
        }
        return ev::undefined();
    });

    b.def("setOrientation", 6, [](Value, std::span<const Value> a) -> Value {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) {
            e->setListenerOrientation(static_cast<float>(numAt(a, 0)),
                                      static_cast<float>(numAt(a, 1)),
                                      static_cast<float>(numAt(a, 2)),
                                      static_cast<float>(numAt(a, 3)),
                                      static_cast<float>(numAt(a, 4)),
                                      static_cast<float>(numAt(a, 5)));
        }
        return ev::undefined();
    });

    b.def("setVelocity", 3, [](Value, std::span<const Value> a) -> Value {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) {
            e->setListenerVelocity(static_cast<float>(numAt(a, 0)),
                                   static_cast<float>(numAt(a, 1)),
                                   static_cast<float>(numAt(a, 2)));
        }
        return ev::undefined();
    });

    b.def("setListenerPosition", 3, [](Value, std::span<const Value> a) -> Value {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) {
            e->setListenerPosition(static_cast<float>(numAt(a, 0)),
                                   static_cast<float>(numAt(a, 1)),
                                   static_cast<float>(numAt(a, 2)));
        }
        return ev::undefined();
    });

    b.def("setListenerOrientation", 6, [](Value, std::span<const Value> a) -> Value {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) {
            e->setListenerOrientation(static_cast<float>(numAt(a, 0)),
                                      static_cast<float>(numAt(a, 1)),
                                      static_cast<float>(numAt(a, 2)),
                                      static_cast<float>(numAt(a, 3)),
                                      static_cast<float>(numAt(a, 4)),
                                      static_cast<float>(numAt(a, 5)));
        }
        return ev::undefined();
    });

    b.set("positionX", makeAudioParamValue(AudioParamTarget::Generic, -1, 0.0f, -3.4e38f, 3.4e38f, 0.0f));
    b.set("positionY", makeAudioParamValue(AudioParamTarget::Generic, -1, 0.0f, -3.4e38f, 3.4e38f, 0.0f));
    b.set("positionZ", makeAudioParamValue(AudioParamTarget::Generic, -1, 0.0f, -3.4e38f, 3.4e38f, 0.0f));
    b.set("forwardX", makeAudioParamValue(AudioParamTarget::Generic, -1, 0.0f, -1.0f, 1.0f, 0.0f));
    b.set("forwardY", makeAudioParamValue(AudioParamTarget::Generic, -1, 0.0f, -1.0f, 1.0f, 0.0f));
    b.set("forwardZ", makeAudioParamValue(AudioParamTarget::Generic, -1, -1.0f, -1.0f, 1.0f, -1.0f));
    b.set("upX", makeAudioParamValue(AudioParamTarget::Generic, -1, 0.0f, -1.0f, 1.0f, 0.0f));
    b.set("upY", makeAudioParamValue(AudioParamTarget::Generic, -1, 1.0f, -1.0f, 1.0f, 1.0f));
    b.set("upZ", makeAudioParamValue(AudioParamTarget::Generic, -1, 0.0f, -1.0f, 1.0f, 0.0f));

    return b.get();
}

// ---------------------------------------------------------------------------
// decodeAudioData
// ---------------------------------------------------------------------------

Value audioCtxDecodeAudioData(Value, std::span<const Value> a) {
    Value inputV = argAt(a, 0);
    Value successCb = argAt(a, 1);
    Value errorCb = argAt(a, 2);

    ev::Persistent promise(ev::createPromise());
    ev::Persistent successP(ev::isFunction(successCb) ? successCb : ev::undefined());
    ev::Persistent errorP(ev::isFunction(errorCb) ? errorCb : ev::undefined());

    const uint8_t* rawData = nullptr;
    size_t rawLen = 0;
    size_t elemSize = 1;

    if (!bufferBytes(inputV, &rawData, &rawLen, &elemSize) || rawLen == 0) {
        Value err = hostMakeDomError("DataCloneError", "decodeAudioData: invalid or empty buffer");
        ev::Persistent errP(err);
        ev::rejectPromise(promise.get(), errP.get());
        if (ev::isFunction(errorP.get())) {
            Value evErr = errP.get();
            ev::call(errorP.get(), ev::undefined(), std::span<const Value>(&evErr, 1));
        }
        return promise.get();
    }

    broaudio::AudioFileData data = broaudio::loadAudioFileFromMemory(rawData, rawLen);
    if (!data.valid()) {
        std::string msg = data.error.empty() ? "decodeAudioData: failed to decode audio" : data.error;
        Value err = hostMakeDomError("EncodingError", msg);
        ev::Persistent errP(err);
        ev::rejectPromise(promise.get(), errP.get());
        if (ev::isFunction(errorP.get())) {
            Value evErr = errP.get();
            ev::call(errorP.get(), ev::undefined(), std::span<const Value>(&evErr, 1));
        }
        return promise.get();
    }

    Value bufferVal = makeAudioBufferValue(data.channels, data.numFrames, data.sampleRate);
    ev::Persistent bufferP(bufferVal);
    HostAudioBuffer* hostBuf = hostAudioBufferOf(bufferP.get());
    if (hostBuf) {
        int chs = data.channels;
        int frames = data.numFrames;
        for (int c = 0; c < chs; ++c) {
            std::vector<float>& chData = hostBuf->channels[c];
            chData.resize(frames);
            for (int f = 0; f < frames; ++f) {
                chData[f] = data.samples[f * chs + c];
            }
            Value arr = ev::getProperty(bufferP.get(), "_ch" + std::to_string(c));
            if (ev::isTypedArray(arr)) {
                ev::fillTypedArray(arr, std::span<const uint8_t>(
                    reinterpret_cast<const uint8_t*>(chData.data()),
                    frames * sizeof(float)));
            }
        }
    }

    ev::resolvePromise(promise.get(), bufferP.get());
    if (ev::isFunction(successP.get())) {
        Value bufV = bufferP.get();
        ev::call(successP.get(), ev::undefined(), std::span<const Value>(&bufV, 1));
    }

    return promise.get();
}

// ---------------------------------------------------------------------------
// AudioContext instance builder
// ---------------------------------------------------------------------------

Value makeAudioContextValue() {
    auto* ctx = new HostAudioContext();
    ObjectBuilder b(ev::makeHandle(ctx, hostAudioContextDtor));

    // Standard properties
    b.accessor("currentTime", [](Value, std::span<const Value>) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e ? e->currentTime() : 0.0);
    }, nullptr);

    b.accessor("sampleRate", [](Value, std::span<const Value>) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e ? static_cast<double>(e->sampleRate()) : 44100.0);
    }, nullptr);

    b.accessor("state", [](Value, std::span<const Value>) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && e->masterPaused()) return ev::fromUtf8("suspended");
        return ev::fromUtf8("running");
    }, nullptr);

    b.accessor("outputLatency", [](Value, std::span<const Value>) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e ? e->outputLatencySeconds() : 0.0);
    }, nullptr);

    b.accessor("masterGain",
               [](Value, std::span<const Value>) {
                   auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
                   return ev::fromDouble(e ? e->masterGain() : 1.0);
               },
               [](Value, std::span<const Value> a) {
                   auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
                   if (e) e->setMasterGain(static_cast<float>(numAt(a, 0)));
                   return ev::undefined();
               });

    b.set("destination", makeDestinationNodeValue());
    b.set("listener", makeListenerValue());

    // Node factories
    b.def("createGain", 0, [](Value, std::span<const Value>) { return makeGainNodeValue(); });
    b.def("createOscillator", 0, [](Value, std::span<const Value>) { return makeOscillatorNodeValue(); });
    b.def("createBiquadFilter", 0, [](Value, std::span<const Value>) { return makeBiquadFilterNodeValue(); });
    b.def("createAnalyser", 0, [](Value, std::span<const Value>) { return makeAnalyserNodeValue(); });
    b.def("createBufferSource", 0, [](Value, std::span<const Value>) { return makeAudioBufferSourceNodeValue(); });

    b.def("createBuffer", 3, [](Value, std::span<const Value> a) {
        int channels = i32At(a, 0);
        int length = i32At(a, 1);
        int sampleRate = i32At(a, 2);
        return makeAudioBufferValue(channels, length, sampleRate);
    });

    b.def("decodeAudioData", 3, audioCtxDecodeAudioData);

    // Lifecycle methods (return resolved Promises)
    b.def("resume", 0, [](Value, std::span<const Value>) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setMasterPaused(false);
        ev::Persistent p(ev::createPromise());
        ev::resolvePromise(p.get(), ev::undefined());
        return p.get();
    });

    b.def("suspend", 0, [](Value, std::span<const Value>) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setMasterPaused(true);
        ev::Persistent p(ev::createPromise());
        ev::resolvePromise(p.get(), ev::undefined());
        return p.get();
    });

    b.def("close", 0, [](Value, std::span<const Value>) {
        ev::Persistent p(ev::createPromise());
        ev::resolvePromise(p.get(), ev::undefined());
        return p.get();
    });

    // Bro Mix Bus & Effects methods
    b.def("createBus", 0, [](Value, std::span<const Value>) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e ? e->createBus() : -1);
    });

    b.def("deleteBus", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->deleteBus(i32At(a, 0));
        return ev::undefined();
    });

    b.def("setBusGain", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setBusGain(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setBusPan", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setBusPan(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setBusMuted", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setBusMuted(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("setBusSolo", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setBusSolo(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("getBusSolo", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromBool(e ? e->getBusSolo(i32At(a, 0)) : false);
    });

    b.def("setBusDelayEnabled", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setBusDelayEnabled(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("setBusDelayTime", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setBusDelayTime(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setBusDelayFeedback", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setBusDelayFeedback(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setBusDelayMix", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setBusDelayMix(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setBusReverbEnabled", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setBusReverbEnabled(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("setBusReverbRoomSize", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setBusReverbRoomSize(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setBusReverbDamping", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setBusReverbDamping(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setBusReverbMix", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setBusReverbMix(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setBusChorusEnabled", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setBusChorusEnabled(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("setBusChorusRate", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setBusChorusRate(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setBusChorusDepth", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setBusChorusDepth(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setBusChorusMix", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setBusChorusMix(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setBusChorusFeedback", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setBusChorusFeedback(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setBusCompressorEnabled", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setBusCompressorEnabled(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("setBusCompressorThreshold", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setBusCompressorThreshold(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setBusCompressorRatio", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setBusCompressorRatio(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setBusCompressorAttack", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setBusCompressorAttack(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setBusCompressorRelease", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setBusCompressorRelease(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setDelayEnabled", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setDelayEnabled(boolAt(a, 0));
        return ev::undefined();
    });

    b.def("setDelayTime", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setDelayTime(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("setDelayFeedback", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setDelayFeedback(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("setDelayMix", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setDelayMix(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("setReverbEnabled", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setBusReverbEnabled(0, boolAt(a, 0));
        return ev::undefined();
    });

    b.def("getBusPeakL", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e ? e->getBusPeakL(i32At(a, 0)) : 0.0);
    });

    b.def("getBusPeakR", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e ? e->getBusPeakR(i32At(a, 0)) : 0.0);
    });

    b.def("getBusRmsL", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e ? e->getBusRmsL(i32At(a, 0)) : 0.0);
    });

    b.def("getBusRmsR", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e ? e->getBusRmsR(i32At(a, 0)) : 0.0);
    });

    // Bro Clip API
    b.def("createClip", 3, [](Value, std::span<const Value> a) -> Value {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (!e || a.empty()) return ev::fromDouble(-1);

        Value first = a[0];
        if (auto* hostBuf = hostAudioBufferOf(first)) {
            int channels = hostBuf->numberOfChannels;
            int frames = hostBuf->length;
            if (frames <= 0 || channels <= 0) return ev::fromDouble(-1);

            std::vector<std::vector<float>> chData(channels);
            for (int c = 0; c < channels; ++c) {
                chData[c].resize(frames, 0.0f);
                std::string key = "_ch" + std::to_string(c);
                Value arr = ev::getProperty(first, key);
                if (ev::isTypedArray(arr)) {
                    ev::TypedArrayInfo info = ev::typedArrayInfo(arr);
                    if (info && info.data) {
                        size_t count = std::min(static_cast<size_t>(frames), static_cast<size_t>(info.elementCount));
                        std::memcpy(chData[c].data(), info.data, count * sizeof(float));
                    }
                } else if (c < static_cast<int>(hostBuf->channels.size())) {
                    chData[c] = hostBuf->channels[c];
                }
            }

            std::vector<float> interleaved(frames * channels);
            for (int f = 0; f < frames; ++f) {
                for (int c = 0; c < channels; ++c) {
                    interleaved[f * channels + c] = chData[c][f];
                }
            }

            int clipId = e->createClip(interleaved.data(), frames * channels, channels);
            return ev::fromDouble(clipId);
        }

        const uint8_t* rawData = nullptr;
        size_t rawLen = 0;
        size_t elemSize = 1;
        if (!bufferBytes(first, &rawData, &rawLen, &elemSize) || rawLen == 0) {
            return ev::throwTypeError("createClip: expected AudioBuffer or Float32Array");
        }

        int numSamples = static_cast<int>(rawLen / sizeof(float));
        int channels = a.size() >= 2 ? i32At(a, 1) : 1;
        if (channels <= 0) channels = 1;

        const float* samples = reinterpret_cast<const float*>(rawData);
        std::vector<float> resampled;
        if (a.size() >= 3 && !ev::isUndefined(a[2])) {
            int srcRate = i32At(a, 2);
            int engRate = e->sampleRate();
            if (srcRate > 0 && srcRate != engRate && channels > 0) {
                resampled = broaudio::resample(samples, numSamples / channels, channels, srcRate, engRate);
                if (!resampled.empty()) {
                    samples = resampled.data();
                    numSamples = static_cast<int>(resampled.size());
                }
            }
        }

        int clipId = e->createClip(samples, numSamples, channels);
        return ev::fromDouble(clipId);
    });

    b.def("deleteClip", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->deleteClip(i32At(a, 0));
        return ev::undefined();
    });

    b.def("playClip", 4, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (!e || a.empty()) return ev::fromDouble(-1);
        int clipId = i32At(a, 0);
        float gain = a.size() >= 2 ? static_cast<float>(numAt(a, 1)) : 1.0f;
        bool loop = false;
        float pan = 0.0f;

        if (a.size() >= 3) {
            if (a[2].isBool()) {
                loop = boolAt(a, 2);
            } else {
                pan = static_cast<float>(numAt(a, 2));
                if (a.size() >= 4) loop = boolAt(a, 3);
            }
        }

        int playbackId = e->playClip(clipId, gain, loop);
        if (pan != 0.0f && playbackId >= 0) {
            e->setPlaybackPan(playbackId, pan);
        }
        return ev::fromDouble(playbackId);
    });

    b.def("stopPlayback", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->stopPlayback(i32At(a, 0));
        return ev::undefined();
    });

    b.def("createClipFromFile", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (!e || a.empty()) return ev::fromDouble(-1);
        std::string path = ev::toUtf8(a[0]);
        int clipId = e->createClipFromFile(path.c_str());
        return ev::fromDouble(clipId);
    });

    b.def("getClipSampleCount", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e ? e->getClipSampleCount(i32At(a, 0)) : 0);
    });

    b.def("getClipChannels", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e ? e->getClipChannels(i32At(a, 0)) : 0);
    });

    b.def("getClipWaveform", 2, [](Value, std::span<const Value> a) -> Value {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (!e || a.size() < 2) return ev::null();
        int clipId = i32At(a, 0);
        int numBins = i32At(a, 1);
        if (numBins <= 0 || numBins > 1024) return ev::null();

        auto wf = e->getClipWaveform(clipId, numBins);
        if (wf.empty()) return ev::null();

        Value arr = ev::createTypedArray(ev::elements::Float32, static_cast<uint32_t>(wf.size()));
        ev::fillTypedArray(arr, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(wf.data()), wf.size() * sizeof(float)));
        return arr;
    });

    // Spatial helpers
    b.def("setListenerPosition", 3, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setListenerPosition(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)));
        return ev::undefined();
    });

    b.def("setListenerOrientation", 6, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setListenerOrientation(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)),
                                         static_cast<float>(numAt(a, 3)), static_cast<float>(numAt(a, 4)), static_cast<float>(numAt(a, 5)));
        return ev::undefined();
    });

    b.def("setVoiceBus", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setVoiceBus(i32At(a, 0), i32At(a, 1));
        return ev::undefined();
    });

    b.def("setPlaybackBus", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setPlaybackBus(i32At(a, 0), i32At(a, 1));
        return ev::undefined();
    });

    return b.get();
}

}  // namespace

// ---------------------------------------------------------------------------
// Public Constructor & Globals Installer
// ---------------------------------------------------------------------------

Value makeAudioContextConstructor() {
    return ev::makeFunction([](Value, std::span<const Value>) {
        return makeAudioContextValue();
    }, 0);
}

Value makeAudioBufferConstructor() {
    return ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        int length = 0;
        int channels = 1;
        int sampleRate = 44100;
        if (!a.empty() && ev::isObject(a[0])) {
            Value opt = a[0];
            Value lenV = ev::getProperty(opt, "length");
            if (!ev::isUndefined(lenV) && !ev::isObject(lenV)) length = static_cast<int>(ev::toDouble(lenV));
            Value chV = ev::getProperty(opt, "numberOfChannels");
            if (!ev::isUndefined(chV) && !ev::isObject(chV)) channels = static_cast<int>(ev::toDouble(chV));
            Value srV = ev::getProperty(opt, "sampleRate");
            if (!ev::isUndefined(srV) && !ev::isObject(srV)) sampleRate = static_cast<int>(ev::toDouble(srV));
        }
        if (length <= 0) return ev::throwTypeError("AudioBuffer: length must be positive");
        return makeAudioBufferValue(channels, length, sampleRate);
    }, 1);
}

void installAudioGlobals() {
    Value audioCtx = makeAudioContextConstructor();
    ev::registerGlobal("AudioContext", audioCtx);
    ev::registerGlobal("webkitAudioContext", audioCtx);
    ev::registerGlobal("AudioNode", makeBrandConstructor("AudioNode"));
    ev::registerGlobal("AudioParam", makeBrandConstructor("AudioParam"));
    ev::registerGlobal("GainNode", makeBrandConstructor("GainNode"));
    ev::registerGlobal("OscillatorNode", makeBrandConstructor("OscillatorNode"));
    ev::registerGlobal("AudioBuffer", makeAudioBufferConstructor());
    ev::registerGlobal("AudioBufferSourceNode", makeBrandConstructor("AudioBufferSourceNode"));
    ev::registerGlobal("BiquadFilterNode", makeBrandConstructor("BiquadFilterNode"));
    ev::registerGlobal("AnalyserNode", makeBrandConstructor("AnalyserNode"));
    ev::registerGlobal("PannerNode", makeBrandConstructor("PannerNode"));
    ev::registerGlobal("StereoPannerNode", makeBrandConstructor("StereoPannerNode"));
}

}  // namespace bro::bronze_host
