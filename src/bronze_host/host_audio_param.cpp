// Web Audio & Sound Engine Integration — AudioParam Subsystem
//
// AudioParam, parameter automation, and value synchronization.

#include "bronze_host/host_audio_internal.h"
#include <algorithm>

namespace bro::bronze_host {

void hostAudioParamDtor(void* p) {
    delete static_cast<HostAudioParam*>(p);
}

HostAudioParam* hostAudioParamOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* p = static_cast<HostAudioParam*>(ev::handleData(v));
    if (!p || p->tag != kHostAudioParamTag) return nullptr;
    return p;
}

void syncAudioParamValue(HostAudioParam* p, float val) {
    p->value = std::clamp(val, p->minValue, p->maxValue);
    auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
    if (!e || p->targetId < 0) return;
    switch (p->target) {
        case AudioParamTarget::VoiceFrequency:
            e->setFrequency(p->targetId, p->value);
            break;
        case AudioParamTarget::VoicePan:
        case AudioParamTarget::Pan:
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
        case AudioParamTarget::DelayTime:
            e->setDelayTime(p->value);
            break;
        case AudioParamTarget::VoiceDetune:
        case AudioParamTarget::PlaybackDetune:
        case AudioParamTarget::Gain:
        case AudioParamTarget::PannerPositionX:
        case AudioParamTarget::PannerPositionY:
        case AudioParamTarget::PannerPositionZ:
        case AudioParamTarget::PannerOrientationX:
        case AudioParamTarget::PannerOrientationY:
        case AudioParamTarget::PannerOrientationZ:
        case AudioParamTarget::CompressorThreshold:
        case AudioParamTarget::CompressorKnee:
        case AudioParamTarget::CompressorRatio:
        case AudioParamTarget::CompressorAttack:
        case AudioParamTarget::CompressorRelease:
        case AudioParamTarget::Generic:
        default:
            break;
    }
}

void decorateAudioParamProto(ObjectBuilder& b) {
    b.accessor("value",
               [](Value self_, std::span<const Value>) {
                   HostAudioParam* p = hostAudioParamOf(self_);
                   if (!p) return ev::undefined();
                   return ev::fromDouble(p->value);
               },
               [](Value self_, std::span<const Value> a) {
                   HostAudioParam* p = hostAudioParamOf(self_);
                   if (!p) return ev::undefined();
                   syncAudioParamValue(p, static_cast<float>(numAt(a, 0)));
                   return ev::undefined();
               });

    b.accessor("defaultValue", [](Value self_, std::span<const Value>) {
        HostAudioParam* p = hostAudioParamOf(self_);
        if (!p) return ev::undefined();
        return ev::fromDouble(p->defaultValue);
    }, nullptr);

    b.accessor("minValue", [](Value self_, std::span<const Value>) {
        HostAudioParam* p = hostAudioParamOf(self_);
        if (!p) return ev::undefined();
        return ev::fromDouble(p->minValue);
    }, nullptr);

    b.accessor("maxValue", [](Value self_, std::span<const Value>) {
        HostAudioParam* p = hostAudioParamOf(self_);
        if (!p) return ev::undefined();
        return ev::fromDouble(p->maxValue);
    }, nullptr);

    // Automation methods (all return `this` AudioParam)
    b.def("setValueAtTime", 2, [](Value thisValue, std::span<const Value> a) -> Value {
        HostAudioParam* p = hostAudioParamOf(thisValue);
        if (!p) return ev::undefined();
        syncAudioParamValue(p, static_cast<float>(numAt(a, 0)));
        return thisValue;
    });

    b.def("linearRampToValueAtTime", 2, [](Value thisValue, std::span<const Value> a) -> Value {
        HostAudioParam* p = hostAudioParamOf(thisValue);
        if (!p) return ev::undefined();
        syncAudioParamValue(p, static_cast<float>(numAt(a, 0)));
        return thisValue;
    });

    b.def("exponentialRampToValueAtTime", 2, [](Value thisValue, std::span<const Value> a) -> Value {
        HostAudioParam* p = hostAudioParamOf(thisValue);
        if (!p) return ev::undefined();
        syncAudioParamValue(p, static_cast<float>(numAt(a, 0)));
        return thisValue;
    });

    b.def("setTargetAtTime", 3, [](Value thisValue, std::span<const Value> a) -> Value {
        HostAudioParam* p = hostAudioParamOf(thisValue);
        if (!p) return ev::undefined();
        syncAudioParamValue(p, static_cast<float>(numAt(a, 0)));
        return thisValue;
    });

    b.def("setValueCurveAtTime", 3, [](Value thisValue, std::span<const Value> a) -> Value {
        HostAudioParam* p = hostAudioParamOf(thisValue);
        if (!p) return ev::undefined();
        if (!a.empty()) {
            std::vector<float> vals;
            const float* data = nullptr;
            size_t count = 0;
            if (floatData(a[0], vals, &data, &count) && count > 0 && data) {
                syncAudioParamValue(p, data[count - 1]);
            }
        }
        return thisValue;
    });

    b.def("cancelScheduledValues", 1, [](Value thisValue, std::span<const Value>) -> Value {
        return thisValue;
    });

    b.def("cancelAndHoldAtTime", 1, [](Value thisValue, std::span<const Value>) -> Value {
        return thisValue;
    });
}

Value makeAudioParamValue(AudioParamTarget target, int targetId,
                          float initialVal, float minVal, float maxVal, float defaultVal) {
    auto* p = new HostAudioParam();
    p->target = target;
    p->targetId = targetId;
    p->value = initialVal;
    p->defaultValue = defaultVal;
    p->minValue = minVal;
    p->maxValue = maxVal;

    return g_audioParamClass.make(p, hostAudioParamDtor);
}

}  // namespace bro::bronze_host
