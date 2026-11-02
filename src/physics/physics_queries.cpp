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

// --- Raycasts, shape casts & overlap queries ---
//
// All queries go through the narrow phase: exact geometry, real contact
// points/normals, and layer/body filtering. Call only when idle.

namespace {

// Passes bodies whose object layer bit is set in the mask. Independent of the
// collision matrix — queries may see layers that never collide.
class MaskObjectLayerFilter final : public ObjectLayerFilter {
public:
    explicit MaskObjectLayerFilter(uint32_t mask) : mask_(mask) {}
    bool ShouldCollide(ObjectLayer layer) const override {
        return layer < 32 && ((mask_ >> layer) & 1u) != 0;
    }
private:
    uint32_t mask_;
};

// Excludes QueryFilter::ignoreBody plus everything in ignoreBodies. Linear
// scan — exclude lists are a handful of bodies (a character + its mount, a
// projectile's owner), not a broadphase.
class IgnoreBodiesFilter final : public BodyFilter {
public:
    explicit IgnoreBodiesFilter(const QueryFilter& f) : f_(f) {}
    bool ShouldCollide(const BodyID& id) const override {
        if (id == f_.ignoreBody) return false;
        for (const BodyID& b : f_.ignoreBodies)
            if (id == b) return false;
        return true;
    }
private:
    const QueryFilter& f_;
};

} // namespace

// Surface normal on a body's shape at a world-space hit point. Same
// convention as castShape: the normal on the hit body, which for a ray
// arriving from outside points back toward the ray origin.
static Vec3 hitSurfaceNormal(const PhysicsSystem& system, BodyID id,
                             const SubShapeID& subShapeID, RVec3 position) {
    BodyLockRead lock(system.GetBodyLockInterface(), id);
    if (!lock.Succeeded()) return Vec3(0, 1, 0);
    return lock.GetBody().GetWorldSpaceSurfaceNormal(subShapeID, position);
}

// A query's numbers come from script, and Jolt's broad phase walks off its
// tree on a NaN or infinite bound (overlapSphere({}) crashed in CollideShape).
// A query that is not a place hits nothing.
static bool finiteRay(RVec3 origin, Vec3 direction, float maxDistance) {
    return std::isfinite(origin.GetX()) && std::isfinite(origin.GetY()) &&
           std::isfinite(origin.GetZ()) && std::isfinite(direction.GetX()) &&
           std::isfinite(direction.GetY()) && std::isfinite(direction.GetZ()) &&
           std::isfinite(maxDistance);
}

static bool finiteQueryShape(const BodyOptions& o) {
    const Vec4 q = o.rotation.GetXYZW();
    return finiteRay(o.position, o.halfExtents, o.radius) && std::isfinite(o.halfHeight) &&
           std::isfinite(q.GetX()) && std::isfinite(q.GetY()) && std::isfinite(q.GetZ()) &&
           std::isfinite(q.GetW());
}

std::vector<RayHit> PhysicsWorld::raycast(RVec3 origin, Vec3 direction,
                                          float maxDistance,
                                          const QueryFilter& filter) const {
    if (!finiteRay(origin, direction, maxDistance)) return {};
    awaitStepForQuery();
    RRayCast ray(origin, direction * maxDistance);
    RayCastSettings settings;
    MaskObjectLayerFilter layerFilter(filter.layerMask);
    IgnoreBodiesFilter bodyFilter(filter);
    AllHitCollisionCollector<CastRayCollector> collector;
    physicsSystem_.GetNarrowPhaseQuery().CastRay(ray, settings, collector, {},
                                                 layerFilter, bodyFilter);
    collector.Sort();

    // Mesh-family shapes report per-triangle hits; sorted ascending, the first
    // occurrence of each body is its earliest contact — keep only that one.
    std::vector<RayHit> hits;
    for (auto& hit : collector.mHits) {
        bool seen = false;
        for (auto& e : hits) {
            if (e.bodyID == hit.mBodyID) { seen = true; break; }
        }
        if (seen) continue;
        RayHit h;
        h.bodyID = hit.mBodyID;
        h.fraction = hit.mFraction;
        h.position = ray.GetPointOnRay(hit.mFraction);
        h.normal = hitSurfaceNormal(physicsSystem_, hit.mBodyID,
                                    hit.mSubShapeID2, h.position);
        hits.push_back(h);
    }
    return hits;
}

bool PhysicsWorld::raycastClosest(RVec3 origin, Vec3 direction,
                                  RayHit& outHit, float maxDistance,
                                  const QueryFilter& filter) const {
    if (!finiteRay(origin, direction, maxDistance)) return false;
    awaitStepForQuery();
    RRayCast ray(origin, direction * maxDistance);
    MaskObjectLayerFilter layerFilter(filter.layerMask);
    IgnoreBodiesFilter bodyFilter(filter);
    RayCastResult hit;
    if (!physicsSystem_.GetNarrowPhaseQuery().CastRay(ray, hit, {},
                                                      layerFilter, bodyFilter))
        return false;

    outHit.bodyID = hit.mBodyID;
    outHit.fraction = hit.mFraction;
    outHit.position = ray.GetPointOnRay(hit.mFraction);
    outHit.normal = hitSurfaceNormal(physicsSystem_, hit.mBodyID,
                                     hit.mSubShapeID2, outHit.position);
    return true;
}

