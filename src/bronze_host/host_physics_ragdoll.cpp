// Physics ragdoll bindings for the bronze host layer.

#include "bronze_host/host_physics_internal.h"

namespace bro::bronze_host {

namespace {

void readRagdollMotor(Value oVal, physics::RagdollMotorOptions& m) {
    if (!ev::isObject(oVal)) return;
    ev::Persistent o(oVal);
    m.frequency = static_cast<float>(getPropNumber(o, "frequency", m.frequency));
    m.damping = static_cast<float>(getPropNumber(o, "damping", m.damping));
    m.maxTorque = static_cast<float>(getPropNumber(o, "maxTorque", m.maxTorque));
}

bool readRagdollPart(Value oVal, const std::vector<std::string>& names,
                     physics::RagdollPartOptions& p, std::string& err) {
    if (!ev::isObject(oVal)) { err = "part must be an object"; return false; }
    ev::Persistent o(oVal);
    p.name = getPropString(o, "name");

    Value pv = ev::getProperty(o.get(), "parent");
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

    p.position = readVec3(ev::getProperty(o.get(), "position"));
    p.rotation = readQuat(ev::getProperty(o.get(), "rotation"));

    std::string shape = getPropString(o, "shape");
    if (shape == "capsule" || shape.empty()) p.shape = physics::RagdollPartOptions::ShapeCapsule;
    else if (shape == "box")    p.shape = physics::RagdollPartOptions::ShapeBox;
    else if (shape == "sphere") p.shape = physics::RagdollPartOptions::ShapeSphere;
    else { err = "part shape must be 'capsule' | 'box' | 'sphere'"; return false; }

    p.halfHeight = static_cast<float>(getPropNumber(o, "halfHeight", p.halfHeight));
    p.radius = static_cast<float>(getPropNumber(o, "radius", p.radius));
    p.halfExtents = readVec3(ev::getProperty(o.get(), "halfExtents"), JPH::Vec3(0.1f, 0.1f, 0.1f));

    p.density = static_cast<float>(getPropNumber(o, "density", p.density));
    p.mass = static_cast<float>(getPropNumber(o, "mass", p.mass));
    p.friction = static_cast<float>(getPropNumber(o, "friction", p.friction));
    p.restitution = static_cast<float>(getPropNumber(o, "restitution", p.restitution));

    Value jv = ev::getProperty(o.get(), "joint");
    if (ev::isObject(jv)) {
        ev::Persistent jp(jv);
        std::string jt = getPropString(jp, "type");
        if (jt == "fixed") p.joint = physics::RagdollPartOptions::JointFixed;
        else if (jt == "swingTwist" || jt == "swing-twist" || jt.empty())
            p.joint = physics::RagdollPartOptions::JointSwingTwist;
        else {
            err = "joint type must be 'swingTwist' | 'fixed'";
            return false;
        }
        Value ptVal = ev::getProperty(jp.get(), "point");
        if (ev::isObject(ptVal)) { p.hasJointPoint = true; p.jointPoint = readVec3(ptVal); }
        Value taVal = ev::getProperty(jp.get(), "twistAxis");
        if (ev::isObject(taVal)) { p.hasTwistAxis = true; p.twistAxis = readVec3(taVal, JPH::Vec3(0, 1, 0)); }
        Value paVal = ev::getProperty(jp.get(), "planeAxis");
        if (ev::isObject(paVal)) { p.hasPlaneAxis = true; p.planeAxis = readVec3(paVal); }
        p.normalHalfConeAngle = static_cast<float>(getPropNumber(jp, "normalHalfConeAngle", 0.0));
        p.planeHalfConeAngle = static_cast<float>(getPropNumber(jp, "planeHalfConeAngle", p.normalHalfConeAngle));
        p.twistMinAngle = static_cast<float>(getPropNumber(jp, "twistMin", 0.0));
        p.twistMaxAngle = static_cast<float>(getPropNumber(jp, "twistMax", 0.0));
        p.maxFrictionTorque = static_cast<float>(getPropNumber(jp, "frictionTorque", 0.0));
    }
    return true;
}

bool readRagdollPose(Value v, int partCount, std::vector<physics::RagdollPartState>& out) {
    std::vector<float> flat;
    if (!readFloatVector(v, flat)) return false;
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
    HostPhysicsWorld* pw = r->world ? r->world : &g_defaultWorld;
    auto* world = pw->getWorld();
    if (world) {
        world->destroyRagdoll(r->handle, [pw](JPH::BodyID bid) { pw->unregisterBodyId(bid); });
        for (int32_t tag : r->partTags) pw->unregisterBody(tag);
    }
    if (r->world) r->world->liveRagdolls.erase(r);
    r->handle = 0;
    r->partTags.clear();
}

}  // namespace

void decorateRagdollProto(ObjectBuilder& rb) {
    rb.def("pose", 0, [](Value self, std::span<const Value>) -> Value {
        HostPhysicsRagdoll* r = unwrapRagdoll(self);
        if (!r || !r->handle) return ev::null();
        HostPhysicsWorld* pw = r->world ? r->world : &g_defaultWorld;
        auto* world = pw->getWorld();
        if (!world) return ev::null();

        std::vector<physics::RagdollPartState> states;
        if (!world->getRagdollPose(r->handle, states)) return ev::null();
        std::vector<float> flat(states.size() * 7);
        for (size_t i = 0; i < states.size(); ++i) {
            float* p = flat.data() + i * 7;
            p[0] = static_cast<float>(states[i].position.GetX());
            p[1] = static_cast<float>(states[i].position.GetY());
            p[2] = static_cast<float>(states[i].position.GetZ());
            p[3] = states[i].rotation.GetX();
            p[4] = states[i].rotation.GetY();
            p[5] = states[i].rotation.GetZ();
            p[6] = states[i].rotation.GetW();
        }
        return makeFloat32Array(flat.data(), flat.size());
    });

    rb.def("localPose", 0, [](Value self, std::span<const Value>) -> Value {
        HostPhysicsRagdoll* r = unwrapRagdoll(self);
        if (!r || !r->handle) return ev::null();
        HostPhysicsWorld* pw = r->world ? r->world : &g_defaultWorld;
        auto* world = pw->getWorld();
        if (!world) return ev::null();

        std::vector<physics::RagdollPartState> states;
        if (!world->getRagdollPose(r->handle, states)) return ev::null();
        std::vector<float> flat(states.size() * 7);
        for (size_t i = 0; i < states.size(); ++i) {
            int parent = i < r->parents.size() ? r->parents[i] : -1;
            JPH::RVec3 pos = states[i].position;
            JPH::Quat rot = states[i].rotation;
            if (parent >= 0 && static_cast<size_t>(parent) < states.size()) {
                JPH::Quat pinv = states[parent].rotation.Conjugated();
                pos = pinv * (states[i].position - states[parent].position);
                rot = (pinv * rot).Normalized();
            }
            float* p = flat.data() + i * 7;
            p[0] = static_cast<float>(pos.GetX());
            p[1] = static_cast<float>(pos.GetY());
            p[2] = static_cast<float>(pos.GetZ());
            p[3] = rot.GetX();
            p[4] = rot.GetY();
            p[5] = rot.GetZ();
            p[6] = rot.GetW();
        }
        return makeFloat32Array(flat.data(), flat.size());
    });

    rb.def("setPose", 1, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsRagdoll* r = unwrapRagdoll(self);
        if (!r || !r->handle || a.empty()) return ev::fromBool(false);
        HostPhysicsWorld* pw = r->world ? r->world : &g_defaultWorld;
        auto* world = pw->getWorld();
        if (!world) return ev::fromBool(false);

        int n = world->ragdollPartCount(r->handle);
        std::vector<physics::RagdollPartState> states;
        if (n <= 0 || !readRagdollPose(a[0], n, states)) {
            return ev::throwTypeError("setPose expects partCount*7 ([px,py,pz,qx,qy,qz,qw] per part) or partCount*16 (mat4 per part) floats");
        }
        return ev::fromBool(world->setRagdollPose(r->handle, states));
    });

