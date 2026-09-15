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
    auto* w = g_defaultWorld.getWorld();
    if (!w || !config || !*config) return -1;
    auto res = ev::parseJson(config);
    if (res.thrown || !ev::isObject(res.value)) return -1;

    physics::ConstraintOptions cs;
    std::string type = getPropString(res.value, "type");
    if (type == "distance") cs.type = physics::ConstraintOptions::Distance;
    else if (type == "point") cs.type = physics::ConstraintOptions::Point;
    else if (type == "hinge") cs.type = physics::ConstraintOptions::Hinge;
    else if (type == "fixed") cs.type = physics::ConstraintOptions::Fixed;
    else if (type == "slider") cs.type = physics::ConstraintOptions::Slider;
    else if (type == "sixdof" || type == "sixDof") cs.type = physics::ConstraintOptions::SixDOF;
    else if (type == "cone") cs.type = physics::ConstraintOptions::Cone;
    else if (type == "swingTwist") cs.type = physics::ConstraintOptions::SwingTwist;
    else if (type == "pulley") cs.type = physics::ConstraintOptions::Pulley;
    else if (type == "gear") cs.type = physics::ConstraintOptions::Gear;
    else if (type == "rackAndPinion") cs.type = physics::ConstraintOptions::RackAndPinion;
    else return -1;

    int32_t b1Tag = static_cast<int32_t>(getPropNumber(res.value, "body1", -1));
    int32_t b2Tag = static_cast<int32_t>(getPropNumber(res.value, "body2", -1));
    cs.body1 = g_defaultWorld.bodyIdForTag(b1Tag);
    cs.body2 = g_defaultWorld.bodyIdForTag(b2Tag);
    if (cs.body1.IsInvalid() || cs.body2.IsInvalid()) return -1;

    cs.point1 = readRVec3(ev::getProperty(res.value, "point1"));
    cs.point2 = readRVec3(ev::getProperty(res.value, "point2"));
    cs.minDistance = static_cast<float>(getPropNumber(res.value, "minDistance", cs.minDistance));
    cs.maxDistance = static_cast<float>(getPropNumber(res.value, "maxDistance", cs.maxDistance));

    return w->createConstraint(cs);
}

void bro_physics_destroyConstraint(int32_t tag) {
    if (auto* w = g_defaultWorld.getWorld()) w->destroyConstraint(tag);
}

void bro_physics_setConstraintEnabled(int32_t tag, bool enabled) {
    if (auto* w = g_defaultWorld.getWorld()) w->setConstraintEnabled(tag, enabled);
}

bool bro_physics_isConstraintEnabled(int32_t tag) {
    auto* w = g_defaultWorld.getWorld();
    return w ? w->isConstraintEnabled(tag) : false;
}

void bro_physics_setWheelMotor(int32_t vehicleTag, int32_t wheelIndex, double motorTorque, double brakeTorque) {}

void bro_physics_setConstraintMotor(int32_t tag, const char* config) {}

void bro_physics_setConstraintBreakingImpulse(int32_t tag, double impulse) {}

double bro_physics_getConstraintBreakingImpulse(int32_t tag) {
    return 0.0;
}

void bro_physics_getBrokenConstraints(bronze_native_buffer* out) {
    tl_brokenConstraints.clear();
    out->data = tl_brokenConstraints.data();
    out->length = static_cast<uint32_t>(tl_brokenConstraints.size());
    out->release = nullptr;
}

}  // extern "C"
