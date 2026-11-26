// native_dynamics_decl.h — the C entry points native_dynamics_register.cpp registers, one
// prototype per native. Hand-maintained: a native is added or changed here, in the
// register file, in its JS wrapper and in its body together. The bodies are
// hand-written in bro against the engine, and the compiler checks them here.
//
// How each type crosses (bronze/src/abi/bronze_native_type.h):
//   double / int32_t / bool      f64 / i32 / bool
//   (const T*, uint32_t)         a typed array: elements and count, valid for the call
//   bronze_native_buffer* out    a typed-array RESULT: the function returns void and
//                                fills *out (data, length in elements); release set
//                                and the array is a view over data, with release(ctx)
//                                run when it is collected
//
// A world is named by a small integer; so are its bodies and its joints (-1: none).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "abi/bronze_native_type.h"  // bronze_native_buffer, owned by bronze

#ifdef __cplusplus
extern "C" {
#endif

// bro.dynamics.createWorld
//   registered at __bro_native.dynamics.create; the world's number
int32_t bro_dynamics_create(void);

// World.destroy
//   registered at __bro_native.dynamics.destroy
void bro_dynamics_destroy(int32_t world);

// World.gravity write
//   registered at __bro_native.dynamics.setGravity
void bro_dynamics_setGravity(int32_t world, double x, double y, double z);

// World.addBox / World.addSphere / World.addCapsule
//   registered at __bro_native.dynamics.addBody
//   shape: 0 box, 1 sphere, 2 capsule
//   desc: position 3, rotation 4 (x y z w), size 3 (a box's half extents; a sphere's
//   radius first; a capsule's radius and half height), mass, friction, restitution,
//   velocity 3, angular 3, linear damping, angular damping, gravity scale: 22 floats
//   flags: 1 kinematic, 2 bullet, 4 planar. layers, mask: bit sets
int32_t bro_dynamics_addBody(int32_t world, int32_t shape, const float* desc, uint32_t descLen, int32_t flags,
                             int32_t layers, int32_t mask);

// World.addHull / World.addCylinder
//   registered at __bro_native.dynamics.addHull
//   xyz: the points, body space; -1 when they make no hull
int32_t bro_dynamics_addHull(int32_t world, const float* desc, uint32_t descLen, int32_t flags, int32_t layers,
                             int32_t mask, const float* xyz, uint32_t xyzLen);

// World.addMesh
//   registered at __bro_native.dynamics.addMesh
//   xyz, indices: triangles, body space; -1 when they make no mesh
int32_t bro_dynamics_addMesh(int32_t world, const float* desc, uint32_t descLen, int32_t layers, int32_t mask,
                             const float* xyz, uint32_t xyzLen, const int32_t* indices, uint32_t indexLen);

// World.maxSpeed / World.hitSpeed writes
//   registered at __bro_native.dynamics.setMaxSpeed / setHitSpeed
void bro_dynamics_setMaxSpeed(int32_t world, double speed);
void bro_dynamics_setHitSpeed(int32_t world, double speed);

// World.moveTo
//   registered at __bro_native.dynamics.moveTo; pose: position 3, rotation 4
void bro_dynamics_moveTo(int32_t world, int32_t body, const float* pose, uint32_t poseLen, double dt);

// World.wake / World.hasBody
//   registered at __bro_native.dynamics.wake / hasBody
void bro_dynamics_wake(int32_t world, int32_t body);
bool bro_dynamics_hasBody(int32_t world, int32_t body);

// World.touched
//   registered at __bro_native.dynamics.touched; two bodies per pair
void bro_dynamics_touched(int32_t world, bronze_native_buffer* out);

// World.hits
//   registered at __bro_native.dynamics.hits; per hit: a, b, point 3, normal 3, speed
void bro_dynamics_hits(int32_t world, bronze_native_buffer* out);

// World.raycast
//   registered at __bro_native.dynamics.raycast
//   ray: origin 3, direction 3. Empty when nothing is hit, else body, point 3, normal 3,
//   distance
void bro_dynamics_raycast(int32_t world, const float* ray, uint32_t rayLen, double maxDistance, int32_t mask,
                          bronze_native_buffer* out);

// World.removeBody
//   registered at __bro_native.dynamics.removeBody
void bro_dynamics_removeBody(int32_t world, int32_t body);

// World.addJoint
//   registered at __bro_native.dynamics.addJoint
//   kind: 0 weld, 1 hinge, 2 ball. a, b: bodies, -1 the world itself.
//   desc: anchor 3, axis 3, breakForce, breakTorque: 8 floats
int32_t bro_dynamics_addJoint(int32_t world, int32_t kind, int32_t a, int32_t b, const float* desc, uint32_t descLen,
                              bool collide);

// World.removeJoint
//   registered at __bro_native.dynamics.removeJoint
void bro_dynamics_removeJoint(int32_t world, int32_t joint);

// World.hasJoint
//   registered at __bro_native.dynamics.hasJoint
bool bro_dynamics_hasJoint(int32_t world, int32_t joint);

// World.jointLoad
//   registered at __bro_native.dynamics.jointLoad; force 3, torque 3
void bro_dynamics_jointLoad(int32_t world, int32_t joint, bronze_native_buffer* out);

// World.step
//   registered at __bro_native.dynamics.step
void bro_dynamics_step(int32_t world, double dt, int32_t substeps);

// World.pose
//   registered at __bro_native.dynamics.pose; position 3, rotation 4
void bro_dynamics_pose(int32_t world, int32_t body, bronze_native_buffer* out);

// World.setPose
//   registered at __bro_native.dynamics.setPose
void bro_dynamics_setPose(int32_t world, int32_t body, const float* pose, uint32_t poseLen);

// World.velocity
//   registered at __bro_native.dynamics.velocity; linear 3, angular 3
void bro_dynamics_velocity(int32_t world, int32_t body, bronze_native_buffer* out);

// World.setVelocity
//   registered at __bro_native.dynamics.setVelocity
void bro_dynamics_setVelocity(int32_t world, int32_t body, const float* v, uint32_t vLen);

// World.applyImpulse
//   registered at __bro_native.dynamics.applyImpulse; impulse 3, the point 3
void bro_dynamics_applyImpulse(int32_t world, int32_t body, const float* v, uint32_t vLen);

// World.mass
//   registered at __bro_native.dynamics.mass
double bro_dynamics_mass(int32_t world, int32_t body);

// World.moved
//   registered at __bro_native.dynamics.moved
//   the bodies the last step moved, nine floats each: body, position 3, rotation 4,
//   1 when it came to rest in that step
void bro_dynamics_moved(int32_t world, bronze_native_buffer* out);

// World.broken
//   registered at __bro_native.dynamics.broken; the joints the last step broke
void bro_dynamics_broken(int32_t world, bronze_native_buffer* out);

// World.counts
//   registered at __bro_native.dynamics.counts; bodies, joints, awake, contacts, stepMs
void bro_dynamics_counts(int32_t world, bronze_native_buffer* out);

#ifdef __cplusplus
}  // extern "C"
#endif
