// Web Audio & Sound Engine Integration — Core Subsystem
//
// AudioContext, AudioNode base, AudioParam automation, AudioBuffer,
// AudioDestinationNode, AudioListener, and globals installation.

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
        if (e && a.size() >= 3) {
            e->setListenerPosition(static_cast<float>(numAt(a, 0)),
                                   static_cast<float>(numAt(a, 1)),
                                   static_cast<float>(numAt(a, 2)));
        }
        return ev::undefined();
    });

    b.def("setOrientation", 6, [](Value, std::span<const Value> a) -> Value {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 6) {
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
        if (e && a.size() >= 3) {
            e->setListenerVelocity(static_cast<float>(numAt(a, 0)),
                                   static_cast<float>(numAt(a, 1)),
                                   static_cast<float>(numAt(a, 2)));
        }
        return ev::undefined();
    });

    b.def("setListenerPosition", 3, [](Value, std::span<const Value> a) -> Value {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 3) {
            e->setListenerPosition(static_cast<float>(numAt(a, 0)),
                                   static_cast<float>(numAt(a, 1)),
                                   static_cast<float>(numAt(a, 2)));
        }
        return ev::undefined();
    });

    b.def("setListenerOrientation", 6, [](Value, std::span<const Value> a) -> Value {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 6) {
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

    // 16. Synth & Sequencer Globals
    installAudioSynthGlobals();
    installAudioSequencerGlobals();
}

}  // namespace bro::bronze_host
