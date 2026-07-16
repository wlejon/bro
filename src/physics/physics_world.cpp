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
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Jolt/Skeleton/Skeleton.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>

#include "util/log.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>

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
    ListenerImpl(size_t capacity, size_t maxBodies)
        : buffer(capacity), sensorFlags(maxBodies, 0) {}

    void OnContactAdded(const Body& b1, const Body& b2,
                        const ContactManifold&, ContactSettings&) override {
        push(ContactEvent::Added, b1.GetID(), b2.GetID(), b1.IsSensor() || b2.IsSensor());
    }

    void OnContactRemoved(const SubShapeIDPair& pair) override {
        // Jolt hands us only BodyIDs here, and the pair may be separating
        // *because* a body was just destroyed — so there is nothing safe to
        // look up. This used to report isSensor=false unconditionally, which
        // meant every sensor *exit* arrived mislabelled: an app could see a
        // trigger entered but never cleanly see it left. We keep our own
        // sensor bit per body index instead, written at create time.
        push(ContactEvent::Removed, pair.GetBody1ID(), pair.GetBody2ID(),
             isSensorId(pair.GetBody1ID()) || isSensorId(pair.GetBody2ID()));
    }

    // Read concurrently from Jolt's job threads during Update(); only ever
    // written from the main thread between steps (body creation), so no
    // synchronization is needed — the table is immutable for the duration of a
    // step. Indexed by BodyID index, which Jolt bounds by maxBodies.
    bool isSensorId(BodyID id) const {
        const uint32_t idx = id.GetIndex();
        return idx < sensorFlags.size() && sensorFlags[idx] != 0;
    }

    // Set at body creation. Deliberately NOT cleared on destruction: a body's
    // removal is exactly what produces the final OnContactRemoved for it, and
    // that event must still be able to see what the body was. The slot is
    // overwritten when Jolt reuses the index for a new body.
    void setSensorId(BodyID id, bool sensor) {
        const uint32_t idx = id.GetIndex();
        if (idx < sensorFlags.size()) sensorFlags[idx] = sensor ? 1 : 0;
    }

    // drain() is only called after physicsSystem_.Update() has returned (all
    // Jolt job threads synced back), so there are no concurrent writers left
    // to race against here — safe to read/reset buffer/writeIdx directly.
    std::vector<ContactEvent> drain() {
        size_t n = std::min(writeIdx.load(std::memory_order_relaxed), buffer.size());
        std::vector<ContactEvent> out(buffer.begin(), buffer.begin() + n);
        writeIdx.store(0, std::memory_order_relaxed);
        return out;
    }

    // Lock-free MPSC append: Jolt's job system invokes OnContact* concurrently
    // from multiple worker threads during Update(), so each caller claims a
    // disjoint slot via fetch_add rather than taking a lock. A slot beyond
    // capacity is dropped rather than risk an OOB write — capacity is sized
    // against maxBodies in PhysicsWorld::init().
    void push(ContactEvent::Type type, BodyID b1, BodyID b2, bool sensor) {
        size_t idx = writeIdx.fetch_add(1, std::memory_order_relaxed);
        if (idx >= buffer.size()) return;
        buffer[idx] = ContactEvent{type, b1, b2, sensor};
    }

    std::vector<ContactEvent> buffer;
    std::atomic<size_t> writeIdx{0};
    std::vector<uint8_t> sensorFlags; // by BodyID index; see isSensorId/setSensorId
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

    // Install per-world contact listener. Buffer capacity is sized generously
    // against maxBodies; see ListenerImpl::push for the overflow behavior.
    size_t contactCapacity = std::clamp<size_t>(static_cast<size_t>(maxBodies) * 4, 1024, 65536);
    listener_ = std::make_unique<ListenerImpl>(contactCapacity, static_cast<size_t>(maxBodies));
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
    std::unique_lock<std::mutex> lk(shared_.m);
    while (true) {
        shared_.cv.wait(lk, [this] {
            return shared_.shutdownRequested || shared_.state == kPhysicsStep;
        });
        if (shared_.shutdownRequested) return;

        shared_.state = kPhysicsBusy;
        lk.unlock();

        physicsSystem_.Update(timeStep_, 1, tempAllocator_.get(), jobSystem_.get());

        lk.lock();
        shared_.state = kPhysicsDone;
    }
}

void PhysicsWorld::signalStep() {
    // Characters step on this (main) thread while we still own the world —
    // the phase is Idle until the flip below, so the physics thread is parked.
    updateCharacters(timeStep_);
    {
        std::lock_guard<std::mutex> lk(shared_.m);
        shared_.state = kPhysicsStep;
    }
    shared_.cv.notify_one();
}

bool PhysicsWorld::consumeStep() {
    {
        std::lock_guard<std::mutex> lk(shared_.m);
        if (shared_.state != kPhysicsDone)
            return false;
    }
    // Done ⇒ the physics thread is parked in its wait; the world is ours.
    if (listener_) {
        contactsFront_ = listener_->drain();
    }
    checkBrokenConstraints();

    std::lock_guard<std::mutex> lk(shared_.m);
    shared_.state = kPhysicsIdle;
    return true;
}

bool PhysicsWorld::isIdle() const {
    std::lock_guard<std::mutex> lk(shared_.m);
    return shared_.state == kPhysicsIdle;
}

void PhysicsWorld::stepInline() {
    if (!initialized_) return;
    updateCharacters(timeStep_);
    physicsSystem_.Update(timeStep_, 1, tempAllocator_.get(), jobSystem_.get());
    if (listener_) {
        contactsFront_ = listener_->drain();
    }
    checkBrokenConstraints();
}

