#include "physics/physics_world.h"

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
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
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

#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <mutex>

JPH_SUPPRESS_WARNINGS

using namespace JPH;

namespace bro::physics {

// --- Layer setup ---
//
// We allocate a fixed kMaxLayers ObjectLayer slots and a fixed 2 BroadPhase layers
// (NON_MOVING=0, MOVING=1). Each ObjectLayer maps to BP_NON_MOVING or BP_MOVING
// based on whether it's "static-ish" — by default, layer 0 is static, layer 1+ moving.

namespace LayerDefs {
    static constexpr BroadPhaseLayer BP_NON_MOVING(0);
    static constexpr BroadPhaseLayer BP_MOVING(1);
    static constexpr uint32_t BP_NUM_LAYERS = 2;
}

struct PhysicsWorld::Layers {
    std::unique_ptr<ObjectLayerPairFilterTable> objectFilter;
    std::unique_ptr<BroadPhaseLayerInterfaceTable> bpLayerInterface;
    std::unique_ptr<ObjectVsBroadPhaseLayerFilterTable> objectVsBpFilter;

    Layers() {
        // Always allocate kMaxLayers slots so we can rename/reuse without re-Init.
        objectFilter = std::make_unique<ObjectLayerPairFilterTable>(PhysicsWorld::kMaxLayers);

        bpLayerInterface = std::make_unique<BroadPhaseLayerInterfaceTable>(
            PhysicsWorld::kMaxLayers, LayerDefs::BP_NUM_LAYERS);

        // Default: layer 0 = static (BP_NON_MOVING), 1..n = moving (BP_MOVING)
        bpLayerInterface->MapObjectToBroadPhaseLayer(0, LayerDefs::BP_NON_MOVING);
        for (int i = 1; i < PhysicsWorld::kMaxLayers; ++i) {
            bpLayerInterface->MapObjectToBroadPhaseLayer(
                static_cast<ObjectLayer>(i), LayerDefs::BP_MOVING);
        }

        objectVsBpFilter = std::make_unique<ObjectVsBroadPhaseLayerFilterTable>(
            *bpLayerInterface, LayerDefs::BP_NUM_LAYERS,
            *objectFilter, PhysicsWorld::kMaxLayers);
    }
};

// --- Contact listener (collects events for JS, per-world) ---
//
// Contact listener semantics (intentional):
//  - OnContactAdded fires once when a new pair forms → ContactEvent::Added
//  - OnContactRemoved fires once when a pair separates → ContactEvent::Removed
//  - OnContactPersisted is intentionally NOT subscribed to. We do not surface
//    per-step "still in contact" events to JS; users wanting that can iterate
//    bodies themselves. This keeps the event queue O(events) not O(pairs*frames).

struct PhysicsWorld::ListenerImpl : public ContactListener {
    void OnContactAdded(const Body& b1, const Body& b2,
                        const ContactManifold&, ContactSettings&) override {
        std::lock_guard lock(mutex);
        ContactEvent e;
        e.type = ContactEvent::Added;
        e.body1 = b1.GetID();
        e.body2 = b2.GetID();
        e.isSensor = b1.IsSensor() || b2.IsSensor();
        events.push_back(e);
    }

    void OnContactRemoved(const SubShapeIDPair& pair) override {
        std::lock_guard lock(mutex);
        ContactEvent e;
        e.type = ContactEvent::Removed;
        e.body1 = pair.GetBody1ID();
        e.body2 = pair.GetBody2ID();
        e.isSensor = false;  // can't tell here without body lookup; leave false
        events.push_back(e);
    }

    std::vector<ContactEvent> drain() {
        std::lock_guard lock(mutex);
        std::vector<ContactEvent> out;
        out.swap(events);
        return out;
    }