std::vector<ShapeCastHit> PhysicsWorld::castShape(const BodyOptions& shapeOpts,
                                                  Vec3 direction, float maxDistance,
                                                  const QueryFilter& filter) const {
    std::vector<ShapeCastHit> hits;
    if (!finiteQueryShape(shapeOpts) || !finiteRay(shapeOpts.position, direction, maxDistance))
        return hits;
    awaitStepForQuery();
    auto shape = buildShape(shapeOpts);
    if (!shape || shape->GetType() != EShapeType::Convex) return hits;

    RShapeCast cast = RShapeCast::sFromWorldTransform(
        shape.GetPtr(), Vec3::sOne(),
        RMat44::sRotationTranslation(shapeOpts.rotation, shapeOpts.position),
        direction * maxDistance);
    ShapeCastSettings settings;
    MaskObjectLayerFilter layerFilter(filter.layerMask);
    IgnoreBodiesFilter bodyFilter(filter);
    AllHitCollisionCollector<CastShapeCollector> collector;
    physicsSystem_.GetNarrowPhaseQuery().CastShape(
        cast, settings, RVec3::sZero(), collector, {}, layerFilter, bodyFilter);
    collector.Sort();

    // Mesh-family shapes report per-triangle hits; sorted ascending, the first
    // occurrence of each body is its earliest contact — keep only that one.
    for (auto& h : collector.mHits) {
        bool seen = false;
        for (auto& e : hits) {
            if (e.bodyID == h.mBodyID2) { seen = true; break; }
        }
        if (seen) continue;
        ShapeCastHit out;
        out.bodyID = h.mBodyID2;
        out.fraction = h.mFraction;
        out.position = RVec3(h.mContactPointOn2);
        out.normal = (-h.mPenetrationAxis).NormalizedOr(Vec3(0, 1, 0));
        hits.push_back(out);
    }
    return hits;
}

bool PhysicsWorld::castShapeClosest(const BodyOptions& shapeOpts, Vec3 direction,
                                    float maxDistance, ShapeCastHit& outHit,
                                    const QueryFilter& filter) const {
    if (!finiteQueryShape(shapeOpts) || !finiteRay(shapeOpts.position, direction, maxDistance))
        return false;
    awaitStepForQuery();
    auto shape = buildShape(shapeOpts);
    if (!shape || shape->GetType() != EShapeType::Convex) return false;

    RShapeCast cast = RShapeCast::sFromWorldTransform(
        shape.GetPtr(), Vec3::sOne(),
        RMat44::sRotationTranslation(shapeOpts.rotation, shapeOpts.position),
        direction * maxDistance);
    ShapeCastSettings settings;
    MaskObjectLayerFilter layerFilter(filter.layerMask);
    IgnoreBodiesFilter bodyFilter(filter);
    ClosestHitCollisionCollector<CastShapeCollector> collector;
    physicsSystem_.GetNarrowPhaseQuery().CastShape(
        cast, settings, RVec3::sZero(), collector, {}, layerFilter, bodyFilter);
    if (!collector.HadHit()) return false;

    outHit.bodyID = collector.mHit.mBodyID2;
    outHit.fraction = collector.mHit.mFraction;
    outHit.position = RVec3(collector.mHit.mContactPointOn2);
    outHit.normal = (-collector.mHit.mPenetrationAxis).NormalizedOr(Vec3(0, 1, 0));
    return true;
}

std::vector<OverlapHit> PhysicsWorld::overlapShape(const BodyOptions& shapeOpts,
                                                   const QueryFilter& filter) const {
    std::vector<OverlapHit> hits;
    if (!finiteQueryShape(shapeOpts)) return hits;
    awaitStepForQuery();
    auto shape = buildShape(shapeOpts);
    if (!shape || shape->GetType() != EShapeType::Convex) return hits;

    // CollideShape wants the center-of-mass transform, not the body transform.
    RMat44 com = RMat44::sRotationTranslation(shapeOpts.rotation, shapeOpts.position)
                     .PreTranslated(shape->GetCenterOfMass());
    CollideShapeSettings settings;
    MaskObjectLayerFilter layerFilter(filter.layerMask);
    IgnoreBodiesFilter bodyFilter(filter);
    AllHitCollisionCollector<CollideShapeCollector> collector;
    physicsSystem_.GetNarrowPhaseQuery().CollideShape(
        shape.GetPtr(), Vec3::sOne(), com, settings, RVec3::sZero(), collector,
        {}, layerFilter, bodyFilter);

    // One hit per body — keep the deepest contact.
    for (auto& h : collector.mHits) {
        OverlapHit* existing = nullptr;
        for (auto& e : hits) {
            if (e.bodyID == h.mBodyID2) { existing = &e; break; }
        }
        if (existing && existing->depth >= h.mPenetrationDepth) continue;
        OverlapHit out;
        out.bodyID = h.mBodyID2;
        out.depth = h.mPenetrationDepth;
        out.position = RVec3(h.mContactPointOn2);
        out.normal = (-h.mPenetrationAxis).NormalizedOr(Vec3(0, 1, 0));
        if (existing) *existing = out;
        else hits.push_back(out);
    }
    return hits;
}

