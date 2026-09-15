#include "bronze_host/host_audio_internal.h"

namespace bro::bronze_host {

void registerAudioContextBuses(ObjectBuilder& b) {
    b.def("createBus", 0, [](Value, std::span<const Value>) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e ? e->createBus() : -1);
    });

    b.def("deleteBus", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && !a.empty()) e->deleteBus(i32At(a, 0));
        return ev::undefined();
    });

    b.def("setBusGain", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setBusGain(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusGain", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->getBusGain(i32At(a, 0)) : 1.0);
    });

    b.def("setBusPan", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setBusPan(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusPan", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->getBusPan(i32At(a, 0)) : 0.0);
    });

    b.def("setBusMuted", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setBusMuted(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("getBusMuted", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromBool(e && !a.empty() ? e->getBusMuted(i32At(a, 0)) : false);
    });

    b.def("setBusSolo", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setBusSolo(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("getBusSolo", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromBool(e && !a.empty() ? e->getBusSolo(i32At(a, 0)) : false);
    });

    b.def("setBusSend", 3, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 3) e->setBusSend(i32At(a, 0), i32At(a, 1), static_cast<float>(numAt(a, 2)));
        return ev::undefined();
    });

    b.def("getBusPeakL", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->getBusPeakL(i32At(a, 0)) : 0.0);
    });

    b.def("getBusPeakR", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->getBusPeakR(i32At(a, 0)) : 0.0);
    });

    b.def("getBusRmsL", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->getBusRmsL(i32At(a, 0)) : 0.0);
    });

    b.def("getBusRmsR", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->getBusRmsR(i32At(a, 0)) : 0.0);
    });

    // Filters
    b.def("allocateBusFilterSlot", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->allocateBusFilterSlot(i32At(a, 0)) : -1);
    });

    b.def("releaseBusFilterSlot", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->releaseBusFilterSlot(i32At(a, 0), i32At(a, 1));
        return ev::undefined();
    });

    b.def("setBusFilterEnabled", 3, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 3) e->setBusFilterEnabled(i32At(a, 0), i32At(a, 1), boolAt(a, 2));
        return ev::undefined();
    });

    b.def("getBusFilterEnabled", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromBool(e && a.size() >= 2 ? e->getBusFilterEnabled(i32At(a, 0), i32At(a, 1)) : false);
    });

    b.def("setBusFilterType", 3, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 3) e->setBusFilterType(i32At(a, 0), i32At(a, 1), parseFilterType(ev::toUtf8(a[2])));
        return ev::undefined();
    });

    b.def("getBusFilterType", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromUtf8(e && a.size() >= 2 ? filterTypeToString(e->getBusFilterType(i32At(a, 0), i32At(a, 1))) : "lowpass");
    });

    b.def("setBusFilterFrequency", 3, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 3) e->setBusFilterFrequency(i32At(a, 0), i32At(a, 1), static_cast<float>(numAt(a, 2)));
        return ev::undefined();
    });

    b.def("getBusFilterFrequency", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && a.size() >= 2 ? e->getBusFilterFrequency(i32At(a, 0), i32At(a, 1)) : 1000.0);
    });

    b.def("setBusFilterQ", 3, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 3) e->setBusFilterQ(i32At(a, 0), i32At(a, 1), static_cast<float>(numAt(a, 2)));
        return ev::undefined();
    });

    b.def("getBusFilterQ", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && a.size() >= 2 ? e->getBusFilterQ(i32At(a, 0), i32At(a, 1)) : 1.0);
    });

    b.def("setBusFilterGain", 3, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 3) e->setBusFilterGain(i32At(a, 0), i32At(a, 1), static_cast<float>(numAt(a, 2)));
        return ev::undefined();
    });

    b.def("getBusFilterGain", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && a.size() >= 2 ? e->getBusFilterGain(i32At(a, 0), i32At(a, 1)) : 0.0);
    });

    // Delay
    b.def("setBusDelayEnabled", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setBusDelayEnabled(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("getBusDelayEnabled", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromBool(e && !a.empty() ? e->getBusDelayEnabled(i32At(a, 0)) : false);
    });

    b.def("setBusDelayTime", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setBusDelayTime(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusDelayTime", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->getBusDelayTime(i32At(a, 0)) : 0.0);
    });

    b.def("setBusDelayFeedback", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setBusDelayFeedback(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusDelayFeedback", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->getBusDelayFeedback(i32At(a, 0)) : 0.0);
    });

    b.def("setBusDelayMix", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setBusDelayMix(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusDelayMix", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->getBusDelayMix(i32At(a, 0)) : 0.0);
    });

    // Reverb
    b.def("setBusReverbEnabled", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setBusReverbEnabled(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("getBusReverbEnabled", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromBool(e && !a.empty() ? e->getBusReverbEnabled(i32At(a, 0)) : false);
    });

    b.def("setBusReverbRoomSize", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setBusReverbRoomSize(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusReverbRoomSize", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->getBusReverbRoomSize(i32At(a, 0)) : 0.0);
    });

    b.def("setBusReverbDamping", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setBusReverbDamping(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusReverbDamping", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->getBusReverbDamping(i32At(a, 0)) : 0.0);
    });

    b.def("setBusReverbMix", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setBusReverbMix(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusReverbMix", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->getBusReverbMix(i32At(a, 0)) : 0.0);
    });

    // Chorus
    b.def("setBusChorusEnabled", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setBusChorusEnabled(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("getBusChorusEnabled", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromBool(e && !a.empty() ? e->getBusChorusEnabled(i32At(a, 0)) : false);
    });

    b.def("setBusChorusRate", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setBusChorusRate(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusChorusRate", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->getBusChorusRate(i32At(a, 0)) : 0.0);
    });

    b.def("setBusChorusDepth", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setBusChorusDepth(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusChorusDepth", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->getBusChorusDepth(i32At(a, 0)) : 0.0);
    });

    b.def("setBusChorusMix", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setBusChorusMix(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusChorusMix", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->getBusChorusMix(i32At(a, 0)) : 0.0);
    });

    b.def("setBusChorusFeedback", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setBusChorusFeedback(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusChorusFeedback", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->getBusChorusFeedback(i32At(a, 0)) : 0.0);
    });

    // Compressor
    b.def("setBusCompressorEnabled", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setBusCompressorEnabled(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("getBusCompressorEnabled", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromBool(e && !a.empty() ? e->getBusCompressorEnabled(i32At(a, 0)) : false);
    });

    b.def("setBusCompressorThreshold", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setBusCompressorThreshold(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusCompressorThreshold", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->getBusCompressorThreshold(i32At(a, 0)) : 0.0);
    });

    b.def("setBusCompressorRatio", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setBusCompressorRatio(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusCompressorRatio", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->getBusCompressorRatio(i32At(a, 0)) : 1.0);
    });

    b.def("setBusCompressorAttack", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setBusCompressorAttack(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusCompressorAttack", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->getBusCompressorAttack(i32At(a, 0)) : 0.0);
    });

    b.def("setBusCompressorRelease", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setBusCompressorRelease(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusCompressorRelease", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->getBusCompressorRelease(i32At(a, 0)) : 0.0);
    });

    b.def("setBusCompressorSidechain", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setBusCompressorSidechain(i32At(a, 0), i32At(a, 1));
        return ev::undefined();
    });

    b.def("getBusCompressorSidechain", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->getBusCompressorSidechain(i32At(a, 0)) : -1);
    });

    // Equalizer
    b.def("setBusEqEnabled", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setBusEqEnabled(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("getBusEqEnabled", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromBool(e && !a.empty() ? e->getBusEqEnabled(i32At(a, 0)) : false);
    });

    b.def("setBusEqBandGain", 3, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 3) e->setBusEqBandGain(i32At(a, 0), i32At(a, 1), static_cast<float>(numAt(a, 2)));
        return ev::undefined();
    });

    b.def("getBusEqBandGain", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && a.size() >= 2 ? e->getBusEqBandGain(i32At(a, 0), i32At(a, 1)) : 0.0);
    });

    b.def("setBusEqMasterGain", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setBusEqMasterGain(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusEqMasterGain", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->getBusEqMasterGain(i32At(a, 0)) : 0.0);
    });

    // Distortion
    b.def("setBusDistortionEnabled", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setBusDistortionEnabled(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("getBusDistortionEnabled", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromBool(e && !a.empty() ? e->getBusDistortionEnabled(i32At(a, 0)) : false);
    });

    b.def("setBusDistortionMode", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setBusDistortionMode(i32At(a, 0), parseDistortionMode(ev::toUtf8(a[1])));
        return ev::undefined();
    });

    b.def("getBusDistortionMode", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromUtf8(e && !a.empty() ? distortionModeToString(e->getBusDistortionMode(i32At(a, 0))) : "softclip");
    });

    b.def("setBusDistortionDrive", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setBusDistortionDrive(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusDistortionDrive", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->getBusDistortionDrive(i32At(a, 0)) : 1.0);
    });

    b.def("setBusDistortionMix", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setBusDistortionMix(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusDistortionMix", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->getBusDistortionMix(i32At(a, 0)) : 1.0);
    });

    b.def("setBusDistortionOutputGain", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setBusDistortionOutputGain(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusDistortionOutputGain", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->getBusDistortionOutputGain(i32At(a, 0)) : 1.0);
    });

    b.def("setBusDistortionCrushBits", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setBusDistortionCrushBits(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusDistortionCrushBits", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->getBusDistortionCrushBits(i32At(a, 0)) : 16.0);
    });

    b.def("setBusDistortionCrushRate", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setBusDistortionCrushRate(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusDistortionCrushRate", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->getBusDistortionCrushRate(i32At(a, 0)) : 1.0);
    });

    // Effect Order
    b.def("setBusEffectOrder", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2 && ev::isObject(a[1])) {
            int busId = i32At(a, 0);
            Value arr = a[1];
            Value lenV = ev::getProperty(arr, "length");
            if (ev::isNumber(lenV)) {
                int len = static_cast<int>(ev::toDouble(lenV));
                std::vector<broaudio::EffectSlot> slots;
                for (int i = 0; i < len; ++i) {
                    Value item = ev::getProperty(arr, std::to_string(i));
                    if (ev::isString(item)) {
                        slots.push_back(parseEffectSlot(ev::toUtf8(item), broaudio::EffectSlot::Filter));
                    }
                }
                if (!slots.empty()) {
                    e->setBusEffectOrder(busId, slots.data(), static_cast<int>(slots.size()));
                }
            }
        }
        return ev::undefined();
    });

    // Offline processing
    b.def("processEffectsOffline", 2, [](Value, std::span<const Value> a) -> Value {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (!e || a.size() < 2) return ev::null();
        int busId = i32At(a, 0);
        const uint8_t* rawData = nullptr;
        size_t rawLen = 0;
        size_t elemSize = 1;
        if (!bufferBytes(a[1], &rawData, &rawLen, &elemSize) || rawLen == 0) return ev::null();
        int count = static_cast<int>(rawLen / sizeof(float));
        std::vector<float> res = e->processEffectsOffline(busId, reinterpret_cast<const float*>(rawData), count);
        Value out = ev::createTypedArray(ev::elements::Float32, static_cast<uint32_t>(res.size()));
        ev::fillTypedArray(out, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(res.data()), res.size() * sizeof(float)));
        return out;
    });
}

}  // namespace bro::bronze_host