    std::mutex mutex;
    std::vector<ContactEvent> events;
};

// --- Jolt global init (once) ---

static bool s_joltInitialized = false;

static void ensureJoltInit() {
    if (s_joltInitialized) return;
    RegisterDefaultAllocator();
    Factory::sInstance = new Factory();
    RegisterTypes();
    s_joltInitialized = true;
}

// --- PhysicsWorld ---

PhysicsWorld::PhysicsWorld() = default;

PhysicsWorld::~PhysicsWorld() {
    shutdown();
}

bool PhysicsWorld::init(int maxBodies) {
    if (initialized_) return true;

    ensureJoltInit();

    tempAllocator_ = std::make_unique<TempAllocatorImpl>(10 * 1024 * 1024);
    jobSystem_ = std::make_unique<JobSystemThreadPool>(
        cMaxPhysicsJobs, cMaxPhysicsBarriers,
        std::max(1u, std::thread::hardware_concurrency() - 2));

    layers_ = std::make_unique<Layers>();

    // Default layer config: 2 layers ("static", "moving"), they collide with each other
    // and "moving" with itself.
    layerNames_ = {"static", "moving"};
    numLayers_ = 2;
    layerMatrix_ = {
        false, true,   // static vs static, static vs moving
        true,  true,   // moving vs static, moving vs moving
    };
    // Apply matrix to filter, THEN rebuild the BP filter table from it,
    // BEFORE handing pointers to PhysicsSystem (which caches them).
    rebuildLayerFilters();

    physicsSystem_.Init(
        (uint)maxBodies, 0,
        (uint)std::min(maxBodies, 4096),       // max body pairs
        (uint)std::min(maxBodies * 2, 4096),   // max contact constraints
        *layers_->bpLayerInterface,
        *layers_->objectVsBpFilter,
        *layers_->objectFilter);

    physicsSystem_.SetGravity(Vec3(0, -9.81f, 0));

    // Install per-world contact listener
    listener_ = std::make_unique<ListenerImpl>();
    physicsSystem_.SetContactListener(listener_.get());

    initialized_ = true;
    return true;
}

void PhysicsWorld::rebuildLayerFilters() {
    if (!layers_) return;
    auto& f = *layers_->objectFilter;
    // Clear: disable all pairs, then enable from matrix.
    for (int i = 0; i < kMaxLayers; ++i) {
        for (int j = 0; j < kMaxLayers; ++j) {
            f.DisableCollision(static_cast<ObjectLayer>(i), static_cast<ObjectLayer>(j));
        }
    }
    int n = numLayers_;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            if (layerMatrix_[i * n + j]) {
                f.EnableCollision(static_cast<ObjectLayer>(i), static_cast<ObjectLayer>(j));
            }
        }
    }
    // ObjectVsBroadPhaseLayerFilterTable caches the matrix at construction time.
    // PhysicsSystem stores a const ref to it after Init(), so we MUST keep the
    // same heap address. Rebuild in place via placement-new.
    if (layers_->objectVsBpFilter) {
        auto* p = layers_->objectVsBpFilter.get();
        p->~ObjectVsBroadPhaseLayerFilterTable();
        new (p) ObjectVsBroadPhaseLayerFilterTable(
            *layers_->bpLayerInterface, LayerDefs::BP_NUM_LAYERS,
            *layers_->objectFilter, kMaxLayers);
    } else {
        layers_->objectVsBpFilter = std::make_unique<ObjectVsBroadPhaseLayerFilterTable>(
            *layers_->bpLayerInterface, LayerDefs::BP_NUM_LAYERS,
            *layers_->objectFilter, kMaxLayers);
    }
}

bool PhysicsWorld::configureLayers(const std::vector<std::string>& names,
                                   const std::vector<bool>& matrix) {
    int n = (int)names.size();
    if (n <= 0 || n > kMaxLayers) return false;
    if ((int)matrix.size() != n * n) return false;
    layerNames_ = names;
    layerMatrix_ = matrix;
    numLayers_ = n;
    if (initialized_) rebuildLayerFilters();
    return true;
}

int PhysicsWorld::layerIndex(const std::string& name) const {
    for (int i = 0; i < (int)layerNames_.size(); ++i) {
        if (layerNames_[i] == name) return i;
    }
    return -1;
}

const std::string& PhysicsWorld::layerName(int idx) const {
    static const std::string empty;
    if (idx < 0 || idx >= (int)layerNames_.size()) return empty;
    return layerNames_[idx];
}

void PhysicsWorld::startThread() {
    if (physicsThread_.joinable()) return;
    physicsThread_ = std::thread([this]() { physicsThreadFunc(); });
}

