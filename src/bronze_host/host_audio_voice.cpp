#include "bronze_host/host_audio_internal.h"

namespace bro::bronze_host {

void registerAudioContextVoice(ObjectBuilder& b) {
    b.def("createVoice", 0, [](Value, std::span<const Value>) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e ? e->createVoice() : -1);
    });

    b.def("removeVoice", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && !a.empty()) e->removeVoice(i32At(a, 0));
        return ev::undefined();
    });

    b.def("startVoice", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && !a.empty()) {
            double when = a.size() >= 2 ? numAt(a, 1) : 0.0;
            e->startVoice(i32At(a, 0), when);
        }
        return ev::undefined();
    });

    b.def("stopVoice", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && !a.empty()) {
            double when = a.size() >= 2 ? numAt(a, 1) : 0.0;
            e->stopVoice(i32At(a, 0), when);
        }
        return ev::undefined();
    });

    b.def("setVoicePersistent", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setVoicePersistent(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("setVoiceNote", 3, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) {
            float vel = a.size() >= 3 ? static_cast<float>(numAt(a, 2)) : 1.0f;
            e->setVoiceNote(i32At(a, 0), i32At(a, 1), vel);
        }
        return ev::undefined();
    });

    b.def("setVoiceWaveform", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) {
            e->setWaveform(i32At(a, 0), parseWaveform(ev::toUtf8(a[1])));
        }
        return ev::undefined();
    });

    b.def("setVoiceFrequency", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setFrequency(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setVoiceGain", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setGain(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setVoicePan", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setVoicePan(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setVoiceAttack", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setAttackTime(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setVoiceDecay", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setDecayTime(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setVoiceSustain", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setSustainLevel(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setVoiceRelease", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setReleaseTime(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setVoicePitchBend", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setVoicePitchBend(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setVoiceUnisonCount", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setVoiceUnisonCount(i32At(a, 0), i32At(a, 1));
        return ev::undefined();
    });

    b.def("setVoiceUnisonDetune", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setVoiceUnisonDetune(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setVoiceUnisonStereoWidth", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setVoiceUnisonStereoWidth(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setVoiceFilterEnabled", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setVoiceFilterEnabled(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("setVoiceFilterType", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setVoiceFilterType(i32At(a, 0), parseFilterType(ev::toUtf8(a[1])));
        return ev::undefined();
    });

    b.def("setVoiceFilterFrequency", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setVoiceFilterFrequency(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setVoiceFilterQ", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setVoiceFilterQ(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setVoiceSpatialEnabled", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setVoiceSpatialEnabled(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("setVoiceSpatialPosition", 4, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 4) {
            e->setVoiceSpatialPosition(i32At(a, 0), static_cast<float>(numAt(a, 1)),
                                       static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        }
        return ev::undefined();
    });

    b.def("setVoiceSpatialVelocity", 4, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 4) {
            e->setVoiceSpatialVelocity(i32At(a, 0), static_cast<float>(numAt(a, 1)),
                                       static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        }
        return ev::undefined();
    });

    b.def("setVoiceSpatialRefDistance", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setVoiceSpatialRefDistance(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setVoiceSpatialMaxDistance", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setVoiceSpatialMaxDistance(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setVoiceSpatialRolloff", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setVoiceSpatialRolloff(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setVoiceSpatialDistanceModel", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setVoiceSpatialDistanceModel(i32At(a, 0), parseDistanceModel(ev::toUtf8(a[1])));
        return ev::undefined();
    });

    b.def("getVoiceDopplerRatio", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->getVoiceDopplerRatio(i32At(a, 0)) : 1.0);
    });

    b.def("setVoiceBus", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setVoiceBus(i32At(a, 0), i32At(a, 1));
        return ev::undefined();
    });

    b.def("setVoiceSend", 3, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 3) e->setVoiceSend(i32At(a, 0), i32At(a, 1), static_cast<float>(numAt(a, 2)));
        return ev::undefined();
    });

    b.def("scheduleNoteOn", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->scheduleNoteOn(i32At(a, 0), numAt(a, 1));
        return ev::undefined();
    });

    b.def("scheduleNoteOff", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->scheduleNoteOff(i32At(a, 0), numAt(a, 1));
        return ev::undefined();
    });
}

}  // namespace bro::bronze_host
