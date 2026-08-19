// Physics joints and constraints bindings for the bronze host layer.

#include "bronze_host/host_physics_internal.h"

namespace bro::bronze_host {

namespace {

static const char* kSixDofAxisNames[6] = {
    "translationX", "translationY", "translationZ",
    "rotationX", "rotationY", "rotationZ",
};

static int motorAxisIndex(const std::string& name) {
    for (int i = 0; i < 6; i++) {
        if (name == kSixDofAxisNames[i]) return i;
    }
    if (name == "tx") return 0;
    if (name == "ty") return 1;
    if (name == "tz") return 2;
    if (name == "rx") return 3;
    if (name == "ry") return 4;
    if (name == "rz") return 5;
    return -1;
}

static bool readMotorOptions(Value oVal, physics::MotorOptions& m, std::string& err) {
    if (!ev::isObject(oVal)) { err = "motor options must be an object"; return false; }
    ev::Persistent o(oVal);
    std::string type = getPropString(o, "type");
    if (type == "velocity")      m.state = physics::MotorOptions::Velocity;
    else if (type == "position") m.state = physics::MotorOptions::Position;
    else if (type == "off" || type.empty()) m.state = physics::MotorOptions::Off;
    else { err = "motor type must be 'velocity' | 'position' | 'off'"; return false; }

    m.target    = static_cast<float>(getPropNumber(o, "target", m.target));
    m.maxForce  = static_cast<float>(getPropNumber(o, "maxForce", m.maxForce));
    m.maxTorque = static_cast<float>(getPropNumber(o, "maxTorque", m.maxTorque));
    m.frequency = static_cast<float>(getPropNumber(o, "frequency", m.frequency));
    m.damping   = static_cast<float>(getPropNumber(o, "damping", m.damping));

    Value av = ev::getProperty(o.get(), "axis");
    if (!ev::isUndefined(av) && !ev::isNull(av)) {
        if (!ev::isObject(av)) {
            std::string s = ev::toUtf8(av);
            int idx = motorAxisIndex(s);
            int numeric = 0;
            if (idx >= 0) {
                m.axis = idx;
            } else if (parseDecimalIndex(s, numeric)) {
                m.axis = numeric;
            } else {
                err = "motor axis must be translationX..Z / rotationX..Z";
                return false;
            }
        }
    }
    return true;
}

static bool readSixDofAxis(Value v, physics::SixDofAxis& a, std::string& err) {
    if (!ev::isObject(v)) {
        std::string mode = ev::toUtf8(v);
        if (mode == "locked")    a.mode = physics::SixDofAxis::Locked;
        else if (mode == "free") a.mode = physics::SixDofAxis::Free;
        else { err = "axis mode must be 'locked' | 'free' | {min,max,...}"; return false; }
        return true;
    }
    ev::Persistent p(v);
    Value minV = ev::getProperty(p.get(), "min");
    Value maxV = ev::getProperty(p.get(), "max");
    if (!ev::isUndefined(minV) && !ev::isObject(minV) && !ev::isUndefined(maxV) && !ev::isObject(maxV)) {
        a.mode = physics::SixDofAxis::Limited;
        a.min = static_cast<float>(ev::toDouble(minV));
        a.max = static_cast<float>(ev::toDouble(maxV));
    } else {
        a.mode = physics::SixDofAxis::Free;
    }
    a.springFrequency = static_cast<float>(getPropNumber(p, "frequency", a.springFrequency));
    a.springDamping   = static_cast<float>(getPropNumber(p, "damping", a.springDamping));
    a.maxFriction     = static_cast<float>(getPropNumber(p, "friction", a.maxFriction));
    return true;
}

}  // namespace

void registerConstraintMethods(ObjectBuilder& b) {
    // Physics.createConstraint(opts)
    b.def("createConstraint", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world) return ev::throwError("PhysicsWorld not available");
        if (a.empty() || !ev::isObject(a[0])) return ev::throwTypeError("createConstraint(opts) requires an object");

        ev::Persistent o(a[0]);
        physics::ConstraintOptions cs;

        std::string type = getPropString(o, "type");
        if (type == "distance") cs.type = physics::ConstraintOptions::Distance;
        else if (type == "point") cs.type = physics::ConstraintOptions::Point;
        else if (type == "hinge") cs.type = physics::ConstraintOptions::Hinge;
        else if (type == "fixed") cs.type = physics::ConstraintOptions::Fixed;
        else if (type == "slider") cs.type = physics::ConstraintOptions::Slider;
        else if (type == "wheel")  cs.type = physics::ConstraintOptions::Wheel;
        else if (type == "cone")   cs.type = physics::ConstraintOptions::Cone;
        else if (type == "swingTwist" || type == "swing-twist") cs.type = physics::ConstraintOptions::SwingTwist;
        else if (type == "pulley") cs.type = physics::ConstraintOptions::Pulley;
        else if (type == "gear")   cs.type = physics::ConstraintOptions::Gear;
        else if (type == "rackAndPinion" || type == "rack-and-pinion") cs.type = physics::ConstraintOptions::RackAndPinion;
        else if (type == "sixdof" || type == "sixDof" || type == "6dof") cs.type = physics::ConstraintOptions::SixDOF;
        else return ev::throwTypeError("constraint type required (distance|point|hinge|fixed|slider|wheel|cone|swingTwist|pulley|gear|rackAndPinion|sixdof)");

        int32_t t1 = static_cast<int32_t>(getPropNumber(o, "body1", -1.0));
        int32_t t2 = static_cast<int32_t>(getPropNumber(o, "body2", -1.0));

        cs.body1 = g_phys.bodyIdForTag(t1);
        cs.body2 = g_phys.bodyIdForTag(t2);
        if (cs.body1.IsInvalid())
            return ev::throwTypeError("constraint body1 tag is invalid");

        cs.point1 = readRVec3(ev::getProperty(o.get(), "point1"));
        cs.point2 = readRVec3(ev::getProperty(o.get(), "point2"));
        cs.axis = readVec3(ev::getProperty(o.get(), "axis"), JPH::Vec3(0, 1, 0));

        cs.minDistance = static_cast<float>(getPropNumber(o, "minDistance", cs.minDistance));
        cs.maxDistance = static_cast<float>(getPropNumber(o, "maxDistance", cs.maxDistance));

        Value lmin = ev::getProperty(o.get(), "limitMin");
        Value lmax = ev::getProperty(o.get(), "limitMax");
        if (!ev::isUndefined(lmin) && !ev::isObject(lmin) && !ev::isUndefined(lmax) && !ev::isObject(lmax)) {
            cs.limitMin = static_cast<float>(ev::toDouble(lmin));
            cs.limitMax = static_cast<float>(ev::toDouble(lmax));
            cs.hasLimits = true;
        }

        cs.breakingImpulse = static_cast<float>(getPropNumber(o, "breakingImpulse", cs.breakingImpulse));
        cs.collideConnected = getPropBool(o, "collideConnected", cs.collideConnected);

        if (cs.type == physics::ConstraintOptions::Wheel) {
            cs.wheelSuspensionAxis = readVec3(ev::getProperty(o.get(), "suspensionAxis"), JPH::Vec3(0, 1, 0));
            cs.wheelHingeAxis = readVec3(ev::getProperty(o.get(), "hingeAxis"), JPH::Vec3(0, 0, 1));
            cs.wheelHertz = static_cast<float>(getPropNumber(o, "hertz", cs.wheelHertz));
            cs.wheelDampingRatio = static_cast<float>(getPropNumber(o, "dampingRatio", cs.wheelDampingRatio));
            Value lo = ev::getProperty(o.get(), "lowerTranslation");
            Value hi = ev::getProperty(o.get(), "upperTranslation");
            if (!ev::isUndefined(lo) && !ev::isObject(lo) && !ev::isUndefined(hi) && !ev::isObject(hi)) {
                cs.wheelLowerTranslation = static_cast<float>(ev::toDouble(lo));
                cs.wheelUpperTranslation = static_cast<float>(ev::toDouble(hi));
                cs.wheelHasTranslationLimits = true;
            }
            cs.wheelEnableMotor = getPropBool(o, "enableMotor", cs.wheelEnableMotor);
            cs.wheelMotorSpeed = static_cast<float>(getPropNumber(o, "motorSpeed", cs.wheelMotorSpeed));
            cs.wheelMaxMotorTorque = static_cast<float>(getPropNumber(o, "maxMotorTorque", cs.wheelMaxMotorTorque));
        }

        if (cs.type == physics::ConstraintOptions::Cone) {
            cs.coneHalfAngle = static_cast<float>(getPropNumber(o, "halfConeAngle", cs.coneHalfAngle));
        }

        if (cs.type == physics::ConstraintOptions::SwingTwist) {
            cs.planeAxis = readVec3(ev::getProperty(o.get(), "planeAxis"), JPH::Vec3(0, 1, 0));
            cs.normalHalfConeAngle = static_cast<float>(getPropNumber(o, "normalHalfConeAngle", cs.normalHalfConeAngle));
            cs.planeHalfConeAngle = static_cast<float>(getPropNumber(o, "planeHalfConeAngle", cs.planeHalfConeAngle));
            cs.twistMinAngle = static_cast<float>(getPropNumber(o, "twistMinAngle", cs.twistMinAngle));
            cs.twistMaxAngle = static_cast<float>(getPropNumber(o, "twistMaxAngle", cs.twistMaxAngle));
            cs.maxFrictionTorque = static_cast<float>(getPropNumber(o, "maxFrictionTorque", cs.maxFrictionTorque));
        }

        if (cs.type == physics::ConstraintOptions::Pulley) {
            cs.bodyPoint1 = readRVec3(ev::getProperty(o.get(), "bodyPoint1"));
            cs.fixedPoint1 = readRVec3(ev::getProperty(o.get(), "fixedPoint1"));
            cs.bodyPoint2 = readRVec3(ev::getProperty(o.get(), "bodyPoint2"));
            cs.fixedPoint2 = readRVec3(ev::getProperty(o.get(), "fixedPoint2"));
            cs.ratio = static_cast<float>(getPropNumber(o, "ratio", cs.ratio));
            cs.minLength = static_cast<float>(getPropNumber(o, "minLength", cs.minLength));
            cs.maxLength = static_cast<float>(getPropNumber(o, "maxLength", cs.maxLength));
        }

        if (cs.type == physics::ConstraintOptions::Gear ||
            cs.type == physics::ConstraintOptions::RackAndPinion) {
            cs.hingeAxis1 = readVec3(ev::getProperty(o.get(), "hingeAxis1"), JPH::Vec3(1, 0, 0));
            const char* ha2Prop = (cs.type == physics::ConstraintOptions::Gear) ? "hingeAxis2" : "sliderAxis";
            cs.hingeAxis2 = readVec3(ev::getProperty(o.get(), ha2Prop), JPH::Vec3(1, 0, 0));
            cs.ratio = static_cast<float>(getPropNumber(o, "ratio", 1.0));
            cs.dependentConstraint1 = static_cast<uint32_t>(getPropNumber(o, "constraint1", 0.0));
            cs.dependentConstraint2 = static_cast<uint32_t>(getPropNumber(o, "constraint2", 0.0));
            if (!cs.dependentConstraint1 || !cs.dependentConstraint2) {
                const char* m = (cs.type == physics::ConstraintOptions::Gear)
                    ? "gear requires constraint1/constraint2 (two hinge constraint handles)"
                    : "rackAndPinion requires constraint1 (pinion hinge) and constraint2 (rack slider) handles";
                return ev::throwTypeError(m);
            }
        }

        if (cs.type == physics::ConstraintOptions::SixDOF) {
            cs.sixDofAxisX = readVec3(ev::getProperty(o.get(), "axisX"), JPH::Vec3(1, 0, 0));
            cs.sixDofAxisY = readVec3(ev::getProperty(o.get(), "axisY"), JPH::Vec3(0, 1, 0));
            std::string swing = getPropString(o, "swingType");
            cs.sixDofSwingPyramid = (swing == "pyramid");

            Value axesVal = ev::getProperty(o.get(), "axes");
            if (ev::isObject(axesVal)) {
                ev::Persistent axesPersist(axesVal);
                for (int i = 0; i < 6; i++) {
                    Value v = ev::getProperty(axesPersist.get(), kSixDofAxisNames[i]);
                    if (!ev::isUndefined(v) && !ev::isNull(v)) {
                        std::string err;
                        if (!readSixDofAxis(v, cs.sixDofAxes[i], err)) {
                            return ev::throwTypeError("sixdof axes." + std::string(kSixDofAxisNames[i]) + ": " + err);
                        }
                    }
                }
            }

            Value motorsVal = ev::getProperty(o.get(), "motors");
            if (ev::isObject(motorsVal)) {
                ev::Persistent motorsPersist(motorsVal);
                for (int i = 0; i < 6; i++) {
                    Value v = ev::getProperty(motorsPersist.get(), kSixDofAxisNames[i]);
                    if (ev::isObject(v)) {
                        physics::MotorOptions m;
                        std::string err;
                        if (!readMotorOptions(v, m, err)) {
                            return ev::throwTypeError("sixdof motors." + std::string(kSixDofAxisNames[i]) + ": " + err);
                        }
                        m.axis = i;
                        cs.motors.push_back(m);
                    }
                }
            }
        }

        if (cs.type == physics::ConstraintOptions::Hinge ||
            cs.type == physics::ConstraintOptions::Slider) {
            Value mv = ev::getProperty(o.get(), "motor");
            if (ev::isObject(mv)) {
                physics::MotorOptions m;
                std::string err;
                if (!readMotorOptions(mv, m, err)) {
                    return ev::throwTypeError("motor: " + err);
                }
                cs.motors.push_back(m);
            }
        }

        uint32_t handle = world->createConstraint(cs);
        if (!handle) return ev::throwError("Failed to create constraint");
        return ev::fromDouble(static_cast<double>(handle));
    });

    // Physics.destroyConstraint(handle)
    b.def("destroyConstraint", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (world && !a.empty()) {
            uint32_t h = static_cast<uint32_t>(numAt(a, 0));
            world->destroyConstraint(h);
        }
        return ev::undefined();
    });

    // Physics.setConstraintEnabled(handle, enabled)
    b.def("setConstraintEnabled", 2, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (world && a.size() >= 2) {
            uint32_t h = static_cast<uint32_t>(numAt(a, 0));
            bool en = boolAt(a, 1);
            world->setConstraintEnabled(h, en);
        }
        return ev::undefined();
    });

    // Physics.isConstraintEnabled(handle)
    b.def("isConstraintEnabled", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::fromBool(false);
        uint32_t h = static_cast<uint32_t>(numAt(a, 0));
        return ev::fromBool(world->isConstraintEnabled(h));
    });

    // Physics.setWheelMotor(handle, enabled, speed, maxTorque)
    b.def("setWheelMotor", 4, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.size() < 4) return ev::throwTypeError("setWheelMotor(handle, enabled, speed, maxTorque)");
        uint32_t h = static_cast<uint32_t>(numAt(a, 0));
        bool en = boolAt(a, 1);
        double speed = numAt(a, 2);
        double torque = numAt(a, 3);
        world->setWheelMotor(h, en, static_cast<float>(speed), static_cast<float>(torque));
        return ev::undefined();
    });

    // Physics.setConstraintMotor(handle, opts)
    b.def("setConstraintMotor", 2, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.size() < 2) return ev::throwTypeError("setConstraintMotor(handle, opts)");
        uint32_t h = static_cast<uint32_t>(numAt(a, 0));
        physics::MotorOptions m;
        std::string err;
        if (!readMotorOptions(a[1], m, err)) {
            return ev::throwTypeError("setConstraintMotor: " + err);
        }
        return ev::fromBool(world->setConstraintMotor(h, m));
    });

    // Physics.setConstraintBreakingImpulse(handle, threshold)
    b.def("setConstraintBreakingImpulse", 2, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.size() < 2) return ev::throwTypeError("setConstraintBreakingImpulse(handle, threshold)");
        uint32_t h = static_cast<uint32_t>(numAt(a, 0));
        double t = numAt(a, 1);
        world->setConstraintBreakingImpulse(h, static_cast<float>(t));
        return ev::undefined();
    });

    // Physics.getConstraintBreakingImpulse(handle)
    b.def("getConstraintBreakingImpulse", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::fromDouble(0.0);
        uint32_t h = static_cast<uint32_t>(numAt(a, 0));
        return ev::fromDouble(world->getConstraintBreakingImpulse(h));
    });

    // Physics.getBrokenConstraints()
    b.def("getBrokenConstraints", 0, [](Value, std::span<const Value>) -> Value {
        auto* world = getPhysicsWorld();
        if (!world) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        auto broken = world->drainBrokenConstraints();
        return hostArrayOf(broken.size(), [&](size_t i) {
            return ev::fromDouble(static_cast<double>(broken[i]));
        });
    });

    // Physics.getConstraintLimits(handle)
    b.def("getConstraintLimits", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::null();
        uint32_t h = static_cast<uint32_t>(numAt(a, 0));
        JPH::Constraint* c = world->getConstraint(h);
        if (!c) return ev::null();

        switch (c->GetSubType()) {
            case JPH::EConstraintSubType::Hinge: {
                auto* hc = static_cast<JPH::HingeConstraint*>(c);
                ObjectBuilder res;
                res.set("min", ev::fromDouble(hc->GetLimitsMin()));
                res.set("max", ev::fromDouble(hc->GetLimitsMax()));
                res.set("hasLimits", ev::fromBool(hc->HasLimits()));
                return res.get();
            }
            case JPH::EConstraintSubType::Slider: {
                auto* sc = static_cast<JPH::SliderConstraint*>(c);
                ObjectBuilder res;
                res.set("min", ev::fromDouble(sc->GetLimitsMin()));
                res.set("max", ev::fromDouble(sc->GetLimitsMax()));
                res.set("hasLimits", ev::fromBool(sc->HasLimits()));
                return res.get();
            }
            case JPH::EConstraintSubType::Distance: {
                auto* dc = static_cast<JPH::DistanceConstraint*>(c);
                ObjectBuilder res;
                res.set("min", ev::fromDouble(dc->GetMinDistance()));
                res.set("max", ev::fromDouble(dc->GetMaxDistance()));
                return res.get();
            }
            case JPH::EConstraintSubType::Cone: {
                auto* cc = static_cast<JPH::ConeConstraint*>(c);
                ObjectBuilder res;
                res.set("halfConeAngle", ev::fromDouble(static_cast<double>(std::acos(std::clamp(cc->GetCosHalfConeAngle(), -1.0f, 1.0f)))));
                return res.get();
            }
            case JPH::EConstraintSubType::SwingTwist: {
                auto* stc = static_cast<JPH::SwingTwistConstraint*>(c);
                ObjectBuilder res;
                res.set("normalHalfConeAngle", ev::fromDouble(stc->GetNormalHalfConeAngle()));
                res.set("planeHalfConeAngle", ev::fromDouble(stc->GetPlaneHalfConeAngle()));
                res.set("twistMinAngle", ev::fromDouble(stc->GetTwistMinAngle()));
                res.set("twistMaxAngle", ev::fromDouble(stc->GetTwistMaxAngle()));
                return res.get();
            }
            case JPH::EConstraintSubType::SixDOF: {
                auto* sdc = static_cast<JPH::SixDOFConstraint*>(c);
                ObjectBuilder res;
                for (int i = 0; i < 6; i++) {
                    auto axis = static_cast<JPH::SixDOFConstraintSettings::EAxis>(i);
                    ObjectBuilder ab;
                    ab.set("min", ev::fromDouble(sdc->GetLimitsMin(axis)));
                    ab.set("max", ev::fromDouble(sdc->GetLimitsMax(axis)));
                    res.set(kSixDofAxisNames[i], ab.get());
                }
                return res.get();
            }
            default:
                return ev::null();
        }
    });

    // Physics.setConstraintLimits(handle, opts)
    b.def("setConstraintLimits", 2, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.size() < 2 || !ev::isObject(a[1])) return ev::fromBool(false);
        uint32_t h = static_cast<uint32_t>(numAt(a, 0));
        JPH::Constraint* c = world->getConstraint(h);
        if (!c) return ev::fromBool(false);

        ev::Persistent opts(a[1]);
        bool ok = false;

        switch (c->GetSubType()) {
            case JPH::EConstraintSubType::Hinge: {
                auto* hc = static_cast<JPH::HingeConstraint*>(c);
                double min = getPropNumber(opts, "min", getPropNumber(opts, "limitMin", hc->GetLimitsMin()));
                double max = getPropNumber(opts, "max", getPropNumber(opts, "limitMax", hc->GetLimitsMax()));
                hc->SetLimits(static_cast<float>(min), static_cast<float>(max));
                ok = true;
                break;
            }
            case JPH::EConstraintSubType::Slider: {
                auto* sc = static_cast<JPH::SliderConstraint*>(c);
                double min = getPropNumber(opts, "min", getPropNumber(opts, "limitMin", sc->GetLimitsMin()));
                double max = getPropNumber(opts, "max", getPropNumber(opts, "limitMax", sc->GetLimitsMax()));
                sc->SetLimits(static_cast<float>(min), static_cast<float>(max));
                ok = true;
                break;
            }
            case JPH::EConstraintSubType::Distance: {
                auto* dc = static_cast<JPH::DistanceConstraint*>(c);
                double min = getPropNumber(opts, "min", getPropNumber(opts, "minDistance", dc->GetMinDistance()));
                double max = getPropNumber(opts, "max", getPropNumber(opts, "maxDistance", dc->GetMaxDistance()));
                dc->SetDistance(static_cast<float>(min), static_cast<float>(max));
                ok = true;
                break;
            }
            case JPH::EConstraintSubType::Cone: {
                auto* cc = static_cast<JPH::ConeConstraint*>(c);
                double curAngle = static_cast<double>(std::acos(std::clamp(cc->GetCosHalfConeAngle(), -1.0f, 1.0f)));
                double angle = getPropNumber(opts, "halfConeAngle", curAngle);
                cc->SetHalfConeAngle(static_cast<float>(angle));
                ok = true;
                break;
            }
            case JPH::EConstraintSubType::SwingTwist: {
                auto* stc = static_cast<JPH::SwingTwistConstraint*>(c);
                float nAngle = static_cast<float>(getPropNumber(opts, "normalHalfConeAngle", stc->GetNormalHalfConeAngle()));
                float pAngle = static_cast<float>(getPropNumber(opts, "planeHalfConeAngle", stc->GetPlaneHalfConeAngle()));
                float tMin = static_cast<float>(getPropNumber(opts, "twistMinAngle", stc->GetTwistMinAngle()));
                float tMax = static_cast<float>(getPropNumber(opts, "twistMaxAngle", stc->GetTwistMaxAngle()));
                stc->SetNormalHalfConeAngle(nAngle);
                stc->SetPlaneHalfConeAngle(pAngle);
                stc->SetTwistMinAngle(tMin);
                stc->SetTwistMaxAngle(tMax);
                ok = true;
                break;
            }
            case JPH::EConstraintSubType::SixDOF: {
                auto* sdc = static_cast<JPH::SixDOFConstraint*>(c);
                JPH::Vec3 tMin = sdc->GetTranslationLimitsMin();
                JPH::Vec3 tMax = sdc->GetTranslationLimitsMax();
                JPH::Vec3 rMin = sdc->GetRotationLimitsMin();
                JPH::Vec3 rMax = sdc->GetRotationLimitsMax();
                for (int i = 0; i < 6; i++) {
                    Value axVal = ev::getProperty(opts.get(), kSixDofAxisNames[i]);
                    if (ev::isObject(axVal)) {
                        ev::Persistent ax(axVal);
                        auto axis = static_cast<JPH::SixDOFConstraintSettings::EAxis>(i);
                        double min = getPropNumber(ax, "min", sdc->GetLimitsMin(axis));
                        double max = getPropNumber(ax, "max", sdc->GetLimitsMax(axis));
                        if (i == 0) { tMin.SetX(static_cast<float>(min)); tMax.SetX(static_cast<float>(max)); }
                        else if (i == 1) { tMin.SetY(static_cast<float>(min)); tMax.SetY(static_cast<float>(max)); }
                        else if (i == 2) { tMin.SetZ(static_cast<float>(min)); tMax.SetZ(static_cast<float>(max)); }
                        else if (i == 3) { rMin.SetX(static_cast<float>(min)); rMax.SetX(static_cast<float>(max)); }
                        else if (i == 4) { rMin.SetY(static_cast<float>(min)); rMax.SetY(static_cast<float>(max)); }
                        else if (i == 5) { rMin.SetZ(static_cast<float>(min)); rMax.SetZ(static_cast<float>(max)); }
                        ok = true;
                    }
                }
                if (ok) {
                    sdc->SetTranslationLimits(tMin, tMax);
                    sdc->SetRotationLimits(rMin, rMax);
                }
                break;
            }
            default:
                break;
        }

        if (ok && c->GetType() == JPH::EConstraintType::TwoBodyConstraint) {
            auto* tb = static_cast<JPH::TwoBodyConstraint*>(c);
            if (tb->GetBody1()) world->activate(tb->GetBody1()->GetID());
            if (tb->GetBody2()) world->activate(tb->GetBody2()->GetID());
        }

        return ev::fromBool(ok);
    });
}

}  // namespace bro::bronze_host
