// native_physics_ragdoll.cpp — Physics ragdoll natives.

#include "bronze_host/native_physics_internal.h"
#include <sstream>

namespace bro::bronze_host {

namespace {

static thread_local std::vector<float> tl_poseBuf;
static thread_local std::vector<float> tl_localPoseBuf;

void readRagdollMotor(Value oVal, physics::RagdollMotorOptions& m) {
    if (!ev::isObject(oVal)) return;
    m.frequency = static_cast<float>(getPropNumber(oVal, "frequency", m.frequency));
    m.damping = static_cast<float>(getPropNumber(oVal, "damping", m.damping));
    m.maxTorque = static_cast<float>(getPropNumber(oVal, "maxTorque", m.maxTorque));
}

bool readRagdollPart(Value oVal, const std::vector<std::string>& names,
                     physics::RagdollPartOptions& p, std::string& err) {
    if (!ev::isObject(oVal)) { err = "part must be an object"; return false; }
    p.name = getPropString(oVal, "name");

    Value pv = ev::getProperty(oVal, "parent");
    if (!ev::isUndefined(pv) && !ev::isNull(pv)) {
        if (!ev::isObject(pv)) {
            std::string s = ev::toUtf8(pv);
            int idx = -2;
            if (parseDecimalIndex(s, idx)) {
                p.parentIndex = idx;
            } else {
                for (size_t i = 0; i < names.size(); ++i) {
                    if (names[i] == s) { idx = static_cast<int>(i); break; }
                }
                if (idx == -2) {
                    err = "unknown parent part '" + s + "' (parents must appear earlier in the parts array)";
                    return false;
                }
                p.parentIndex = idx;
            }
        }
    } else {
        p.parentIndex = -1;
    }
    if (p.parentIndex < -1 || p.parentIndex >= static_cast<int>(names.size())) {
        err = "part parent must be -1 (root) or an EARLIER part's index/name";
        return false;
    }

    p.position = readVec3(ev::getProperty(oVal, "position"));
    p.rotation = readQuat(ev::getProperty(oVal, "rotation"));

    std::string shape = getPropString(oVal, "shape");
    if (shape == "capsule" || shape.empty()) p.shape = physics::RagdollPartOptions::ShapeCapsule;
    else if (shape == "box")    p.shape = physics::RagdollPartOptions::ShapeBox;
    else if (shape == "sphere") p.shape = physics::RagdollPartOptions::ShapeSphere;
    else { err = "part shape must be 'capsule' | 'box' | 'sphere'"; return false; }

    p.halfHeight = static_cast<float>(getPropNumber(oVal, "halfHeight", p.halfHeight));
    p.radius = static_cast<float>(getPropNumber(oVal, "radius", p.radius));
    p.halfExtents = readVec3(ev::getProperty(oVal, "halfExtents"), JPH::Vec3(0.1f, 0.1f, 0.1f));

    p.density = static_cast<float>(getPropNumber(oVal, "density", p.density));
    p.mass = static_cast<float>(getPropNumber(oVal, "mass", p.mass));
    p.friction = static_cast<float>(getPropNumber(oVal, "friction", p.friction));
    p.restitution = static_cast<float>(getPropNumber(oVal, "restitution", p.restitution));

    Value jv = ev::getProperty(oVal, "joint");
    if (ev::isObject(jv)) {
        std::string jt = getPropString(jv, "type");
        if (jt == "fixed") p.joint = physics::RagdollPartOptions::JointFixed;
        else if (jt == "swingTwist" || jt == "swing-twist" || jt.empty())
            p.joint = physics::RagdollPartOptions::JointSwingTwist;
        else {
            err = "joint type must be 'swingTwist' | 'fixed'";
            return false;
        }
        Value ptVal = ev::getProperty(jv, "point");
        if (ev::isObject(ptVal)) { p.hasJointPoint = true; p.jointPoint = readVec3(ptVal); }
        Value taVal = ev::getProperty(jv, "twistAxis");
        if (ev::isObject(taVal)) { p.hasTwistAxis = true; p.twistAxis = readVec3(taVal, JPH::Vec3(0, 1, 0)); }
        Value paVal = ev::getProperty(jv, "planeAxis");
        if (ev::isObject(paVal)) { p.hasPlaneAxis = true; p.planeAxis = readVec3(paVal); }
        p.normalHalfConeAngle = static_cast<float>(getPropNumber(jv, "normalHalfConeAngle", 0.0));
        p.planeHalfConeAngle = static_cast<float>(getPropNumber(jv, "planeHalfConeAngle", p.normalHalfConeAngle));
        p.twistMinAngle = static_cast<float>(getPropNumber(jv, "twistMin", 0.0));
        p.twistMaxAngle = static_cast<float>(getPropNumber(jv, "twistMax", 0.0));
        p.maxFrictionTorque = static_cast<float>(getPropNumber(jv, "frictionTorque", 0.0));
    }
    return true;
}

bool readRagdollPose(const char* poseJson, int partCount, std::vector<physics::RagdollPartState>& out) {
    if (!poseJson || !*poseJson) return false;
    auto res = ev::parseJson(poseJson);
    if (res.thrown || !ev::isObject(res.value)) return false;
    std::vector<float> flat;
    if (!readFloatVector(res.value, flat)) return false;
    out.resize(partCount);
    if (flat.size() == static_cast<size_t>(partCount) * 7) {
        for (int i = 0; i < partCount; ++i) {
            const float* p = flat.data() + i * 7;
            out[i].position = JPH::RVec3(p[0], p[1], p[2]);
            out[i].rotation = JPH::Quat(p[3], p[4], p[5], p[6]).Normalized();
        }
        return true;
    }
    if (flat.size() == static_cast<size_t>(partCount) * 16) {
        for (int i = 0; i < partCount; ++i) {
            const float* m = flat.data() + i * 16;
            JPH::Vec3 c0 = JPH::Vec3(m[0], m[1], m[2]).NormalizedOr(JPH::Vec3::sAxisX());
            JPH::Vec3 c1 = JPH::Vec3(m[4], m[5], m[6]).NormalizedOr(JPH::Vec3::sAxisY());
            JPH::Vec3 c2 = JPH::Vec3(m[8], m[9], m[10]).NormalizedOr(JPH::Vec3::sAxisZ());
            JPH::Mat44 rot(JPH::Vec4(c0, 0), JPH::Vec4(c1, 0), JPH::Vec4(c2, 0),
                           JPH::Vec4(0, 0, 0, 1));
            out[i].position = JPH::RVec3(m[12], m[13], m[14]);
            out[i].rotation = rot.GetQuaternion().Normalized();
        }
        return true;
    }
    return false;
}

void destroyRagdollImpl(HostPhysicsRagdoll* r) {
    if (!r || !r->handle) return;
    HostPhysicsWorld* pw = r->world ? r->world : getActiveWorld();
    auto* world = pw ? pw->getWorld() : nullptr;
    if (world) {
        world->destroyRagdoll(r->handle, [pw](JPH::BodyID bid) { pw->unregisterBodyId(bid); });
        for (int32_t tag : r->partTags) pw->unregisterBody(tag);
    }
    // Off the live set, so the world's destructor no longer clears this
    // pointer: drop it here (native_physics_softbody.cpp says why).
    if (r->world) r->world->liveRagdolls.erase(r);
    r->world = nullptr;
    r->handle = 0;
    r->partTags.clear();
}

}  // namespace

}  // namespace bro::bronze_host

