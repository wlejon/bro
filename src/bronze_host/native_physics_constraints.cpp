// native_physics_constraints.cpp — Physics constraint and joint natives.

#include "bronze_host/native_physics_internal.h"

namespace bro::bronze_host {

namespace {

static thread_local std::vector<int32_t> tl_brokenConstraints;

}  // namespace

}  // namespace bro::bronze_host

extern "C" {

using namespace bro;
using namespace bro::bronze_host;

int32_t bro_physics_createConstraint(const char* config) {
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w || !config || !*config) return -1;
    auto res = ev::parseJson(config);
    if (res.thrown || !ev::isObject(res.value)) return -1;

    Value obj = res.value;
    physics::ConstraintOptions cs;

    std::string type = getPropString(obj, "type");
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
    else return -1;

    int32_t b1Tag = static_cast<int32_t>(getPropNumber(obj, "body1", -1.0));
    int32_t b2Tag = static_cast<int32_t>(getPropNumber(obj, "body2", -1.0));

    cs.body1 = pw->bodyIdForTag(b1Tag);
    if (cs.body1.IsInvalid()) return -1;

    if (b2Tag >= 0) {
        cs.body2 = pw->bodyIdForTag(b2Tag);
        if (cs.body2.IsInvalid()) return -1;
    } else {
        cs.body2 = JPH::BodyID();
    }

    cs.point1 = readRVec3(ev::getProperty(obj, "point1"));
    cs.point2 = readRVec3(ev::getProperty(obj, "point2"));
    cs.axis = readVec3(ev::getProperty(obj, "axis"), JPH::Vec3(0, 1, 0));

    cs.minDistance = static_cast<float>(getPropNumber(obj, "minDistance", cs.minDistance));
    cs.maxDistance = static_cast<float>(getPropNumber(obj, "maxDistance", cs.maxDistance));

    Value lmin = ev::getProperty(obj, "limitMin");
    Value lmax = ev::getProperty(obj, "limitMax");
    if (!ev::isUndefined(lmin) && !ev::isObject(lmin) && !ev::isUndefined(lmax) && !ev::isObject(lmax)) {
        cs.limitMin = static_cast<float>(ev::toDouble(lmin));
        cs.limitMax = static_cast<float>(ev::toDouble(lmax));
        cs.hasLimits = true;
    }

    cs.breakingImpulse = static_cast<float>(getPropNumber(obj, "breakingImpulse", cs.breakingImpulse));
    cs.collideConnected = getPropBool(obj, "collideConnected", cs.collideConnected);

    if (cs.type == physics::ConstraintOptions::Wheel) {
        cs.wheelSuspensionAxis = readVec3(ev::getProperty(obj, "suspensionAxis"), JPH::Vec3(0, 1, 0));
        cs.wheelHingeAxis = readVec3(ev::getProperty(obj, "hingeAxis"), JPH::Vec3(0, 0, 1));
        cs.wheelHertz = static_cast<float>(getPropNumber(obj, "hertz", cs.wheelHertz));
        cs.wheelDampingRatio = static_cast<float>(getPropNumber(obj, "dampingRatio", cs.wheelDampingRatio));
        Value lo = ev::getProperty(obj, "lowerTranslation");
        Value hi = ev::getProperty(obj, "upperTranslation");
        if (!ev::isUndefined(lo) && !ev::isObject(lo) && !ev::isUndefined(hi) && !ev::isObject(hi)) {
            cs.wheelLowerTranslation = static_cast<float>(ev::toDouble(lo));
            cs.wheelUpperTranslation = static_cast<float>(ev::toDouble(hi));
            cs.wheelHasTranslationLimits = true;
        }
        cs.wheelEnableMotor = getPropBool(obj, "enableMotor", cs.wheelEnableMotor);
        cs.wheelMotorSpeed = static_cast<float>(getPropNumber(obj, "motorSpeed", cs.wheelMotorSpeed));
        cs.wheelMaxMotorTorque = static_cast<float>(getPropNumber(obj, "maxMotorTorque", cs.wheelMaxMotorTorque));
    }

    if (cs.type == physics::ConstraintOptions::Cone) {
        cs.coneHalfAngle = static_cast<float>(getPropNumber(obj, "halfConeAngle", cs.coneHalfAngle));
    }

    if (cs.type == physics::ConstraintOptions::SwingTwist) {
        cs.planeAxis = readVec3(ev::getProperty(obj, "planeAxis"), JPH::Vec3(0, 1, 0));
        cs.normalHalfConeAngle = static_cast<float>(getPropNumber(obj, "normalHalfConeAngle", cs.normalHalfConeAngle));
        cs.planeHalfConeAngle = static_cast<float>(getPropNumber(obj, "planeHalfConeAngle", cs.planeHalfConeAngle));
        cs.twistMinAngle = static_cast<float>(getPropNumber(obj, "twistMinAngle", cs.twistMinAngle));
        if (cs.twistMinAngle == 0.0f) cs.twistMinAngle = static_cast<float>(getPropNumber(obj, "twistMin", 0.0));
        cs.twistMaxAngle = static_cast<float>(getPropNumber(obj, "twistMaxAngle", cs.twistMaxAngle));
        if (cs.twistMaxAngle == 0.0f) cs.twistMaxAngle = static_cast<float>(getPropNumber(obj, "twistMax", 0.0));
        cs.maxFrictionTorque = static_cast<float>(getPropNumber(obj, "maxFrictionTorque", cs.maxFrictionTorque));
        if (cs.maxFrictionTorque == 0.0f) cs.maxFrictionTorque = static_cast<float>(getPropNumber(obj, "frictionTorque", 0.0));
    }

    if (cs.type == physics::ConstraintOptions::Pulley) {
        cs.bodyPoint1 = readRVec3(ev::getProperty(obj, "bodyPoint1"));
        cs.fixedPoint1 = readRVec3(ev::getProperty(obj, "fixedPoint1"));
        cs.bodyPoint2 = readRVec3(ev::getProperty(obj, "bodyPoint2"));
        cs.fixedPoint2 = readRVec3(ev::getProperty(obj, "fixedPoint2"));
        cs.ratio = static_cast<float>(getPropNumber(obj, "ratio", cs.ratio));
        cs.minLength = static_cast<float>(getPropNumber(obj, "minLength", cs.minLength));
        cs.maxLength = static_cast<float>(getPropNumber(obj, "maxLength", cs.maxLength));
    }

    if (cs.type == physics::ConstraintOptions::Gear ||
        cs.type == physics::ConstraintOptions::RackAndPinion) {
        cs.hingeAxis1 = readVec3(ev::getProperty(obj, "hingeAxis1"), JPH::Vec3(1, 0, 0));
        const char* ha2Prop = (cs.type == physics::ConstraintOptions::Gear) ? "hingeAxis2" : "sliderAxis";
        cs.hingeAxis2 = readVec3(ev::getProperty(obj, ha2Prop), JPH::Vec3(1, 0, 0));
        cs.ratio = static_cast<float>(getPropNumber(obj, "ratio", 1.0));
        cs.dependentConstraint1 = static_cast<uint32_t>(getPropNumber(obj, "constraint1", 0.0));
        cs.dependentConstraint2 = static_cast<uint32_t>(getPropNumber(obj, "constraint2", 0.0));
        if (!cs.dependentConstraint1 || !cs.dependentConstraint2) return -1;
    }

    if (cs.type == physics::ConstraintOptions::SixDOF) {
        cs.sixDofAxisX = readVec3(ev::getProperty(obj, "axisX"), JPH::Vec3(1, 0, 0));
        cs.sixDofAxisY = readVec3(ev::getProperty(obj, "axisY"), JPH::Vec3(0, 1, 0));
        std::string swing = getPropString(obj, "swingType");
        cs.sixDofSwingPyramid = (swing == "pyramid");

        Value axesVal = ev::getProperty(obj, "axes");
        if (ev::isObject(axesVal)) {
            for (int i = 0; i < 6; i++) {
                Value v = ev::getProperty(axesVal, kSixDofAxisNames[i]);
                if (!ev::isUndefined(v) && !ev::isNull(v)) {
                    std::string err;
                    if (!readSixDofAxis(v, cs.sixDofAxes[i], err)) return -1;
                }
            }
        }

        Value motorsVal = ev::getProperty(obj, "motors");
        if (ev::isObject(motorsVal)) {
            for (int i = 0; i < 6; i++) {
                Value v = ev::getProperty(motorsVal, kSixDofAxisNames[i]);
                if (ev::isObject(v)) {
                    physics::MotorOptions m;
                    std::string err;
                    if (!readMotorOptions(v, m, err)) return -1;
                    m.axis = i;
                    cs.motors.push_back(m);
                }
            }
        }
    }

    if (cs.type == physics::ConstraintOptions::Hinge ||
        cs.type == physics::ConstraintOptions::Slider) {
        Value mv = ev::getProperty(obj, "motor");
        if (ev::isObject(mv)) {
            physics::MotorOptions m;
            std::string err;
            if (readMotorOptions(mv, m, err)) {
                cs.motors.push_back(m);
            }
        }
    }

    uint32_t handle = w->createConstraint(cs);
    return handle ? static_cast<int32_t>(handle) : -1;
}

