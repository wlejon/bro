// Web Audio & Sound Engine Integration — Core Subsystem
//
// AudioContext, AudioNode base, AudioParam automation, AudioBuffer,
// AudioDestinationNode, AudioListener, decodeAudioData, and globals installation.

#include "bronze_host/host_audio_internal.h"

namespace bro::bronze_host {

// ---------------------------------------------------------------------------
// HostClass Storage Definitions
// ---------------------------------------------------------------------------

HostClass g_audioNodeClass;
HostClass g_audioParamClass;
HostClass g_audioContextClass;
HostClass g_audioBufferClass;
HostClass g_gainNodeClass;
HostClass g_oscillatorNodeClass;
HostClass g_periodicWaveClass;
HostClass g_biquadFilterNodeClass;
HostClass g_analyserNodeClass;
HostClass g_audioBufferSourceNodeClass;
HostClass g_pannerNodeClass;
HostClass g_stereoPannerNodeClass;
HostClass g_delayNodeClass;
HostClass g_dynamicsCompressorNodeClass;
HostClass g_waveShaperNodeClass;
HostClass g_convolverNodeClass;
HostClass g_channelSplitterNodeClass;
HostClass g_channelMergerNodeClass;

// ---------------------------------------------------------------------------
// Core Destructors (Finalizers)
// ---------------------------------------------------------------------------

void hostAudioContextDtor(void* p) {
    delete static_cast<HostAudioContext*>(p);
}

void hostAudioNodeDtor(void* p) {
    delete static_cast<HostAudioNode*>(p);
}

// ---------------------------------------------------------------------------
// Core Unwrap Helpers
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

// ---------------------------------------------------------------------------
// AudioNode Base Surface
// ---------------------------------------------------------------------------

void decorateAudioNodeProto(ObjectBuilder& b) {
    b.def("connect", 3, [](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::throwTypeError("AudioNode.connect: destination argument required");
        return a[0];
    });

    b.def("disconnect", 1, [](Value, std::span<const Value>) -> Value {
        return ev::undefined();
    });

    b.accessor("numberOfInputs", [](Value self_, std::span<const Value>) {
        HostAudioNode* node = hostAudioNodeOf(self_);
        if (!node) return ev::fromDouble(1.0);
        if (node->nodeType == AudioNodeType::Oscillator ||
            node->nodeType == AudioNodeType::BufferSource) {
            return ev::fromDouble(0.0);
        }
        if (node->nodeType == AudioNodeType::ChannelMerger) {
            auto* m = reinterpret_cast<HostChannelMergerNode*>(node);
            return ev::fromDouble(m->numberOfInputs);
        }
        return ev::fromDouble(1.0);
    }, nullptr);

    b.accessor("numberOfOutputs", [](Value self_, std::span<const Value>) {
        HostAudioNode* node = hostAudioNodeOf(self_);
        if (!node) return ev::fromDouble(1.0);
        if (node->nodeType == AudioNodeType::Destination) {
            return ev::fromDouble(0.0);
        }
        if (node->nodeType == AudioNodeType::ChannelSplitter) {
            auto* s = reinterpret_cast<HostChannelSplitterNode*>(node);
            return ev::fromDouble(s->numberOfOutputs);
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
// Destination Node & AudioListener
// ---------------------------------------------------------------------------

Value makeDestinationNodeValue() {
    auto* dest = new HostAudioNode();
    dest->nodeType = AudioNodeType::Destination;

    ObjectBuilder b(g_audioNodeClass.make(dest, hostAudioNodeDtor));
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
// AudioContext Prototype Decoration
// ---------------------------------------------------------------------------

void decorateAudioContextProto(ObjectBuilder& b) {
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
    b.accessor("baseLatency", [](Value, std::span<const Value>) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        double sr = (e && e->sampleRate() > 0) ? static_cast<double>(e->sampleRate()) : 44100.0;
        return ev::fromDouble(512.0 / sr);
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

    // Standard node factories
    b.def("createGain", 0, [](Value, std::span<const Value>) { return makeGainNodeValue(); });
    b.def("createOscillator", 0, [](Value, std::span<const Value>) { return makeOscillatorNodeValue(); });
    b.def("createPeriodicWave", 3, [](Value, std::span<const Value> a) {
        std::vector<float> rStorage, iStorage;
        const float* rData = nullptr;
        const float* iData = nullptr;
        size_t rCount = 0, iCount = 0;
        if (!a.empty()) floatData(a[0], rStorage, &rData, &rCount);
        if (a.size() >= 2) floatData(a[1], iStorage, &iData, &iCount);
        bool disableNorm = false;
        if (a.size() >= 3 && ev::isObject(a[2])) {
            Value opt = a[2];
            Value dn = ev::getProperty(opt, "disableNormalization");
            if (!ev::isUndefined(dn)) disableNorm = ev::toBool(dn);
        }
        int count = static_cast<int>(std::max(rCount, iCount));
        return makePeriodicWaveValue(rData, iData, count, disableNorm);
    });
    b.def("createBiquadFilter", 0, [](Value, std::span<const Value>) { return makeBiquadFilterNodeValue(); });
    b.def("createAnalyser", 0, [](Value, std::span<const Value>) { return makeAnalyserNodeValue(); });
    b.def("createBufferSource", 0, [](Value, std::span<const Value>) { return makeAudioBufferSourceNodeValue(); });
    b.def("createBuffer", 3, [](Value, std::span<const Value> a) {
        int channels = i32At(a, 0);
        int length = i32At(a, 1);
        int sampleRate = i32At(a, 2);
        return makeAudioBufferValue(channels, length, sampleRate);
    });
    b.def("createPanner", 0, [](Value, std::span<const Value>) { return makePannerNodeValue(); });
    b.def("createStereoPanner", 0, [](Value, std::span<const Value>) { return makeStereoPannerNodeValue(); });
    b.def("createDelay", 1, [](Value, std::span<const Value> a) {
        double maxDelay = a.empty() ? 1.0 : numAt(a, 0);
        return makeDelayNodeValue(maxDelay);
    });
    b.def("createDynamicsCompressor", 0, [](Value, std::span<const Value>) { return makeDynamicsCompressorNodeValue(); });
    b.def("createWaveShaper", 0, [](Value, std::span<const Value>) { return makeWaveShaperNodeValue(); });
    b.def("createConvolver", 0, [](Value, std::span<const Value>) { return makeConvolverNodeValue(); });
    b.def("createChannelSplitter", 1, [](Value, std::span<const Value> a) {
        int outputs = a.empty() ? 6 : i32At(a, 0);
        return makeChannelSplitterNodeValue(outputs);
    });
    b.def("createChannelMerger", 1, [](Value, std::span<const Value> a) {
        int inputs = a.empty() ? 6 : i32At(a, 0);
        return makeChannelMergerNodeValue(inputs);
    });

    b.def("decodeAudioData", 3, audioCtxDecodeAudioData);

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
}

Value makeAudioContextValue() {
    auto* ctx = new HostAudioContext();
    ObjectBuilder b(g_audioContextClass.make(ctx, hostAudioContextDtor));

    b.set("destination", makeDestinationNodeValue());
    b.set("listener", makeListenerValue());

    return b.get();
}

// ---------------------------------------------------------------------------
// Audio Globals Installation
// ---------------------------------------------------------------------------

void installAudioGlobals() {
    // 1. AudioContext and alias webkitAudioContext
    g_audioContextClass.install(
        "AudioContext", 0,
        [](Value, std::span<const Value>) { return makeAudioContextValue(); },
        decorateAudioContextProto);
    g_audioContextClass.alias("webkitAudioContext");

    // 2. AudioNode and AudioParam base surfaces
    g_audioNodeClass.install("AudioNode", 0, nullptr, decorateAudioNodeProto);
    g_audioParamClass.install("AudioParam", 0, nullptr, decorateAudioParamProto);

    // 3. GainNode
    g_gainNodeClass.install("GainNode", 0,
        [](Value, std::span<const Value>) { return makeGainNodeValue(); },
        nullptr);
    g_gainNodeClass.inherit(g_audioNodeClass);

    // 4. OscillatorNode
    g_oscillatorNodeClass.install("OscillatorNode", 0,
        [](Value, std::span<const Value>) { return makeOscillatorNodeValue(); },
        decorateOscillatorNodeProto);
    g_oscillatorNodeClass.inherit(g_audioNodeClass);

    // 5. AudioBuffer
    g_audioBufferClass.install(
        "AudioBuffer", 1,
        [](Value, std::span<const Value> a) -> Value {
            int length = 0;
            int channels = 1;
            int sampleRate = 44100;
            if (!a.empty() && ev::isObject(a[0])) {
                Value opt = a[0];
                Value lenV = ev::getProperty(opt, "length");
                if (!ev::isUndefined(lenV) && !ev::isObject(lenV))
                    length = static_cast<int>(ev::toDouble(lenV));
                Value chV = ev::getProperty(opt, "numberOfChannels");
                if (!ev::isUndefined(chV) && !ev::isObject(chV))
                    channels = static_cast<int>(ev::toDouble(chV));
                Value srV = ev::getProperty(opt, "sampleRate");
                if (!ev::isUndefined(srV) && !ev::isObject(srV))
                    sampleRate = static_cast<int>(ev::toDouble(srV));
            }
            if (length <= 0) {
                return ev::throwTypeError("AudioBuffer: length must be positive");
            }
            return makeAudioBufferValue(channels, length, sampleRate);
        },
        decorateAudioBufferProto);

    // 6. AudioBufferSourceNode
    g_audioBufferSourceNodeClass.install("AudioBufferSourceNode", 0,
        [](Value, std::span<const Value>) { return makeAudioBufferSourceNodeValue(); },
        decorateAudioBufferSourceNodeProto);
    g_audioBufferSourceNodeClass.inherit(g_audioNodeClass);

    // 7. BiquadFilterNode
    g_biquadFilterNodeClass.install("BiquadFilterNode", 0,
        [](Value, std::span<const Value>) { return makeBiquadFilterNodeValue(); },
        decorateBiquadFilterNodeProto);
    g_biquadFilterNodeClass.inherit(g_audioNodeClass);

    // 8. AnalyserNode
    g_analyserNodeClass.install("AnalyserNode", 0,
        [](Value, std::span<const Value>) { return makeAnalyserNodeValue(); },
        decorateAnalyserNodeProto);
    g_analyserNodeClass.inherit(g_audioNodeClass);

    // 9. PannerNode & StereoPannerNode
    g_pannerNodeClass.install("PannerNode", 0,
        [](Value, std::span<const Value>) { return makePannerNodeValue(); },
        decoratePannerNodeProto);
    g_pannerNodeClass.inherit(g_audioNodeClass);

    g_stereoPannerNodeClass.install("StereoPannerNode", 0,
        [](Value, std::span<const Value>) { return makeStereoPannerNodeValue(); },
        decorateStereoPannerNodeProto);
    g_stereoPannerNodeClass.inherit(g_audioNodeClass);

    // 10. DelayNode
    g_delayNodeClass.install("DelayNode", 0,
        [](Value, std::span<const Value> a) {
            double maxDelay = a.empty() ? 1.0 : numAt(a, 0);
            return makeDelayNodeValue(maxDelay);
        },
        decorateDelayNodeProto);
    g_delayNodeClass.inherit(g_audioNodeClass);

    // 11. DynamicsCompressorNode
    g_dynamicsCompressorNodeClass.install("DynamicsCompressorNode", 0,
        [](Value, std::span<const Value>) { return makeDynamicsCompressorNodeValue(); },
        decorateDynamicsCompressorNodeProto);
    g_dynamicsCompressorNodeClass.inherit(g_audioNodeClass);

    // 12. WaveShaperNode
    g_waveShaperNodeClass.install("WaveShaperNode", 0,
        [](Value, std::span<const Value>) { return makeWaveShaperNodeValue(); },
        decorateWaveShaperNodeProto);
    g_waveShaperNodeClass.inherit(g_audioNodeClass);

    // 13. ConvolverNode
    g_convolverNodeClass.install("ConvolverNode", 0,
        [](Value, std::span<const Value>) { return makeConvolverNodeValue(); },
        decorateConvolverNodeProto);
    g_convolverNodeClass.inherit(g_audioNodeClass);

    // 14. ChannelSplitterNode & ChannelMergerNode
    g_channelSplitterNodeClass.install("ChannelSplitterNode", 0,
        [](Value, std::span<const Value> a) {
            int outputs = a.empty() ? 6 : i32At(a, 0);
            return makeChannelSplitterNodeValue(outputs);
        },
        decorateChannelSplitterNodeProto);
    g_channelSplitterNodeClass.inherit(g_audioNodeClass);

    g_channelMergerNodeClass.install("ChannelMergerNode", 0,
        [](Value, std::span<const Value> a) {
            int inputs = a.empty() ? 6 : i32At(a, 0);
            return makeChannelMergerNodeValue(inputs);
        },
        decorateChannelMergerNodeProto);
    g_channelMergerNodeClass.inherit(g_audioNodeClass);

    // 15. PeriodicWave
    g_periodicWaveClass.install("PeriodicWave", 0,
        [](Value, std::span<const Value> a) {
            std::vector<float> rStorage, iStorage;
            const float* rData = nullptr;
            const float* iData = nullptr;
            size_t rCount = 0, iCount = 0;
            if (!a.empty()) floatData(a[0], rStorage, &rData, &rCount);
            if (a.size() >= 2) floatData(a[1], iStorage, &iData, &iCount);
            bool disableNorm = false;
            if (a.size() >= 3 && ev::isObject(a[2])) {
                Value opt = a[2];
                Value dn = ev::getProperty(opt, "disableNormalization");
                if (!ev::isUndefined(dn)) disableNorm = ev::toBool(dn);
            }
            int count = static_cast<int>(std::max(rCount, iCount));
            return makePeriodicWaveValue(rData, iData, count, disableNorm);
        },
        decoratePeriodicWaveProto);
}

}  // namespace bro::bronze_host