    rb.def("driveToPose", 2, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsRagdoll* r = unwrapRagdoll(self);
        if (!r || !r->handle || a.empty()) return ev::fromBool(false);
        HostPhysicsWorld* pw = r->world ? r->world : &g_defaultWorld;
        auto* world = pw->getWorld();
        if (!world) return ev::fromBool(false);

        int n = world->ragdollPartCount(r->handle);
        std::vector<physics::RagdollPartState> states;
        if (n <= 0 || !readRagdollPose(a[0], n, states)) {
            return ev::throwTypeError("driveToPose expects partCount*7 or partCount*16 floats");
        }
        physics::RagdollMotorOptions motor;
        bool hasMotor = false;
        if (a.size() >= 2 && ev::isObject(a[1])) {
            readRagdollMotor(a[1], motor);
            hasMotor = true;
        }
        return ev::fromBool(world->driveRagdollToPose(r->handle, states, hasMotor ? &motor : nullptr));
    });

    rb.def("driveToPoseKinematic", 2, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsRagdoll* r = unwrapRagdoll(self);
        if (!r || !r->handle || a.size() < 2) return ev::fromBool(false);
        HostPhysicsWorld* pw = r->world ? r->world : &g_defaultWorld;
        auto* world = pw->getWorld();
        if (!world) return ev::fromBool(false);

        int n = world->ragdollPartCount(r->handle);
        std::vector<physics::RagdollPartState> states;
        if (n <= 0 || !readRagdollPose(a[0], n, states)) {
            return ev::throwTypeError("driveToPoseKinematic expects partCount*7 or partCount*16 floats");
        }
        double dt = numAt(a, 1);
        return ev::fromBool(world->driveRagdollToPoseKinematic(r->handle, states, static_cast<float>(dt)));
    });

    rb.def("stopDrive", 0, [](Value self, std::span<const Value>) -> Value {
        HostPhysicsRagdoll* r = unwrapRagdoll(self);
        if (!r || !r->handle) return ev::undefined();
        HostPhysicsWorld* pw = r->world ? r->world : &g_defaultWorld;
        auto* world = pw->getWorld();
        if (world) world->stopRagdollDrive(r->handle);
        return ev::undefined();
    });

    rb.def("addImpulse", 3, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsRagdoll* r = unwrapRagdoll(self);
        if (!r || !r->handle || a.size() < 3) return ev::undefined();
        HostPhysicsWorld* pw = r->world ? r->world : &g_defaultWorld;
        auto* world = pw->getWorld();
        if (world) {
            world->addRagdollImpulse(r->handle, JPH::Vec3(static_cast<float>(numAt(a, 0)),
                                                          static_cast<float>(numAt(a, 1)),
                                                          static_cast<float>(numAt(a, 2))));
        }
        return ev::undefined();
    });

    rb.def("activate", 0, [](Value self, std::span<const Value>) -> Value {
        HostPhysicsRagdoll* r = unwrapRagdoll(self);
        if (!r || !r->handle) return ev::undefined();
        HostPhysicsWorld* pw = r->world ? r->world : &g_defaultWorld;
        auto* world = pw->getWorld();
        if (world) world->activateRagdoll(r->handle);
        return ev::undefined();
    });

    rb.def("deactivate", 0, [](Value self, std::span<const Value>) -> Value {
        HostPhysicsRagdoll* r = unwrapRagdoll(self);
        if (!r || !r->handle) return ev::undefined();
        HostPhysicsWorld* pw = r->world ? r->world : &g_defaultWorld;
        auto* world = pw->getWorld();
        if (world) world->deactivateRagdoll(r->handle);
        return ev::undefined();
    });

    rb.accessor("isActive", [](Value self, std::span<const Value>) -> Value {
        HostPhysicsRagdoll* r = unwrapRagdoll(self);
        if (!r || !r->handle) return ev::fromBool(false);
        HostPhysicsWorld* pw = r->world ? r->world : &g_defaultWorld;
        auto* world = pw->getWorld();
        return ev::fromBool(world ? world->isRagdollActive(r->handle) : false);
    }, nullptr);

    rb.accessor("partCount", [](Value self, std::span<const Value>) -> Value {
        HostPhysicsRagdoll* r = unwrapRagdoll(self);
        if (!r || !r->handle) return ev::fromDouble(0);
        HostPhysicsWorld* pw = r->world ? r->world : &g_defaultWorld;
        auto* world = pw->getWorld();
        int n = world ? world->ragdollPartCount(r->handle) : 0;
        return ev::fromDouble(n < 0 ? 0 : n);
    }, nullptr);

    rb.def("partBody", 1, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsRagdoll* r = unwrapRagdoll(self);
        if (!r || !r->handle || a.empty()) return ev::fromDouble(-1);
        int32_t i = i32At(a, 0);
        if (i < 0 || i >= static_cast<int32_t>(r->partTags.size())) return ev::fromDouble(-1);
        return ev::fromDouble(r->partTags[i]);
    });

    rb.def("partParent", 1, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsRagdoll* r = unwrapRagdoll(self);
        if (!r || a.empty()) return ev::fromDouble(-1);
        int32_t i = i32At(a, 0);
        if (i < 0 || i >= static_cast<int32_t>(r->parents.size())) return ev::fromDouble(-1);
        return ev::fromDouble(r->parents[i]);
    });

    rb.def("partIndex", 1, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsRagdoll* r = unwrapRagdoll(self);
        if (!r || a.empty()) return ev::fromDouble(-1);
        std::string s = !ev::isObject(a[0]) ? ev::toUtf8(a[0]) : "";
        int idx = -1;
        for (size_t i = 0; i < r->names.size(); ++i) {
            if (r->names[i] == s) { idx = static_cast<int>(i); break; }
        }
        return ev::fromDouble(idx);
    });

    rb.def("destroy", 0, [](Value self, std::span<const Value>) -> Value {
        HostPhysicsRagdoll* r = unwrapRagdoll(self);
        if (!r) return ev::undefined();
        destroyRagdollImpl(r);
        return ev::undefined();
    });
}