void bro_physics_destroyConstraint(int32_t tag) {
    auto* pw = getActiveWorld();
    if (auto* w = pw ? pw->getWorld() : nullptr) w->destroyConstraint(static_cast<uint32_t>(tag));
}

void bro_physics_setConstraintEnabled(int32_t tag, bool enabled) {
    auto* pw = getActiveWorld();
    if (auto* w = pw ? pw->getWorld() : nullptr) w->setConstraintEnabled(static_cast<uint32_t>(tag), enabled);
}

bool bro_physics_isConstraintEnabled(int32_t tag) {
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    return w ? w->isConstraintEnabled(static_cast<uint32_t>(tag)) : false;
}

void bro_physics_setWheelMotor(int32_t handle, int32_t enabled, double speed, double maxTorque) {
    auto* pw = getActiveWorld();
    if (auto* w = pw ? pw->getWorld() : nullptr) {
        w->setWheelMotor(static_cast<uint32_t>(handle), enabled != 0, static_cast<float>(speed), static_cast<float>(maxTorque));
    }
}

bool bro_physics_setConstraintMotor(int32_t tag, const char* config) {
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w || !config || !*config) return false;
    auto res = ev::parseJson(config);
    if (res.thrown || !ev::isObject(res.value)) return false;

    physics::MotorOptions m;
    std::string err;
    if (!readMotorOptions(res.value, m, err)) return false;
    return w->setConstraintMotor(static_cast<uint32_t>(tag), m);
}

void bro_physics_setConstraintBreakingImpulse(int32_t tag, double impulse) {
    auto* pw = getActiveWorld();
    if (auto* w = pw ? pw->getWorld() : nullptr) {
        w->setConstraintBreakingImpulse(static_cast<uint32_t>(tag), static_cast<float>(impulse));
    }
}

double bro_physics_getConstraintBreakingImpulse(int32_t tag) {
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    return w ? static_cast<double>(w->getConstraintBreakingImpulse(static_cast<uint32_t>(tag))) : 0.0;
}

void bro_physics_getBrokenConstraints(bronze_native_buffer* out) {
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    tl_brokenConstraints.clear();
    if (w) {
        auto broken = w->drainBrokenConstraints();
        for (uint32_t c : broken) {
            tl_brokenConstraints.push_back(static_cast<int32_t>(c));
        }
    }
    out->data = tl_brokenConstraints.data();
    out->length = static_cast<uint32_t>(tl_brokenConstraints.size());
    out->release = nullptr;
}

}  // extern "C"
