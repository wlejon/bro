#if BRO_WITH_3D

#include "bronze_host/host_rigging_internal.h"

#include <cstring>
#include <string>
#include <vector>

namespace bro::bronze_host {

static void hostAnimationDtor(void* p) {
    delete static_cast<HostAnimation*>(p);
}

Value wrapAnimation(std::unique_ptr<bromesh::Animation> anim) {
    if (!anim) return ev::undefined();
    auto* cell = new HostAnimation();
    cell->anim = std::move(anim);
    return g_animationClass.make(cell, hostAnimationDtor);
}

Value wrapAnimation(bromesh::Animation&& anim) {
    auto* cell = new HostAnimation();
    cell->anim = std::make_unique<bromesh::Animation>(std::move(anim));
    return g_animationClass.make(cell, hostAnimationDtor);
}

namespace {

const char* pathToString(bromesh::AnimChannel::Path p) {
    switch (p) {
        case bromesh::AnimChannel::Path::Translation: return "translation";
        case bromesh::AnimChannel::Path::Rotation:    return "rotation";
        case bromesh::AnimChannel::Path::Scale:       return "scale";
    }
    return "translation";
}

bromesh::AnimChannel::Path pathFromString(const char* s) {
    if (!s) return bromesh::AnimChannel::Path::Translation;
    if (std::strcmp(s, "rotation") == 0) return bromesh::AnimChannel::Path::Rotation;
    if (std::strcmp(s, "scale") == 0)    return bromesh::AnimChannel::Path::Scale;
    return bromesh::AnimChannel::Path::Translation;
}

const char* interpToString(bromesh::AnimChannel::Interp i) {
    switch (i) {
        case bromesh::AnimChannel::Interp::Linear:      return "linear";
        case bromesh::AnimChannel::Interp::Step:        return "step";
        case bromesh::AnimChannel::Interp::CubicSpline: return "cubicSpline";
    }
    return "linear";
}

bromesh::AnimChannel::Interp interpFromString(const char* s) {
    if (!s) return bromesh::AnimChannel::Interp::Linear;
    if (std::strcmp(s, "step") == 0)        return bromesh::AnimChannel::Interp::Step;
    if (std::strcmp(s, "cubicSpline") == 0) return bromesh::AnimChannel::Interp::CubicSpline;
    return bromesh::AnimChannel::Interp::Linear;
}

bromesh::AnimChannel readChannel(Value obj) {
    bromesh::AnimChannel c;
    if (!ev::isObject(obj)) return c;

    Value biV = ev::getProperty(obj, "boneIndex");
    if (ev::isNumber(biV)) c.boneIndex = static_cast<int>(ev::toDouble(biV));

    Value pathV = ev::getProperty(obj, "path");
    if (ev::isString(pathV)) {
        std::string s = ev::toUtf8(pathV);
        c.path = pathFromString(s.c_str());
    }

    Value interpV = ev::getProperty(obj, "interp");
    if (ev::isString(interpV)) {
        std::string s = ev::toUtf8(interpV);
        c.interp = interpFromString(s.c_str());
    }

    readFloatVector(ev::getProperty(obj, "times"), c.times);
    readFloatVector(ev::getProperty(obj, "values"), c.values);
    return c;
}

Value makeChannel(const bromesh::AnimChannel& c) {
    ev::Persistent obj(ev::createObject());
    ev::setProperty(obj.get(), "boneIndex", ev::fromDouble(c.boneIndex));
    ev::setProperty(obj.get(), "path", ev::fromUtf8(pathToString(c.path)));
    ev::setProperty(obj.get(), "interp", ev::fromUtf8(interpToString(c.interp)));
    ev::setProperty(obj.get(), "times", makeFloatNArray(c.times.data(), c.times.size()));
    ev::setProperty(obj.get(), "values", makeFloatNArray(c.values.data(), c.values.size()));
    return obj.get();
}

void readChannelsArray(Value arr, std::vector<bromesh::AnimChannel>& out) {
    out.clear();
    if (!ev::isObject(arr)) return;
    Value lenV = ev::getProperty(arr, "length");
    if (!ev::isNumber(lenV)) return;
    uint32_t len = static_cast<uint32_t>(ev::toDouble(lenV));
    out.reserve(len);
    for (uint32_t i = 0; i < len; ++i) {
        out.push_back(readChannel(ev::getElement(arr, i)));
    }
}

Value makeChannelsArray(const std::vector<bromesh::AnimChannel>& chs) {
    return hostArrayOf(chs.size(), [&](size_t i) {
        return makeChannel(chs[i]);
    });
}

void readAnimationFromObj(Value obj, bromesh::Animation& out) {
    if (!ev::isObject(obj)) return;

    Value nameV = ev::getProperty(obj, "name");
    if (ev::isString(nameV)) out.name = ev::toUtf8(nameV);

    Value durV = ev::getProperty(obj, "duration");
    if (ev::isNumber(durV)) out.duration = static_cast<float>(ev::toDouble(durV));

    readChannelsArray(ev::getProperty(obj, "channels"), out.channels);

    if (out.duration <= 0.0f) {
        for (const auto& c : out.channels) {
            for (float t : c.times) {
                if (t > out.duration) out.duration = t;
            }
        }
    }
}

}  // namespace

void installAnimationClass(HostClass& cls) {
    cls.install(
        "Animation", 0,
        [](Value, std::span<const Value> a) {
            auto anim = std::make_unique<bromesh::Animation>();
            if (!a.empty() && ev::isObject(a[0])) {
                readAnimationFromObj(a[0], *anim);
            }
            return wrapAnimation(std::move(anim));
        },
        [](ObjectBuilder& proto) {
            proto.accessor("name",
                [](Value self_, std::span<const Value>) {
                    auto* a = hostAnimationOf(self_);
                    return a ? ev::fromUtf8(a->name) : ev::undefined();
                },
                [](Value self_, std::span<const Value> a) {
                    auto* anim = hostAnimationOf(self_);
                    if (anim && !a.empty() && ev::isString(a[0])) {
                        anim->name = ev::toUtf8(a[0]);
                    }
                    return ev::undefined();
                });

            proto.accessor("duration",
                [](Value self_, std::span<const Value>) {
                    auto* a = hostAnimationOf(self_);
                    return a ? ev::fromDouble(a->duration) : ev::fromDouble(0.0);
                },
                [](Value self_, std::span<const Value> a) {
                    auto* anim = hostAnimationOf(self_);
                    if (anim && !a.empty() && ev::isNumber(a[0])) {
                        anim->duration = static_cast<float>(ev::toDouble(a[0]));
                    }
                    return ev::undefined();
                });

            proto.accessor("channels",
                [](Value self_, std::span<const Value>) {
                    auto* a = hostAnimationOf(self_);
                    return a ? makeChannelsArray(a->channels) : ev::undefined();
                },
                [](Value self_, std::span<const Value> a) {
                    auto* anim = hostAnimationOf(self_);
                    if (anim && !a.empty()) {
                        readChannelsArray(a[0], anim->channels);
                    }
                    return ev::undefined();
                });

            proto.accessor("channelCount", [](Value self_, std::span<const Value>) {
                auto* a = hostAnimationOf(self_);
                return ev::fromDouble(a ? static_cast<double>(a->channels.size()) : 0.0);
            }, nullptr);

            proto.def("evaluate", 3, [](Value self_, std::span<const Value> a) {
                auto* anim = hostAnimationOf(self_);
                if (!anim || a.size() < 2) return ev::throwTypeError("evaluate requires (Skeleton, t)");
                auto* sk = hostSkeletonOf(a[0]);
                if (!sk) return ev::throwTypeError("first argument must be a Skeleton");
                float t = static_cast<float>(ev::toDouble(a[1]));
                bool loop = true;
                if (a.size() > 2 && ev::isObject(a[2])) {
                    Value loopV = ev::getProperty(a[2], "loop");
                    if (!ev::isUndefined(loopV)) loop = ev::toBool(loopV);
                }
                return wrapPose(bromesh::evaluateAnimation(*sk, *anim, t, loop));
            });

            proto.def("evaluateInto", 4, [](Value self_, std::span<const Value> a) {
                auto* anim = hostAnimationOf(self_);
                if (!anim || a.size() < 3) return ev::throwTypeError("evaluateInto requires (Skeleton, t, Pose)");
                auto* sk = hostSkeletonOf(a[0]);
                auto* pw = hostPoseOf(a[2]);
                if (!sk || !pw) return ev::throwTypeError("evaluateInto requires (Skeleton, t, Pose)");
                float t = static_cast<float>(ev::toDouble(a[1]));
                bool loop = true;
                if (a.size() > 3 && ev::isObject(a[3])) {
                    Value loopV = ev::getProperty(a[3], "loop");
                    if (!ev::isUndefined(loopV)) loop = ev::toBool(loopV);
                }
                bromesh::evaluateAnimationInto(*sk, *anim, t, loop, *pw);
                return a[2];
            });
        });

    cls.setStatic("retarget", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.size() < 3) return ev::throwTypeError("retarget requires (Animation, srcSkeleton, dstSkeleton)");
        auto* anim = hostAnimationOf(a[0]);
        auto* ss = hostSkeletonOf(a[1]);
        auto* ds = hostSkeletonOf(a[2]);
        if (!anim || !ss || !ds) return ev::throwTypeError("retarget requires (Animation, srcSkeleton, dstSkeleton)");
        return wrapAnimation(bromesh::retargetAnimation(*anim, *ss, *ds));
    }, 3));
}

