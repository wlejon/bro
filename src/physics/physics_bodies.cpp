#include "physics/physics_internal.h"
#include "physics/physics_decomposition.h"

#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollidePointResult.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Constraints/ConeConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/Constraints/PulleyConstraint.h>
#include <Jolt/Physics/Constraints/GearConstraint.h>
#include <Jolt/Physics/Constraints/RackAndPinionConstraint.h>
#include <Jolt/Physics/Constraints/TwoBodyConstraint.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>
#include <Jolt/Physics/Vehicle/TrackedVehicleController.h>
#include <Jolt/Physics/Vehicle/MotorcycleController.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Jolt/Skeleton/Skeleton.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>

#include "util/log.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <unordered_set>

JPH_SUPPRESS_WARNINGS

using namespace JPH;

namespace bro::physics {

// --- Body creation ---

RefConst<Shape> buildShape(const BodyOptions& opts) {
    switch (opts.shape) {
        case BodyOptions::ShapeBox: {
            BoxShapeSettings s(opts.halfExtents);
            s.SetDensity(opts.density);
            auto r = s.Create();
            return r.HasError() ? RefConst<Shape>() : r.Get();
        }
        case BodyOptions::ShapeSphere: {
            SphereShapeSettings s(opts.radius);
            s.SetDensity(opts.density);
            auto r = s.Create();
            return r.HasError() ? RefConst<Shape>() : r.Get();
        }
        case BodyOptions::ShapeCapsule: {
            CapsuleShapeSettings s(opts.halfHeight, opts.radius);
            s.SetDensity(opts.density);
            auto r = s.Create();
            return r.HasError() ? RefConst<Shape>() : r.Get();
        }
        case BodyOptions::ShapeCylinder: {
            CylinderShapeSettings s(opts.halfHeight, opts.radius);
            s.SetDensity(opts.density);
            auto r = s.Create();
            return r.HasError() ? RefConst<Shape>() : r.Get();
        }
        case BodyOptions::ShapeConvexHull: {
            if (opts.hullPoints.size() < 4) return RefConst<Shape>();
            Array<Vec3> pts;
            pts.reserve(opts.hullPoints.size());
            for (auto& p : opts.hullPoints) pts.push_back(p);
            ConvexHullShapeSettings s(pts);
            if (opts.convexRadius >= 0.0f) s.mMaxConvexRadius = opts.convexRadius;
            s.SetDensity(opts.density);
            auto r = s.Create();
            return r.HasError() ? RefConst<Shape>() : r.Get();
        }
        case BodyOptions::ShapeMesh: {
            if (opts.meshIndices.size() < 3 || opts.meshVertices.empty()) return RefConst<Shape>();
            VertexList verts;
            verts.reserve(opts.meshVertices.size());
            for (auto& v : opts.meshVertices) verts.push_back(Float3(v.GetX(), v.GetY(), v.GetZ()));
            IndexedTriangleList tris;
            for (size_t i = 0; i + 2 < opts.meshIndices.size(); i += 3) {
                tris.push_back(IndexedTriangle(
                    opts.meshIndices[i], opts.meshIndices[i+1], opts.meshIndices[i+2], 0));
            }
            MeshShapeSettings s(std::move(verts), std::move(tris));
            auto r = s.Create();
            return r.HasError() ? RefConst<Shape>() : r.Get();
        }
        case BodyOptions::ShapeChain: {
            // Polyline in XY plane → vertical wall (extruded along Z) as a
            // single Jolt MeshShape. Front-face direction is the in-plane
            // left-normal of the walking direction (rotate +90° CCW); set
            // chainFlipNormal=true to swap. MeshShape culls back faces by
            // default, giving one-sided collision.
            //
            // bromesh::sweep would be the natural fit here, but its
            // parallel-transport frame doesn't reliably pin profile-local-Y
            // to world-Z for planar paths — direct geometry construction is
            // more predictable and ~30 LOC.
            if (opts.chainPoints.size() < 2) return RefConst<Shape>();
            const float halfDepth = opts.chainDepth * 0.5f;
            const size_t n = opts.chainPoints.size();
            const size_t nSeg = opts.chainClosed ? n : (n - 1);

            VertexList verts;
            verts.reserve(2 * (n + (opts.chainClosed ? 1 : 0)));
            for (size_t i = 0; i < n; i++) {
                const auto& p = opts.chainPoints[i];
                verts.push_back(Float3(p.x, p.y, -halfDepth)); // bottom (z-)
                verts.push_back(Float3(p.x, p.y, +halfDepth)); // top    (z+)
            }
            // For closed loops, wrap by indexing back to vertex 0/1.
            auto botIdx = [&](size_t i) -> uint32_t { return uint32_t(2 * (i % n)); };
            auto topIdx = [&](size_t i) -> uint32_t { return uint32_t(2 * (i % n) + 1); };

            IndexedTriangleList tris;
            tris.reserve(2 * nSeg);
            for (size_t i = 0; i < nSeg; i++) {
                uint32_t a_b = botIdx(i),     a_t = topIdx(i);
                uint32_t b_b = botIdx(i + 1), b_t = topIdx(i + 1);
                if (!opts.chainFlipNormal) {
                    // Front face = in-plane left of the walking direction
                    // (so a +X segment in XY has its front facing +Y).
                    tris.push_back(IndexedTriangle(a_b, b_t, b_b, 0));
                    tris.push_back(IndexedTriangle(a_b, a_t, b_t, 0));
                } else {
                    // Reverse winding → front face on the opposite side.
                    tris.push_back(IndexedTriangle(a_b, b_b, b_t, 0));
                    tris.push_back(IndexedTriangle(a_b, b_t, a_t, 0));
                }
            }

            MeshShapeSettings s(std::move(verts), std::move(tris));
            auto r = s.Create();
            return r.HasError() ? RefConst<Shape>() : r.Get();
        }
        case BodyOptions::ShapeHeightField: {
            const size_t sz = opts.heightSamples.size();
            const uint32_t n = (opts.heightSampleCount >= 4) ? opts.heightSampleCount
                                                             : static_cast<uint32_t>(std::sqrt(sz));
            // Jolt requires sampleCount / blockSize (default 2) >= 2.
            if (n < 4 || sz != static_cast<size_t>(n) * n || (opts.heightSampleCount > 0 && opts.heightSampleCount != n))
                return RefConst<Shape>();
            HeightFieldShapeSettings s(opts.heightSamples.data(), opts.heightOffset,
                                       opts.heightScale, n);
            auto r = s.Create();
            return r.HasError() ? RefConst<Shape>() : r.Get();
        }
        case BodyOptions::ShapeCompound: {
            if (opts.compoundParts.empty()) return RefConst<Shape>();
            if (opts.compoundParts.size() == 1) {
                auto sub = buildShape(opts.compoundParts[0]);
                if (!sub) return RefConst<Shape>();
                const auto& part = opts.compoundParts[0];
                if (part.localPosition != Vec3::sZero() || part.localRotation != Quat::sIdentity()) {
                    RotatedTranslatedShapeSettings rts(part.localPosition, part.localRotation, sub.GetPtr());
                    auto r = rts.Create();
                    return r.HasError() ? RefConst<Shape>() : r.Get();
                }
                return sub;
            }
            StaticCompoundShapeSettings s;
            for (auto& part : opts.compoundParts) {
                auto sub = buildShape(part);
                if (!sub) return RefConst<Shape>();
                s.AddShape(part.localPosition, part.localRotation, sub.GetPtr());
            }
            auto r = s.Create();
            return r.HasError() ? RefConst<Shape>() : r.Get();
        }
        case BodyOptions::ShapeDecomposedMesh:
            return buildDecomposedMeshShape(opts);
    }
    return RefConst<Shape>();
}

BodyID PhysicsWorld::createBody(const BodyOptions& opts) {
    if (opts.hasArea && !opts.isSensor) return BodyID();
    if (opts.shape == BodyOptions::ShapeConvexHull && opts.hullPoints.size() < 4) return BodyID();
    auto shape = buildShape(opts);
    if (!shape) return BodyID();

    // Mesh / chain / heightfield shapes can only be static.
    bool isStatic = opts.isStatic;
    if (opts.shape == BodyOptions::ShapeMesh || opts.shape == BodyOptions::ShapeChain ||
        opts.shape == BodyOptions::ShapeHeightField) isStatic = true;

    int layer = opts.layer;
    if (layer < 0) layer = isStatic ? 0 : 1;
    if (layer >= numLayers_) layer = numLayers_ - 1;

    EMotionType motion = EMotionType::Dynamic;
    if (opts.isKinematic || opts.motionType == BodyOptions::MotionKinematic) motion = EMotionType::Kinematic;
    else if (isStatic || opts.motionType == BodyOptions::MotionStatic) motion = EMotionType::Static;

    BodyCreationSettings settings(shape.GetPtr(), opts.position, opts.rotation,
                                  motion, static_cast<ObjectLayer>(layer));
    settings.mFriction = opts.friction;
    settings.mRestitution = opts.restitution;
    settings.mIsSensor = opts.isSensor;
    settings.mGravityFactor = opts.gravityFactor;
    settings.mLinearDamping = opts.linearDamping;
    settings.mAngularDamping = opts.angularDamping;
    settings.mMaxLinearVelocity = opts.maxLinearVelocity;
    settings.mMaxAngularVelocity = opts.maxAngularVelocity;
    settings.mUserData = opts.userData;
    settings.mAllowedDOFs = opts.dofs;
    settings.mMotionQuality = opts.ccd ? EMotionQuality::LinearCast : EMotionQuality::Discrete;
    if (motion == EMotionType::Dynamic) {
        if (opts.mass > 0.0f) {
            // Direct mass (kg) wins over density: inertia is still derived
            // from the shape, then scaled to the requested mass.
            settings.mOverrideMassProperties = EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass = opts.mass;
        } else {
            settings.mOverrideMassProperties = EOverrideMassProperties::CalculateMassAndInertia;
        }
    }

    BodyInterface& bi = physicsSystem_.GetBodyInterface();
    BodyID id = bi.CreateAndAddBody(settings,
        motion == EMotionType::Dynamic ? EActivation::Activate : EActivation::DontActivate);

    // Remember whether this body is a sensor. OnContactRemoved gets only a
    // BodyID — possibly of a body that no longer exists — so this table is the
    // only way it can label a sensor *exit* correctly. Combine modes live in
    // the same per-index tables and must be (re)written here too, or a reused
    // body index would inherit a destroyed body's modes.
    if (listener_ && !id.IsInvalid()) {
        listener_->setSensorId(id, opts.isSensor);
        listener_->setCombineModes(id, opts.frictionCombine, opts.restitutionCombine);
    }
    // Creation-time area field override (sensors only).
    if (!id.IsInvalid() && opts.hasArea && opts.isSensor)
        setAreaOverride(id, opts.area);
    return id;
}

BodyID PhysicsWorld::createBox(RVec3 position, Quat rotation,
                               Vec3 halfExtents, bool isStatic,
                               float friction, float restitution) {
    BodyOptions o;
    o.shape = BodyOptions::ShapeBox;
    o.position = position; o.rotation = rotation;
    o.halfExtents = halfExtents;
    o.isStatic = isStatic;
    o.friction = friction; o.restitution = restitution;
    return createBody(o);
}

BodyID PhysicsWorld::createSphere(RVec3 position, Quat rotation,
                                  float radius, bool isStatic,
                                  float friction, float restitution) {
    BodyOptions o;
    o.shape = BodyOptions::ShapeSphere;
    o.position = position; o.rotation = rotation;
    o.radius = radius;
    o.isStatic = isStatic;
    o.friction = friction; o.restitution = restitution;
    return createBody(o);
}

BodyID PhysicsWorld::createCapsule(RVec3 position, Quat rotation,
                                   float halfHeight, float radius, bool isStatic,
                                   float friction, float restitution) {
    BodyOptions o;
    o.shape = BodyOptions::ShapeCapsule;
    o.position = position; o.rotation = rotation;
    o.halfHeight = halfHeight; o.radius = radius;
    o.isStatic = isStatic;
    o.friction = friction; o.restitution = restitution;
    return createBody(o);
}

BodyID PhysicsWorld::createCylinder(RVec3 position, Quat rotation,
                                    float halfHeight, float radius, bool isStatic,
                                    float friction, float restitution) {
    BodyOptions o;
    o.shape = BodyOptions::ShapeCylinder;
    o.position = position; o.rotation = rotation;
    o.halfHeight = halfHeight; o.radius = radius;
    o.isStatic = isStatic;
    o.friction = friction; o.restitution = restitution;
    return createBody(o);
}

void PhysicsWorld::removeConstraintsReferencing(BodyID id) {
    // Remove any TwoBodyConstraint that references this body; otherwise Jolt will assert.
    for (auto it = constraints_.begin(); it != constraints_.end(); ) {
        bool refsBody = false;
        if (it->second.ref) {
            auto* c = it->second.ref.GetPtr();
            if (auto* tb = static_cast<TwoBodyConstraint*>(c)) {
                if ((tb->GetBody1() && tb->GetBody1()->GetID() == id) ||
                    (tb->GetBody2() && tb->GetBody2()->GetID() == id)) {
                    refsBody = true;
                }
            }
        }
        if (refsBody) {
            physicsSystem_.RemoveConstraint(it->second.ref.GetPtr());
            if (it->second.ref2) physicsSystem_.RemoveConstraint(it->second.ref2.GetPtr());
            evictConstraintPair(it->second);
            it = constraints_.erase(it);
        } else {
            ++it;
        }
    }
}

void PhysicsWorld::destroyBody(BodyID id,
                               const std::function<void(BodyID)>& onBodyDestroyed) {
    // Liveness gate: a stale id (e.g. a ragdoll sibling already destroyed as
    // part of the unit, or a plain double-destroy) must never reach Jolt's
    // DestroyBody — that asserts in Debug and corrupts the body-manager free
    // list in Release. TryGetBody via the lock interface checks index bounds
    // + sequence number, so a dead id fails cleanly here.
    {
        BodyLockRead lock(physicsSystem_.GetBodyLockInterface(), id);
        if (!lock.Succeeded()) return;
    }
    queryScale_.erase(id.GetIndexAndSequenceNumber());

    // A character's inner body? It is owned by the CharacterVirtual
    // (~CharacterVirtual destroys it) — destroying it here would leave the
    // character referencing a dead body and corrupt the body manager on the
    // second destroy. Destroy the character instead.
    for (const auto& [h, ch] : characters_) {
        if (ch.character && ch.character->GetInnerBodyID() == id) {
            LOG_WARN("destroyBody: body is a character's inner body — "
                     "destroy the character instead (no-op)");
            return;
        }
    }

    // A ragdoll part? The ragdoll is one unit — destroy the whole thing
    // (removeRagdollFromSystem detaches, erasing the entry destroys ALL part
    // bodies via ~Ragdoll, including `id`, so return without the sweep below).
    for (auto it = ragdolls_.begin(); it != ragdolls_.end(); ++it) {
        const auto& ids = it->second.ragdoll->GetBodyIDs();
        if (std::find(ids.begin(), ids.end(), id) != ids.end()) {
            // Report every part body (not just `id`) BEFORE the entry is
            // erased so callers can evict all their per-body bookkeeping.
            if (onBodyDestroyed)
                for (const BodyID& pid : ids) onBodyDestroyed(pid);
            for (const BodyID& pid : ids) evictAreaBookkeeping(pid);
            removeRagdollFromSystem(it->second);
            ragdolls_.erase(it);
            return;
        }
    }

    // Remove any vehicle whose chassis is this body (constraint + step listener).
    for (auto it = vehicles_.begin(); it != vehicles_.end(); ) {
        if (it->second.body == id) {
            removeVehicleFromSystem(it->second);
            it = vehicles_.erase(it);
        } else {
            ++it;
        }
    }

    // A soft body? Evict its registry entry (metadata only — the body itself
    // is destroyed by the generic path below like any other body).
    for (auto it = softBodies_.begin(); it != softBodies_.end(); ++it) {
        if (it->second.body == id) {
            softBodies_.erase(it);
            break;
        }
    }

    removeConstraintsReferencing(id);
    evictAreaBookkeeping(id);

    if (onBodyDestroyed) onBodyDestroyed(id);

    BodyInterface& bi = physicsSystem_.GetBodyInterface();
    if (bi.IsAdded(id)) {
        bi.RemoveBody(id);
    }
    bi.DestroyBody(id);
}


bool PhysicsWorld::bodyExists(BodyID id) const {
    BodyLockRead lock(physicsSystem_.GetBodyLockInterface(), id);
    return lock.Succeeded();
}

// --- Body state ---

RVec3 PhysicsWorld::getPosition(BodyID id) const {
    return physicsSystem_.GetBodyInterface().GetPosition(id);
}

Quat PhysicsWorld::getRotation(BodyID id) const {
    return physicsSystem_.GetBodyInterface().GetRotation(id);
}

Vec3 PhysicsWorld::getLinearVelocity(BodyID id) const {
    return physicsSystem_.GetBodyInterface().GetLinearVelocity(id);
}

Vec3 PhysicsWorld::getAngularVelocity(BodyID id) const {
    return physicsSystem_.GetBodyInterface().GetAngularVelocity(id);
}

void PhysicsWorld::setPosition(BodyID id, RVec3 pos) {
    physicsSystem_.GetBodyInterface().SetPosition(id, pos, EActivation::Activate);
    // Explicit teleport: drop the interpolation snapshot so render-side
    // consumers snap to the new pose instead of gliding across the world.
    prevTransforms_.erase(id.GetIndexAndSequenceNumber());
}

void PhysicsWorld::setRotation(BodyID id, Quat rot) {
    physicsSystem_.GetBodyInterface().SetRotation(id, rot, EActivation::Activate);
    prevTransforms_.erase(id.GetIndexAndSequenceNumber());  // teleport: snap
}

void PhysicsWorld::setLinearVelocity(BodyID id, Vec3 vel) {
    physicsSystem_.GetBodyInterface().SetLinearVelocity(id, vel);
}

void PhysicsWorld::setAngularVelocity(BodyID id, Vec3 vel) {
    physicsSystem_.GetBodyInterface().SetAngularVelocity(id, vel);
}

// Soft bodies ignore the body-level velocity — the XPBD solver integrates
// per-vertex velocities — so AddForce/AddImpulse on the body would be a
// no-op. Spread a uniform velocity change over the non-pinned vertices
// instead (dv = impulse / total vertex mass) so the soft body's body id
// composes with the generic impulse API like every other body.
static bool softBodyAddImpulse(PhysicsSystem& system, BodyID id, Vec3 impulse) {
    bool soft = false;
    {
        BodyLockWrite lock(system.GetBodyLockInterface(), id);
        if (lock.Succeeded() && lock.GetBody().IsSoftBody()) {
            soft = true;
            auto* mp = static_cast<SoftBodyMotionProperties*>(
                lock.GetBody().GetMotionProperties());
            float totalMass = 0.0f;
            for (const auto& v : mp->GetVertices())
                if (v.mInvMass > 0.0f) totalMass += 1.0f / v.mInvMass;
            if (totalMass > 0.0f) {
                Vec3 dv = impulse / totalMass;
                for (auto& v : mp->GetVertices())
                    if (v.mInvMass > 0.0f) v.mVelocity += dv;
            }
        }
    }
    if (soft) system.GetBodyInterface().ActivateBody(id);
    return soft;
}

void PhysicsWorld::addForce(BodyID id, Vec3 force) {
    // Jolt applies AddForce over the next step only; mirror that for soft
    // bodies as a one-step impulse.
    if (softBodyAddImpulse(physicsSystem_, id, force * timeStep_)) return;
    physicsSystem_.GetBodyInterface().AddForce(id, force);
}

void PhysicsWorld::addImpulse(BodyID id, Vec3 impulse) {
    if (softBodyAddImpulse(physicsSystem_, id, impulse)) return;
    physicsSystem_.GetBodyInterface().AddImpulse(id, impulse);
}

void PhysicsWorld::addTorque(BodyID id, Vec3 torque) {
    physicsSystem_.GetBodyInterface().AddTorque(id, torque);
}


void PhysicsWorld::setLayer(BodyID id, int layer) {
    if (layer < 0 || layer >= numLayers_) return;
    auto& bi = physicsSystem_.GetBodyInterface();
    bi.SetObjectLayer(id, static_cast<ObjectLayer>(layer));
    // Jolt's BodyInterface::SetObjectLayer triggers a broadphase notification
    // internally; no extra action needed. (Verified against Jolt source.)
}

void PhysicsWorld::setKinematic(BodyID id) {
    auto& bi = physicsSystem_.GetBodyInterface();
    bi.SetMotionType(id, EMotionType::Kinematic, EActivation::Activate);
}

void PhysicsWorld::moveKinematic(BodyID id, RVec3 targetPos, Quat targetRot, float dt) {
    if (dt <= 0.0f) {
        physicsSystem_.GetBodyInterface().SetPositionAndRotation(
            id, targetPos, targetRot, EActivation::Activate);
        return;
    }
    physicsSystem_.GetBodyInterface().MoveKinematic(id, targetPos, targetRot, dt);
}

void PhysicsWorld::setMotionType(BodyID id, bool isStatic) {
    auto& bi = physicsSystem_.GetBodyInterface();
    {
        BodyLockRead lock(physicsSystem_.GetBodyLockInterface(), id);
        if (!lock.Succeeded()) return;
        // A body CREATED static has no MotionProperties and can never become
        // dynamic (Jolt only allocates them for dynamic/kinematic-capable
        // bodies) — guard instead of tripping Jolt's assert.
        if (!isStatic && lock.GetBody().GetMotionPropertiesUnchecked() == nullptr)
            return;
    }
    EMotionType motion = isStatic ? EMotionType::Static : EMotionType::Dynamic;
    bi.SetMotionType(id, motion, isStatic ? EActivation::DontActivate
                                          : EActivation::Activate);
    // Preserve the body's object layer — a user-configured layer must survive
    // a motion-type toggle. The one swap the broadphase mapping requires:
    // layer 0 is the only layer mapped to the NON_MOVING broadphase tree
    // (see Layers), so a body going dynamic while on layer 0 moves to the
    // default moving layer. A static body on a MOVING-mapped layer is fine
    // (mildly suboptimal broadphase placement, still correct), so the
    // static direction never touches the layer.
    if (!isStatic && bi.GetObjectLayer(id) == 0) {
        bi.SetObjectLayer(id, static_cast<ObjectLayer>(numLayers_ > 1 ? 1 : 0));
    }
}

void PhysicsWorld::activate(BodyID id) {
    physicsSystem_.GetBodyInterface().ActivateBody(id);
}

bool PhysicsWorld::isActive(BodyID id) const {
    return physicsSystem_.GetBodyInterface().IsActive(id);
}

bool PhysicsWorld::isSensor(BodyID id) const {
    BodyLockRead lock(physicsSystem_.GetBodyLockInterface(), id);
    if (!lock.Succeeded()) return false;
    return lock.GetBody().IsSensor();
}

void PhysicsWorld::setFrictionCombine(BodyID id, CombineMode mode) {
    if (!listener_) return;
    BodyLockRead lock(physicsSystem_.GetBodyLockInterface(), id);
    if (!lock.Succeeded()) return;   // never write a dead index's slot
    const uint32_t idx = id.GetIndex();
    if (idx < listener_->frictionModes.size()) {
        listener_->frictionModes[idx] = (uint8_t)mode;
        if (mode != CombineMode::Default) listener_->anyCombineModes = true;
    }
}

void PhysicsWorld::setRestitutionCombine(BodyID id, CombineMode mode) {
    if (!listener_) return;
    BodyLockRead lock(physicsSystem_.GetBodyLockInterface(), id);
    if (!lock.Succeeded()) return;
    const uint32_t idx = id.GetIndex();
    if (idx < listener_->restitutionModes.size()) {
        listener_->restitutionModes[idx] = (uint8_t)mode;
        if (mode != CombineMode::Default) listener_->anyCombineModes = true;
    }
}

// --- Runtime body property mutation ---
//
// All of these run on the phase-owning thread while the physics thread is
// parked (idle-only contract), so BodyLockWrite here is the cold control
// plane — never a per-step cost.

void PhysicsWorld::setMass(BodyID id, float massKg) {
    if (massKg <= 0.0f) return;
    {
        BodyLockWrite lock(physicsSystem_.GetBodyLockInterface(), id);
        if (!lock.Succeeded()) return;
        Body& b = lock.GetBody();
        // Soft-body mass lives per-vertex; static/kinematic have no finite
        // mass to set (Jolt asserts on a zero-inv-mass dynamic body instead).
        if (b.IsSoftBody() || b.GetMotionType() != EMotionType::Dynamic) return;
        MotionProperties* mp = b.GetMotionPropertiesUnchecked();
        if (!mp) return;
        MassProperties m = b.GetShape()->GetMassProperties();
        m.ScaleToMass(massKg);
        mp->SetMassProperties(mp->GetAllowedDOFs(), m);
    }
    // Outside the write lock (ActivateBody takes its own lock on the body).
    physicsSystem_.GetBodyInterface().ActivateBody(id);
}

float PhysicsWorld::getMass(BodyID id) const {
    BodyLockRead lock(physicsSystem_.GetBodyLockInterface(), id);
    if (!lock.Succeeded()) return 0.0f;
    const Body& b = lock.GetBody();
    if (b.IsSoftBody()) return 0.0f;
    const MotionProperties* mp = b.GetMotionPropertiesUnchecked();
    if (!mp) return 0.0f;
    float inv = mp->GetInverseMassUnchecked();
    return inv > 0.0f ? 1.0f / inv : 0.0f;
}

void PhysicsWorld::setLinearDamping(BodyID id, float damping) {
    if (damping < 0.0f) return;
    // Inside an area damping override the live value belongs to the area —
    // update the BASE (what gets restored on exit) instead of fighting it.
    auto it = bodyAreas_.find(id.GetIndexAndSequenceNumber());
    if (it != bodyAreas_.end() && it->second.linOverridden) {
        it->second.baseLinearDamping = damping;
        return;
    }
    BodyLockWrite lock(physicsSystem_.GetBodyLockInterface(), id);
    if (!lock.Succeeded()) return;
    MotionProperties* mp = lock.GetBody().GetMotionPropertiesUnchecked();
    if (mp) mp->SetLinearDamping(damping);
}

float PhysicsWorld::getLinearDamping(BodyID id) const {
    // The user-facing value is the BASE while an area override holds the
    // live slot (symmetric with the setter above).
    auto it = bodyAreas_.find(id.GetIndexAndSequenceNumber());
    if (it != bodyAreas_.end() && it->second.linOverridden)
        return it->second.baseLinearDamping;
    BodyLockRead lock(physicsSystem_.GetBodyLockInterface(), id);
    if (!lock.Succeeded()) return 0.0f;
    const MotionProperties* mp = lock.GetBody().GetMotionPropertiesUnchecked();
    return mp ? mp->GetLinearDamping() : 0.0f;
}

void PhysicsWorld::setAngularDamping(BodyID id, float damping) {
    if (damping < 0.0f) return;
    auto it = bodyAreas_.find(id.GetIndexAndSequenceNumber());
    if (it != bodyAreas_.end() && it->second.angOverridden) {
        it->second.baseAngularDamping = damping;   // takes effect on area exit
        return;
    }
    BodyLockWrite lock(physicsSystem_.GetBodyLockInterface(), id);
    if (!lock.Succeeded()) return;
    MotionProperties* mp = lock.GetBody().GetMotionPropertiesUnchecked();
    if (mp) mp->SetAngularDamping(damping);
}

float PhysicsWorld::getAngularDamping(BodyID id) const {
    auto it = bodyAreas_.find(id.GetIndexAndSequenceNumber());
    if (it != bodyAreas_.end() && it->second.angOverridden)
        return it->second.baseAngularDamping;
    BodyLockRead lock(physicsSystem_.GetBodyLockInterface(), id);
    if (!lock.Succeeded()) return 0.0f;
    const MotionProperties* mp = lock.GetBody().GetMotionPropertiesUnchecked();
    return mp ? mp->GetAngularDamping() : 0.0f;
}

void PhysicsWorld::setGravityFactor(BodyID id, float factor) {
    auto& bi = physicsSystem_.GetBodyInterface();
    bi.SetGravityFactor(id, factor);
    // Wake it: a sleeping island never re-evaluates gravity, so e.g. flipping
    // 0 → 1 on a parked body would otherwise do nothing until poked.
    bi.ActivateBody(id);
}

float PhysicsWorld::getGravityFactor(BodyID id) const {
    return physicsSystem_.GetBodyInterface().GetGravityFactor(id);
}

void PhysicsWorld::setFriction(BodyID id, float friction) {
    physicsSystem_.GetBodyInterface().SetFriction(id, std::max(0.0f, friction));
}

float PhysicsWorld::getFriction(BodyID id) const {
    return physicsSystem_.GetBodyInterface().GetFriction(id);
}

void PhysicsWorld::setRestitution(BodyID id, float restitution) {
    physicsSystem_.GetBodyInterface().SetRestitution(id, std::max(0.0f, restitution));
}

float PhysicsWorld::getRestitution(BodyID id) const {
    return physicsSystem_.GetBodyInterface().GetRestitution(id);
}

// --- Area field overrides ---
//
// All functions here except applyAreaGravity run on the phase-owning thread
// between steps (idle-only setters + the post-step overlap fold), so the
// maps need no locking; applyAreaGravity runs INSIDE the step from the one
// AreaStepListener and only reads them (immutable during a step).

void PhysicsWorld::setAreaOverride(BodyID id, const AreaOverride& o) {
    {
        BodyLockRead lock(physicsSystem_.GetBodyLockInterface(), id);
        if (!lock.Succeeded()) return;
        if (!lock.GetBody().IsSensor()) {
            LOG_WARN("setAreaOverride: body is not a sensor — ignored");
            return;
        }
    }
    const uint32_t key = id.GetIndexAndSequenceNumber();
    areas_[key] = o;
    // Overlap membership is tracked for every sensor, so bodies already
    // inside pick the (new) override up immediately: re-rank their area
    // lists and re-apply damping now; gravity follows on the next step.
    refreshBodiesTouchingArea(key);
}

void PhysicsWorld::clearAreaOverride(BodyID id) {
    const uint32_t key = id.GetIndexAndSequenceNumber();
    if (areas_.erase(key) == 0) return;
    // Membership stays (still a sensor, still overlapping) — only the
    // fields go away: restore damping on affected bodies.
    refreshBodiesTouchingArea(key);
}

bool PhysicsWorld::getAreaOverride(BodyID id, AreaOverride& out) const {
    auto it = areas_.find(id.GetIndexAndSequenceNumber());
    if (it == areas_.end()) return false;
    out = it->second;
    return true;
}

void PhysicsWorld::sortBodyAreas(BodyAreaState& st) {
    std::sort(st.areas.begin(), st.areas.end(),
              [this](const BodyAreaState::AreaRef& a, const BodyAreaState::AreaRef& b) {
        auto ia = areas_.find(a.key);
        auto ib = areas_.find(b.key);
        const int pa = ia != areas_.end() ? ia->second.priority : 0;
        const int pb = ib != areas_.end() ? ib->second.priority : 0;
        if (pa != pb) return pa > pb;   // higher priority first
        return a.key < b.key;           // ties: earlier-created area first
    });
}

void PhysicsWorld::refreshBodiesTouchingArea(uint32_t areaKey) {
    for (auto& [bodyKey, st] : bodyAreas_) {
        bool touches = false;
        for (const auto& r : st.areas)
            if (r.key == areaKey) { touches = true; break; }
        if (!touches) continue;
        sortBodyAreas(st);
        applyAreaDamping(BodyID(bodyKey), st);
        physicsSystem_.GetBodyInterface().ActivateBody(BodyID(bodyKey));
    }
}

void PhysicsWorld::applyAreaDamping(BodyID id, BodyAreaState& st) {
    // Highest-priority overlapping area that specifies a channel wins
    // (st.areas is already priority-sorted). Damping is replace-only.
    float lin = -1.0f, ang = -1.0f;
    for (const auto& r : st.areas) {
        auto it = areas_.find(r.key);
        if (it == areas_.end()) continue;   // plain sensor, no override
        if (lin < 0.0f && it->second.linearDamping >= 0.0f)
            lin = it->second.linearDamping;
        if (ang < 0.0f && it->second.angularDamping >= 0.0f)
            ang = it->second.angularDamping;
        if (lin >= 0.0f && ang >= 0.0f) break;
    }
    BodyLockWrite lock(physicsSystem_.GetBodyLockInterface(), id);
    if (!lock.Succeeded()) return;
    Body& b = lock.GetBody();
    if (b.IsSoftBody()) return;
    MotionProperties* mp = b.GetMotionPropertiesUnchecked();
    if (!mp) return;
    if (lin >= 0.0f) {
        if (!st.linOverridden) {   // save the body's own damping once
            st.baseLinearDamping = mp->GetLinearDamping();
            st.linOverridden = true;
        }
        mp->SetLinearDamping(lin);
    } else if (st.linOverridden) {
        mp->SetLinearDamping(st.baseLinearDamping);
        st.linOverridden = false;
    }
    if (ang >= 0.0f) {
        if (!st.angOverridden) {
            st.baseAngularDamping = mp->GetAngularDamping();
            st.angOverridden = true;
        }
        mp->SetAngularDamping(ang);
    } else if (st.angOverridden) {
        mp->SetAngularDamping(st.baseAngularDamping);
        st.angOverridden = false;
    }
}

void PhysicsWorld::updateAreaOverlaps(const std::vector<ContactEvent>& events) {
    if (!listener_) return;
    for (const auto& e : events) {
        if (!e.isSensor) continue;
        if (e.type == ContactEvent::Persisted) continue;
        // Exactly one side must be the sensor (Jolt sensors don't detect
        // other sensors; skip defensively if both flags agree).
        const bool s1 = listener_->isSensorId(e.body1);
        const bool s2 = listener_->isSensorId(e.body2);
        if (s1 == s2) continue;
        const BodyID areaId = s1 ? e.body1 : e.body2;
        const BodyID bodyId = s1 ? e.body2 : e.body1;
        const uint32_t areaKey = areaId.GetIndexAndSequenceNumber();
        const uint32_t bodyKey = bodyId.GetIndexAndSequenceNumber();
        if (e.type == ContactEvent::Added) {
            BodyAreaState& st = bodyAreas_[bodyKey];
            bool found = false;
            for (auto& r : st.areas)
                if (r.key == areaKey) { r.refs++; found = true; break; }
            if (!found) {
                st.areas.push_back({areaKey, 1});
                sortBodyAreas(st);
                applyAreaDamping(bodyId, st);
            }
        } else {  // Removed
            auto it = bodyAreas_.find(bodyKey);
            if (it == bodyAreas_.end()) continue;
            auto& v = it->second.areas;
            for (size_t i = 0; i < v.size(); i++) {
                if (v[i].key != areaKey) continue;
                if (--v[i].refs > 0) break;
                v.erase(v.begin() + i);
                applyAreaDamping(bodyId, it->second);   // restores base
                if (v.empty() && !it->second.linOverridden &&
                    !it->second.angOverridden)
                    bodyAreas_.erase(it);
                break;
            }
        }
    }
}

void PhysicsWorld::applyAreaGravity(float dt) {
    if (bodyAreas_.empty() || areas_.empty()) return;
    const Vec3 worldG = physicsSystem_.GetGravity();
    const BodyLockInterfaceNoLock& li = physicsSystem_.GetBodyLockInterfaceNoLock();
    for (const auto& [bodyKey, st] : bodyAreas_) {
        Body* body = li.TryGetBody(BodyID(bodyKey));
        if (!body || !body->IsDynamic() || !body->IsActive() || body->IsSoftBody())
            continue;

        // Walk the priority-sorted areas accumulating the effective gravity
        // (see AreaOverride: combine adds, scale multiplies the world term,
        // replace finalizes and stops).
        Vec3 additive = Vec3::sZero();
        float worldScale = 1.0f;
        bool any = false, replaced = false;
        Vec3 total = Vec3::sZero();
        for (const auto& r : st.areas) {
            auto it = areas_.find(r.key);
            if (it == areas_.end()) continue;   // plain sensor
            const AreaOverride& a = it->second;
            if (a.gravityMode == AreaOverride::GravityNone) continue;
            if (a.gravityMode == AreaOverride::GravityScale) {
                worldScale *= a.gravityScale;
                any = true;
                continue;
            }
            Vec3 g = a.gravity;
            if (a.pointGravity) {
                const Body* areaBody = li.TryGetBody(BodyID(r.key));
                if (!areaBody) continue;
                Vec3 delta = Vec3(areaBody->GetCenterOfMassPosition() -
                                  body->GetCenterOfMassPosition());
                float dist = delta.Length();
                if (dist < 1.0e-6f) {
                    g = Vec3::sZero();
                } else {
                    float s = a.strength;
                    if (a.falloffDistance > 0.0f) {
                        float f = a.falloffDistance / dist;   // inverse-square
                        s *= f * f;
                    }
                    g = delta * (s / dist);
                }
            }
            any = true;
            if (a.gravityMode == AreaOverride::GravityReplace) {
                total = additive + g;
                replaced = true;
                break;
            }
            additive += g;   // GravityCombine
        }
        if (!any) continue;
        if (!replaced) total = additive + worldG * worldScale;

        // Jolt integrates worldG * gravityFactor itself this step; add only
        // the difference so the body ends up under `total * gravityFactor`
        // (the body's own gravityFactor scales area gravity too — gf=0
        // floats through every field, matching Godot's gravity_scale).
        const float gf = body->GetMotionProperties()->GetGravityFactor();
        Vec3 dv = (total - worldG) * (gf * dt);
        if (dv != Vec3::sZero())
            body->SetLinearVelocityClamped(body->GetLinearVelocity() + dv);
    }
}

void PhysicsWorld::evictAreaBookkeeping(BodyID id) {
    const uint32_t key = id.GetIndexAndSequenceNumber();
    // As an overlapped body: drop its state (no damping restore — the body
    // is being destroyed).
    bodyAreas_.erase(key);
    // As an area: drop the override and leave every list it was in. The
    // eventual OnContactRemoved for the dead pair is then a harmless no-op.
    areas_.erase(key);
    for (auto it = bodyAreas_.begin(); it != bodyAreas_.end();) {
        auto& v = it->second.areas;
        bool removed = false;
        for (size_t i = 0; i < v.size(); i++) {
            if (v[i].key == key) {
                v.erase(v.begin() + i);
                removed = true;
                break;
            }
        }
        if (removed) {
            applyAreaDamping(BodyID(it->first), it->second);
            if (v.empty() && !it->second.linOverridden && !it->second.angOverridden) {
                it = bodyAreas_.erase(it);
                continue;
            }
        }
        ++it;
    }
}

void PhysicsWorld::setUserData(BodyID id, uint64_t data) {
    physicsSystem_.GetBodyInterface().SetUserData(id, data);
}

uint64_t PhysicsWorld::getUserData(BodyID id) const {
    return physicsSystem_.GetBodyInterface().GetUserData(id);
}


} // namespace bro::physics