Value physicsCreateRagdoll(Value self, std::span<const Value> a) {
    HostPhysicsWorld* pw = unwrapWorld(self);
    auto* world = pw->getWorld();
    if (!world) return ev::throwError("World not available");
    if (a.empty() || !ev::isObject(a[0])) return ev::throwTypeError("createRagdoll(opts) requires an object");

    ev::Persistent optsVal(a[0]);
    physics::RagdollOptions opts;

    opts.position = readVec3(ev::getProperty(optsVal.get(), "position"));
    opts.rotation = readQuat(ev::getProperty(optsVal.get(), "rotation"));

    Value layerVal = ev::getProperty(optsVal.get(), "layer");
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

    Value motorV = ev::getProperty(optsVal.get(), "motor");
    if (ev::isObject(motorV)) readRagdollMotor(motorV, opts.motor);

    Value partsV = ev::getProperty(optsVal.get(), "parts");
    std::vector<std::string> names;
    if (ev::isObject(partsV)) {
        ev::Persistent pp(partsV);
        Value lenV = ev::getProperty(pp.get(), "length");
        uint32_t np = (!ev::isUndefined(lenV) && !ev::isObject(lenV)) ? static_cast<uint32_t>(ev::toDouble(lenV)) : 0;
        for (uint32_t i = 0; i < np; ++i) {
            Value pv = ev::getElement(pp.get(), i);
            physics::RagdollPartOptions part;
            std::string err;
            if (!readRagdollPart(pv, names, part, err)) {
                return ev::throwTypeError("ragdoll part " + std::to_string(i) + ": " + err);
            }
            names.push_back(part.name.empty() ? ("part" + std::to_string(i)) : part.name);
            opts.parts.push_back(std::move(part));
        }
    }
    if (opts.parts.empty()) return ev::throwTypeError("createRagdoll requires a non-empty parts array");

    uint32_t handle = world->createRagdoll(opts);
    if (!handle) return ev::throwError("Failed to create ragdoll");

    auto* jr = new HostPhysicsRagdoll();
    jr->handle = handle;
    jr->names = std::move(names);
    const int n = static_cast<int>(opts.parts.size());
    jr->partTags.reserve(n);
    jr->parents.reserve(n);
    for (int i = 0; i < n; ++i) {
        jr->partTags.push_back(pw->registerBody(world->ragdollPartBody(handle, i)));
        jr->parents.push_back(opts.parts[i].parentIndex);
    }
    if (pw->ownsWorld) {
        jr->world = pw;
        pw->liveRagdolls.insert(jr);
    }

    ObjectBuilder rb(g_ragdollClass.make(jr, [](void* p) {
        auto* r = static_cast<HostPhysicsRagdoll*>(p);
        destroyRagdollImpl(r);
        delete r;
    }));
    return rb.get();
}

}  // namespace bro::bronze_host