extern "C" {

using namespace bro;
using namespace bro::bronze_host;

void bro_physics_PhysicsRagdoll_dtor(void* self) {
    auto* pr = static_cast<HostPhysicsRagdoll*>(self);
    if (!pr) return;
    destroyRagdollImpl(pr);
    delete pr;
}

void* bro_physics_PhysicsRagdoll_ctor(void) {
    return nullptr;
}

void* bro_physics_createRagdoll(const char* config) {
    auto* pw = getActiveWorld();
    auto* world = pw ? pw->getWorld() : nullptr;
    if (!world || !config || !*config) return nullptr;
    auto res = ev::parseJson(config);
    if (res.thrown || !ev::isObject(res.value)) return nullptr;

    Value optsVal = res.value;
    physics::RagdollOptions opts;

    opts.position = readVec3(ev::getProperty(optsVal, "position"));
    opts.rotation = readQuat(ev::getProperty(optsVal, "rotation"));

    Value layerVal = ev::getProperty(optsVal, "layer");
    if (!ev::isUndefined(layerVal) && !ev::isNull(layerVal)) {
        if (!ev::isObject(layerVal)) {
            std::string s = ev::toUtf8(layerVal);
            int idx = -1;
            if (parseDecimalIndex(s, idx)) opts.layer = idx;
            else opts.layer = world->layerIndex(s);
        }
    }

    opts.gravityFactor = static_cast<float>(getPropNumber(optsVal, "gravityFactor", opts.gravityFactor));
    opts.linearDamping = static_cast<float>(getPropNumber(optsVal, "linearDamping", opts.linearDamping));
    opts.angularDamping = static_cast<float>(getPropNumber(optsVal, "angularDamping", opts.angularDamping));
    opts.stabilize = getPropBool(optsVal, "stabilize", opts.stabilize);
    opts.activate = getPropBool(optsVal, "activate", opts.activate);

    Value motorV = ev::getProperty(optsVal, "motor");
    if (ev::isObject(motorV)) readRagdollMotor(motorV, opts.motor);

    Value partsV = ev::getProperty(optsVal, "parts");
    std::vector<std::string> names;
    if (ev::isObject(partsV)) {
        Value lenV = ev::getProperty(partsV, "length");
        uint32_t np = (!ev::isUndefined(lenV) && !ev::isObject(lenV)) ? static_cast<uint32_t>(ev::toDouble(lenV)) : 0;
        for (uint32_t i = 0; i < np; ++i) {
            Value pv = ev::getElement(partsV, i);
            physics::RagdollPartOptions part;
            std::string err;
            if (!readRagdollPart(pv, names, part, err)) return nullptr;
            names.push_back(part.name.empty() ? ("part" + std::to_string(i)) : part.name);
            opts.parts.push_back(std::move(part));
        }
    }
    if (opts.parts.empty()) return nullptr;

    uint32_t handle = world->createRagdoll(opts);
    if (!handle) return nullptr;

    auto* jr = new HostPhysicsRagdoll();
    jr->world = pw;
    jr->handle = handle;
    jr->names = std::move(names);
    const int n = static_cast<int>(opts.parts.size());
    jr->partTags.reserve(n);
    jr->parents.reserve(n);
    for (int i = 0; i < n; ++i) {
        jr->partTags.push_back(pw->registerBody(world->ragdollPartBody(handle, i)));
        jr->parents.push_back(opts.parts[i].parentIndex);
    }
    pw->liveRagdolls.insert(jr);
    return jr;
}

void bro_physics_PhysicsRagdoll_pose(void* self, bronze_native_buffer* out) {
    auto* r = static_cast<HostPhysicsRagdoll*>(self);
    if (!r || !r->handle) {
        out->data = nullptr; out->length = 0; out->release = nullptr; return;
    }
    HostPhysicsWorld* pw = r->world ? r->world : getActiveWorld();
    auto* world = pw ? pw->getWorld() : nullptr;
    if (!world) {
        out->data = nullptr; out->length = 0; out->release = nullptr; return;
    }
    std::vector<physics::RagdollPartState> states;
    if (!world->getRagdollPose(r->handle, states)) {
        out->data = nullptr; out->length = 0; out->release = nullptr; return;
    }
    tl_poseBuf.resize(states.size() * 7);
    for (size_t i = 0; i < states.size(); ++i) {
        float* p = tl_poseBuf.data() + i * 7;
        p[0] = static_cast<float>(states[i].position.GetX());
        p[1] = static_cast<float>(states[i].position.GetY());
        p[2] = static_cast<float>(states[i].position.GetZ());
        p[3] = states[i].rotation.GetX();
        p[4] = states[i].rotation.GetY();
        p[5] = states[i].rotation.GetZ();
        p[6] = states[i].rotation.GetW();
    }
    out->data = tl_poseBuf.data();
    out->length = static_cast<uint32_t>(tl_poseBuf.size());
    out->release = nullptr;
}

void bro_physics_PhysicsRagdoll_localPose(void* self, bronze_native_buffer* out) {
    auto* r = static_cast<HostPhysicsRagdoll*>(self);
    if (!r || !r->handle) {
        out->data = nullptr; out->length = 0; out->release = nullptr; return;
    }
    HostPhysicsWorld* pw = r->world ? r->world : getActiveWorld();
    auto* world = pw ? pw->getWorld() : nullptr;
    if (!world) {
        out->data = nullptr; out->length = 0; out->release = nullptr; return;
    }
    std::vector<physics::RagdollPartState> states;
    if (!world->getRagdollPose(r->handle, states)) {
        out->data = nullptr; out->length = 0; out->release = nullptr; return;
    }
    tl_localPoseBuf.resize(states.size() * 7);
    for (size_t i = 0; i < states.size(); ++i) {
        int parent = i < r->parents.size() ? r->parents[i] : -1;
        JPH::RVec3 pos = states[i].position;
        JPH::Quat rot = states[i].rotation;
        if (parent >= 0 && static_cast<size_t>(parent) < states.size()) {
            JPH::Quat pinv = states[parent].rotation.Conjugated();
            pos = pinv * (states[i].position - states[parent].position);
            rot = (pinv * rot).Normalized();
        }
        float* p = tl_localPoseBuf.data() + i * 7;
        p[0] = static_cast<float>(pos.GetX());
        p[1] = static_cast<float>(pos.GetY());
        p[2] = static_cast<float>(pos.GetZ());
        p[3] = rot.GetX();
        p[4] = rot.GetY();
        p[5] = rot.GetZ();
        p[6] = rot.GetW();
    }
    out->data = tl_localPoseBuf.data();
    out->length = static_cast<uint32_t>(tl_localPoseBuf.size());
    out->release = nullptr;
}

bool bro_physics_PhysicsRagdoll_setPose(void* self, const char* poseJson) {
    auto* r = static_cast<HostPhysicsRagdoll*>(self);
    if (!r || !r->handle || !poseJson) return false;
    HostPhysicsWorld* pw = r->world ? r->world : getActiveWorld();
    auto* world = pw ? pw->getWorld() : nullptr;
    if (!world) return false;

    int n = world->ragdollPartCount(r->handle);
    std::vector<physics::RagdollPartState> states;
    if (n <= 0 || !readRagdollPose(poseJson, n, states)) return false;
    return world->setRagdollPose(r->handle, states);
}

bool bro_physics_PhysicsRagdoll_driveToPose(void* self, const char* poseJson, const char* motorJson) {
    auto* r = static_cast<HostPhysicsRagdoll*>(self);
    if (!r || !r->handle || !poseJson) return false;
    HostPhysicsWorld* pw = r->world ? r->world : getActiveWorld();
    auto* world = pw ? pw->getWorld() : nullptr;
    if (!world) return false;

    int n = world->ragdollPartCount(r->handle);
    std::vector<physics::RagdollPartState> states;
    if (n <= 0 || !readRagdollPose(poseJson, n, states)) return false;

    physics::RagdollMotorOptions motor;
    bool hasMotor = false;
    if (motorJson && *motorJson) {
        auto res = ev::parseJson(motorJson);
        if (!res.thrown && ev::isObject(res.value)) {
            readRagdollMotor(res.value, motor);
            hasMotor = true;
        }
    }
    return world->driveRagdollToPose(r->handle, states, hasMotor ? &motor : nullptr);
}

bool bro_physics_PhysicsRagdoll_driveToPoseKinematic(void* self, const char* poseJson, double dt) {
    auto* r = static_cast<HostPhysicsRagdoll*>(self);
    if (!r || !r->handle || !poseJson) return false;
    HostPhysicsWorld* pw = r->world ? r->world : getActiveWorld();
    auto* world = pw ? pw->getWorld() : nullptr;
    if (!world) return false;

    int n = world->ragdollPartCount(r->handle);
    std::vector<physics::RagdollPartState> states;
    if (n <= 0 || !readRagdollPose(poseJson, n, states)) return false;
    return world->driveRagdollToPoseKinematic(r->handle, states, static_cast<float>(dt));
}

void bro_physics_PhysicsRagdoll_stopDrive(void* self) {
    auto* r = static_cast<HostPhysicsRagdoll*>(self);
    if (!r || !r->handle) return;
    HostPhysicsWorld* pw = r->world ? r->world : getActiveWorld();
    if (auto* world = pw ? pw->getWorld() : nullptr) world->stopRagdollDrive(r->handle);
}

void bro_physics_PhysicsRagdoll_addImpulse(void* self, double x, double y, double z) {
    auto* r = static_cast<HostPhysicsRagdoll*>(self);
    if (!r || !r->handle) return;
    HostPhysicsWorld* pw = r->world ? r->world : getActiveWorld();
    if (auto* world = pw ? pw->getWorld() : nullptr) {
        world->addRagdollImpulse(r->handle, JPH::Vec3((float)x, (float)y, (float)z));
    }
}

void bro_physics_PhysicsRagdoll_activate(void* self) {
    auto* r = static_cast<HostPhysicsRagdoll*>(self);
    if (!r || !r->handle) return;
    HostPhysicsWorld* pw = r->world ? r->world : getActiveWorld();
    if (auto* world = pw ? pw->getWorld() : nullptr) world->activateRagdoll(r->handle);
}

void bro_physics_PhysicsRagdoll_deactivate(void* self) {
    auto* r = static_cast<HostPhysicsRagdoll*>(self);
    if (!r || !r->handle) return;
    HostPhysicsWorld* pw = r->world ? r->world : getActiveWorld();
    if (auto* world = pw ? pw->getWorld() : nullptr) world->deactivateRagdoll(r->handle);
}

bool bro_physics_PhysicsRagdoll_isActive(void* self) {
    auto* r = static_cast<HostPhysicsRagdoll*>(self);
    if (!r || !r->handle) return false;
    HostPhysicsWorld* pw = r->world ? r->world : getActiveWorld();
    auto* world = pw ? pw->getWorld() : nullptr;
    return world ? world->isRagdollActive(r->handle) : false;
}

int32_t bro_physics_PhysicsRagdoll_partCount_get(void* self) {
    auto* r = static_cast<HostPhysicsRagdoll*>(self);
    if (!r || !r->handle) return 0;
    HostPhysicsWorld* pw = r->world ? r->world : getActiveWorld();
    auto* world = pw ? pw->getWorld() : nullptr;
    int n = world ? world->ragdollPartCount(r->handle) : 0;
    return n < 0 ? 0 : n;
}

int32_t bro_physics_PhysicsRagdoll_partBody(void* self, int32_t index) {
    auto* r = static_cast<HostPhysicsRagdoll*>(self);
    if (!r || !r->handle) return -1;
    HostPhysicsWorld* pw = r->world ? r->world : getActiveWorld();
    auto* world = pw ? pw->getWorld() : nullptr;
    if (!world || world->ragdollPartCount(r->handle) <= 0) return -1;
    if (index < 0 || index >= static_cast<int32_t>(r->partTags.size())) return -1;
    return r->partTags[index];
}

int32_t bro_physics_PhysicsRagdoll_partParent(void* self, int32_t index) {
    auto* r = static_cast<HostPhysicsRagdoll*>(self);
    if (!r) return -1;
    if (index < 0 || index >= static_cast<int32_t>(r->parents.size())) return -1;
    return r->parents[index];
}

int32_t bro_physics_PhysicsRagdoll_partIndex(void* self, const char* name) {
    auto* r = static_cast<HostPhysicsRagdoll*>(self);
    if (!r || !name) return -1;
    std::string s = name;
    for (size_t i = 0; i < r->names.size(); ++i) {
        if (r->names[i] == s) return static_cast<int32_t>(i);
    }
    return -1;
}

void bro_physics_PhysicsRagdoll_destroy(void* self) {
    auto* pr = static_cast<HostPhysicsRagdoll*>(self);
    if (!pr) return;
    destroyRagdollImpl(pr);
}

}  // extern "C"
