// =============================================================================
// bro_physics_c_abi.cpp — C++ forwarding implementations for bro.physics
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_physics_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <cmath>

struct BroPhysicsWorldHandleImpl {
    int32_t id = 1;
    double timeStep = 1.0 / 60.0;
};

struct BroPhysicsCharacterImpl {
    double posX = 0.0, posY = 0.0, posZ = 0.0;
    double velX = 0.0, velY = 0.0, velZ = 0.0;
};

struct BroPhysicsVehicleImpl {
    double forward = 0.0, steer = 0.0, brake = 0.0, handBrake = 0.0;
};

struct BroPhysicsRagdollImpl {
    void* currentPose = nullptr;
};

struct BroPhysicsSoftBodyImpl {
    double minX = -1.0, minY = -1.0, minZ = -1.0;
    double maxX = 1.0, maxY = 1.0, maxZ = 1.0;
};

struct BroPhysicsBodyData {
    int32_t tag = 0;
    int32_t layer = 0;
    bool active = true;
    double posX = 0.0, posY = 0.0, posZ = 0.0;
    double rotX = 0.0, rotY = 0.0, rotZ = 0.0, rotW = 1.0;
    double linVelX = 0.0, linVelY = 0.0, linVelZ = 0.0;
    double angVelX = 0.0, angVelY = 0.0, angVelZ = 0.0;
    double mass = 1.0;
    double friction = 0.5;
    double restitution = 0.0;
    double gravityFactor = 1.0;
    double linearDamping = 0.05;
    double angularDamping = 0.05;
    void* userData = nullptr;
};

static int32_t s_nextBodyTag = 1;
static int32_t s_nextConstraintTag = 1;
static double s_gravityX = 0.0, s_gravityY = -9.81, s_gravityZ = 0.0;
static double s_timeStep = 1.0 / 60.0;
static bool s_interpolation = false;
static std::unordered_map<int32_t, BroPhysicsBodyData> s_bodies;
static double s_gravityArr[3] = {0.0, -9.81, 0.0};