Value js_rig_generateLocomotionCycle(std::span<const Value> a) {
    if (a.size() < 2) return ev::throwTypeError("Rig.generateLocomotionCycle(skeleton, spec, params?)");
    auto* sk = hostSkeletonOf(a[0]);
    auto* sw = hostRigSpecOf(a[1]);
    if (!sk) return ev::throwTypeError("first arg must be a Skeleton");
    if (!sw) return ev::throwTypeError("second arg must be a RigSpec");

    bromesh::LocomotionParams p;
    if (a.size() > 2 && ev::isObject(a[2])) {
        Value slV = ev::getProperty(a[2], "strideLength");
        if (ev::isNumber(slV)) p.strideLength = static_cast<float>(ev::toDouble(slV));
        Value cdV = ev::getProperty(a[2], "cycleDuration");
        if (ev::isNumber(cdV)) p.cycleDuration = static_cast<float>(ev::toDouble(cdV));
        Value flhV = ev::getProperty(a[2], "footLiftHeight");
        if (ev::isNumber(flhV)) p.footLiftHeight = static_cast<float>(ev::toDouble(flhV));
        Value kpcV = ev::getProperty(a[2], "keyframesPerCycle");
        if (ev::isNumber(kpcV)) p.keyframesPerCycle = static_cast<int>(ev::toDouble(kpcV));
    }

    auto anim = bromesh::generateLocomotionCycle(*sk, *sw, p);
    return wrapAnimation(std::move(anim));
}

}  // namespace bro::bronze_host

#endif  // BRO_WITH_3D
