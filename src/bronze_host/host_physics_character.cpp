// Physics character controller bindings for the bronze host layer.

#include "bronze_host/host_physics_internal.h"

namespace bro::bronze_host {

void decorateCharacterProto(ObjectBuilder& cb) {
    cb.def("setVelocity", 3, [](Value self_, std::span<const Value> args) {
        HostPhysicsCharacter* pc = unwrapCharacter(self_);
        if (!pc) return ev::undefined();
        auto* w = getPhysicsWorld();
        if (w && pc->handle) {
            JPH::Vec3 v;
            if (args.size() >= 3) {
                v = JPH::Vec3(static_cast<float>(numAt(args, 0)),
                              static_cast<float>(numAt(args, 1)),
                              static_cast<float>(numAt(args, 2)));
            } else if (!args.empty() && ev::isObject(args[0])) {
                v = readVec3(args[0]);
            }
            w->setCharacterVelocity(pc->handle, v);
        }
        return ev::undefined();
    });

    cb.def("getVelocity", 0, [](Value self_, std::span<const Value>) {
        HostPhysicsCharacter* pc = unwrapCharacter(self_);
        if (!pc) return ev::undefined();
        auto* w = getPhysicsWorld();
        if (!w || !pc->handle) return ev::null();
        physics::CharacterState st;
        if (!w->getCharacterState(pc->handle, st)) return ev::null();
        return makeVec3Value(st.velocity.GetX(), st.velocity.GetY(), st.velocity.GetZ());
    });

    cb.def("setPosition", 3, [](Value self_, std::span<const Value> args) {
        HostPhysicsCharacter* pc = unwrapCharacter(self_);
        if (!pc) return ev::undefined();
        auto* w = getPhysicsWorld();
        if (w && pc->handle) {
            JPH::RVec3 p;
            if (args.size() >= 3) {
                p = JPH::RVec3(static_cast<float>(numAt(args, 0)),
                               static_cast<float>(numAt(args, 1)),
                               static_cast<float>(numAt(args, 2)));
            } else if (!args.empty() && ev::isObject(args[0])) {
                p = readRVec3(args[0]);
            }
            w->setCharacterPosition(pc->handle, p);
        }
        return ev::undefined();
    });

    cb.def("getPosition", 0, [](Value self_, std::span<const Value>) {
        HostPhysicsCharacter* pc = unwrapCharacter(self_);
        if (!pc) return ev::undefined();
        auto* w = getPhysicsWorld();
        if (!w || !pc->handle) return ev::null();
        physics::CharacterState st;
        if (!w->getCharacterState(pc->handle, st)) return ev::null();
        return makeVec3Value(static_cast<float>(st.position.GetX()),
                             static_cast<float>(st.position.GetY()),
                             static_cast<float>(st.position.GetZ()));
    });

    cb.def("getState", 0, [](Value self_, std::span<const Value>) {
        HostPhysicsCharacter* pc = unwrapCharacter(self_);
        if (!pc) return ev::undefined();
        auto* w = getPhysicsWorld();
        if (!w || !pc->handle) return ev::null();
        physics::CharacterState st;
        if (!w->getCharacterState(pc->handle, st)) return ev::null();
        int32_t groundTag = st.groundBody.IsInvalid() ? -1 : g_phys.tagForBodyId(st.groundBody);
        return makeCharacterStateValue(st, groundTag);
    });

    cb.def("setShape", 1, [](Value self_, std::span<const Value> args) {
        HostPhysicsCharacter* pc = unwrapCharacter(self_);
        if (!pc) return ev::undefined();
        auto* w = getPhysicsWorld();
        if (!w || !pc->handle || args.empty() || !ev::isObject(args[0])) return ev::fromBool(false);
        physics::BodyOptions shape;
        std::string err;
        if (!readBodyOptions(args[0], shape, err)) return ev::throwTypeError("setShape: " + err);
        return ev::fromBool(w->setCharacterShape(pc->handle, shape));
    });

    cb.accessor("innerBody", [](Value self_, std::span<const Value>) {
        HostPhysicsCharacter* pc = unwrapCharacter(self_);
        if (!pc) return ev::undefined();
        return ev::fromDouble(pc->innerTag);
    }, nullptr);

    cb.def("destroy", 0, [](Value self_, std::span<const Value>) {
        HostPhysicsCharacter* pc = unwrapCharacter(self_);
        if (!pc) return ev::undefined();
        auto* w = getPhysicsWorld();
        if (w && pc->handle) {
            if (pc->innerTag >= 0) g_phys.unregisterBody(pc->innerTag);
            w->destroyCharacter(pc->handle);
        }
        pc->handle = 0;
        pc->innerTag = -1;
        return ev::undefined();
    });
}

Value physicsCreateCharacter(Value, std::span<const Value> a) {
    auto* world = getPhysicsWorld();
    if (!world) return ev::throwError("PhysicsWorld not available");
    if (a.empty() || !ev::isObject(a[0])) return ev::throwTypeError("createCharacter(options) requires an object");

    ev::Persistent opts(a[0]);
    physics::CharacterOptions copts;
    copts.position = readRVec3(ev::getProperty(opts.get(), "position"));
    copts.up = readVec3(ev::getProperty(opts.get(), "up"), JPH::Vec3(0, 1, 0));
    copts.radius = static_cast<float>(getPropNumber(opts, "radius", copts.radius));
    copts.halfHeight = static_cast<float>(getPropNumber(opts, "halfHeight", copts.halfHeight));
    copts.mass = static_cast<float>(getPropNumber(opts, "mass", copts.mass));
    copts.maxSlopeAngle = static_cast<float>(getPropNumber(opts, "maxSlopeAngle", copts.maxSlopeAngle));
    copts.maxStrength = static_cast<float>(getPropNumber(opts, "maxStrength", copts.maxStrength));
    copts.padding = static_cast<float>(getPropNumber(opts, "padding", copts.padding));
    copts.stepUp = static_cast<float>(getPropNumber(opts, "stepUp", copts.stepUp));
    copts.stickToFloor = static_cast<float>(getPropNumber(opts, "stickToFloor", copts.stickToFloor));
    copts.innerBody = getPropBool(opts, "innerBody", copts.innerBody);

    Value layerVal = ev::getProperty(opts.get(), "layer");
    if (!ev::isUndefined(layerVal) && !ev::isNull(layerVal)) {
        if (!ev::isObject(layerVal)) {
            std::string s = ev::toUtf8(layerVal);
            bool isNumber = !s.empty() && std::all_of(s.begin(), s.end(), ::isdigit);
            if (isNumber) copts.layer = std::stoi(s);
            else copts.layer = world->layerIndex(s);
        }
    }

    uint32_t handle = world->createCharacter(copts);
    if (!handle) return ev::throwError("Failed to create character");

    auto* pc = new HostPhysicsCharacter();
    pc->handle = handle;
    if (copts.innerBody) {
        JPH::BodyID innerId = world->characterInnerBody(handle);
        pc->innerTag = g_phys.registerBody(innerId);
    }

    ObjectBuilder cb(g_characterClass.make(pc, [](void* p) {
        delete static_cast<HostPhysicsCharacter*>(p);
    }));

    return cb.get();
}

}  // namespace bro::bronze_host
