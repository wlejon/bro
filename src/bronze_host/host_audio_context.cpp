#include "bronze_host/host_audio_internal.h"

namespace bro::bronze_host {

void decorateAudioContextProto(ObjectBuilder& b) {
    // 1. Properties
    b.accessor("currentTime", [](Value, std::span<const Value>) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e ? e->currentTime() : 0.0);
    }, nullptr);

    b.accessor("sampleRate", [](Value, std::span<const Value>) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e ? e->sampleRate() : 44100);
    }, nullptr);

    b.accessor("state", [](Value, std::span<const Value>) {
        return ev::fromUtf8("running");
    }, nullptr);

    b.accessor("outputLatency", [](Value, std::span<const Value>) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e ? e->outputLatencySeconds() : 0.0);
    }, nullptr);

    b.accessor("baseLatency", [](Value, std::span<const Value>) {
        return ev::fromDouble(0.0);
    }, nullptr);

    b.accessor("masterGain",
        [](Value, std::span<const Value>) {
            auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
            return ev::fromDouble(e ? e->masterGain() : 1.0);
        },
        [](Value, std::span<const Value> a) {
            auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
            if (e && !a.empty()) e->setMasterGain(static_cast<float>(numAt(a, 0)));
            return ev::undefined();
        });

    b.accessor("recording", [](Value, std::span<const Value>) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromBool(e ? e->isRecording() : false);
    }, nullptr);

    b.accessor("dopplerFactor",
        [](Value, std::span<const Value>) {
            auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
            return ev::fromDouble(e ? e->dopplerFactor() : 1.0);
        },
        [](Value, std::span<const Value> a) {
            auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
            if (e && !a.empty()) e->setDopplerFactor(static_cast<float>(numAt(a, 0)));
            return ev::undefined();
        });

    b.accessor("micMuted",
        [](Value, std::span<const Value>) {
            auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
            return ev::fromBool(e ? e->isMicMuted() : false);
        },
        [](Value, std::span<const Value> a) {
            auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
            if (e && !a.empty()) e->setMicMuted(boolAt(a, 0));
            return ev::undefined();
        });

    b.accessor("micMonitorGain",
        [](Value, std::span<const Value>) {
            auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
            return ev::fromDouble(e ? e->micMonitorGain() : 0.0);
        },
        [](Value, std::span<const Value> a) {
            auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
            if (e && !a.empty()) e->setMicMonitorGain(static_cast<float>(numAt(a, 0)));
            return ev::undefined();
        });

    b.accessor("micBus",
        [](Value, std::span<const Value>) {
            auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
            return ev::fromDouble(e ? e->micBus() : 0);
        },
        [](Value, std::span<const Value> a) {
            auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
            if (e && !a.empty()) e->setMicBus(i32At(a, 0));
            return ev::undefined();
        });

    // 2. Lifecycle
    b.def("suspend", 0, [](Value, std::span<const Value>) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setMasterPaused(true);
        ev::Persistent p{ev::createPromise()};
        ev::resolvePromise(p.get(), ev::undefined());
        return p.get();
    });

    b.def("resume", 0, [](Value, std::span<const Value>) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->setMasterPaused(false);
        ev::Persistent p{ev::createPromise()};
        ev::resolvePromise(p.get(), ev::undefined());
        return p.get();
    });

    b.def("close", 0, [](Value, std::span<const Value>) {
        ev::Persistent p{ev::createPromise()};
        ev::resolvePromise(p.get(), ev::undefined());
        return p.get();
    });

    // 3. Node factories
    b.def("createGain", 0, [](Value, std::span<const Value>) {
        return makeGainNodeValue();
    });

    b.def("createOscillator", 0, [](Value, std::span<const Value>) {
        return makeOscillatorNodeValue();
    });

    b.def("createPeriodicWave", 3, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2) return ev::null();
        const uint8_t* rData = nullptr; size_t rLen = 0, rElem = 1;
        const uint8_t* iData = nullptr; size_t iLen = 0, iElem = 1;
        if (!bufferBytes(a[0], &rData, &rLen, &rElem) || !bufferBytes(a[1], &iData, &iLen, &iElem)) {
            return ev::null();
        }
        int count = static_cast<int>(std::min(rLen, iLen) / sizeof(float));
        bool disableNorm = false;
        if (a.size() >= 3 && ev::isObject(a[2])) {
            Value opt = a[2];
            Value dn = ev::getProperty(opt, "disableNormalization");
            if (ev::isBool(dn)) disableNorm = ev::toBool(dn);
        }
        return makePeriodicWaveValue(reinterpret_cast<const float*>(rData), reinterpret_cast<const float*>(iData), count, disableNorm);
    });

    b.def("createBiquadFilter", 0, [](Value, std::span<const Value>) {
        return makeBiquadFilterNodeValue();
    });

    b.def("createAnalyser", 0, [](Value, std::span<const Value>) {
        return makeAnalyserNodeValue();
    });

    b.def("createBufferSource", 0, [](Value, std::span<const Value>) {
        return makeAudioBufferSourceNodeValue();
    });

    b.def("createBuffer", 3, [](Value, std::span<const Value> a) {
        int ch = a.size() >= 1 ? i32At(a, 0) : 1;
        int len = a.size() >= 2 ? i32At(a, 1) : 0;
        int sr = a.size() >= 3 ? i32At(a, 2) : 44100;
        return makeAudioBufferValue(ch, len, sr);
    });

    b.def("createPanner", 0, [](Value, std::span<const Value>) {
        return makePannerNodeValue();
    });

    b.def("createStereoPanner", 0, [](Value, std::span<const Value>) {
        return makeStereoPannerNodeValue();
    });

    b.def("createDelay", 1, [](Value, std::span<const Value> a) {
        double maxTime = !a.empty() ? numAt(a, 0) : 1.0;
        return makeDelayNodeValue(maxTime);
    });

    b.def("createDynamicsCompressor", 0, [](Value, std::span<const Value>) {
        return makeDynamicsCompressorNodeValue();
    });

    b.def("createWaveShaper", 0, [](Value, std::span<const Value>) {
        return makeWaveShaperNodeValue();
    });

    b.def("createConvolver", 0, [](Value, std::span<const Value>) {
        return makeConvolverNodeValue();
    });

    b.def("createChannelSplitter", 1, [](Value, std::span<const Value> a) {
        int outputs = !a.empty() ? i32At(a, 0) : 6;
        return makeChannelSplitterNodeValue(outputs);
    });

    b.def("createChannelMerger", 1, [](Value, std::span<const Value> a) {
        int inputs = !a.empty() ? i32At(a, 0) : 6;
        return makeChannelMergerNodeValue(inputs);
    });

    // 4. Master effect shortcuts
    b.def("allocateFilterSlot", 0, [](Value, std::span<const Value>) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e ? e->allocateFilterSlot() : -1);
    });

    b.def("releaseFilterSlot", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && !a.empty()) e->releaseFilterSlot(i32At(a, 0));
        return ev::undefined();
    });

    b.def("setFilterEnabled", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setFilterEnabled(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("setFilterType", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setFilterType(i32At(a, 0), parseFilterType(ev::toUtf8(a[1])));
        return ev::undefined();
    });

    b.def("setFilterFrequency", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setFilterFrequency(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setFilterQ", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setFilterQ(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setFilterGain", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setFilterGain(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setDelayEnabled", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && !a.empty()) e->setDelayEnabled(boolAt(a, 0));
        return ev::undefined();
    });

    b.def("setDelayTime", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && !a.empty()) e->setDelayTime(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("setDelayFeedback", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && !a.empty()) e->setDelayFeedback(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("setDelayMix", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && !a.empty()) e->setDelayMix(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("setReverbEnabled", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && !a.empty()) e->setBusReverbEnabled(0, boolAt(a, 0));
        return ev::undefined();
    });

    b.def("setLimiterEnabled", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && !a.empty()) e->setLimiterEnabled(boolAt(a, 0));
        return ev::undefined();
    });

    b.def("setLimiterThreshold", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && !a.empty()) e->setLimiterThreshold(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("setLimiterRelease", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && !a.empty()) e->setLimiterRelease(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    // 5. Listener spatial & head model
    b.def("setListenerPosition", 3, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 3) {
            e->setListenerPosition(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)));
        }
        return ev::undefined();
    });

    b.def("setListenerOrientation", 6, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 6) {
            e->setListenerOrientation(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)),
                                     static_cast<float>(numAt(a, 3)), static_cast<float>(numAt(a, 4)), static_cast<float>(numAt(a, 5)));
        }
        return ev::undefined();
    });

    b.def("setListenerVelocity", 3, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 3) {
            e->setListenerVelocity(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)));
        }
        return ev::undefined();
    });

    b.def("setHeadModelEnabled", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && !a.empty()) e->setHeadModelEnabled(boolAt(a, 0));
        return ev::undefined();
    });

    b.def("setHeadModelIldStrength", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && !a.empty()) e->setHeadModelIldStrength(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("setHeadModelBehindAttenuation", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && !a.empty()) e->setHeadModelBehindAttenuation(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("setHeadModelNearCutoff", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setHeadModelNearCutoff(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setHeadModelFarCutoffRatio", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && !a.empty()) e->setHeadModelFarCutoffRatio(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("setHeadModelElevation", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setHeadModelElevation(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setHeadModelCutoffRange", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setHeadModelCutoffRange(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    // 6. Register modular chunks
    registerAudioContextClips(b);
    registerAudioContextVoice(b);
    registerAudioContextBuses(b);
    registerAudioContextSynth(b);
    registerAudioContextSequencer(b);
}

}  // namespace bro::bronze_host
