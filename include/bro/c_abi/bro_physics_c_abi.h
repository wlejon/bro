// =============================================================================
// bro_physics_c_abi.h — Pure C-ABI declarations for bro.physics
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_PHYSICS_C_ABI_H
#define BRO_PHYSICS_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.physics.PhysicsWorldHandle ---
void* bro_PhysicsWorldHandle_create(void);
void  bro_PhysicsWorldHandle_destroy(void* self);
void bro_PhysicsWorldHandle_destroy(void* self);
void bro_PhysicsWorldHandle_step(void* self, double dt);

// --- Interface bro.physics.PhysicsCharacter ---
void* bro_PhysicsCharacter_create(void);
void  bro_PhysicsCharacter_destroy(void* self);
void bro_PhysicsCharacter_setPosition(void* self, double x, double y, double z);
void bro_PhysicsCharacter_setLinearVelocity(void* self, double x, double y, double z);
void* bro_PhysicsCharacter_getPosition(void* self);
void* bro_PhysicsCharacter_getLinearVelocity(void* self);
void bro_PhysicsCharacter_update(void* self, double dt);

// --- Interface bro.physics.PhysicsVehicle ---
void* bro_PhysicsVehicle_create(void);
void  bro_PhysicsVehicle_destroy(void* self);
void bro_PhysicsVehicle_setDriverInput(void* self, double forward, double steer, double brake, double handBrake);
void* bro_PhysicsVehicle_getTransform(void* self);

// --- Interface bro.physics.PhysicsRagdoll ---
void* bro_PhysicsRagdoll_create(void);
void  bro_PhysicsRagdoll_destroy(void* self);
void bro_PhysicsRagdoll_driveToPose(void* self, void* pose, double dt);
void* bro_PhysicsRagdoll_getPose(void* self);

// --- Interface bro.physics.PhysicsSoftBody ---
void* bro_PhysicsSoftBody_create(void);
void  bro_PhysicsSoftBody_destroy(void* self);
void* bro_PhysicsSoftBody_getBounds(void* self);

// --- Namespace bro.Physics ---
void* bro_Physics_createWorldHandle(void* opts);
void bro_Physics_createWorld(void* opts);
void bro_Physics_setGravity(double x, double y, double z);
void* bro_Physics_getGravity(void);
void bro_Physics_setLayers(void* config);
int32_t bro_Physics_createBody(void* config);
void bro_Physics_destroyBody(int32_t tag);
void bro_Physics_destroyAll(void);
void* bro_Physics_getTransform(int32_t tag);
void* bro_Physics_getVelocity(int32_t tag);
void bro_Physics_setPosition(int32_t tag, double x, double y, double z);
void bro_Physics_setRotation(int32_t tag, double x, double y, double z, double w);
void bro_Physics_setLinearVelocity(int32_t tag, double x, double y, double z);
void bro_Physics_setAngularVelocity(int32_t tag, double x, double y, double z);
void bro_Physics_addForce(int32_t tag, double x, double y, double z);
void bro_Physics_addImpulse(int32_t tag, double x, double y, double z);
void bro_Physics_addTorque(int32_t tag, double x, double y, double z);
void bro_Physics_setUserData(int32_t tag, void* data);
void* bro_Physics_getUserData(int32_t tag);
void bro_Physics_setLayer(int32_t tag, int32_t layer);
void bro_Physics_setKinematic(int32_t tag);
void bro_Physics_setMotionType(int32_t tag, void* type);
void bro_Physics_moveKinematic(int32_t tag, double x, double y, double z, double dt);
void* bro_Physics_raycast(double ox, double oy, double oz, double dx, double dy, double dz, double maxDist, int32_t mask);
void* bro_Physics_raycastClosest(double ox, double oy, double oz, double dx, double dy, double dz, double maxDist, int32_t mask);
void* bro_Physics_castShape(void* config);
void* bro_Physics_castShapeClosest(void* config);
void* bro_Physics_overlapShape(void* config);
void* bro_Physics_overlapPoint(double x, double y, double z, int32_t mask);
void* bro_Physics_getContacts(void);
void bro_Physics_setFrictionCombine(int32_t tag, const char* mode);
void bro_Physics_setRestitutionCombine(int32_t tag, const char* mode);
void bro_Physics_setMass(int32_t tag, double mass);
void bro_Physics_setLinearDamping(int32_t tag, double damping);
void bro_Physics_setAngularDamping(int32_t tag, double damping);
void bro_Physics_setGravityFactor(int32_t tag, double factor);
void bro_Physics_setFriction(int32_t tag, double friction);
void bro_Physics_setRestitution(int32_t tag, double restitution);
void* bro_Physics_getBodyProperties(int32_t tag);
void bro_Physics_setAreaOverride(int32_t tag, void* config);
void bro_Physics_setTimeStep(double dt);
void bro_Physics_step(double dt);
void bro_Physics_setInterpolation(bool enabled);
bool bro_Physics_getInterpolation(void);
bool bro_Physics_isActive(int32_t tag);
void bro_Physics_activate(int32_t tag);
void* bro_Physics_getAllTransforms(int32_t worldHandle);
void* bro_Physics_createCharacter(void* config);
void* bro_Physics_createVehicle(void* config);
void* bro_Physics_createRagdoll(void* config);
void* bro_Physics_createSoftBody(void* config);
int32_t bro_Physics_createConstraint(void* config);
void bro_Physics_destroyConstraint(int32_t tag);
void bro_Physics_setConstraintEnabled(int32_t tag, bool enabled);
void bro_Physics_setWheelMotor(int32_t vehicleTag, int32_t wheelIndex, double motorTorque, double brakeTorque);
void bro_Physics_setConstraintMotor(int32_t tag, void* config);
void bro_Physics_setConstraintBreakingImpulse(int32_t tag, double impulse);
double bro_Physics_getConstraintBreakingImpulse(int32_t tag);
void* bro_Physics_getBrokenConstraints(void);

#ifdef __cplusplus
}
#endif

#endif // BRO_PHYSICS_C_ABI_H