void PhysicsWorld::physicsThreadFunc() {
    while (true) {
        shared_.state.wait(kPhysicsIdle, std::memory_order_acquire);

        uint32_t s = shared_.state.load(std::memory_order_acquire);
        if (s == kPhysicsShutdown) return;
        if (s != kPhysicsStep) continue;

        shared_.state.store(kPhysicsBusy, std::memory_order_release);

        physicsSystem_.Update(timeStep_, 1, tempAllocator_.get(), jobSystem_.get());

        shared_.state.store(kPhysicsDone, std::memory_order_release);
        shared_.state.notify_one();
    }
}

void PhysicsWorld::signalStep() {
    shared_.state.store(kPhysicsStep, std::memory_order_release);
    shared_.state.notify_one();
}

bool PhysicsWorld::consumeStep() {
    if (shared_.state.load(std::memory_order_acquire) != kPhysicsDone)
        return false;

    if (listener_) {
        contactsFront_ = listener_->drain();
    }
    checkBrokenConstraints();

    shared_.state.store(kPhysicsIdle, std::memory_order_release);
    return true;
}

bool PhysicsWorld::isIdle() const {
    return shared_.state.load(std::memory_order_acquire) == kPhysicsIdle;
}

void PhysicsWorld::stepInline() {
    if (!initialized_) return;
    physicsSystem_.Update(timeStep_, 1, tempAllocator_.get(), jobSystem_.get());
    if (listener_) {
        contactsFront_ = listener_->drain();
    }
    checkBrokenConstraints();
}

void PhysicsWorld::shutdown() {
    if (physicsThread_.joinable()) {
        shared_.state.store(kPhysicsShutdown, std::memory_order_release);
        shared_.state.notify_one();
        physicsThread_.join();
    }

    if (initialized_) {
        // Remove constraints first
        for (auto& [h, c] : constraints_) {
            if (c.ref) physicsSystem_.RemoveConstraint(c.ref.GetPtr());
            if (c.ref2) physicsSystem_.RemoveConstraint(c.ref2.GetPtr());
        }
        constraints_.clear();

        BodyInterface& bi = physicsSystem_.GetBodyInterface();
        BodyIDVector bodyIDs;
        physicsSystem_.GetBodies(bodyIDs);
        for (const BodyID& id : bodyIDs) {
            if (!id.IsInvalid() && bi.IsAdded(id)) {
                bi.RemoveBody(id);
                bi.DestroyBody(id);
            }
        }
    }

    tempAllocator_.reset();
    jobSystem_.reset();
    layers_.reset();
    initialized_ = false;
}

// --- Gravity ---

void PhysicsWorld::setGravity(float x, float y, float z) {
    physicsSystem_.SetGravity(Vec3(x, y, z));
}

Vec3 PhysicsWorld::gravity() const {
    return physicsSystem_.GetGravity();
}

// --- Body creation ---

static RefConst<Shape> buildShape(const BodyOptions& opts) {
    switch (opts.shape) {
        case BodyOptions::ShapeBox: {
            BoxShapeSettings s(opts.halfExtents);
            auto r = s.Create();
            return r.HasError() ? RefConst<Shape>() : r.Get();
        }
        case BodyOptions::ShapeSphere: {
            SphereShapeSettings s(opts.radius);
            auto r = s.Create();
            return r.HasError() ? RefConst<Shape>() : r.Get();
        }
        case BodyOptions::ShapeCapsule: {
            CapsuleShapeSettings s(opts.halfHeight, opts.radius);
            auto r = s.Create();
            return r.HasError() ? RefConst<Shape>() : r.Get();
        }
        case BodyOptions::ShapeCylinder: {
            CylinderShapeSettings s(opts.halfHeight, opts.radius);
            auto r = s.Create();
            return r.HasError() ? RefConst<Shape>() : r.Get();
        }
        case BodyOptions::ShapeConvexHull: {
            if (opts.hullPoints.size() < 4) return RefConst<Shape>();
            Array<Vec3> pts;
            pts.reserve(opts.hullPoints.size());
            for (auto& p : opts.hullPoints) pts.push_back(p);
            ConvexHullShapeSettings s(pts);
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
        case BodyOptions::ShapeCompound: {
            if (opts.compoundParts.empty()) return RefConst<Shape>();
            StaticCompoundShapeSettings s;
            for (auto& part : opts.compoundParts) {
                auto sub = buildShape(part);
                if (!sub) return RefConst<Shape>();
                s.AddShape(part.localPosition, part.localRotation, sub.GetPtr());
            }
            auto r = s.Create();
            return r.HasError() ? RefConst<Shape>() : r.Get();
        }
    }
    return RefConst<Shape>();
}

BodyID PhysicsWorld::createBody(const BodyOptions& opts) {
    auto shape = buildShape(opts);
    if (!shape) return BodyID();

    // Mesh / chain shapes can only be static.
    bool isStatic = opts.isStatic;
    if (opts.shape == BodyOptions::ShapeMesh || opts.shape == BodyOptions::ShapeChain) isStatic = true;

    int layer = opts.layer;
    if (layer < 0) layer = isStatic ? 0 : 1;
    if (layer >= numLayers_) layer = numLayers_ - 1;

    EMotionType motion = isStatic ? EMotionType::Static : EMotionType::Dynamic;

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
    if (!isStatic) {
        settings.mOverrideMassProperties = EOverrideMassProperties::CalculateMassAndInertia;
    }

    BodyInterface& bi = physicsSystem_.GetBodyInterface();
    return bi.CreateAndAddBody(settings,
        isStatic ? EActivation::DontActivate : EActivation::Activate);
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

void PhysicsWorld::destroyBody(BodyID id) {
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
            it = constraints_.erase(it);
        } else {
            ++it;
        }
    }

    BodyInterface& bi = physicsSystem_.GetBodyInterface();
    if (bi.IsAdded(id)) {
        bi.RemoveBody(id);
    }
    bi.DestroyBody(id);
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
}

