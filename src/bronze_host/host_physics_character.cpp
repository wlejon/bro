// Physics character controller bindings for the bronze host layer.

#include "bronze_host/host_physics_internal.h"

namespace bro::bronze_host {

void decorateCharacterProto(ObjectBuilder& cb) {
    cb.def("setVelocity", 3, [](Value self_, std::span<const Value> args) {
        HostPhysicsCharacter* pc = unwrapCharacter(self_);
        if (!pc || !pc->handle) return ev::undefined();
        HostPhysicsWorld* pw = pc->world ? pc->world : &g_defaultWorld;
        auto* w = pw->getWorld();
        if (w) {
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
        if (!pc || !pc->handle) return ev::null();
        HostPhysicsWorld* pw = pc->world ? pc->world : &g_defaultWorld;
        auto* w = pw->getWorld();
        if (!w) return ev::null();
        physics::CharacterState st;
        if (!w->getCharacterState(pc->handle, st)) return ev::null();
        return makeVec3Value(st.velocity.GetX(), st.velocity.GetY(), st.velocity.GetZ());
    });

    cb.def("setPosition", 3, [](Value self_, std::span<const Value> args) {
        HostPhysicsCharacter* pc = unwrapCharacter(self_);
        if (!pc || !pc->handle) return ev::undefined();
        HostPhysicsWorld* pw = pc->world ? pc->world : &g_defaultWorld;
        auto* w = pw->getWorld();
        if (w) {
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
        if (!pc || !pc->handle) return ev::null();
        HostPhysicsWorld* pw = pc->world ? pc->world : &g_defaultWorld;
        auto* w = pw->getWorld();
        if (!w) return ev::null();
        physics::CharacterState st;
        if (!w->getCharacterState(pc->handle, st)) return ev::null();
        return makeVec3Value(static_cast<float>(st.position.GetX()),
                             static_cast<float>(st.position.GetY()),
                             static_cast<float>(st.position.GetZ()));
    });

    cb.def("getState", 0, [](Value self_, std::span<const Value>) {
        HostPhysicsCharacter* pc = unwrapCharacter(self_);
        if (!pc || !pc->handle) return ev::null();
        HostPhysicsWorld* pw = pc->world ? pc->world : &g_defaultWorld;
        auto* w = pw->getWorld();
        if (!w) return ev::null();
        physics::CharacterState st;
        if (!w->getCharacterState(pc->handle, st)) return ev::null();
        int32_t groundTag = st.groundBody.IsInvalid() ? -1 : pw->tagForBodyId(st.groundBody);
        return makeCharacterStateValue(st, groundTag);
    });

    cb.def("setShape", 1, [](Value self_, std::span<const Value> args) {
        HostPhysicsCharacter* pc = unwrapCharacter(self_);
        if (!pc || !pc->handle || args.empty() || !ev::isObject(args[0])) return ev::fromBool(false);
        HostPhysicsWorld* pw = pc->world ? pc->world : &g_defaultWorld;
        auto* w = pw->getWorld();
        if (!w) return ev::fromBool(false);
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
        if (!pc || !pc->handle) return ev::undefined();
        HostPhysicsWorld* pw = pc->world ? pc->world : &g_defaultWorld;
        auto* w = pw->getWorld();
        if (w) {
            if (pc->innerTag >= 0) pw->unregisterBody(pc->innerTag);
            w->destroyCharacter(pc->handle);
        }
        if (pc->world) pc->world->liveCharacters.erase(pc);
        pc->handle = 0;
        pc->innerTag = -1;
        return ev::undefined();
    });
}

Value physicsCreateCharacter(Value self, std::span<const Value> a) {
    HostPhysicsWorld* pw = unwrapWorld(self);
    auto* world = pw->getWorld();
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
            int idx = 0;
            if (parseDecimalIndex(s, idx)) copts.layer = idx;
            else copts.layer = world->layerIndex(s);
        }
    }

    uint32_t handle = world->createCharacter(copts);
    if (!handle) return ev::throwError("Failed to create character");

    auto* pc = new HostPhysicsCharacter();
    pc->handle = handle;
    if (copts.innerBody) {
        JPH::BodyID innerId = world->characterInnerBody(handle);
        pc->innerTag = pw->registerBody(innerId);
    }
    if (pw->ownsWorld) {
        pc->world = pw;
        pw->liveCharacters.insert(pc);
    }

    ObjectBuilder cb(g_characterClass.make(pc, [](void* p) {
        auto* c = static_cast<HostPhysicsCharacter*>(p);
        if (c && c->handle) {
            HostPhysicsWorld* w = c->world ? c->world : &g_defaultWorld;
            auto* pwld = w->getWorld();
            if (pwld) {
                if (c->innerTag >= 0) w->unregisterBody(c->innerTag);
                pwld->destroyCharacter(c->handle);
            }
            if (c->world) c->world->liveCharacters.erase(c);
            c->handle = 0;
        }
        delete c;
    }));

    return cb.get();
}

}  // namespace bro::bronze_host