void PhysicsWorld::shutdown() {
    if (physicsThread_.joinable()) {
        {
            std::lock_guard<std::mutex> lk(shared_.m);
            shared_.shutdownRequested = true;
        }
        shared_.cv.notify_one();
        physicsThread_.join();
    }

    characters_.clear();

    if (initialized_) {
        // Vehicles first (each is both a constraint and a step listener).
        for (auto& [h, v] : vehicles_) removeVehicleFromSystem(v);
        vehicles_.clear();

        // Remove constraints first
        for (auto& [h, c] : constraints_) {
            if (c.ref) physicsSystem_.RemoveConstraint(c.ref.GetPtr());
            if (c.ref2) physicsSystem_.RemoveConstraint(c.ref2.GetPtr());
        }
        constraints_.clear();

        // Ragdolls before the generic sweep (~Ragdoll destroys its own bodies).
        for (auto& [h, r] : ragdolls_)
            if (r.ragdoll) r.ragdoll->RemoveFromPhysicsSystem();
        ragdolls_.clear();

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
            const uint32_t n = opts.heightSampleCount;
            // Jolt requires sampleCount / blockSize (default 2) >= 2.
            if (n < 4 || opts.heightSamples.size() != size_t(n) * n)
                return RefConst<Shape>();
            HeightFieldShapeSettings s(opts.heightSamples.data(), opts.heightOffset,
                                       opts.heightScale, n);
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

    // Mesh / chain / heightfield shapes can only be static.
    bool isStatic = opts.isStatic;
    if (opts.shape == BodyOptions::ShapeMesh || opts.shape == BodyOptions::ShapeChain ||
        opts.shape == BodyOptions::ShapeHeightField) isStatic = true;

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
    BodyID id = bi.CreateAndAddBody(settings,
        isStatic ? EActivation::DontActivate : EActivation::Activate);

    // Remember whether this body is a sensor. OnContactRemoved gets only a
    // BodyID — possibly of a body that no longer exists — so this table is the
    // only way it can label a sensor *exit* correctly.
    if (listener_ && !id.IsInvalid()) listener_->setSensorId(id, opts.isSensor);
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
            it = constraints_.erase(it);
        } else {
            ++it;
        }
    }
}