std::vector<BodyID> PhysicsWorld::overlapPoint(RVec3 point,
                                               const QueryFilter& filter) const {
    if (!finiteRay(point, Vec3::sZero(), 0.0f)) return {};
    awaitStepForQuery();
    MaskObjectLayerFilter layerFilter(filter.layerMask);
    IgnoreBodiesFilter bodyFilter(filter);
    AllHitCollisionCollector<CollidePointCollector> collector;
    physicsSystem_.GetNarrowPhaseQuery().CollidePoint(
        point, collector, {}, layerFilter, bodyFilter);

    std::vector<BodyID> out;
    for (auto& h : collector.mHits) {
        if (std::find(out.begin(), out.end(), h.mBodyID) == out.end())
            out.push_back(h.mBodyID);
    }
    return out;
}

std::vector<PhysicsWorld::StaticBodyInfo> PhysicsWorld::collectStaticBodies() const {
    std::vector<StaticBodyInfo> out;
    BodyIDVector ids;
    physicsSystem_.GetBodies(ids);
    // No-lock fast path only while the world is provably idle. The frame-start
    // consumeStep is non-blocking, so a step that overruns a frame leaves the
    // physics thread inside Update() while JS runs — fall back to the locking
    // interface then (Jolt supports it concurrently with Update).
    const BodyLockInterface& li = isIdle()
        ? static_cast<const BodyLockInterface&>(physicsSystem_.GetBodyLockInterfaceNoLock())
        : physicsSystem_.GetBodyLockInterface();
    for (BodyID id : ids) {
        BodyLockRead lock(li, id);
        if (!lock.Succeeded()) continue;
        const Body& b = lock.GetBody();
        if (!b.IsStatic()) continue;
        const AABox& box = b.GetWorldSpaceBounds();
        out.push_back(StaticBodyInfo{
            id,
            box.mMin,
            box.mMax,
            (int)b.GetObjectLayer(),
            b.IsSensor(),
        });
    }
    return out;
}

void PhysicsWorld::collectStaticTriangles(std::vector<float>& outXyz,
                                          std::vector<uint32_t>& outIndices,
                                          uint32_t layerMask) const {
    BodyIDVector ids;
    physicsSystem_.GetBodies(ids);
    // Same lock-choice rule as collectStaticBodies above.
    const BodyLockInterface& li = isIdle()
        ? static_cast<const BodyLockInterface&>(physicsSystem_.GetBodyLockInterfaceNoLock())
        : physicsSystem_.GetBodyLockInterface();

    // Jolt streams triangles in batches; 256 comfortably exceeds the
    // cGetTrianglesMinTrianglesRequested floor (32).
    constexpr int kBatch = 256;
    std::vector<Float3> buf(static_cast<size_t>(kBatch) * 3);
    Shape::GetTrianglesContext triCtx;

    for (BodyID id : ids) {
        BodyLockRead lock(li, id);
        if (!lock.Succeeded()) continue;
        const Body& b = lock.GetBody();
        if (!b.IsStatic() || b.IsSensor()) continue;
        const uint32_t layer = static_cast<uint32_t>(b.GetObjectLayer());
        if (layer < 32 && !(layerMask & (1u << layer))) continue;
        const Shape* shape = b.GetShape();
        if (!shape) continue;

        // World-space triangles: shape geometry is COM-relative, so the COM
        // transform maps it into world space (rotation included).
        shape->GetTrianglesStart(triCtx, AABox::sBiggest(),
                                 Vec3(b.GetCenterOfMassPosition()),
                                 b.GetRotation(), Vec3::sReplicate(1.0f));
        for (;;) {
            const int n = shape->GetTrianglesNext(triCtx, kBatch, buf.data(), nullptr);
            if (n <= 0) break;
            const uint32_t base = static_cast<uint32_t>(outXyz.size() / 3);
            outXyz.reserve(outXyz.size() + static_cast<size_t>(n) * 9);
            outIndices.reserve(outIndices.size() + static_cast<size_t>(n) * 3);
            for (int i = 0; i < n * 3; i++) {
                outXyz.push_back(buf[i].x);
                outXyz.push_back(buf[i].y);
                outXyz.push_back(buf[i].z);
                outIndices.push_back(base + static_cast<uint32_t>(i));
            }
        }
    }
}


} // namespace bro::physics