void PhysicsWorld::setRotation(BodyID id, Quat rot) {
    physicsSystem_.GetBodyInterface().SetRotation(id, rot, EActivation::Activate);
}

void PhysicsWorld::setLinearVelocity(BodyID id, Vec3 vel) {
    physicsSystem_.GetBodyInterface().SetLinearVelocity(id, vel);
}

void PhysicsWorld::setAngularVelocity(BodyID id, Vec3 vel) {
    physicsSystem_.GetBodyInterface().SetAngularVelocity(id, vel);
}

void PhysicsWorld::addForce(BodyID id, Vec3 force) {
    physicsSystem_.GetBodyInterface().AddForce(id, force);
}

void PhysicsWorld::addImpulse(BodyID id, Vec3 impulse) {
    physicsSystem_.GetBodyInterface().AddImpulse(id, impulse);
}

void PhysicsWorld::addTorque(BodyID id, Vec3 torque) {
    physicsSystem_.GetBodyInterface().AddTorque(id, torque);
}

void PhysicsWorld::destroyAll(const std::function<void(JPH::BodyID)>& onBodyDestroyed) {
    if (!initialized_) return;

    // Constraints first (so removing bodies doesn't trip Jolt's constraint asserts).
    for (auto& [h, c] : constraints_) {
        if (c.ref) physicsSystem_.RemoveConstraint(c.ref.GetPtr());
        if (c.ref2) physicsSystem_.RemoveConstraint(c.ref2.GetPtr());
    }
    constraints_.clear();

    BodyInterface& bi = physicsSystem_.GetBodyInterface();
    BodyIDVector bodyIDs;
    physicsSystem_.GetBodies(bodyIDs);
    for (const BodyID& id : bodyIDs) {
        if (id.IsInvalid()) continue;
        if (onBodyDestroyed) onBodyDestroyed(id);
        if (bi.IsAdded(id)) bi.RemoveBody(id);
        bi.DestroyBody(id);
    }

    // Drop any pending contact events from this world.
    if (listener_) listener_->drain();
    contactsFront_.clear();
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
    EMotionType motion = isStatic ? EMotionType::Static : EMotionType::Dynamic;
    physicsSystem_.GetBodyInterface().SetMotionType(id, motion, EActivation::Activate);
    physicsSystem_.GetBodyInterface().SetObjectLayer(id, isStatic ? 0 : 1);
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

void PhysicsWorld::setUserData(BodyID id, uint64_t data) {
    physicsSystem_.GetBodyInterface().SetUserData(id, data);
}

uint64_t PhysicsWorld::getUserData(BodyID id) const {
    return physicsSystem_.GetBodyInterface().GetUserData(id);
}

// --- Constraints ---

uint32_t PhysicsWorld::createConstraint(const ConstraintOptions& opts) {
    auto& bi = physicsSystem_.GetBodyInterface();

    // Wheel = SixDOFConstraint configured Box2D-style: free translation along
    // the suspension axis (with optional soft limits via spring), free rotation
    // around the hinge axis (with optional motor); all other DOFs locked.
    // Slider+Hinge composition was tried and over-constrains (each constraint
    // fixes the DOF the other frees).
    if (opts.type == ConstraintOptions::Wheel) {
        Vec3 suspAxis  = opts.wheelSuspensionAxis.NormalizedOr(Vec3(0, 1, 0));
        Vec3 hingeAxis = opts.wheelHingeAxis.NormalizedOr(Vec3(0, 0, 1));
        // Build an orthonormal frame where local-Y = suspAxis, local-Z = hingeAxis.
        Vec3 axisX = suspAxis.Cross(hingeAxis).NormalizedOr(Vec3::sAxisX());
        Vec3 axisY = suspAxis;
        // If the user-supplied axes weren't perfectly perpendicular, re-derive Y.
        Vec3 axisZ = axisX.Cross(axisY).NormalizedOr(hingeAxis);
        axisY = axisZ.Cross(axisX).NormalizedOr(suspAxis);

        RVec3 hub = opts.point1;
        if (hub == RVec3::sZero())
            hub = bi.GetCenterOfMassPosition(opts.body2);

        auto* s = new SixDOFConstraintSettings();
        s->mSpace = EConstraintSpace::WorldSpace;
        s->mPosition1 = hub;
        s->mPosition2 = hub;
        s->mAxisX1 = axisX; s->mAxisY1 = axisY;
        s->mAxisX2 = axisX; s->mAxisY2 = axisY;

        using EAxis = SixDOFConstraintSettings::EAxis;
        // Lock everything by default, then free the two wheel DOFs.
        s->MakeFixedAxis(EAxis::TranslationX);
        s->MakeFixedAxis(EAxis::TranslationZ);
        s->MakeFixedAxis(EAxis::RotationX);
        s->MakeFixedAxis(EAxis::RotationY);
        // Suspension along local Y.
        if (opts.wheelHasTranslationLimits) {
            s->SetLimitedAxis(EAxis::TranslationY,
                              opts.wheelLowerTranslation,
                              opts.wheelUpperTranslation);
        } else {
            s->MakeFreeAxis(EAxis::TranslationY);
        }
        if (opts.wheelHertz > 0.0f) {
            s->mLimitsSpringSettings[EAxis::TranslationY].mMode = ESpringMode::FrequencyAndDamping;
            s->mLimitsSpringSettings[EAxis::TranslationY].mFrequency = opts.wheelHertz;
            s->mLimitsSpringSettings[EAxis::TranslationY].mDamping = opts.wheelDampingRatio;
        }
        // Wheel pin = rotation around local Z, always free.
        s->MakeFreeAxis(EAxis::RotationZ);
        if (opts.wheelEnableMotor) {
            s->mMotorSettings[EAxis::RotationZ].SetTorqueLimit(opts.wheelMaxMotorTorque);
        }
        Ref<TwoBodyConstraintSettings> sRef = s;

        TwoBodyConstraint* raw = bi.CreateConstraint(s, opts.body1, opts.body2);
        if (!raw) return 0;
        Ref<Constraint> c(raw);
        physicsSystem_.AddConstraint(c.GetPtr());

        if (opts.wheelEnableMotor) {
            auto* sd = static_cast<SixDOFConstraint*>(c.GetPtr());
            sd->SetMotorState(EAxis::RotationZ, EMotorState::Velocity);
            // Target angular velocity is in constraint space; only Z component matters.
            sd->SetTargetAngularVelocityCS(Vec3(0, 0, opts.wheelMotorSpeed));
        }

        uint32_t handle = nextConstraintHandle_++;
        constraints_[handle] = ConstraintEntry{c, nullptr, opts.breakingImpulse};
        return handle;
    }

    TwoBodyConstraintSettings* settings = nullptr;
    Ref<TwoBodyConstraintSettings> settingsRef;

    switch (opts.type) {
        case ConstraintOptions::Distance: {
            auto* s = new DistanceConstraintSettings();
            s->mPoint1 = opts.point1;
            s->mPoint2 = opts.point2;
            s->mSpace = EConstraintSpace::WorldSpace;
            if (opts.minDistance >= 0) s->mMinDistance = opts.minDistance;
            if (opts.maxDistance >= 0) s->mMaxDistance = opts.maxDistance;
            settings = s;
            break;
        }
        case ConstraintOptions::Point: {
            auto* s = new PointConstraintSettings();
            s->mPoint1 = opts.point1;
            s->mPoint2 = opts.point2;
            s->mSpace = EConstraintSpace::WorldSpace;
            settings = s;
            break;
        }
        case ConstraintOptions::Hinge: {
            auto* s = new HingeConstraintSettings();
            s->mPoint1 = opts.point1;
            s->mPoint2 = opts.point2;
            s->mHingeAxis1 = opts.axis;
            s->mHingeAxis2 = opts.axis;
            Vec3 normalAxis = std::abs(opts.axis.GetY()) < 0.9f
                ? opts.axis.Cross(Vec3(0,1,0)).Normalized()
                : opts.axis.Cross(Vec3(1,0,0)).Normalized();
            s->mNormalAxis1 = normalAxis;
            s->mNormalAxis2 = normalAxis;
            s->mSpace = EConstraintSpace::WorldSpace;
            if (opts.hasLimits) {
                s->mLimitsMin = opts.limitMin;
                s->mLimitsMax = opts.limitMax;
            }
            settings = s;
            break;
        }
        case ConstraintOptions::Fixed: {
            auto* s = new FixedConstraintSettings();
            s->mAutoDetectPoint = true;
            s->mSpace = EConstraintSpace::WorldSpace;
            settings = s;
            break;
        }
        case ConstraintOptions::Slider: {
            auto* s = new SliderConstraintSettings();
            s->mAutoDetectPoint = true;
            s->SetSliderAxis(opts.axis.NormalizedOr(Vec3(1,0,0)));
            s->mSpace = EConstraintSpace::WorldSpace;
            if (opts.hasLimits) {
                s->mLimitsMin = opts.limitMin;
                s->mLimitsMax = opts.limitMax;
            }
            settings = s;
            break;
        }
        case ConstraintOptions::Cone: {
            auto* s = new ConeConstraintSettings();
            s->mPoint1 = opts.point1;
            s->mPoint2 = opts.point2;
            s->mTwistAxis1 = opts.axis.NormalizedOr(Vec3::sAxisX());
            s->mTwistAxis2 = opts.axis.NormalizedOr(Vec3::sAxisX());
            s->mHalfConeAngle = opts.coneHalfAngle;
            s->mSpace = EConstraintSpace::WorldSpace;
            settings = s;
            break;
        }
        case ConstraintOptions::SwingTwist: {
            auto* s = new SwingTwistConstraintSettings();
            s->mPosition1 = opts.point1;
            s->mPosition2 = opts.point2;
            s->mTwistAxis1 = opts.axis.NormalizedOr(Vec3::sAxisX());
            s->mTwistAxis2 = opts.axis.NormalizedOr(Vec3::sAxisX());
            s->mPlaneAxis1 = opts.planeAxis.NormalizedOr(Vec3::sAxisY());
            s->mPlaneAxis2 = opts.planeAxis.NormalizedOr(Vec3::sAxisY());
            s->mNormalHalfConeAngle = opts.normalHalfConeAngle;
            s->mPlaneHalfConeAngle = opts.planeHalfConeAngle;
            s->mTwistMinAngle = opts.twistMinAngle;
            s->mTwistMaxAngle = opts.twistMaxAngle;
            s->mMaxFrictionTorque = opts.maxFrictionTorque;
            s->mSpace = EConstraintSpace::WorldSpace;
            settings = s;
            break;
        }
        case ConstraintOptions::Pulley: {
            auto* s = new PulleyConstraintSettings();
            s->mBodyPoint1 = opts.bodyPoint1;
            s->mFixedPoint1 = opts.fixedPoint1;
            s->mBodyPoint2 = opts.bodyPoint2;
            s->mFixedPoint2 = opts.fixedPoint2;
            s->mRatio = opts.ratio;
            s->mMinLength = opts.minLength;
            s->mMaxLength = opts.maxLength;
            s->mSpace = EConstraintSpace::WorldSpace;
            settings = s;
            break;
        }
        case ConstraintOptions::Gear: {
            auto* s = new GearConstraintSettings();
            s->mHingeAxis1 = opts.hingeAxis1.NormalizedOr(Vec3::sAxisX());
            s->mHingeAxis2 = opts.hingeAxis2.NormalizedOr(Vec3::sAxisX());
            s->mRatio = opts.ratio;
            s->mSpace = EConstraintSpace::WorldSpace;
            settings = s;
            break;
        }
        case ConstraintOptions::RackAndPinion: {
            auto* s = new RackAndPinionConstraintSettings();
            s->mHingeAxis = opts.hingeAxis1.NormalizedOr(Vec3::sAxisX());
            s->mSliderAxis = opts.hingeAxis2.NormalizedOr(Vec3::sAxisX());
            s->mRatio = opts.ratio;
            s->mSpace = EConstraintSpace::WorldSpace;
            settings = s;
            break;
        }
        case ConstraintOptions::Wheel:
            // handled above; unreachable here.
            return 0;
    }

    if (!settings) return 0;
    settingsRef = settings;  // take ownership via Ref

    TwoBodyConstraint* raw = bi.CreateConstraint(settings, opts.body1, opts.body2);
    if (!raw) return 0;
    Ref<Constraint> c(raw);

    // Gear / rack-and-pinion couple two *existing* constraints; wire them up from
    // the registry handles before adding to the system.
    if (opts.type == ConstraintOptions::Gear) {
        auto it1 = constraints_.find(opts.dependentConstraint1);
        auto it2 = constraints_.find(opts.dependentConstraint2);
        if (it1 == constraints_.end() || it2 == constraints_.end() ||
            !it1->second.ref || !it2->second.ref) {
            return 0;  // gear requires two valid existing hinge constraints
        }
        static_cast<GearConstraint*>(raw)->SetConstraints(
            it1->second.ref.GetPtr(), it2->second.ref.GetPtr());
    } else if (opts.type == ConstraintOptions::RackAndPinion) {
        auto it1 = constraints_.find(opts.dependentConstraint1);
        auto it2 = constraints_.find(opts.dependentConstraint2);
        if (it1 == constraints_.end() || it2 == constraints_.end() ||
            !it1->second.ref || !it2->second.ref) {
            return 0;  // requires a valid pinion hinge + rack slider
        }
        static_cast<RackAndPinionConstraint*>(raw)->SetConstraints(
            it1->second.ref.GetPtr(), it2->second.ref.GetPtr());
    }

    physicsSystem_.AddConstraint(c.GetPtr());

    uint32_t handle = nextConstraintHandle_++;
    constraints_[handle] = ConstraintEntry{c, nullptr, opts.breakingImpulse};
    return handle;
}

void PhysicsWorld::destroyConstraint(uint32_t handle) {
    auto it = constraints_.find(handle);
    if (it == constraints_.end()) return;
    if (it->second.ref) physicsSystem_.RemoveConstraint(it->second.ref.GetPtr());
    if (it->second.ref2) physicsSystem_.RemoveConstraint(it->second.ref2.GetPtr());
    constraints_.erase(it);
}

void PhysicsWorld::setConstraintEnabled(uint32_t handle, bool enabled) {
    auto it = constraints_.find(handle);
    if (it == constraints_.end() || !it->second.ref) return;
    it->second.ref->SetEnabled(enabled);
    if (it->second.ref2) it->second.ref2->SetEnabled(enabled);
}

void PhysicsWorld::setWheelMotor(uint32_t handle, bool enabled, float speed, float maxTorque) {
    auto it = constraints_.find(handle);
    if (it == constraints_.end() || !it->second.ref) return;
    if (it->second.ref->GetSubType() != EConstraintSubType::SixDOF) return;
    auto* sd = static_cast<SixDOFConstraint*>(it->second.ref.GetPtr());
    using EAxis = SixDOFConstraintSettings::EAxis;
    sd->SetMotorState(EAxis::RotationZ, enabled ? EMotorState::Velocity : EMotorState::Off);
    sd->SetTargetAngularVelocityCS(Vec3(0, 0, speed));
    sd->GetMotorSettings(EAxis::RotationZ).SetTorqueLimit(maxTorque);
}

void PhysicsWorld::setConstraintBreakingImpulse(uint32_t handle, float threshold) {
    auto it = constraints_.find(handle);
    if (it == constraints_.end()) return;
    it->second.breakingImpulse = threshold < 0.0f ? 0.0f : threshold;
}

float PhysicsWorld::getConstraintBreakingImpulse(uint32_t handle) const {
    auto it = constraints_.find(handle);
    return it == constraints_.end() ? 0.0f : it->second.breakingImpulse;
}

// Magnitude of the position-constraint impulse (N·s) applied last step. Jolt has
// no generic accessor (see Constraint.h ~150), so dispatch on the sub-type. This
// is the "force holding the bodies together" — the natural breaking metric.
static float constraintPositionImpulse(JPH::Constraint* c) {
    switch (c->GetSubType()) {
        case EConstraintSubType::Point:
            return static_cast<PointConstraint*>(c)->GetTotalLambdaPosition().Length();
        case EConstraintSubType::Distance:
            return std::abs(static_cast<DistanceConstraint*>(c)->GetTotalLambdaPosition());
        case EConstraintSubType::Fixed:
            return static_cast<FixedConstraint*>(c)->GetTotalLambdaPosition().Length();
        case EConstraintSubType::Hinge:
            return static_cast<HingeConstraint*>(c)->GetTotalLambdaPosition().Length();
        case EConstraintSubType::Slider: {
            auto v = static_cast<SliderConstraint*>(c)->GetTotalLambdaPosition();
            return std::sqrt(v[0] * v[0] + v[1] * v[1]);
        }
        case EConstraintSubType::Cone:
            return static_cast<ConeConstraint*>(c)->GetTotalLambdaPosition().Length();
        case EConstraintSubType::SwingTwist:
            return static_cast<SwingTwistConstraint*>(c)->GetTotalLambdaPosition().Length();
        case EConstraintSubType::Pulley:
            return std::abs(static_cast<PulleyConstraint*>(c)->GetTotalLambdaPosition());
        case EConstraintSubType::SixDOF:
            return static_cast<SixDOFConstraint*>(c)->GetTotalLambdaPosition().Length();
        default:
            return 0.0f;  // gear/rack/path/vehicle — no meaningful position impulse
    }
}

void PhysicsWorld::checkBrokenConstraints() {
    for (auto& [handle, entry] : constraints_) {
        if (entry.breakingImpulse <= 0.0f || !entry.ref) continue;
        if (!entry.ref->GetEnabled()) continue;
        if (constraintPositionImpulse(entry.ref.GetPtr()) > entry.breakingImpulse) {
            entry.ref->SetEnabled(false);
            if (entry.ref2) entry.ref2->SetEnabled(false);
            brokenConstraints_.push_back(handle);
        }
    }
}

std::vector<uint32_t> PhysicsWorld::drainBrokenConstraints() {
    std::vector<uint32_t> out;
    out.swap(brokenConstraints_);
    return out;
}

// --- Raycasts ---

std::vector<RayHit> PhysicsWorld::raycast(RVec3 origin, Vec3 direction,
                                          float maxDistance) const {
    RayCast ray(origin, direction * maxDistance);
    AllHitCollisionCollector<RayCastBodyCollector> collector;
    physicsSystem_.GetBroadPhaseQuery().CastRay(ray, collector);

    std::vector<RayHit> hits;
    for (auto& hit : collector.mHits) {
        RayHit h;
        h.bodyID = hit.mBodyID;
        h.fraction = hit.mFraction;
        h.position = origin + direction * (maxDistance * hit.mFraction);
        h.normal = Vec3(0, 1, 0);
        hits.push_back(h);
    }

    std::sort(hits.begin(), hits.end(),
              [](const RayHit& a, const RayHit& b) { return a.fraction < b.fraction; });
    return hits;
}

bool PhysicsWorld::raycastClosest(RVec3 origin, Vec3 direction,
                                  RayHit& outHit, float maxDistance) const {
    RayCast ray(origin, direction * maxDistance);
    ClosestHitCollisionCollector<RayCastBodyCollector> collector;
    physicsSystem_.GetBroadPhaseQuery().CastRay(ray, collector);

    if (!collector.HadHit()) return false;

    outHit.bodyID = collector.mHit.mBodyID;
    outHit.fraction = collector.mHit.mFraction;
    outHit.position = origin + direction * (maxDistance * collector.mHit.mFraction);
    outHit.normal = Vec3(0, 1, 0);
    return true;
}

// --- Contact events ---

std::vector<ContactEvent> PhysicsWorld::drainContactEvents() {
    std::vector<ContactEvent> out;
    out.swap(contactsFront_);
    return out;
}

} // namespace bro::physics
