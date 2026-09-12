// Physics core rigid body management and globals for the bronze host layer.

#include "bronze_host/host_physics_internal.h"

namespace bro::bronze_host {

HostPhysicsWorld g_defaultWorld;
HostClass g_physicsWorldClass;
HostClass g_characterClass;
HostClass g_softBodyClass;
HostClass g_vehicleClass;
HostClass g_ragdollClass;

physics::PhysicsWorld* getPhysicsWorld() {
    auto* e = hostEngine();
    return e ? e->physicsWorld() : nullptr;
}

void registerCommonWorldMethods(ObjectBuilder& b) {
    // Body Management
    b.def("createBody", 1, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world) return ev::throwError("PhysicsWorld not available");
        if (a.empty() || !ev::isObject(a[0])) return ev::throwTypeError("createBody(options) requires an object");

        physics::BodyOptions opts;
        std::string err;
        if (!readBodyOptions(a[0], opts, err, world)) return ev::throwTypeError("createBody: " + err);

        JPH::BodyID id = world->createBody(opts);
        if (id.IsInvalid()) return ev::throwError("Failed to create body");
        return ev::fromDouble(pw->registerBody(id));
    });

    b.def("destroyBody", 1, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.empty()) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();

        world->destroyBody(id, [pw](JPH::BodyID bid) { pw->unregisterBodyId(bid); });
        if (!world->bodyExists(id)) pw->unregisterBody(tag);
        return ev::undefined();
    });

    b.def("destroyAll", 0, [](Value self, std::span<const Value>) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (world) {
            world->destroyAll();
            pw->clear();
        }
        return ev::undefined();
    });

    b.def("getTransform", 2, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.empty()) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid() || !world->bodyExists(id)) return ev::undefined();

        bool interp = a.size() >= 2 && ev::isObject(a[1]) && getPropBool(ev::Persistent(a[1]), "interpolated", false);
        return makeTransformValue(world, id, interp);
    });

    b.def("setPosition", 4, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.empty()) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();

        JPH::RVec3 pos;
        if (a.size() >= 4) {
            pos = JPH::RVec3(static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        } else if (a.size() >= 2 && ev::isObject(a[1])) {
            pos = readRVec3(a[1]);
        }
        world->setPosition(id, pos);
        return ev::undefined();
    });

    b.def("setRotation", 5, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.empty()) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();

        JPH::Quat rot;
        if (a.size() >= 5) {
            rot = JPH::Quat(static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)),
                            static_cast<float>(numAt(a, 3)), static_cast<float>(numAt(a, 4))).Normalized();
        } else if (a.size() >= 2 && ev::isObject(a[1])) {
            rot = readQuat(a[1]);
        }
        world->setRotation(id, rot);
        return ev::undefined();
    });

    b.def("setLinearVelocity", 4, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.empty()) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();

        JPH::Vec3 vel;
        if (a.size() >= 4) {
            vel = JPH::Vec3(static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        } else if (a.size() >= 2 && ev::isObject(a[1])) {
            vel = readVec3(a[1]);
        }
        world->setLinearVelocity(id, vel);
        return ev::undefined();
    });

    b.def("getLinearVelocity", 1, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.empty()) return makeVec3Value(0, 0, 0);
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid()) return makeVec3Value(0, 0, 0);

        JPH::Vec3 v = world->getLinearVelocity(id);
        return makeVec3Value(v.GetX(), v.GetY(), v.GetZ());
    });

    b.def("setAngularVelocity", 4, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.empty()) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();

        JPH::Vec3 vel;
        if (a.size() >= 4) {
            vel = JPH::Vec3(static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        } else if (a.size() >= 2 && ev::isObject(a[1])) {
            vel = readVec3(a[1]);
        }
        world->setAngularVelocity(id, vel);
        return ev::undefined();
    });

    b.def("getAngularVelocity", 1, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.empty()) return makeVec3Value(0, 0, 0);
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid()) return makeVec3Value(0, 0, 0);

        JPH::Vec3 v = world->getAngularVelocity(id);
        return makeVec3Value(v.GetX(), v.GetY(), v.GetZ());
    });

    b.def("getVelocity", 1, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.empty()) return makeVelocityValue(JPH::Vec3::sZero(), JPH::Vec3::sZero());
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid()) return makeVelocityValue(JPH::Vec3::sZero(), JPH::Vec3::sZero());

        return makeVelocityValue(world->getLinearVelocity(id), world->getAngularVelocity(id));
    });

    b.def("addForce", 4, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.empty()) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();

        JPH::Vec3 force;
        if (a.size() >= 4) {
            force = JPH::Vec3(static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        } else if (a.size() >= 2 && ev::isObject(a[1])) {
            force = readVec3(a[1]);
        }
        world->addForce(id, force);
        return ev::undefined();
    });

    b.def("addImpulse", 4, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.empty()) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();

        JPH::Vec3 impulse;
        if (a.size() >= 4) {
            impulse = JPH::Vec3(static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        } else if (a.size() >= 2 && ev::isObject(a[1])) {
            impulse = readVec3(a[1]);
        }
        world->addImpulse(id, impulse);
        return ev::undefined();
    });

    b.def("addTorque", 4, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.empty()) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();

        JPH::Vec3 torque;
        if (a.size() >= 4) {
            torque = JPH::Vec3(static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        } else if (a.size() >= 2 && ev::isObject(a[1])) {
            torque = readVec3(a[1]);
        }
        world->addTorque(id, torque);
        return ev::undefined();
    });

    b.def("setUserData", 2, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.size() < 2) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();
        uint64_t udata = 0;
        if (ev::isNumber(a[1])) {
            udata = static_cast<uint64_t>(ev::toDouble(a[1]));
        } else {
            std::string s = ev::toUtf8(a[1]);
            try { if (!s.empty()) udata = std::stoull(s); } catch (...) {}
        }
        world->setUserData(id, udata);
        return ev::undefined();
    });

    b.def("getUserData", 1, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.empty()) return makeBigIntValue(0);
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid()) return makeBigIntValue(0);
        return makeBigIntValue(world->getUserData(id));
    });

    b.def("setLayer", 2, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.size() < 2) return ev::fromBool(false);
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::fromBool(false);

        int32_t layerIdx = -1;
        if (!ev::isObject(a[1])) {
            std::string s = ev::toUtf8(a[1]);
            int parsed = 0;
            if (parseDecimalIndex(s, parsed)) layerIdx = parsed;
            else layerIdx = world->layerIndex(s);
        }
        if (layerIdx < 0) return ev::fromBool(false);
        world->setLayer(id, layerIdx);
        return ev::fromBool(true);
    });

    b.def("setKinematic", 1, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.empty()) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();
        world->setKinematic(id);
        return ev::undefined();
    });

    b.def("setMotionType", 2, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.size() < 2) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();

        if (ev::isBool(a[1])) {
            world->setMotionType(id, ev::toBool(a[1]));
        } else if (!ev::isObject(a[1])) {
            std::string type = ev::toUtf8(a[1]);
            if (type == "static") world->setMotionType(id, true);
            else if (type == "kinematic") world->setKinematic(id);
            else if (type == "dynamic") world->setMotionType(id, false);
        }
        return ev::undefined();
    });

    b.def("moveKinematic", 5, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.empty()) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();

        JPH::RVec3 pos = world->getPosition(id);
        JPH::Quat rot = world->getRotation(id);
        float dt = 1.0f / 60.0f;

        if (a.size() >= 3 && ev::isObject(a[1])) {
            pos = readRVec3(a[1]);
            if (ev::isObject(a[2])) rot = readQuat(a[2]);
            if (hasArg(a, 3)) dt = static_cast<float>(numAt(a, 3));
        } else if (hasArg(a, 4)) {
            pos = JPH::RVec3(static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
            if (hasArg(a, 4)) dt = static_cast<float>(numAt(a, 4));
        }
        world->moveKinematic(id, pos, rot, dt);
        return ev::undefined();
    });

    b.def("setMass", 2, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.size() < 2) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();
        world->setMass(id, static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getMass", 1, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.empty()) return ev::fromDouble(0.0);
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::fromDouble(0.0);
        return ev::fromDouble(world->getMass(id));
    });

    b.def("setLinearDamping", 2, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.size() < 2) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();
        world->setLinearDamping(id, static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getLinearDamping", 1, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.empty()) return ev::fromDouble(0.0);
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::fromDouble(0.0);
        return ev::fromDouble(world->getLinearDamping(id));
    });

    b.def("setAngularDamping", 2, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.size() < 2) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();
        world->setAngularDamping(id, static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getAngularDamping", 1, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.empty()) return ev::fromDouble(0.0);
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::fromDouble(0.0);
        return ev::fromDouble(world->getAngularDamping(id));
    });

    b.def("setGravityFactor", 2, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.size() < 2) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();
        world->setGravityFactor(id, static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getGravityFactor", 1, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.empty()) return ev::fromDouble(1.0);
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::fromDouble(1.0);
        return ev::fromDouble(world->getGravityFactor(id));
    });

    b.def("setFriction", 2, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.size() < 2) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();
        world->setFriction(id, static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getFriction", 1, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.empty()) return ev::fromDouble(0.0);
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::fromDouble(0.0);
        return ev::fromDouble(world->getFriction(id));
    });

    b.def("setRestitution", 2, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.size() < 2) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();
        world->setRestitution(id, static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getRestitution", 1, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.empty()) return ev::fromDouble(0.0);
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::fromDouble(0.0);
        return ev::fromDouble(world->getRestitution(id));
    });

    b.def("getBodyProperties", 1, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.empty()) return ev::null();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::null();

        ObjectBuilder o;
        o.set("mass", ev::fromDouble(world->getMass(id)));
        o.set("friction", ev::fromDouble(world->getFriction(id)));
        o.set("restitution", ev::fromDouble(world->getRestitution(id)));
        o.set("linearDamping", ev::fromDouble(world->getLinearDamping(id)));
        o.set("angularDamping", ev::fromDouble(world->getAngularDamping(id)));
        o.set("gravityFactor", ev::fromDouble(world->getGravityFactor(id)));
        return o.get();
    });

    b.def("setAreaOverride", 2, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.empty()) return ev::fromBool(false);
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::fromBool(false);
        if (a.size() < 2 || ev::isNull(a[1]) || ev::isUndefined(a[1])) {
            world->clearAreaOverride(id);
            return ev::fromBool(true);
        }
        if (!ev::isObject(a[1])) return ev::throwTypeError("setAreaOverride(tag, opts | null)");
        if (!world->isSensor(id)) return ev::throwTypeError("setAreaOverride: body is not a sensor");
        physics::AreaOverride area;
        std::string err;
        ev::Persistent optsPersist(a[1]);
        if (!readAreaOverride(optsPersist, area, err)) return ev::throwTypeError(err);
        world->setAreaOverride(id, area);
        return ev::fromBool(true);
    });

    b.def("setFrictionCombine", 2, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.size() < 2) return ev::fromBool(false);
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::fromBool(false);
        physics::CombineMode mode = physics::CombineMode::Default;
        if (!ev::isObject(a[1]) && !ev::isNull(a[1]) && !ev::isUndefined(a[1])) {
            std::string str = ev::toUtf8(a[1]);
            if (str != "default" && !parseCombineMode(str, mode)) {
                return ev::throwTypeError("combine mode must be 'default' | 'average' | 'min' | 'max' | 'multiply'");
            }
        } else if (!ev::isNull(a[1]) && !ev::isUndefined(a[1])) {
            return ev::throwTypeError("combine mode must be a string");
        }
        world->setFrictionCombine(id, mode);
        return ev::fromBool(true);
    });

    b.def("setRestitutionCombine", 2, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.size() < 2) return ev::fromBool(false);
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::fromBool(false);
        physics::CombineMode mode = physics::CombineMode::Default;
        if (!ev::isObject(a[1]) && !ev::isNull(a[1]) && !ev::isUndefined(a[1])) {
            std::string str = ev::toUtf8(a[1]);
            if (str != "default" && !parseCombineMode(str, mode)) {
                return ev::throwTypeError("combine mode must be 'default' | 'average' | 'min' | 'max' | 'multiply'");
            }
        } else if (!ev::isNull(a[1]) && !ev::isUndefined(a[1])) {
            return ev::throwTypeError("combine mode must be a string");
        }
        world->setRestitutionCombine(id, mode);
        return ev::fromBool(true);
    });

    b.def("setGravity", 3, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.size() < 3) return ev::undefined();
        world->setGravity(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)));
        return ev::undefined();
    });

    b.def("getGravity", 0, [](Value self, std::span<const Value>) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world) return makeVec3Value(0, -9.81f, 0);
        auto g = world->gravity();
        return makeVec3Value(g.GetX(), g.GetY(), g.GetZ());
    });

    b.def("setLayers", 1, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.empty() || !ev::isObject(a[0])) return ev::fromBool(false);
        ev::Persistent root(a[0]);
        Value namesVal = ev::getProperty(root.get(), "names");
        Value matVal = ev::getProperty(root.get(), "matrix");

        std::vector<std::string> names;
        if (ev::isObject(namesVal)) {
            ev::Persistent np(namesVal);
            Value lenV = ev::getProperty(np.get(), "length");
            if (!ev::isUndefined(lenV) && !ev::isObject(lenV)) {
                uint32_t n = static_cast<uint32_t>(ev::toDouble(lenV));
                for (uint32_t i = 0; i < n; ++i) {
                    Value el = ev::getElement(np.get(), i);
                    names.push_back(!ev::isObject(el) ? ev::toUtf8(el) : "");
                }
            }
        }

        std::vector<bool> matrix;
        if (ev::isObject(matVal)) {
            ev::Persistent mp(matVal);
            Value lenV = ev::getProperty(mp.get(), "length");
            if (!ev::isUndefined(lenV) && !ev::isObject(lenV)) {
                uint32_t n = static_cast<uint32_t>(ev::toDouble(lenV));
                for (uint32_t i = 0; i < n; ++i) {
                    Value el = ev::getElement(mp.get(), i);
                    matrix.push_back(ev::toBool(el));
                }
            }
        }
        return ev::fromBool(world->configureLayers(names, matrix));
    });

    b.def("isActive", 1, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.empty()) return ev::fromBool(false);
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = pw->bodyIdForTag(tag);
        return ev::fromBool(!id.IsInvalid() && world->isActive(id));
    });

    b.def("activate", 1, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (world && !a.empty()) {
            int32_t tag = i32At(a, 0);
            JPH::BodyID id = pw->bodyIdForTag(tag);
            if (!id.IsInvalid()) world->activate(id);
        }
        return ev::undefined();
    });

    b.def("setTimeStep", 1, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (world && !a.empty()) world->setTimeStep(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("getTimeStep", 0, [](Value self, std::span<const Value>) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        return ev::fromDouble(world ? world->timeStep() : (1.0 / 60.0));
    });

    b.def("setInterpolation", 1, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (world && !a.empty()) world->setInterpolation(ev::toBool(a[0]));
        return ev::undefined();
    });

    b.def("getInterpolation", 0, [](Value self, std::span<const Value>) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        return ev::fromBool(world ? world->interpolation() : false);
    });

    b.def("getAllTransforms", 1, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world) return makeFloat32Array(nullptr, 0);

        bool interp = false;
        if (!a.empty()) {
            if (ev::isObject(a[0])) interp = getPropBool(ev::Persistent(a[0]), "interpolated", false);
            else interp = ev::toBool(a[0]);
        }

        size_t count = pw->bodyTags.size();
        if (count == 0) return makeFloat32Array(nullptr, 0);

        constexpr size_t stride = 8;
        std::vector<float> buf(count * stride);
        size_t i = 0;
        auto& bi = world->getBodyInterface();
        for (auto& [key, tag] : pw->bodyTags) {
            JPH::BodyID id(key);
            JPH::RVec3 pos;
            JPH::Quat rot;
            if (interp) {
                world->getRenderTransform(id, pos, rot);
            } else {
                pos = bi.GetPosition(id);
                rot = bi.GetRotation(id);
            }
            float* p = buf.data() + i * stride;
            p[0] = static_cast<float>(tag);
            p[1] = static_cast<float>(pos.GetX());
            p[2] = static_cast<float>(pos.GetY());
            p[3] = static_cast<float>(pos.GetZ());
            p[4] = rot.GetX(); p[5] = rot.GetY(); p[6] = rot.GetZ(); p[7] = rot.GetW();
            i++;
        }
        return makeFloat32Array(buf.data(), buf.size());
    });

    // Sub-objects & modular surfaces
    b.def("createCharacter", 1, physicsCreateCharacter);
    b.def("createSoftBody", 1, physicsCreateSoftBody);
    b.def("createVehicle", 1, physicsCreateVehicle);
    b.def("createRagdoll", 1, physicsCreateRagdoll);

    registerQueryMethods(b);
    registerConstraintMethods(b);
}

static Value makePhysicsObject() {
    ObjectBuilder b;
    registerCommonWorldMethods(b);
    b.def("createWorld", 1, physicsCreateWorld);
    b.def("createWorldHandle", 1, physicsCreateWorldHandle);
    return b.get();
}

void installPhysicsGlobals() {
    Value physVal = makePhysicsObject();
    g_defaultWorld.physicsObj.set(physVal);
    ev::registerGlobal("Physics", physVal);
    g_physicsWorldClass.install("PhysicsWorldHandle", 0, nullptr, decorateWorldHandleProto);
    g_characterClass.install("PhysicsCharacter", 0, nullptr, decorateCharacterProto);
    g_softBodyClass.install("PhysicsSoftBody", 0, nullptr, decorateSoftBodyProto);
    g_vehicleClass.install("PhysicsVehicle", 0, nullptr, decorateVehicleProto);
    g_ragdollClass.install("PhysicsRagdoll", 0, nullptr, decorateRagdollProto);
}

}  // namespace bro::bronze_host