void PhysicsWorld::destroyBody(BodyID id) {
    // A ragdoll part? The ragdoll is one unit — destroy the whole thing
    // (removeRagdollFromSystem detaches, erasing the entry destroys ALL part
    // bodies via ~Ragdoll, including `id`, so return without the sweep below).
    for (auto it = ragdolls_.begin(); it != ragdolls_.end(); ++it) {
        const auto& ids = it->second.ragdoll->GetBodyIDs();
        if (std::find(ids.begin(), ids.end(), id) != ids.end()) {
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

    removeConstraintsReferencing(id);

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

    characters_.clear();

    // Vehicles first (each is both a constraint and a step listener).
    for (auto& [h, v] : vehicles_) removeVehicleFromSystem(v);
    vehicles_.clear();

    // Constraints next (so removing bodies doesn't trip Jolt's constraint asserts).
    for (auto& [h, c] : constraints_) {
        if (c.ref) physicsSystem_.RemoveConstraint(c.ref.GetPtr());
        if (c.ref2) physicsSystem_.RemoveConstraint(c.ref2.GetPtr());
    }
    constraints_.clear();

    // Ragdolls before the generic body sweep — ~Ragdoll destroys its part
    // bodies itself, and destroying them twice would corrupt the body manager.
    for (auto& [h, r] : ragdolls_) {
        if (!r.ragdoll) continue;
        if (onBodyDestroyed)
            for (const BodyID& id : r.ragdoll->GetBodyIDs()) onBodyDestroyed(id);
        r.ragdoll->RemoveFromPhysicsSystem();
    }
    ragdolls_.clear();

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
        case ConstraintOptions::SixDOF: {
            auto* s = new SixDOFConstraintSettings();
            s->mSpace = EConstraintSpace::WorldSpace;
            s->mPosition1 = opts.point1;
            s->mPosition2 = opts.point2;
            // Build an orthonormal constraint frame from the requested axes
            // (re-derive Y so imperfectly perpendicular input still works —
            // same treatment as the wheel composite above).
            Vec3 axisX = opts.sixDofAxisX.NormalizedOr(Vec3::sAxisX());
            Vec3 axisY = opts.sixDofAxisY.NormalizedOr(Vec3::sAxisY());
            Vec3 axisZ = axisX.Cross(axisY).NormalizedOr(Vec3::sAxisZ());
            axisY = axisZ.Cross(axisX).NormalizedOr(Vec3::sAxisY());
            s->mAxisX1 = axisX; s->mAxisY1 = axisY;
            s->mAxisX2 = axisX; s->mAxisY2 = axisY;
            s->mSwingType = opts.sixDofSwingPyramid ? ESwingType::Pyramid
                                                    : ESwingType::Cone;
            using EAxis = SixDOFConstraintSettings::EAxis;
            for (int i = 0; i < 6; i++) {
                const SixDofAxis& a = opts.sixDofAxes[i];
                auto axis = static_cast<EAxis>(i);
                switch (a.mode) {
                    case SixDofAxis::Locked:  s->MakeFixedAxis(axis); break;
                    case SixDofAxis::Free:    s->MakeFreeAxis(axis);  break;
                    case SixDofAxis::Limited: s->SetLimitedAxis(axis, a.min, a.max); break;
                }
                s->mMaxFriction[i] = a.maxFriction;
                // Soft (spring) limits exist only for the translation axes.
                if (i < EAxis::NumTranslation && a.springFrequency > 0.0f) {
                    s->mLimitsSpringSettings[i].mMode = ESpringMode::FrequencyAndDamping;
                    s->mLimitsSpringSettings[i].mFrequency = a.springFrequency;
                    s->mLimitsSpringSettings[i].mDamping = a.springDamping;
                }
            }
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

    // Create-time motors (hinge/slider: single entry; sixdof: one per axis).
    for (const MotorOptions& m : opts.motors)
        setConstraintMotor(handle, m);

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

// Apply the shared MotorSettings fields (spring for position motors, symmetric
// force/torque limits). Negative fields mean "leave unchanged".
static void applyMotorSettings(MotorSettings& ms, const MotorOptions& m) {
    if (m.frequency >= 0.0f) {
        ms.mSpringSettings.mMode = ESpringMode::FrequencyAndDamping;
        ms.mSpringSettings.mFrequency = m.frequency;
    }
    if (m.damping >= 0.0f) {
        ms.mSpringSettings.mMode = ESpringMode::FrequencyAndDamping;
        ms.mSpringSettings.mDamping = m.damping;
    }
    if (m.maxForce >= 0.0f)  ms.SetForceLimit(m.maxForce);
    if (m.maxTorque >= 0.0f) ms.SetTorqueLimit(m.maxTorque);
}

bool PhysicsWorld::setConstraintMotor(uint32_t handle, const MotorOptions& motor) {
    auto it = constraints_.find(handle);
    if (it == constraints_.end() || !it->second.ref) return false;
    Constraint* c = it->second.ref.GetPtr();

    EMotorState state = EMotorState::Off;
    if (motor.state == MotorOptions::Velocity) state = EMotorState::Velocity;
    else if (motor.state == MotorOptions::Position) state = EMotorState::Position;

    bool ok = false;
    switch (c->GetSubType()) {
        case EConstraintSubType::Hinge: {
            auto* h = static_cast<HingeConstraint*>(c);
            applyMotorSettings(h->GetMotorSettings(), motor);
            if (state == EMotorState::Velocity)      h->SetTargetAngularVelocity(motor.target);
            else if (state == EMotorState::Position) h->SetTargetAngle(motor.target);
            h->SetMotorState(state);
            ok = true;
            break;
        }
        case EConstraintSubType::Slider: {
            auto* s = static_cast<SliderConstraint*>(c);
            applyMotorSettings(s->GetMotorSettings(), motor);
            if (state == EMotorState::Velocity)      s->SetTargetVelocity(motor.target);
            else if (state == EMotorState::Position) s->SetTargetPosition(motor.target);
            s->SetMotorState(state);
            ok = true;
            break;
        }
        case EConstraintSubType::SixDOF: {
            // Covers both explicit sixdof constraints and wheel composites
            // (whose driven axis is RotationZ = 5).
            if (motor.axis < 0 || motor.axis > 5) return false;
            auto* sd = static_cast<SixDOFConstraint*>(c);
            using EAxis = SixDOFConstraintSettings::EAxis;
            auto axis = static_cast<EAxis>(motor.axis);
            applyMotorSettings(sd->GetMotorSettings(axis), motor);
            if (motor.axis < 3) {
                if (state == EMotorState::Velocity) {
                    Vec3 v = sd->GetTargetVelocityCS();
                    v.SetComponent(motor.axis, motor.target);
                    sd->SetTargetVelocityCS(v);
                } else if (state == EMotorState::Position) {
                    Vec3 p = sd->GetTargetPositionCS();
                    p.SetComponent(motor.axis, motor.target);
                    sd->SetTargetPositionCS(p);
                }
            } else {
                if (state == EMotorState::Velocity) {
                    Vec3 w = sd->GetTargetAngularVelocityCS();
                    w.SetComponent(motor.axis - 3, motor.target);
                    sd->SetTargetAngularVelocityCS(w);
                } else if (state == EMotorState::Position) {
                    float* t = it->second.sixDofRotTarget;
                    t[motor.axis - 3] = motor.target;
                    sd->SetTargetOrientationCS(Quat::sEulerAngles(Vec3(t[0], t[1], t[2])));
                }
            }
            sd->SetMotorState(axis, state);
            ok = true;
            break;
        }
        default:
            return false;  // no motor on this constraint type
    }

    // Wake both bodies so a motor change acts on sleeping islands.
    if (ok) {
        auto* tb = static_cast<TwoBodyConstraint*>(c);
        auto& bi = physicsSystem_.GetBodyInterface();
        Body* b1 = tb->GetBody1();
        Body* b2 = tb->GetBody2();
        if (b1 && !b1->IsStatic()) bi.ActivateBody(b1->GetID());
        if (b2 && !b2->IsStatic()) bi.ActivateBody(b2->GetID());
    }
    return ok;
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

// --- Character controllers ---

uint32_t PhysicsWorld::createCharacter(const CharacterOptions& opts) {
    if (!initialized_) return 0;
    Vec3 up = opts.up.NormalizedOr(Vec3::sAxisY());

    CapsuleShapeSettings capsule(opts.halfHeight, opts.radius);
    auto shape = capsule.Create();
    if (shape.HasError()) return 0;

    Ref<CharacterVirtualSettings> settings = new CharacterVirtualSettings();
    settings->mShape = shape.Get();
    settings->mUp = up;
    settings->mMass = opts.mass;
    settings->mMaxSlopeAngle = DegreesToRadians(opts.maxSlopeAngle);
    settings->mMaxStrength = opts.maxStrength;
    settings->mCharacterPadding = opts.padding;
    // Only contacts on the bottom sphere cap can support the character —
    // without this, side contacts on the cylinder count as "ground".
    settings->mSupportingVolume = Plane(up, -opts.radius);

    CharacterEntry entry;
    entry.character = new CharacterVirtual(settings, opts.position,
                                           Quat::sIdentity(), 0, &physicsSystem_);
    entry.stepUp = opts.stepUp;
    entry.stickToFloor = opts.stickToFloor;
    int layer = opts.layer;
    if (layer < 0 || layer >= numLayers_) layer = numLayers_ > 1 ? 1 : 0;
    entry.layer = layer;

    uint32_t handle = nextCharacterHandle_++;
    characters_[handle] = std::move(entry);
    return handle;
}

void PhysicsWorld::destroyCharacter(uint32_t handle) {
    characters_.erase(handle);
}

void PhysicsWorld::setCharacterVelocity(uint32_t handle, Vec3 v) {
    auto it = characters_.find(handle);
    if (it != characters_.end()) it->second.desiredVelocity = v;
}

void PhysicsWorld::setCharacterPosition(uint32_t handle, RVec3 pos) {
    auto it = characters_.find(handle);
    if (it != characters_.end()) it->second.character->SetPosition(pos);
}

bool PhysicsWorld::getCharacterState(uint32_t handle, CharacterState& out) const {
    auto it = characters_.find(handle);
    if (it == characters_.end()) return false;
    const CharacterVirtual* c = it->second.character.GetPtr();
    out.position = c->GetPosition();
    out.velocity = c->GetLinearVelocity();
    switch (c->GetGroundState()) {
        case CharacterBase::EGroundState::OnGround:      out.ground = CharacterGround::OnGround; break;
        case CharacterBase::EGroundState::OnSteepGround: out.ground = CharacterGround::OnSteepGround; break;
        case CharacterBase::EGroundState::NotSupported:  out.ground = CharacterGround::NotSupported; break;
        case CharacterBase::EGroundState::InAir:         out.ground = CharacterGround::InAir; break;
    }
    out.groundNormal = c->GetGroundNormal();
    out.groundVelocity = c->GetGroundVelocity();
    out.groundBody = c->GetGroundBodyID();
    return true;
}

void PhysicsWorld::updateCharacters(float dt) {
    if (characters_.empty() || dt <= 0.0f) return;
    const Vec3 gravity = physicsSystem_.GetGravity();

    for (auto& [handle, ch] : characters_) {
        CharacterVirtual* c = ch.character.GetPtr();
        // The ground body may have moved/changed velocity last world step.
        c->UpdateGroundVelocity();

        const Vec3 up = c->GetUp();
        const Vec3 desired = ch.desiredVelocity;
        const Vec3 desiredHorizontal = desired - desired.Dot(up) * up;
        const Vec3 groundVelocity = c->GetGroundVelocity();
        const float currentUp = c->GetLinearVelocity().Dot(up);

        Vec3 newVelocity;
        const bool movingTowardsGround = (currentUp - groundVelocity.Dot(up)) < 0.1f;
        if (c->GetGroundState() == CharacterBase::EGroundState::OnGround && movingTowardsGround) {
            // Supported: ride the ground (moving-platform carry) plus the
            // app's desired velocity; a positive up component launches a jump.
            newVelocity = groundVelocity + desired;
        } else {
            // Unsupported (in air / too-steep slope) or already moving away
            // from the ground: keep vertical momentum, integrate gravity, and
            // steer horizontally only — the desired up component is ignored so
            // holding "jump" can't fly.
            newVelocity = currentUp * up + gravity * dt + desiredHorizontal;
        }
        c->SetLinearVelocity(newVelocity);

        CharacterVirtual::ExtendedUpdateSettings settings;
        settings.mWalkStairsStepUp = up * ch.stepUp;
        settings.mStickToFloorStepDown = -up * ch.stickToFloor;
        c->ExtendedUpdate(dt, gravity, settings,
                          physicsSystem_.GetDefaultBroadPhaseLayerFilter(ObjectLayer(ch.layer)),
                          physicsSystem_.GetDefaultLayerFilter(ObjectLayer(ch.layer)),
                          {}, {}, *tempAllocator_);
    }
}

// --- Wheeled vehicles ---
//
// A Jolt VehicleConstraint is both a Constraint and a PhysicsStepListener:
// the constraint solves suspension/traction impulses, the step listener runs
// the engine/transmission/wheel-collision update at the start of every
// PhysicsSystem::Update. Both registrations are added and removed together
// (removeVehicleFromSystem) so the listener can never dangle. Because the
// stepping happens inside Update, vehicles need no per-step code on our side
// and the phase-ownership contract holds by construction.

uint32_t PhysicsWorld::createVehicle(const VehicleOptions& opts) {
    if (!initialized_ || opts.wheels.empty()) return 0;

    VehicleConstraintSettings settings;
    settings.mUp = opts.up.NormalizedOr(Vec3::sAxisY());
    settings.mForward = opts.forward.NormalizedOr(Vec3::sAxisZ());
    settings.mMaxPitchRollAngle =
        DegreesToRadians(std::clamp(opts.maxPitchRollAngle, 0.0f, 180.0f));

    float minWidth = FLT_MAX;
    for (const VehicleWheelOptions& w : opts.wheels) {
        auto* ws = new WheelSettingsWV();
        ws->mPosition = w.position;
        ws->mSuspensionDirection = w.suspensionDirection.NormalizedOr(Vec3(0, -1, 0));
        ws->mSteeringAxis = -ws->mSuspensionDirection;
        ws->mRadius = w.radius;
        ws->mWidth = w.width;
        ws->mSuspensionMinLength = w.suspensionMinLength;
        ws->mSuspensionMaxLength = std::max(w.suspensionMaxLength, w.suspensionMinLength);
        ws->mSuspensionSpring.mMode = ESpringMode::FrequencyAndDamping;
        ws->mSuspensionSpring.mFrequency = w.suspensionFrequency;
        ws->mSuspensionSpring.mDamping = w.suspensionDamping;
        ws->mMaxSteerAngle = w.steerable ? DegreesToRadians(w.maxSteerAngle) : 0.0f;
        ws->mMaxBrakeTorque = w.maxBrakeTorque;
        ws->mMaxHandBrakeTorque = w.maxHandBrakeTorque;
        settings.mWheels.push_back(ws);
        minWidth = std::min(minWidth, w.width);
    }

    for (const VehicleAntiRollBarOptions& ar : opts.antiRollBars) {
        const int n = (int)opts.wheels.size();
        if (ar.leftWheel < 0 || ar.leftWheel >= n ||
            ar.rightWheel < 0 || ar.rightWheel >= n) return 0;
        VehicleAntiRollBar bar;
        bar.mLeftWheel = ar.leftWheel;
        bar.mRightWheel = ar.rightWheel;
        bar.mStiffness = ar.stiffness;
        settings.mAntiRollBars.push_back(bar);
    }

    auto* controller = new WheeledVehicleControllerSettings();
    settings.mController = controller;  // takes ownership via Ref
    controller->mEngine.mMaxTorque = opts.engine.maxTorque;
    controller->mEngine.mMinRPM = opts.engine.minRPM;
    controller->mEngine.mMaxRPM = std::max(opts.engine.maxRPM, opts.engine.minRPM);
    VehicleTransmissionSettings& tr = controller->mTransmission;
    tr.mMode = opts.transmission.manual ? ETransmissionMode::Manual
                                        : ETransmissionMode::Auto;
    if (!opts.transmission.gearRatios.empty())
        tr.mGearRatios.assign(opts.transmission.gearRatios.begin(),
                              opts.transmission.gearRatios.end());
    if (!opts.transmission.reverseGearRatios.empty())
        tr.mReverseGearRatios.assign(opts.transmission.reverseGearRatios.begin(),
                                     opts.transmission.reverseGearRatios.end());
    tr.mSwitchTime = opts.transmission.switchTime;
    tr.mClutchStrength = opts.transmission.clutchStrength;
    tr.mShiftUpRPM = opts.transmission.shiftUpRPM;
    tr.mShiftDownRPM = opts.transmission.shiftDownRPM;
    controller->mDifferentialLimitedSlipRatio = opts.differentialLimitedSlipRatio;

    // Differentials: explicit list, or auto-derived by pairing driven wheels
    // in array order with equal torque split.
    std::vector<VehicleDifferentialOptions> diffs = opts.differentials;
    if (diffs.empty()) {
        std::vector<int> driven;
        for (int i = 0; i < (int)opts.wheels.size(); ++i)
            if (opts.wheels[i].driven) driven.push_back(i);
        for (size_t i = 0; i < driven.size(); i += 2) {
            VehicleDifferentialOptions d;
            d.leftWheel = driven[i];
            d.rightWheel = i + 1 < driven.size() ? driven[i + 1] : -1;
            diffs.push_back(d);
        }
        for (auto& d : diffs) d.engineTorqueRatio = 1.0f / (float)diffs.size();
    }
    for (const VehicleDifferentialOptions& d : diffs) {
        const int n = (int)opts.wheels.size();
        if (d.leftWheel >= n || d.rightWheel >= n) return 0;
        VehicleDifferentialSettings ds;
        ds.mLeftWheel = d.leftWheel;
        ds.mRightWheel = d.rightWheel;
        ds.mDifferentialRatio = d.ratio;
        ds.mLeftRightSplit = d.leftRightSplit;
        ds.mLimitedSlipRatio = d.limitedSlipRatio;
        ds.mEngineTorqueRatio = d.engineTorqueRatio;
        controller->mDifferentials.push_back(ds);
    }

    // The constraint constructor needs the Body itself; the phase-idle
    // contract makes the write lock uncontended here.
    Ref<VehicleConstraint> constraint;
    int chassisLayer = 1;
    {
        BodyLockWrite lock(physicsSystem_.GetBodyLockInterface(), opts.body);
        if (!lock.Succeeded()) return 0;
        Body& body = lock.GetBody();
        if (!body.IsDynamic()) return 0;
        chassisLayer = (int)body.GetObjectLayer();
        constraint = new VehicleConstraint(body, settings);
    }

    // Wheel-vs-ground collision tester. The object layer decides what the
    // wheels can hit (via the normal layer-pair matrix); default is the
    // chassis layer so wheels see exactly what the body would collide with.
    int layer = opts.testerLayer;
    if (layer < 0 || layer >= numLayers_) layer = chassisLayer;
    Ref<VehicleCollisionTester> tester;
    switch (opts.tester) {
        case VehicleOptions::TesterRay:
            tester = new VehicleCollisionTesterRay(ObjectLayer(layer), settings.mUp);
            break;
        case VehicleOptions::TesterCastSphere:
            tester = new VehicleCollisionTesterCastSphere(
                ObjectLayer(layer), 0.5f * minWidth, settings.mUp);
            break;
        case VehicleOptions::TesterCastCylinder:
            tester = new VehicleCollisionTesterCastCylinder(ObjectLayer(layer));
            break;
    }
    constraint->SetVehicleCollisionTester(tester);

    physicsSystem_.AddConstraint(constraint.GetPtr());
    physicsSystem_.AddStepListener(constraint.GetPtr());
    physicsSystem_.GetBodyInterface().ActivateBody(opts.body);

    uint32_t handle = nextVehicleHandle_++;
    vehicles_[handle] = VehicleEntry{constraint, opts.body};
    return handle;
}

void PhysicsWorld::removeVehicleFromSystem(VehicleEntry& e) {
    if (!e.constraint) return;
    physicsSystem_.RemoveStepListener(e.constraint.GetPtr());
    physicsSystem_.RemoveConstraint(e.constraint.GetPtr());
}

void PhysicsWorld::destroyVehicle(uint32_t handle) {
    auto it = vehicles_.find(handle);
    if (it == vehicles_.end()) return;
    removeVehicleFromSystem(it->second);
    vehicles_.erase(it);
}

void PhysicsWorld::setVehicleInput(uint32_t handle, float forward, float right,
                                   float brake, float handBrake) {
    auto it = vehicles_.find(handle);
    if (it == vehicles_.end()) return;
    auto* ctrl = static_cast<WheeledVehicleController*>(it->second.constraint->GetController());
    ctrl->SetDriverInput(std::clamp(forward, -1.0f, 1.0f),
                         std::clamp(right, -1.0f, 1.0f),
                         std::clamp(brake, 0.0f, 1.0f),
                         std::clamp(handBrake, 0.0f, 1.0f));
    // Wake the chassis so input acts on a sleeping vehicle (constraint-motor
    // rule). Zero input doesn't wake — a parked car may sleep.
    if (forward != 0.0f || right != 0.0f || brake != 0.0f || handBrake != 0.0f)
        physicsSystem_.GetBodyInterface().ActivateBody(it->second.body);
}

void PhysicsWorld::setVehicleGear(uint32_t handle, int gear, float clutchFriction) {
    auto it = vehicles_.find(handle);
    if (it == vehicles_.end()) return;
    auto* ctrl = static_cast<WheeledVehicleController*>(it->second.constraint->GetController());
    ctrl->GetTransmission().Set(gear, std::clamp(clutchFriction, 0.0f, 1.0f));
    physicsSystem_.GetBodyInterface().ActivateBody(it->second.body);
}

int PhysicsWorld::vehicleWheelCount(uint32_t handle) const {
    auto it = vehicles_.find(handle);
    if (it == vehicles_.end()) return -1;
    return (int)it->second.constraint->GetWheels().size();
}

bool PhysicsWorld::getVehicleWheelState(uint32_t handle, int wheel,
                                        VehicleWheelState& out) const {
    auto it = vehicles_.find(handle);
    if (it == vehicles_.end()) return false;
    const VehicleConstraint* c = it->second.constraint.GetPtr();
    if (wheel < 0 || wheel >= (int)c->GetWheels().size()) return false;
    const Wheel* w = c->GetWheel((uint)wheel);

    // Local transform mapping a Y-axis-aligned unit cylinder onto the wheel
    // (Jolt sample convention: wheelRight = Y, wheelUp = X), in body space.
    Mat44 local = c->GetWheelLocalTransform((uint)wheel, Vec3::sAxisY(), Vec3::sAxisX());
    out.position = local.GetTranslation();
    out.rotation = local.GetQuaternion().Normalized();
    out.suspensionLength = w->GetSuspensionLength();
    out.steerAngle = w->GetSteerAngle();
    out.rotationAngle = w->GetRotationAngle();
    out.angularVelocity = w->GetAngularVelocity();
    out.contact = w->HasContact();
    if (out.contact) {
        out.contactBody = w->GetContactBodyID();
        out.contactNormal = w->GetContactNormal();
    } else {
        out.contactBody = BodyID();
        out.contactNormal = Vec3(0, 1, 0);
    }
    return true;
}

bool PhysicsWorld::getVehicleState(uint32_t handle, VehicleState& out) const {
    auto it = vehicles_.find(handle);
    if (it == vehicles_.end()) return false;
    const VehicleConstraint* c = it->second.constraint.GetPtr();
    const Body* body = c->GetVehicleBody();
    Vec3 worldForward = body->GetRotation() * c->GetLocalForward();
    out.speed = body->GetLinearVelocity().Dot(worldForward);
    const auto* ctrl = static_cast<const WheeledVehicleController*>(c->GetController());
    out.rpm = ctrl->GetEngine().GetCurrentRPM();
    out.gear = ctrl->GetTransmission().GetCurrentGear();
    out.isSwitchingGear = ctrl->GetTransmission().IsSwitchingGear();
    return true;
}

BodyID PhysicsWorld::vehicleBody(uint32_t handle) const {
    auto it = vehicles_.find(handle);
    return it == vehicles_.end() ? BodyID() : it->second.body;
}

// --- Ragdolls ---
//
// A Jolt Ragdoll bundles part bodies + inter-part constraints and owns their
// lifetime: RemoveFromPhysicsSystem detaches everything as a unit and the
// final Ref release destroys the bodies (~Ragdoll). The parts are ordinary
// dynamic bodies while alive — all body APIs work on them — but their
// constraints live inside the Ragdoll, never in constraints_, and the
// destroyAll/shutdown body sweeps must run AFTER the ragdoll registry is
// cleared so no body is destroyed twice.

static RefConst<Shape> buildRagdollPartShape(const RagdollPartOptions& p) {
    switch (p.shape) {
        case RagdollPartOptions::ShapeCapsule: {
            if (p.halfHeight <= 0.0f || p.radius <= 0.0f) return RefConst<Shape>();
            CapsuleShapeSettings s(p.halfHeight, p.radius);
            s.SetDensity(p.density);
            auto r = s.Create();
            return r.HasError() ? RefConst<Shape>() : r.Get();
        }
        case RagdollPartOptions::ShapeSphere: {
            if (p.radius <= 0.0f) return RefConst<Shape>();
            SphereShapeSettings s(p.radius);
            s.SetDensity(p.density);
            auto r = s.Create();
            return r.HasError() ? RefConst<Shape>() : r.Get();
        }
        case RagdollPartOptions::ShapeBox: {
            BoxShapeSettings s(p.halfExtents);
            s.SetDensity(p.density);
            auto r = s.Create();
            return r.HasError() ? RefConst<Shape>() : r.Get();
        }
    }
    return RefConst<Shape>();
}

uint32_t PhysicsWorld::createRagdoll(const RagdollOptions& opts) {
    if (!initialized_ || opts.parts.empty()) return 0;
    const int n = (int)opts.parts.size();

    // Jolt requires parents before children in the joint array.
    for (int i = 0; i < n; i++) {
        int p = opts.parts[i].parentIndex;
        if (p >= i || p < -1) return 0;
    }

    int layer = opts.layer;
    if (layer < 0 || layer >= numLayers_) layer = 1;

    const Quat worldRot = opts.rotation.Normalized();

    Ref<RagdollSettings> settings = new RagdollSettings();
    settings->mSkeleton = new Skeleton();
    settings->mParts.resize(n);

    Array<Mat44> bindModel(n);  // model-space bind matrices (overlap detection)

    for (int i = 0; i < n; i++) {
        const RagdollPartOptions& p = opts.parts[i];
        std::string name = p.name.empty() ? ("part" + std::to_string(i)) : p.name;
        settings->mSkeleton->AddJoint(name, p.parentIndex);

        auto shape = buildRagdollPartShape(p);
        if (!shape) return 0;

        const Quat bindRot = p.rotation.Normalized();
        bindModel[i] = Mat44::sRotationTranslation(bindRot, p.position);

        RagdollSettings::Part& part = settings->mParts[i];
        part.SetShape(shape.GetPtr());
        part.mPosition = opts.position + worldRot * p.position;
        part.mRotation = (worldRot * bindRot).Normalized();
        part.mMotionType = EMotionType::Dynamic;
        part.mObjectLayer = static_cast<ObjectLayer>(layer);
        part.mFriction = p.friction;
        part.mRestitution = p.restitution;
        part.mGravityFactor = opts.gravityFactor;
        part.mLinearDamping = opts.linearDamping;
        part.mAngularDamping = opts.angularDamping;
        if (p.mass > 0.0f) {
            // Clean mass override: keep the shape's inertia distribution,
            // scaled to the requested mass (no density hacks).
            part.mOverrideMassProperties = EOverrideMassProperties::CalculateInertia;
            part.mMassPropertiesOverride.mMass = p.mass;
        }

        if (p.parentIndex < 0) continue;
        const RagdollPartOptions& parent = opts.parts[p.parentIndex];

        // Joint pivot: explicit, or this part's bind position.
        Vec3 pivotModel = p.hasJointPoint ? p.jointPoint : p.position;
        RVec3 pivotWorld = opts.position + worldRot * pivotModel;

        if (p.joint == RagdollPartOptions::JointFixed) {
            auto* fs = new FixedConstraintSettings();
            fs->mSpace = EConstraintSpace::WorldSpace;
            fs->mAutoDetectPoint = true;
            part.mToParent = fs;
            continue;
        }

        // Twist axis: explicit, or parent→child bind direction (model space).
        Vec3 twistModel = p.hasTwistAxis
            ? p.twistAxis
            : (p.position - parent.position);
        twistModel = twistModel.NormalizedOr(Vec3::sAxisY());
        Vec3 twistWorld = (worldRot * twistModel).Normalized();

        // Plane axis: explicit (re-orthonormalized against twist), or any
        // perpendicular — SwingTwist requires plane ⟂ twist.
        Vec3 planeWorld;
        if (p.hasPlaneAxis) {
            Vec3 pw = worldRot * p.planeAxis;
            pw -= twistWorld * pw.Dot(twistWorld);
            planeWorld = pw.NormalizedOr(twistWorld.GetNormalizedPerpendicular());
        } else {
            planeWorld = twistWorld.GetNormalizedPerpendicular();
        }

        auto* ss = new SwingTwistConstraintSettings();
        ss->mSpace = EConstraintSpace::WorldSpace;
        ss->mPosition1 = pivotWorld;
        ss->mPosition2 = pivotWorld;
        ss->mTwistAxis1 = twistWorld;
        ss->mTwistAxis2 = twistWorld;
        ss->mPlaneAxis1 = planeWorld;
        ss->mPlaneAxis2 = planeWorld;
        constexpr float kPi = 3.14159265358979f;
        ss->mNormalHalfConeAngle = std::clamp(p.normalHalfConeAngle, 0.0f, kPi);
        ss->mPlaneHalfConeAngle = std::clamp(p.planeHalfConeAngle, 0.0f, kPi);
        ss->mTwistMinAngle = std::clamp(p.twistMinAngle, -kPi, kPi);
        ss->mTwistMaxAngle = std::clamp(std::max(p.twistMaxAngle, p.twistMinAngle), -kPi, kPi);
        ss->mMaxFrictionTorque = p.maxFrictionTorque;
        // Drive-motor springs (used when driveRagdollToPose powers the joint).
        auto configureMotor = [&](MotorSettings& m) {
            m.mSpringSettings.mMode = ESpringMode::FrequencyAndDamping;
            m.mSpringSettings.mFrequency = opts.motor.frequency;
            m.mSpringSettings.mDamping = opts.motor.damping;
            if (opts.motor.maxTorque >= 0.0f) m.SetTorqueLimit(opts.motor.maxTorque);
        };
        configureMotor(ss->mSwingMotorSettings);
        configureMotor(ss->mTwistMotorSettings);
        part.mToParent = ss;
    }

    // Clamp parent/child mass ratios + grow parent inertia so long chains
    // don't oscillate (Havok-style stabilization).
    if (opts.stabilize) settings->Stabilize();

    // Parent/child pairs never self-collide; the bind pose also disables any
    // part pairs that overlap at rest (e.g. pelvis vs spine capsules).
    settings->DisableParentChildCollisions(bindModel.data());
    settings->CalculateBodyIndexToConstraintIndex();

    Ragdoll* rag = settings->CreateRagdoll(nextRagdollGroup_++, 0, &physicsSystem_);
    if (!rag) return 0;
    Ref<Ragdoll> ragRef(rag);
    rag->AddToPhysicsSystem(opts.activate ? EActivation::Activate
                                          : EActivation::DontActivate);

    RagdollEntry entry;
    entry.ragdoll = ragRef;
    entry.settings = settings;
    entry.parentIndex.reserve(n);
    for (int i = 0; i < n; i++) entry.parentIndex.push_back(opts.parts[i].parentIndex);

    uint32_t handle = nextRagdollHandle_++;
    ragdolls_[handle] = std::move(entry);
    return handle;
}

void PhysicsWorld::removeRagdollFromSystem(RagdollEntry& e) {
    if (!e.ragdoll) return;
    // User constraints attached to part bodies (e.g. a grab) must go first —
    // ~Ragdoll destroys the bodies and a live constraint would dangle.
    for (const BodyID& id : e.ragdoll->GetBodyIDs())
        removeConstraintsReferencing(id);
    e.ragdoll->RemoveFromPhysicsSystem();
}

void PhysicsWorld::destroyRagdoll(uint32_t handle) {
    auto it = ragdolls_.find(handle);
    if (it == ragdolls_.end()) return;
    removeRagdollFromSystem(it->second);
    ragdolls_.erase(it);  // Ref release destroys the part bodies
}

int PhysicsWorld::ragdollPartCount(uint32_t handle) const {
    auto it = ragdolls_.find(handle);
    return it == ragdolls_.end() ? -1 : (int)it->second.ragdoll->GetBodyCount();
}

BodyID PhysicsWorld::ragdollPartBody(uint32_t handle, int part) const {
    auto it = ragdolls_.find(handle);
    if (it == ragdolls_.end()) return BodyID();
    if (part < 0 || part >= (int)it->second.ragdoll->GetBodyCount()) return BodyID();
    return it->second.ragdoll->GetBodyID(part);
}

int PhysicsWorld::ragdollPartParent(uint32_t handle, int part) const {
    auto it = ragdolls_.find(handle);
    if (it == ragdolls_.end()) return -1;
    if (part < 0 || part >= (int)it->second.parentIndex.size()) return -1;
    return it->second.parentIndex[part];
}

bool PhysicsWorld::getRagdollPose(uint32_t handle,
                                  std::vector<RagdollPartState>& out) const {
    auto it = ragdolls_.find(handle);
    if (it == ragdolls_.end()) return false;
    const auto& ids = it->second.ragdoll->GetBodyIDs();
    const BodyInterface& bi = physicsSystem_.GetBodyInterface();
    out.resize(ids.size());
    for (size_t i = 0; i < ids.size(); i++) {
        bi.GetPositionAndRotation(ids[i], out[i].position, out[i].rotation);
    }
    return true;
}

bool PhysicsWorld::setRagdollPose(uint32_t handle,
                                  const std::vector<RagdollPartState>& pose) {
    auto it = ragdolls_.find(handle);
    if (it == ragdolls_.end()) return false;
    const auto& ids = it->second.ragdoll->GetBodyIDs();
    if (pose.size() != ids.size()) return false;
    BodyInterface& bi = physicsSystem_.GetBodyInterface();
    for (size_t i = 0; i < ids.size(); i++) {
        bi.SetPositionAndRotation(ids[i], pose[i].position,
                                  pose[i].rotation.Normalized(),
                                  EActivation::DontActivate);
    }
    return true;
}

bool PhysicsWorld::driveRagdollToPose(uint32_t handle,
                                      const std::vector<RagdollPartState>& pose,
                                      const RagdollMotorOptions* motor) {
    auto it = ragdolls_.find(handle);
    if (it == ragdolls_.end()) return false;
    RagdollEntry& e = it->second;
    const int n = (int)e.ragdoll->GetBodyCount();
    if ((int)pose.size() != n) return false;

    // Jolt DriveToPoseUsingMotors semantics, fed from world-space part
    // transforms: the position-motor target of each swing-twist joint is the
    // child's rotation relative to its parent in the TARGET pose. The root is
    // not driven (no joint) — its position stays free.
    for (int i = 0; i < n; i++) {
        int ci = e.settings->GetConstraintIndexForBodyIndex(i);
        int parent = e.parentIndex[i];
        if (ci < 0 || parent < 0) continue;
        TwoBodyConstraint* c = e.ragdoll->GetConstraint(ci);
        if (c->GetSubType() != EConstraintSubType::SwingTwist) continue;
        auto* st = static_cast<SwingTwistConstraint*>(c);
        if (motor) {
            auto apply = [&](MotorSettings& m) {
                m.mSpringSettings.mMode = ESpringMode::FrequencyAndDamping;
                m.mSpringSettings.mFrequency = motor->frequency;
                m.mSpringSettings.mDamping = motor->damping;
                if (motor->maxTorque >= 0.0f) m.SetTorqueLimit(motor->maxTorque);
                else m.SetTorqueLimits(-FLT_MAX, FLT_MAX);
            };
            apply(st->GetSwingMotorSettings());
            apply(st->GetTwistMotorSettings());
        }
        Quat target =
            (pose[parent].rotation.Conjugated() * pose[i].rotation).Normalized();
        st->SetSwingMotorState(EMotorState::Position);
        st->SetTwistMotorState(EMotorState::Position);
        st->SetTargetOrientationBS(target);
    }
    e.ragdoll->Activate();
    return true;
}

bool PhysicsWorld::driveRagdollToPoseKinematic(uint32_t handle,
                                               const std::vector<RagdollPartState>& pose,
                                               float dt) {
    auto it = ragdolls_.find(handle);
    if (it == ragdolls_.end() || dt <= 0.0f) return false;
    const auto& ids = it->second.ragdoll->GetBodyIDs();
    if (pose.size() != ids.size()) return false;
    it->second.ragdoll->Activate();
    BodyInterface& bi = physicsSystem_.GetBodyInterface();
    for (size_t i = 0; i < ids.size(); i++) {
        bi.MoveKinematic(ids[i], pose[i].position, pose[i].rotation.Normalized(), dt);
    }
    return true;
}

void PhysicsWorld::stopRagdollDrive(uint32_t handle) {
    auto it = ragdolls_.find(handle);
    if (it == ragdolls_.end()) return;
    Ragdoll* rag = it->second.ragdoll.GetPtr();
    for (int ci = 0; ci < (int)rag->GetConstraintCount(); ci++) {
        TwoBodyConstraint* c = rag->GetConstraint(ci);
        if (c->GetSubType() != EConstraintSubType::SwingTwist) continue;
        auto* st = static_cast<SwingTwistConstraint*>(c);
        st->SetSwingMotorState(EMotorState::Off);
        st->SetTwistMotorState(EMotorState::Off);
    }
}

void PhysicsWorld::addRagdollImpulse(uint32_t handle, Vec3 impulse) {
    auto it = ragdolls_.find(handle);
    if (it == ragdolls_.end()) return;
    it->second.ragdoll->AddImpulse(impulse);
}

void PhysicsWorld::activateRagdoll(uint32_t handle) {
    auto it = ragdolls_.find(handle);
    if (it == ragdolls_.end()) return;
    it->second.ragdoll->Activate();
}

void PhysicsWorld::deactivateRagdoll(uint32_t handle) {
    auto it = ragdolls_.find(handle);
    if (it == ragdolls_.end()) return;
    BodyInterface& bi = physicsSystem_.GetBodyInterface();
    for (const BodyID& id : it->second.ragdoll->GetBodyIDs())
        bi.DeactivateBody(id);
}

bool PhysicsWorld::isRagdollActive(uint32_t handle) const {
    auto it = ragdolls_.find(handle);
    if (it == ragdolls_.end()) return false;
    return it->second.ragdoll->IsActive();
}

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

std::vector<RayHit> PhysicsWorld::raycast(RVec3 origin, Vec3 direction,
                                          float maxDistance,
                                          const QueryFilter& filter) const {
    RRayCast ray(origin, direction * maxDistance);
    RayCastSettings settings;
    MaskObjectLayerFilter layerFilter(filter.layerMask);
    IgnoreSingleBodyFilter bodyFilter(filter.ignoreBody);
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
    RRayCast ray(origin, direction * maxDistance);
    MaskObjectLayerFilter layerFilter(filter.layerMask);
    IgnoreSingleBodyFilter bodyFilter(filter.ignoreBody);
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
    auto shape = buildShape(shapeOpts);
    if (!shape || shape->GetType() != EShapeType::Convex) return hits;

    RShapeCast cast = RShapeCast::sFromWorldTransform(
        shape.GetPtr(), Vec3::sOne(),
        RMat44::sRotationTranslation(shapeOpts.rotation, shapeOpts.position),
        direction * maxDistance);
    ShapeCastSettings settings;
    MaskObjectLayerFilter layerFilter(filter.layerMask);
    IgnoreSingleBodyFilter bodyFilter(filter.ignoreBody);
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
    auto shape = buildShape(shapeOpts);
    if (!shape || shape->GetType() != EShapeType::Convex) return false;

    RShapeCast cast = RShapeCast::sFromWorldTransform(
        shape.GetPtr(), Vec3::sOne(),
        RMat44::sRotationTranslation(shapeOpts.rotation, shapeOpts.position),
        direction * maxDistance);
    ShapeCastSettings settings;
    MaskObjectLayerFilter layerFilter(filter.layerMask);
    IgnoreSingleBodyFilter bodyFilter(filter.ignoreBody);
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
    auto shape = buildShape(shapeOpts);
    if (!shape || shape->GetType() != EShapeType::Convex) return hits;

    // CollideShape wants the center-of-mass transform, not the body transform.
    RMat44 com = RMat44::sRotationTranslation(shapeOpts.rotation, shapeOpts.position)
                     .PreTranslated(shape->GetCenterOfMass());
    CollideShapeSettings settings;
    MaskObjectLayerFilter layerFilter(filter.layerMask);
    IgnoreSingleBodyFilter bodyFilter(filter.ignoreBody);
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
    MaskObjectLayerFilter layerFilter(filter.layerMask);
    IgnoreSingleBodyFilter bodyFilter(filter.ignoreBody);
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
    // Phase-idle contract: the caller owns the world, so the NoLock variant
    // is safe (matches the other main-thread readers, e.g. getAllTransforms).
    const BodyLockInterfaceNoLock& li = physicsSystem_.GetBodyLockInterfaceNoLock();
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
    // Phase-idle contract: same as collectStaticBodies above.
    const BodyLockInterfaceNoLock& li = physicsSystem_.GetBodyLockInterfaceNoLock();

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

// --- Contact events ---

std::vector<ContactEvent> PhysicsWorld::drainContactEvents() {
    std::vector<ContactEvent> out;
    out.swap(contactsFront_);
    return out;
}

} // namespace bro::physics
