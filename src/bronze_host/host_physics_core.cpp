// Physics core rigid body management and globals for the bronze host layer.

#include "bronze_host/host_physics_internal.h"

namespace bro::bronze_host {

PhysicsState g_phys;
HostClass g_characterClass;
HostClass g_softBodyClass;

physics::PhysicsWorld* getPhysicsWorld() {
    auto* e = hostEngine();
    return e ? e->physicsWorld() : nullptr;
}

static Value makePhysicsObject() {
    ObjectBuilder b;

    // Body Management
    b.def("createBody", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world) return ev::throwError("PhysicsWorld not available");
        if (a.empty() || !ev::isObject(a[0])) return ev::throwTypeError("createBody(options) requires an object");

        physics::BodyOptions opts;
        std::string err;
        if (!readBodyOptions(a[0], opts, err)) return ev::throwTypeError("createBody: " + err);

        JPH::BodyID id = world->createBody(opts);
        if (id.IsInvalid()) return ev::throwError("Failed to create body");
        return ev::fromDouble(g_phys.registerBody(id));
    });

    b.def("destroyBody", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();

        world->destroyBody(id, [](JPH::BodyID bid) { g_phys.unregisterBodyId(bid); });
        if (!world->bodyExists(id)) g_phys.unregisterBody(tag);
        return ev::undefined();
    });

    b.def("destroyAll", 0, [](Value, std::span<const Value>) -> Value {
        auto* world = getPhysicsWorld();
        if (world) {
            world->destroyAll();
            g_phys.clear();
        }
        return ev::undefined();
    });

    b.def("getTransform", 2, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::null();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::null();

        bool interp = a.size() >= 2 && ev::isObject(a[1]) && getPropBool(ev::Persistent(a[1]), "interpolated", false);
        return makeTransformValue(world, id, interp);
    });

    b.def("setPosition", 4, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
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

    b.def("setRotation", 5, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
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

    b.def("getVelocity", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::null();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::null();

        auto lv = world->getLinearVelocity(id);
        auto av = world->getAngularVelocity(id);
        return makeVelocityValue(lv, av);
    });

    b.def("setLinearVelocity", 4, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();

        JPH::Vec3 v;
        if (a.size() >= 4) {
            v = JPH::Vec3(static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        } else if (a.size() >= 2 && ev::isObject(a[1])) {
            v = readVec3(a[1]);
        }
        world->setLinearVelocity(id, v);
        return ev::undefined();
    });

    b.def("setAngularVelocity", 4, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();

        JPH::Vec3 v;
        if (a.size() >= 4) {
            v = JPH::Vec3(static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        } else if (a.size() >= 2 && ev::isObject(a[1])) {
            v = readVec3(a[1]);
        }
        world->setAngularVelocity(id, v);
        return ev::undefined();
    });

    b.def("addForce", 4, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();

        JPH::Vec3 f;
        if (a.size() >= 4) {
            f = JPH::Vec3(static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        } else if (a.size() >= 2 && ev::isObject(a[1])) {
            f = readVec3(a[1]);
        }
        world->addForce(id, f);
        return ev::undefined();
    });

    b.def("addImpulse", 4, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();

        JPH::Vec3 imp;
        if (a.size() >= 4) {
            imp = JPH::Vec3(static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        } else if (a.size() >= 2 && ev::isObject(a[1])) {
            imp = readVec3(a[1]);
        }
        world->addImpulse(id, imp);
        return ev::undefined();
    });

    b.def("addTorque", 4, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();

        JPH::Vec3 t;
        if (a.size() >= 4) {
            t = JPH::Vec3(static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        } else if (a.size() >= 2 && ev::isObject(a[1])) {
            t = readVec3(a[1]);
        }
        world->addTorque(id, t);
        return ev::undefined();
    });

    b.def("setKinematic", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();
        world->setKinematic(id);
        return ev::undefined();
    });

    b.def("setMotionType", 2, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.size() < 2) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();
        world->setMotionType(id, boolAt(a, 1));
        return ev::undefined();
    });

    b.def("moveKinematic", 5, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.size() < 5) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();

        double x = numAt(a, 1), y = numAt(a, 2), z = numAt(a, 3);
        JPH::Quat rot = world->getRotation(id);
        double dt = 0.0;
        if (a.size() >= 9) {
            rot = JPH::Quat(static_cast<float>(numAt(a, 4)), static_cast<float>(numAt(a, 5)),
                            static_cast<float>(numAt(a, 6)), static_cast<float>(numAt(a, 7))).Normalized();
            dt = numAt(a, 8);
        } else {
            dt = numAt(a, 4);
        }
        world->moveKinematic(id, JPH::RVec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)), rot, static_cast<float>(dt));
        return ev::undefined();
    });

    b.def("setUserData", 2, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.size() < 2) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();
        world->setUserData(id, static_cast<uint64_t>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getUserData", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::fromDouble(0.0);
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::fromDouble(0.0);
        return ev::fromDouble(static_cast<double>(world->getUserData(id)));
    });

    b.def("setLayer", 2, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.size() < 2) return ev::fromBool(false);
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::fromBool(false);

        int layer = -1;
        if (!ev::isObject(a[1])) {
            std::string s = ev::toUtf8(a[1]);
            int idx = 0;
            if (parseDecimalIndex(s, idx)) layer = idx;
            else layer = world->layerIndex(s);
        }
        if (layer < 0) return ev::fromBool(false);
        world->setLayer(id, layer);
        return ev::fromBool(true);
    });

    b.def("setInterpolation", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (world && !a.empty()) world->setInterpolation(boolAt(a, 0));
        return ev::undefined();
    });

    b.def("getInterpolation", 0, [](Value, std::span<const Value>) -> Value {
        auto* world = getPhysicsWorld();
        return ev::fromBool(world ? world->interpolation() : false);
    });

    b.def("setMass", 2, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.size() < 2) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();
        world->setMass(id, static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getMass", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::fromDouble(0.0);
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::fromDouble(0.0);
        return ev::fromDouble(world->getMass(id));
    });

    b.def("setLinearDamping", 2, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.size() < 2) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();
        world->setLinearDamping(id, static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getLinearDamping", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::fromDouble(0.0);
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::fromDouble(0.0);
        return ev::fromDouble(world->getLinearDamping(id));
    });

    b.def("setAngularDamping", 2, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.size() < 2) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();
        world->setAngularDamping(id, static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getAngularDamping", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::fromDouble(0.0);
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::fromDouble(0.0);
        return ev::fromDouble(world->getAngularDamping(id));
    });

    b.def("setGravityFactor", 2, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.size() < 2) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();
        world->setGravityFactor(id, static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getGravityFactor", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::fromDouble(1.0);
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::fromDouble(1.0);
        return ev::fromDouble(world->getGravityFactor(id));
    });

    b.def("setFriction", 2, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.size() < 2) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();
        world->setFriction(id, static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getFriction", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::fromDouble(0.0);
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::fromDouble(0.0);
        return ev::fromDouble(world->getFriction(id));
    });

    b.def("setRestitution", 2, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.size() < 2) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();
        world->setRestitution(id, static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getRestitution", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::fromDouble(0.0);
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::fromDouble(0.0);
        return ev::fromDouble(world->getRestitution(id));
    });

    b.def("getBodyProperties", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::null();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
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

    b.def("setGravity", 3, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.size() < 3) return ev::undefined();
        world->setGravity(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)));
        return ev::undefined();
    });

    b.def("getGravity", 0, [](Value, std::span<const Value>) -> Value {
        auto* world = getPhysicsWorld();
        if (!world) return makeVec3Value(0, -9.81f, 0);
        auto g = world->gravity();
        return makeVec3Value(g.GetX(), g.GetY(), g.GetZ());
    });

    b.def("setLayers", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
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

    b.def("isActive", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::fromBool(false);
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        return ev::fromBool(!id.IsInvalid() && world->isActive(id));
    });

    b.def("activate", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (world && !a.empty()) {
            int32_t tag = i32At(a, 0);
            JPH::BodyID id = g_phys.bodyIdForTag(tag);
            if (!id.IsInvalid()) world->activate(id);
        }
        return ev::undefined();
    });

    b.def("setTimeStep", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (world && !a.empty()) world->setTimeStep(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("getAllTransforms", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world) return makeFloat32Array(nullptr, 0);

        bool interp = !a.empty() && ev::isObject(a[0]) && getPropBool(ev::Persistent(a[0]), "interpolated", false);
        size_t count = g_phys.bodyTags.size();
        size_t stride = 8;
        std::vector<float> buf(count * stride);
        size_t i = 0;
        auto& bi = world->getBodyInterface();
        for (auto& [key, tag] : g_phys.bodyTags) {
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

    // Character & Soft Body constructors
    b.def("createCharacter", 1, physicsCreateCharacter);
    b.def("createSoftBody", 1, physicsCreateSoftBody);

    // Register modular queries and constraints
    registerQueryMethods(b);
    registerConstraintMethods(b);

    return b.get();
}

void installPhysicsGlobals() {
    Value physVal = makePhysicsObject();
    g_phys.physicsObj.set(physVal);
    ev::registerGlobal("Physics", physVal);
    g_characterClass.install("PhysicsCharacter", 0, nullptr, decorateCharacterProto);
    g_softBodyClass.install("PhysicsSoftBody", 0, nullptr, decorateSoftBodyProto);
}

}  // namespace bro::bronze_host