extern "C" {

// --- Interface bro.physics.PhysicsWorldHandle ---
void* bro_PhysicsWorldHandle_create(void) {
    return new BroPhysicsWorldHandleImpl();
}

void bro_PhysicsWorldHandle_destroy(void* self) {
    delete static_cast<BroPhysicsWorldHandleImpl*>(self);
}

void bro_PhysicsWorldHandle_step(void* self, double dt) {
    auto* h = static_cast<BroPhysicsWorldHandleImpl*>(self);
    if (h) h->timeStep = dt;
}

// --- Interface bro.physics.PhysicsCharacter ---
void* bro_PhysicsCharacter_create(void) {
    return new BroPhysicsCharacterImpl();
}

void bro_PhysicsCharacter_destroy(void* self) {
    delete static_cast<BroPhysicsCharacterImpl*>(self);
}

void bro_PhysicsCharacter_setPosition(void* self, double x, double y, double z) {
    auto* c = static_cast<BroPhysicsCharacterImpl*>(self);
    if (c) { c->posX = x; c->posY = y; c->posZ = z; }
}

void bro_PhysicsCharacter_setLinearVelocity(void* self, double x, double y, double z) {
    auto* c = static_cast<BroPhysicsCharacterImpl*>(self);
    if (c) { c->velX = x; c->velY = y; c->velZ = z; }
}

void* bro_PhysicsCharacter_getPosition(void* self) {
    return self;
}

void* bro_PhysicsCharacter_getLinearVelocity(void* self) {
    return self;
}

void bro_PhysicsCharacter_update(void* self, double dt) {
    auto* c = static_cast<BroPhysicsCharacterImpl*>(self);
    if (c) {
        c->posX += c->velX * dt;
        c->posY += c->velY * dt;
        c->posZ += c->velZ * dt;
    }
}

// --- Interface bro.physics.PhysicsVehicle ---
void* bro_PhysicsVehicle_create(void) {
    return new BroPhysicsVehicleImpl();
}

void bro_PhysicsVehicle_destroy(void* self) {
    delete static_cast<BroPhysicsVehicleImpl*>(self);
}

void bro_PhysicsVehicle_setDriverInput(void* self, double forward, double steer, double brake, double handBrake) {
    auto* v = static_cast<BroPhysicsVehicleImpl*>(self);
    if (v) {
        v->forward = forward;
        v->steer = steer;
        v->brake = brake;
        v->handBrake = handBrake;
    }
}

void* bro_PhysicsVehicle_getTransform(void* self) {
    return self;
}

// --- Interface bro.physics.PhysicsRagdoll ---
void* bro_PhysicsRagdoll_create(void) {
    return new BroPhysicsRagdollImpl();
}

void bro_PhysicsRagdoll_destroy(void* self) {
    delete static_cast<BroPhysicsRagdollImpl*>(self);
}

void bro_PhysicsRagdoll_driveToPose(void* self, void* pose, double /*dt*/) {
    auto* r = static_cast<BroPhysicsRagdollImpl*>(self);
    if (r) r->currentPose = pose;
}

void* bro_PhysicsRagdoll_getPose(void* self) {
    auto* r = static_cast<BroPhysicsRagdollImpl*>(self);
    return r ? r->currentPose : nullptr;
}

// --- Interface bro.physics.PhysicsSoftBody ---
void* bro_PhysicsSoftBody_create(void) {
    return new BroPhysicsSoftBodyImpl();
}

void bro_PhysicsSoftBody_destroy(void* self) {
    delete static_cast<BroPhysicsSoftBodyImpl*>(self);
}

void* bro_PhysicsSoftBody_getBounds(void* self) {
    return self;
}

// --- Namespace bro.Physics ---
void* bro_Physics_createWorldHandle(void* /*opts*/) {
    return new BroPhysicsWorldHandleImpl();
}

void bro_Physics_createWorld(void* /*opts*/) {
}

void bro_Physics_setGravity(double x, double y, double z) {
    const auto* b = bro_get_physics_bridge();
    if (b && b->setGravity) {
        b->setGravity(x, y, z);
        return;
    }
    s_gravityX = x;
    s_gravityY = y;
    s_gravityZ = z;
    s_gravityArr[0] = x;
    s_gravityArr[1] = y;
    s_gravityArr[2] = z;
}

void* bro_Physics_getGravity(void) {
    const auto* b = bro_get_physics_bridge();
    if (b && b->getGravity) return b->getGravity();
    s_gravityArr[0] = s_gravityX;
    s_gravityArr[1] = s_gravityY;
    s_gravityArr[2] = s_gravityZ;
    return s_gravityArr;
}

void bro_Physics_setLayers(void* /*config*/) {
}

int32_t bro_Physics_createBody(void* config) {
    const auto* b = bro_get_physics_bridge();
    if (b && b->createBody) return b->createBody(config);
    int32_t tag = s_nextBodyTag++;
    BroPhysicsBodyData body;
    body.tag = tag;
    s_bodies[tag] = body;
    return tag;
}

void bro_Physics_destroyBody(int32_t tag) {
    const auto* b = bro_get_physics_bridge();
    if (b && b->destroyBody) {
        b->destroyBody(tag);
        return;
    }
    s_bodies.erase(tag);
}

void bro_Physics_destroyAll(void) {
    const auto* b = bro_get_physics_bridge();
    if (b && b->destroyAll) {
        b->destroyAll();
        return;
    }
    s_bodies.clear();
}

void* bro_Physics_getTransform(int32_t tag) {
    const auto* b = bro_get_physics_bridge();
    if (b && b->getTransform) return b->getTransform(tag);
    auto it = s_bodies.find(tag);
    return it != s_bodies.end() ? &it->second : nullptr;
}

void* bro_Physics_getVelocity(int32_t tag) {
    const auto* b = bro_get_physics_bridge();
    if (b && b->getVelocity) return b->getVelocity(tag);
    auto it = s_bodies.find(tag);
    return it != s_bodies.end() ? &it->second : nullptr;
}

void bro_Physics_setPosition(int32_t tag, double x, double y, double z) {
    const auto* b = bro_get_physics_bridge();
    if (b && b->setPosition) {
        b->setPosition(tag, x, y, z);
        return;
    }
    auto it = s_bodies.find(tag);
    if (it != s_bodies.end()) {
        it->second.posX = x;
        it->second.posY = y;
        it->second.posZ = z;
    }
}

void bro_Physics_setRotation(int32_t tag, double x, double y, double z, double w) {
    const auto* b = bro_get_physics_bridge();
    if (b && b->setRotation) {
        b->setRotation(tag, x, y, z, w);
        return;
    }
    auto it = s_bodies.find(tag);
    if (it != s_bodies.end()) {
        it->second.rotX = x;
        it->second.rotY = y;
        it->second.rotZ = z;
        it->second.rotW = w;
    }
}

void bro_Physics_setLinearVelocity(int32_t tag, double x, double y, double z) {
    const auto* b = bro_get_physics_bridge();
    if (b && b->setLinearVelocity) {
        b->setLinearVelocity(tag, x, y, z);
        return;
    }
    auto it = s_bodies.find(tag);
    if (it != s_bodies.end()) {
        it->second.linVelX = x;
        it->second.linVelY = y;
        it->second.linVelZ = z;
    }
}

void bro_Physics_setAngularVelocity(int32_t tag, double x, double y, double z) {
    const auto* b = bro_get_physics_bridge();
    if (b && b->setAngularVelocity) {
        b->setAngularVelocity(tag, x, y, z);
        return;
    }
    auto it = s_bodies.find(tag);
    if (it != s_bodies.end()) {
        it->second.angVelX = x;
        it->second.angVelY = y;
        it->second.angVelZ = z;
    }
}

void bro_Physics_addForce(int32_t tag, double x, double y, double z) {
    const auto* b = bro_get_physics_bridge();
    if (b && b->addForce) {
        b->addForce(tag, x, y, z);
        return;
    }
    auto it = s_bodies.find(tag);
    if (it != s_bodies.end() && it->second.mass > 0.0) {
        it->second.linVelX += (x / it->second.mass) * s_timeStep;
        it->second.linVelY += (y / it->second.mass) * s_timeStep;
        it->second.linVelZ += (z / it->second.mass) * s_timeStep;
    }
}

void bro_Physics_addImpulse(int32_t tag, double x, double y, double z) {
    const auto* b = bro_get_physics_bridge();
    if (b && b->addImpulse) {
        b->addImpulse(tag, x, y, z);
        return;
    }
    auto it = s_bodies.find(tag);
    if (it != s_bodies.end() && it->second.mass > 0.0) {
        it->second.linVelX += x / it->second.mass;
        it->second.linVelY += y / it->second.mass;
        it->second.linVelZ += z / it->second.mass;
    }
}

void bro_Physics_addTorque(int32_t tag, double x, double y, double z) {
    const auto* b = bro_get_physics_bridge();
    if (b && b->addTorque) {
        b->addTorque(tag, x, y, z);
        return;
    }
    auto it = s_bodies.find(tag);
    if (it != s_bodies.end()) {
        it->second.angVelX += x;
        it->second.angVelY += y;
        it->second.angVelZ += z;
    }
}

void bro_Physics_setUserData(int32_t tag, void* data) {
    auto it = s_bodies.find(tag);
    if (it != s_bodies.end()) it->second.userData = data;
}

void* bro_Physics_getUserData(int32_t tag) {
    auto it = s_bodies.find(tag);
    return it != s_bodies.end() ? it->second.userData : nullptr;
}

void bro_Physics_setLayer(int32_t tag, int32_t layer) {
    auto it = s_bodies.find(tag);
    if (it != s_bodies.end()) it->second.layer = layer;
}

void bro_Physics_setKinematic(int32_t /*tag*/) {}
void bro_Physics_setMotionType(int32_t /*tag*/, void* /*type*/) {}
void bro_Physics_moveKinematic(int32_t tag, double x, double y, double z, double /*dt*/) {
    bro_Physics_setPosition(tag, x, y, z);
}

void* bro_Physics_raycast(double ox, double oy, double oz, double dx, double dy, double dz, double maxDist, int32_t mask) {
    const auto* b = bro_get_physics_bridge();
    if (b && b->raycast) return b->raycast(ox, oy, oz, dx, dy, dz, maxDist, mask);
    return nullptr;
}

void* bro_Physics_raycastClosest(double ox, double oy, double oz, double dx, double dy, double dz, double maxDist, int32_t mask) {
    const auto* b = bro_get_physics_bridge();
    if (b && b->raycastClosest) return b->raycastClosest(ox, oy, oz, dx, dy, dz, maxDist, mask);
    return nullptr;
}

void* bro_Physics_castShape(void* /*config*/) { return nullptr; }
void* bro_Physics_castShapeClosest(void* /*config*/) { return nullptr; }
void* bro_Physics_overlapShape(void* /*config*/) { return nullptr; }
void* bro_Physics_overlapPoint(double /*x*/, double /*y*/, double /*z*/, int32_t /*mask*/) { return nullptr; }
void* bro_Physics_getContacts(void) { return nullptr; }

void bro_Physics_setFrictionCombine(int32_t /*tag*/, const char* /*mode*/) {}
void bro_Physics_setRestitutionCombine(int32_t /*tag*/, const char* /*mode*/) {}

void bro_Physics_setMass(int32_t tag, double mass) {
    auto it = s_bodies.find(tag);
    if (it != s_bodies.end()) it->second.mass = mass;
}

void bro_Physics_setLinearDamping(int32_t tag, double damping) {
    auto it = s_bodies.find(tag);
    if (it != s_bodies.end()) it->second.linearDamping = damping;
}

void bro_Physics_setAngularDamping(int32_t tag, double damping) {
    auto it = s_bodies.find(tag);
    if (it != s_bodies.end()) it->second.angularDamping = damping;
}

void bro_Physics_setGravityFactor(int32_t tag, double factor) {
    auto it = s_bodies.find(tag);
    if (it != s_bodies.end()) it->second.gravityFactor = factor;
}

void bro_Physics_setFriction(int32_t tag, double friction) {
    auto it = s_bodies.find(tag);
    if (it != s_bodies.end()) it->second.friction = friction;
}

void bro_Physics_setRestitution(int32_t tag, double restitution) {
    auto it = s_bodies.find(tag);
    if (it != s_bodies.end()) it->second.restitution = restitution;
}

void* bro_Physics_getBodyProperties(int32_t tag) {
    auto it = s_bodies.find(tag);
    return it != s_bodies.end() ? &it->second : nullptr;
}

void bro_Physics_setAreaOverride(int32_t /*tag*/, void* /*config*/) {}

void bro_Physics_setTimeStep(double dt) {
    const auto* b = bro_get_physics_bridge();
    if (b && b->setTimeStep) {
        b->setTimeStep(dt);
        return;
    }
    s_timeStep = dt;
}

void bro_Physics_step(double dt) {
    const auto* b = bro_get_physics_bridge();
    if (b && b->step) {
        b->step(dt);
        return;
    }
    for (auto& pair : s_bodies) {
        auto& bData = pair.second;
        if (!bData.active) continue;
        bData.linVelX += s_gravityX * bData.gravityFactor * dt;
        bData.linVelY += s_gravityY * bData.gravityFactor * dt;
        bData.linVelZ += s_gravityZ * bData.gravityFactor * dt;
        bData.posX += bData.linVelX * dt;
        bData.posY += bData.linVelY * dt;
        bData.posZ += bData.linVelZ * dt;
    }
}

void bro_Physics_setInterpolation(bool enabled) {
    const auto* b = bro_get_physics_bridge();
    if (b && b->setInterpolation) {
        b->setInterpolation(enabled);
        return;
    }
    s_interpolation = enabled;
}

bool bro_Physics_getInterpolation(void) {
    const auto* b = bro_get_physics_bridge();
    if (b && b->getInterpolation) return b->getInterpolation();
    return s_interpolation;
}

bool bro_Physics_isActive(int32_t tag) {
    const auto* b = bro_get_physics_bridge();
    if (b && b->isActive) return b->isActive(tag);
    auto it = s_bodies.find(tag);
    return it != s_bodies.end() ? it->second.active : false;
}

void bro_Physics_activate(int32_t tag) {
    const auto* b = bro_get_physics_bridge();
    if (b && b->activate) {
        b->activate(tag);
        return;
    }
    auto it = s_bodies.find(tag);
    if (it != s_bodies.end()) it->second.active = true;
}

void* bro_Physics_getAllTransforms(int32_t /*worldHandle*/) { return nullptr; }

void* bro_Physics_createCharacter(void* config) {
    const auto* b = bro_get_physics_bridge();
    if (b && b->createCharacter) return b->createCharacter(config);
    return new BroPhysicsCharacterImpl();
}

void* bro_Physics_createVehicle(void* config) {
    const auto* b = bro_get_physics_bridge();
    if (b && b->createVehicle) return b->createVehicle(config);
    return new BroPhysicsVehicleImpl();
}

void* bro_Physics_createRagdoll(void* config) {
    const auto* b = bro_get_physics_bridge();
    if (b && b->createRagdoll) return b->createRagdoll(config);
    return new BroPhysicsRagdollImpl();
}

void* bro_Physics_createSoftBody(void* config) {
    const auto* b = bro_get_physics_bridge();
    if (b && b->createSoftBody) return b->createSoftBody(config);
    return new BroPhysicsSoftBodyImpl();
}

int32_t bro_Physics_createConstraint(void* /*config*/) {
    return s_nextConstraintTag++;
}

void bro_Physics_destroyConstraint(int32_t /*tag*/) {}
void bro_Physics_setConstraintEnabled(int32_t /*tag*/, bool /*enabled*/) {}
void bro_Physics_setWheelMotor(int32_t /*vehicleTag*/, int32_t /*wheelIndex*/, double /*motorTorque*/, double /*brakeTorque*/) {}
void bro_Physics_setConstraintMotor(int32_t /*tag*/, void* /*config*/) {}
void bro_Physics_setConstraintBreakingImpulse(int32_t /*tag*/, double /*impulse*/) {}
double bro_Physics_getConstraintBreakingImpulse(int32_t /*tag*/) { return 1000.0; }
void* bro_Physics_getBrokenConstraints(void) { return nullptr; }

} // extern "C"
