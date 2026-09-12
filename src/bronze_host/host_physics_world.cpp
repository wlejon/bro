// Physics sandbox world handle bindings for the bronze host layer.

#include "bronze_host/host_physics_internal.h"

namespace bro::bronze_host {

physics::PhysicsWorld* HostPhysicsWorld::getWorld() const {
    if (ownsWorld) return world;
    return world ? world : getPhysicsWorld();
}

HostPhysicsWorld::~HostPhysicsWorld() {
    for (auto* c : liveCharacters) {
        if (c) { c->world = nullptr; c->handle = 0; }
    }
    liveCharacters.clear();

    for (auto* v : liveVehicles) {
        if (v) { v->world = nullptr; v->handle = 0; }
    }
    liveVehicles.clear();

    for (auto* r : liveRagdolls) {
        if (r) { r->world = nullptr; r->handle = 0; }
    }
    liveRagdolls.clear();

    for (auto* s : liveSoftBodies) {
        if (s) { s->world = nullptr; s->handle = 0; }
    }
    liveSoftBodies.clear();

    if (ownsWorld && world) {
        world->shutdown();
        delete world;
        world = nullptr;
    }
}

Value physicsCreateWorld(Value, std::span<const Value>) {
    auto* world = getPhysicsWorld();
    if (!world) return ev::throwError("Physics not available");
    return ev::fromBool(true);
}

Value physicsCreateWorldHandle(Value, std::span<const Value> a) {
    int maxBodies = 1024;
    int contactBufferSize = 0;
    JPH::Vec3 gravity(0, -9.81f, 0);

    if (!a.empty() && ev::isObject(a[0])) {
        ev::Persistent opts(a[0]);
        maxBodies = static_cast<int>(getPropNumber(opts, "maxBodies", 1024.0));
        contactBufferSize = static_cast<int>(getPropNumber(opts, "contactBufferSize", 0.0));
        Value gv = ev::getProperty(opts.get(), "gravity");
        if (ev::isObject(gv)) gravity = readVec3(gv, gravity);
    }

    auto* pw = new physics::PhysicsWorld();
    if (!pw->init(maxBodies, contactBufferSize)) {
        delete pw;
        return ev::throwError("Failed to init sandbox PhysicsWorld");
    }
    pw->setGravity(gravity.GetX(), gravity.GetY(), gravity.GetZ());

    auto* hw = new HostPhysicsWorld();
    hw->world = pw;
    hw->ownsWorld = true;

    ObjectBuilder wb(g_physicsWorldClass.make(hw, [](void* p) {
        delete static_cast<HostPhysicsWorld*>(p);
    }));
    return wb.get();
}

void decorateWorldHandleProto(ObjectBuilder& wb) {
    registerCommonWorldMethods(wb);

    wb.def("step", 1, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        if (!pw || !pw->world) return ev::undefined();
        double dt = pw->world->timeStep();
        if (!a.empty() && !ev::isUndefined(a[0])) dt = numAt(a, 0);
        pw->world->setTimeStep(static_cast<float>(dt));
        pw->world->stepInline();
        return ev::undefined();
    });

    wb.def("destroy", 0, [](Value self, std::span<const Value>) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        if (!pw) return ev::undefined();
        for (auto* c : pw->liveCharacters) {
            if (c) { c->world = nullptr; c->handle = 0; }
        }
        pw->liveCharacters.clear();
        for (auto* v : pw->liveVehicles) {
            if (v) { v->world = nullptr; v->handle = 0; }
        }
        pw->liveVehicles.clear();
        for (auto* r : pw->liveRagdolls) {
            if (r) { r->world = nullptr; r->handle = 0; }
        }
        pw->liveRagdolls.clear();
        for (auto* s : pw->liveSoftBodies) {
            if (s) { s->world = nullptr; s->handle = 0; }
        }
        pw->liveSoftBodies.clear();
        if (pw->ownsWorld && pw->world) {
            pw->world->shutdown();
            delete pw->world;
            pw->world = nullptr;
        }
        pw->bodyTags.clear();
        pw->tagToBody.clear();
        return ev::undefined();
    });
}

}  // namespace bro::bronze_host
