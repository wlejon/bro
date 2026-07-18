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
#include <Jolt/Physics/Collision/GroupFilter.h>
#include <Jolt/Physics/Collision/CollisionGroup.h>
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
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Jolt/Skeleton/Skeleton.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>
#include <Jolt/Physics/Collision/EstimateCollisionResponse.h>

#include "util/log.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <unordered_set>

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

// --- Constraint collideConnected pair filter ---
//
// Backs ConstraintOptions::collideConnected = false (the default): the two
// bodies joined by a constraint should not collide with each other. Jolt's
// GroupFilterTable wants one group of enumerated subgroups, which doesn't fit
// two arbitrary bodies, so this is a set of disabled body pairs instead. All
// participating bodies share one distinctive GroupID and use their
// index+sequence number as the SubGroupID (sequence bits make an id-reuse
// false positive practically impossible); any pair not in the set collides
// normally, and any group from another filter scheme (ragdoll
// GroupFilterTables use small sequential GroupIDs) passes through untouched.
//
// The set is only mutated on the phase-owning thread (create/destroy
// constraint, idle-only API) and read during Update() — same immutable-during-
// step contract as the rest of the world, so no locking.
class PairGroupFilter : public GroupFilter {
public:
    // Distinct from ragdoll groups (sequential from 1) and cInvalidGroup.
    static constexpr CollisionGroup::GroupID kGroupID = 0xb420b0d1u;

    static uint64_t pairKey(uint32_t a, uint32_t b) {
        if (a > b) std::swap(a, b);
        return (static_cast<uint64_t>(a) << 32) | b;
    }

    bool CanCollide(const CollisionGroup& g1, const CollisionGroup& g2) const override {
        if (g1.GetGroupID() != kGroupID || g2.GetGroupID() != kGroupID) return true;
        return disabled_.find(pairKey(g1.GetSubGroupID(), g2.GetSubGroupID()))
               == disabled_.end();
    }

    // Refcounted: two constraints on the same body pair each hold the pair
    // disabled; it re-enables only when the last one goes away.
    void disablePair(uint64_t key) { disabled_[key]++; }
    void enablePair(uint64_t key) {
        auto it = disabled_.find(key);
        if (it != disabled_.end() && --it->second <= 0) disabled_.erase(it);
    }
    void clear() { disabled_.clear(); }

private:
    std::unordered_map<uint64_t, int> disabled_;
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
        : buffer(capacity), sensorFlags(maxBodies, 0),
          frictionModes(maxBodies, 0), restitutionModes(maxBodies, 0) {}

    void OnContactAdded(const Body& b1, const Body& b2,
                        const ContactManifold& manifold,
                        ContactSettings& ioSettings) override {
        applyCombine(b1, b2, ioSettings);

        ContactEvent e;
        e.type = ContactEvent::Added;
        e.body1 = b1.GetID();
        e.body2 = b2.GetID();
        e.isSensor = b1.IsSensor() || b2.IsSensor();
        // Manifold snapshot: world-space contact points (on body2's surface,
        // capped at kMaxPoints), normal, penetration depth. Bounded copies
        // only — this record crosses the lock-free ring.
        uint n = std::min<uint>(manifold.mRelativeContactPointsOn2.size(),
                                (uint)ContactEvent::kMaxPoints);
        e.numPoints = (uint8_t)n;
        for (uint i = 0; i < n; i++) {
            RVec3 p = manifold.GetWorldSpaceContactPointOn2(i);
            e.points[i] = Float3((float)p.GetX(), (float)p.GetY(), (float)p.GetZ());
        }
        manifold.mWorldSpaceNormal.StoreFloat3(&e.normal);
        e.penetration = manifold.mPenetrationDepth;
        // Estimated impulse (Jolt's standard approach — the solver never hands
        // the solved impulses to the listener). Pre-solve estimate: exact for
        // an isolated two-body impact, approximate in a pile-up. Summed over
        // the manifold points into one scalar. Sensors get none (no response).
        // Runs on Jolt's job threads; EstimateCollisionResponse only reads the
        // two bodies it was handed, which the narrow phase already owns here.
        if (!e.isSensor) {
            CollisionEstimationResult est;
            EstimateCollisionResponse(b1, b2, manifold, est,
                                      ioSettings.mCombinedFriction,
                                      ioSettings.mCombinedRestitution);
            float sum = 0.0f;
            for (const auto& im : est.mImpulses) sum += im.mContactImpulse;
            e.impulse = sum;
        }
        push(e);
    }

    void OnContactPersisted(const Body& b1, const Body& b2,
                            const ContactManifold&,
                            ContactSettings& ioSettings) override {
        // No event (getContacts keeps begin/end semantics), but the combined
        // friction/restitution override must keep applying for the lifetime
        // of the contact — Jolt re-derives ContactSettings every step.
        applyCombine(b1, b2, ioSettings);
    }

    void OnContactRemoved(const SubShapeIDPair& pair) override {
        // Jolt hands us only BodyIDs here, and the pair may be separating
        // *because* a body was just destroyed — so there is nothing safe to
        // look up. This used to report isSensor=false unconditionally, which
        // meant every sensor *exit* arrived mislabelled: an app could see a
        // trigger entered but never cleanly see it left. We keep our own
        // sensor bit per body index instead, written at create time.
        ContactEvent e;
        e.type = ContactEvent::Removed;
        e.body1 = pair.GetBody1ID();
        e.body2 = pair.GetBody2ID();
        e.isSensor = isSensorId(pair.GetBody1ID()) || isSensorId(pair.GetBody2ID());
        push(e);
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
    // `overflowed` (when non-null) reports whether push() dropped any event
    // this step (writeIdx ran past capacity).
    std::vector<ContactEvent> drain(bool* overflowed = nullptr) {
        size_t claimed = writeIdx.load(std::memory_order_relaxed);
        if (overflowed) *overflowed = claimed > buffer.size();
        size_t n = std::min(claimed, buffer.size());
        std::vector<ContactEvent> out(buffer.begin(), buffer.begin() + n);
        writeIdx.store(0, std::memory_order_relaxed);
        return out;
    }

    // Lock-free MPSC append: Jolt's job system invokes OnContact* concurrently
    // from multiple worker threads during Update(), so each caller claims a
    // disjoint slot via fetch_add rather than taking a lock. A slot beyond
    // capacity is dropped rather than risk an OOB write — capacity is sized
    // against maxBodies in PhysicsWorld::init().
    void push(const ContactEvent& e) {
        size_t idx = writeIdx.fetch_add(1, std::memory_order_relaxed);
        if (idx >= buffer.size()) return;
        buffer[idx] = e;
    }

    // --- Per-body friction/restitution combine modes ---
    //
    // Same threading contract as sensorFlags: written only between steps on
    // the phase-owning thread (body creation / idle-only setters), read
    // concurrently from Jolt's job threads during Update() — immutable for
    // the duration of a step, so no synchronization. Indexed by BodyID index.

    void setCombineModes(BodyID id, CombineMode friction, CombineMode restitution) {
        const uint32_t idx = id.GetIndex();
        if (idx >= frictionModes.size()) return;
        frictionModes[idx] = (uint8_t)friction;
        restitutionModes[idx] = (uint8_t)restitution;
        if (friction != CombineMode::Default || restitution != CombineMode::Default)
            anyCombineModes = true;
    }

    static float combineValue(uint8_t mode, float a, float b) {
        switch ((CombineMode)mode) {
            case CombineMode::Average:  return 0.5f * (a + b);
            case CombineMode::Min:      return std::min(a, b);
            case CombineMode::Multiply: return a * b;
            case CombineMode::Max:      return std::max(a, b);
            case CombineMode::Default:  break;
        }
        return a;
    }

    // Override Jolt's combined friction/restitution when either body carries
    // a non-default mode. Precedence when the two disagree: the higher mode
    // wins (average < min < multiply < max — Unity's rule). Jolt's defaults
    // otherwise: friction sqrt(f1*f2), restitution max(r1, r2).
    void applyCombine(const Body& b1, const Body& b2, ContactSettings& io) const {
        if (!anyCombineModes) return;
        const uint32_t i1 = b1.GetID().GetIndex();
        const uint32_t i2 = b2.GetID().GetIndex();
        const uint8_t f1 = i1 < frictionModes.size() ? frictionModes[i1] : 0;
        const uint8_t f2 = i2 < frictionModes.size() ? frictionModes[i2] : 0;
        const uint8_t fm = std::max(f1, f2);
        if (fm != (uint8_t)CombineMode::Default)
            io.mCombinedFriction = combineValue(fm, b1.GetFriction(), b2.GetFriction());
        const uint8_t r1 = i1 < restitutionModes.size() ? restitutionModes[i1] : 0;
        const uint8_t r2 = i2 < restitutionModes.size() ? restitutionModes[i2] : 0;
        const uint8_t rm = std::max(r1, r2);
        if (rm != (uint8_t)CombineMode::Default)
            io.mCombinedRestitution = combineValue(rm, b1.GetRestitution(), b2.GetRestitution());
    }

    std::vector<ContactEvent> buffer;
    std::atomic<size_t> writeIdx{0};
    std::vector<uint8_t> sensorFlags; // by BodyID index; see isSensorId/setSensorId
    std::vector<uint8_t> frictionModes;    // by BodyID index; CombineMode values
    std::vector<uint8_t> restitutionModes; // by BodyID index; CombineMode values
    bool anyCombineModes = false;          // fast path: no mode ever set
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

bool PhysicsWorld::init(int maxBodies, int contactCapacity) {
    if (initialized_) return true;

    ensureJoltInit();

    tempAllocator_ = std::make_unique<TempAllocatorImpl>(10 * 1024 * 1024);
    jobSystem_ = std::make_unique<JobSystemThreadPool>(
        cMaxPhysicsJobs, cMaxPhysicsBarriers,
        std::max(1u, std::thread::hardware_concurrency() - 2));

    layers_ = std::make_unique<Layers>();
    pairFilter_ = new PairGroupFilter();

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

    // Install per-world contact listener. Buffer capacity defaults to a
    // generous multiple of maxBodies; an explicit contactCapacity overrides
    // it (tiny values are legitimate for overflow testing / memory-lean
    // sandboxes). See ListenerImpl::push for the overflow behavior.
    size_t capacity = contactCapacity > 0
        ? std::clamp<size_t>(static_cast<size_t>(contactCapacity), 16, 65536)
        : std::clamp<size_t>(static_cast<size_t>(maxBodies) * 4, 1024, 65536);
    listener_ = std::make_unique<ListenerImpl>(capacity, static_cast<size_t>(maxBodies));
    physicsSystem_.SetContactListener(listener_.get());

    // Characters collide with each other via Jolt's brute-force checker
    // (fine: updateCharacters is serial, and character counts are small).
    charVsChar_ = std::make_unique<CharacterVsCharacterCollisionSimple>();

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
    // Snapshot pre-step transforms first (characters move their inner bodies
    // during updateCharacters, and those should interpolate too), then step
    // characters — both on this (main) thread while we still own the world:
    // the phase is Idle until the flip below, so the physics thread is parked.
    capturePrevTransforms();
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
        bool overflowed = false;
        contactsFront_ = listener_->drain(&overflowed);
        contactsOverflowedFront_ = contactsOverflowedFront_ || overflowed;
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
    capturePrevTransforms();
    updateCharacters(timeStep_);
    physicsSystem_.Update(timeStep_, 1, tempAllocator_.get(), jobSystem_.get());
    if (listener_) {
        bool overflowed = false;
        contactsFront_ = listener_->drain(&overflowed);
        contactsOverflowedFront_ = contactsOverflowedFront_ || overflowed;
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

    if (charVsChar_) charVsChar_->mCharacters.clear();
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
        if (pairFilter_) pairFilter_->clear();

        // Ragdolls before the generic sweep (~Ragdoll destroys its own bodies).
        for (auto& [h, r] : ragdolls_)
            if (r.ragdoll) r.ragdoll->RemoveFromPhysicsSystem();
        ragdolls_.clear();

        // Soft-body registry entries don't own their bodies — the generic
        // sweep below destroys them; this only drops the settings Refs.
        softBodies_.clear();

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
    // only way it can label a sensor *exit* correctly. Combine modes live in
    // the same per-index tables and must be (re)written here too, or a reused
    // body index would inherit a destroyed body's modes.
    if (listener_ && !id.IsInvalid()) {
        listener_->setSensorId(id, opts.isSensor);
        listener_->setCombineModes(id, opts.frictionCombine, opts.restitutionCombine);
    }
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

    if (onBodyDestroyed) onBodyDestroyed(id);

    BodyInterface& bi = physicsSystem_.GetBodyInterface();
    if (bi.IsAdded(id)) {
        bi.RemoveBody(id);
    }
    bi.DestroyBody(id);
}

// --- Render interpolation ---

void PhysicsWorld::setInterpolation(bool enabled) {
    interpolate_ = enabled;
    if (!enabled) prevTransforms_.clear();
}

void PhysicsWorld::setRenderAlpha(float alpha) {
    renderAlpha_ = std::clamp(alpha, 0.0f, 1.0f);
}

// Rebuild the pre-step snapshot from the active-body list. Bodies that are
// asleep (or static) never enter the map, so they always render at their true
// pose — no jitter as alpha sweeps. Called at the start of every step, on the
// thread that owns the world (phase Idle), so the no-lock interface is safe.
void PhysicsWorld::capturePrevTransforms() {
    if (!interpolate_) {
        if (!prevTransforms_.empty()) prevTransforms_.clear();
        return;
    }
    prevTransforms_.clear();
    BodyIDVector active;
    physicsSystem_.GetActiveBodies(EBodyType::RigidBody, active);
    const BodyInterface& bi = physicsSystem_.GetBodyInterfaceNoLock();
    for (const BodyID& id : active) {
        RVec3 pos;
        Quat rot;
        bi.GetPositionAndRotation(id, pos, rot);
        prevTransforms_[id.GetIndexAndSequenceNumber()] = PrevTransform{pos, rot};
    }
}

void PhysicsWorld::getRenderTransform(BodyID id, RVec3& outPos, Quat& outRot) const {
    const BodyInterface& bi = physicsSystem_.GetBodyInterface();
    bi.GetPositionAndRotation(id, outPos, outRot);
    if (!interpolate_ || renderAlpha_ >= 1.0f) return;
    auto it = prevTransforms_.find(id.GetIndexAndSequenceNumber());
    if (it == prevTransforms_.end()) return;   // asleep / static / new — true pose
    const PrevTransform& prev = it->second;
    outPos = prev.pos + (outPos - prev.pos) * renderAlpha_;
    // Jolt's SLERP takes the short arc (flips sign on negative dot); normalize
    // the result so accumulated float error can't leak into scene rotations.
    outRot = prev.rot.SLERP(outRot, renderAlpha_).Normalized();
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

void PhysicsWorld::destroyAll(const std::function<void(JPH::BodyID)>& onBodyDestroyed) {
    if (!initialized_) return;

    // Characters first (their Ref release destroys any inner bodies BEFORE
    // the generic body sweep below, so nothing is destroyed twice); drop the
    // char-vs-char registrations with them.
    if (charVsChar_) charVsChar_->mCharacters.clear();
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
    if (pairFilter_) pairFilter_->clear();

    // Ragdolls before the generic body sweep — ~Ragdoll destroys its part
    // bodies itself, and destroying them twice would corrupt the body manager.
    for (auto& [h, r] : ragdolls_) {
        if (!r.ragdoll) continue;
        if (onBodyDestroyed)
            for (const BodyID& id : r.ragdoll->GetBodyIDs()) onBodyDestroyed(id);
        r.ragdoll->RemoveFromPhysicsSystem();
    }
    ragdolls_.clear();

    // Soft-body registry: metadata only, the generic sweep destroys the bodies.
    softBodies_.clear();

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
    contactsOverflowedFront_ = false;
    prevTransforms_.clear();
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

void PhysicsWorld::setUserData(BodyID id, uint64_t data) {
    physicsSystem_.GetBodyInterface().SetUserData(id, data);
}

uint64_t PhysicsWorld::getUserData(BodyID id) const {
    return physicsSystem_.GetBodyInterface().GetUserData(id);
}

// --- Constraints ---

// collideConnected = false (the default, matching the documented behavior and
// typical engine defaults — Jolt's own default is that constrained bodies DO
// collide): register the pair in pairFilter_ so the narrow phase skips it,
// and stamp both bodies into the pair group. A body already carrying a
// FOREIGN group filter (ragdoll parts use a per-ragdoll GroupFilterTable)
// can't participate — overwriting its group would break that scheme — so the
// pair is skipped with a warning and those two bodies keep colliding.
void PhysicsWorld::applyCollideConnected(const ConstraintOptions& opts,
                                         ConstraintEntry& entry) {
    if (opts.collideConnected) return;   // pair collides — nothing to set up
    if (opts.body1.IsInvalid() || opts.body2.IsInvalid()) return;  // world anchor
    if (opts.body1 == opts.body2) return;

    const BodyLockInterface& li = physicsSystem_.GetBodyLockInterface();
    for (BodyID id : {opts.body1, opts.body2}) {
        BodyLockRead lock(li, id);
        if (!lock.Succeeded()) return;
        const GroupFilter* f = lock.GetBody().GetCollisionGroup().GetGroupFilter();
        if (f && f != pairFilter_.GetPtr()) {
            LOG_WARN("createConstraint: collideConnected=false ignored — a body "
                     "already uses another collision-group filter (e.g. a ragdoll part)");
            return;
        }
    }
    for (BodyID id : {opts.body1, opts.body2}) {
        BodyLockWrite lock(li, id);
        if (!lock.Succeeded()) return;
        lock.GetBody().SetCollisionGroup(CollisionGroup(
            pairFilter_.GetPtr(), PairGroupFilter::kGroupID,
            id.GetIndexAndSequenceNumber()));
    }
    entry.pairKey = PairGroupFilter::pairKey(
        opts.body1.GetIndexAndSequenceNumber(),
        opts.body2.GetIndexAndSequenceNumber());
    entry.hasPair = true;
    pairFilter_->disablePair(entry.pairKey);
}

void PhysicsWorld::evictConstraintPair(const ConstraintEntry& entry) {
    if (entry.hasPair && pairFilter_) pairFilter_->enablePair(entry.pairKey);
}

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
        ConstraintEntry entry{c, nullptr, opts.breakingImpulse};
        applyCollideConnected(opts, entry);
        constraints_[handle] = std::move(entry);
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
    ConstraintEntry entry{c, nullptr, opts.breakingImpulse};
    applyCollideConnected(opts, entry);
    constraints_[handle] = std::move(entry);

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
    evictConstraintPair(it->second);
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

    int layer = opts.layer;
    if (layer < 0 || layer >= numLayers_) layer = numLayers_ > 1 ? 1 : 0;

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
    if (opts.innerBody) {
        // Kinematic body that shadows the character so sensors, raycasts and
        // ordinary bodies can see it (a bare CharacterVirtual never enters
        // the broadphase). Created/destroyed by the CharacterVirtual itself.
        int innerLayer = opts.innerBodyLayer;
        if (innerLayer < 0 || innerLayer >= numLayers_) innerLayer = layer;
        settings->mInnerBodyShape = shape.Get();
        settings->mInnerBodyLayer = static_cast<ObjectLayer>(innerLayer);
    }

    CharacterEntry entry;
    entry.character = new CharacterVirtual(settings, opts.position,
                                           Quat::sIdentity(), 0, &physicsSystem_);
    entry.stepUp = opts.stepUp;
    entry.stickToFloor = opts.stickToFloor;
    entry.layer = layer;

    // Characters collide with each other (instead of ghosting through).
    entry.character->SetCharacterVsCharacterCollision(charVsChar_.get());
    charVsChar_->Add(entry.character.GetPtr());

    // The inner body reuses a body-manager index — reset the per-index
    // listener tables so it can't inherit a destroyed body's flags.
    BodyID innerId = entry.character->GetInnerBodyID();
    if (listener_ && !innerId.IsInvalid()) {
        listener_->setSensorId(innerId, false);
        listener_->setCombineModes(innerId, CombineMode::Default, CombineMode::Default);
    }

    uint32_t handle = nextCharacterHandle_++;
    characters_[handle] = std::move(entry);
    return handle;
}

void PhysicsWorld::destroyCharacter(uint32_t handle) {
    auto it = characters_.find(handle);
    if (it == characters_.end()) return;
    if (charVsChar_ && it->second.character)
        charVsChar_->Remove(it->second.character.GetPtr());
    characters_.erase(it);   // Ref release destroys the inner body (if any)
}

void PhysicsWorld::setCharacterVelocity(uint32_t handle, Vec3 v) {
    auto it = characters_.find(handle);
    if (it != characters_.end()) it->second.desiredVelocity = v;
}

void PhysicsWorld::setCharacterPosition(uint32_t handle, RVec3 pos) {
    auto it = characters_.find(handle);
    if (it != characters_.end()) it->second.character->SetPosition(pos);
}

bool PhysicsWorld::setCharacterShape(uint32_t handle, const BodyOptions& shapeOpts) {
    auto it = characters_.find(handle);
    if (it == characters_.end() || !it->second.character) return false;
    // Mesh-family shapes can't be a character volume.
    if (shapeOpts.shape == BodyOptions::ShapeMesh ||
        shapeOpts.shape == BodyOptions::ShapeChain ||
        shapeOpts.shape == BodyOptions::ShapeHeightField) return false;
    auto shape = buildShape(shapeOpts);
    if (!shape) return false;

    CharacterVirtual* c = it->second.character.GetPtr();
    const ObjectLayer layer(static_cast<ObjectLayer>(it->second.layer));

    // Feet-planted stance change: the character's position is the shape
    // CENTER, so swapping shapes in place would sink a taller shape into the
    // floor (making "stand up" always fail) and float a shorter one. Shift
    // the position along `up` so the new shape's bottom lands where the old
    // one's was, try the switch there, and restore on refusal.
    const Vec3 up = c->GetUp();
    auto bottomAlongUp = [&](const Shape* s) {
        const AABox b = s->GetLocalBounds();
        // Support point along -up: min over box corners of dot(corner, up).
        const Vec3 corner(up.GetX() >= 0.0f ? b.mMin.GetX() : b.mMax.GetX(),
                          up.GetY() >= 0.0f ? b.mMin.GetY() : b.mMax.GetY(),
                          up.GetZ() >= 0.0f ? b.mMin.GetZ() : b.mMax.GetZ());
        return corner.Dot(up);
    };
    const float shift = bottomAlongUp(c->GetShape()) - bottomAlongUp(shape.GetPtr());
    const RVec3 oldPos = c->GetPosition();
    if (shift != 0.0f) c->SetPosition(oldPos + up * shift);

    // Finite max penetration ⇒ Jolt checks the new shape for room first and
    // refuses the switch when it would start deeply penetrating (e.g.
    // standing up under a low ceiling). 1.5*slop is Jolt's sample value.
    const bool ok = c->SetShape(
        shape.GetPtr(),
        1.5f * physicsSystem_.GetPhysicsSettings().mPenetrationSlop,
        physicsSystem_.GetDefaultBroadPhaseLayerFilter(layer),
        physicsSystem_.GetDefaultLayerFilter(layer),
        {}, {}, *tempAllocator_);
    if (ok) {
        if (!c->GetInnerBodyID().IsInvalid()) c->SetInnerBodyShape(shape.GetPtr());
    } else if (shift != 0.0f) {
        c->SetPosition(oldPos);   // refused — stay in the old stance, in place
    }
    return ok;
}

JPH::BodyID PhysicsWorld::characterInnerBody(uint32_t handle) const {
    auto it = characters_.find(handle);
    if (it == characters_.end() || !it->second.character) return BodyID();
    return it->second.character->GetInnerBodyID();
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

// --- Vehicles (wheeled / tracked / motorcycle) ---
//
// A Jolt VehicleConstraint is both a Constraint and a PhysicsStepListener:
// the constraint solves suspension/traction impulses, the step listener runs
// the engine/transmission/wheel-collision update at the start of every
// PhysicsSystem::Update. Both registrations are added and removed together
// (removeVehicleFromSystem) so the listener can never dangle. Because the
// stepping happens inside Update, vehicles need no per-step code on our side
// and the phase-ownership contract holds by construction.
//
// Three controller families share the constraint: WheeledVehicleController
// (cars), TrackedVehicleController (tanks — two tracks, skid steering),
// MotorcycleController (a WheeledVehicleController plus a lean spring).

uint32_t PhysicsWorld::createVehicle(const VehicleOptions& opts) {
    if (!initialized_ || opts.wheels.empty()) return 0;
    const bool isTracked = opts.controller == VehicleOptions::ControllerTracked;

    // Tracked structural validation up front. Jolt's TrackedVehicleController
    // has exactly two tracks and indexes them by each wheel's track index: a
    // wheel outside any track leaves WheelTV::mTrackIndex at -1 and reads out
    // of bounds, and an empty track leaves mDrivenWheel uninitialized garbage.
    // Reject these configs cleanly (mirror of the wheeled zero-driven-wheel
    // rejection).
    if (isTracked) {
        if (opts.tracks.size() != 2) {
            LOG_WARN("createVehicle: tracked vehicles need exactly 2 tracks "
                     "[left, right], got %zu", opts.tracks.size());
            return 0;
        }
        std::vector<int> owner(opts.wheels.size(), -1);
        for (int t = 0; t < 2; ++t) {
            const VehicleTrackOptions& trk = opts.tracks[t];
            if (trk.wheels.empty()) {
                LOG_WARN("createVehicle: track %d has no wheels — every track "
                         "needs at least one", t);
                return 0;
            }
            for (int wi : trk.wheels) {
                if (wi < 0 || wi >= (int)opts.wheels.size()) {
                    LOG_WARN("createVehicle: track %d wheel index %d out of range", t, wi);
                    return 0;
                }
                if (owner[wi] != -1) {
                    LOG_WARN("createVehicle: wheel %d appears in both tracks", wi);
                    return 0;
                }
                owner[wi] = t;
            }
            if (trk.drivenWheel >= 0 &&
                std::find(trk.wheels.begin(), trk.wheels.end(), trk.drivenWheel) ==
                    trk.wheels.end()) {
                LOG_WARN("createVehicle: track %d drivenWheel %d is not a wheel "
                         "of that track", t, trk.drivenWheel);
                return 0;
            }
            if (trk.inertia < 0.0f || trk.angularDamping < 0.0f ||
                trk.maxBrakeTorque < 0.0f || trk.differentialRatio <= 0.0f) {
                LOG_WARN("createVehicle: track %d has invalid inertia/"
                         "angularDamping/maxBrakeTorque/differentialRatio", t);
                return 0;
            }
        }
        for (size_t i = 0; i < owner.size(); ++i) {
            if (owner[i] < 0) {
                LOG_WARN("createVehicle: wheel %zu belongs to no track — a "
                         "tracked vehicle must assign every wheel to a track", i);
                return 0;
            }
        }
    }

    VehicleConstraintSettings settings;
    settings.mUp = opts.up.NormalizedOr(Vec3::sAxisY());
    settings.mForward = opts.forward.NormalizedOr(Vec3::sAxisZ());
    settings.mMaxPitchRollAngle =
        DegreesToRadians(std::clamp(opts.maxPitchRollAngle, 0.0f, 180.0f));

    // Motorcycle lean-spring auto-tuning needs the chassis's roll inertia
    // about the forward axis (see VehicleLeanOptions).
    float leanInertia = 0.0f;
    if (opts.controller == VehicleOptions::ControllerMotorcycle) {
        BodyLockRead lock(physicsSystem_.GetBodyLockInterface(), opts.body);
        if (!lock.Succeeded() || !lock.GetBody().IsDynamic()) return 0;
        Mat44 invI =
            lock.GetBody().GetMotionProperties()->GetLocalSpaceInverseInertia();
        float inv = invI.Multiply3x3(settings.mForward).Dot(settings.mForward);
        if (inv > 1.0e-12f) leanInertia = 1.0f / inv;
    }

    float minWidth = FLT_MAX;
    for (const VehicleWheelOptions& w : opts.wheels) {
        WheelSettings* ws;
        if (isTracked) {
            // Tracked wheels carry scalar terrain friction (Jolt models the
            // track's grip per road wheel), not slip curves: the per-wheel
            // friction scalars multiply Jolt's track defaults (4.0
            // longitudinal, 2.0 lateral) and curve overrides don't apply.
            // Steering/brake/driven fields are ignored — tracked vehicles
            // steer, drive, and brake per TRACK.
            auto* tv = new WheelSettingsTV();
            tv->mLongitudinalFriction *= w.longitudinalFrictionScale;
            tv->mLateralFriction *= w.lateralFrictionScale;
            ws = tv;
        } else {
            auto* wv = new WheelSettingsWV();
            wv->mMaxSteerAngle = w.steerable ? DegreesToRadians(w.maxSteerAngle) : 0.0f;
            wv->mMaxBrakeTorque = w.maxBrakeTorque;
            wv->mMaxHandBrakeTorque = w.maxHandBrakeTorque;
            // Per-wheel tire friction: an explicit curve replaces Jolt's
            // default LinearCurve; otherwise the scale multiplies the default
            // curve's friction (Y) values.
            auto applyFriction = [](LinearCurve& curve, float scale,
                                    const std::vector<Float2>& points) {
                if (!points.empty()) {
                    curve.Clear();
                    curve.Reserve((uint)points.size());
                    for (const Float2& p : points) curve.AddPoint(p.x, p.y);
                    curve.Sort();
                } else if (scale != 1.0f) {
                    for (auto& p : curve.mPoints) p.mY *= scale;
                }
            };
            applyFriction(wv->mLongitudinalFriction, w.longitudinalFrictionScale,
                          w.longitudinalFrictionCurve);
            applyFriction(wv->mLateralFriction, w.lateralFrictionScale,
                          w.lateralFrictionCurve);
            ws = wv;
        }
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

    // Engine + transmission settings have identical shapes across controller
    // families (empty gear-ratio lists keep each family's own Jolt defaults).
    auto fillEngineTransmission = [&opts](VehicleEngineSettings& eng,
                                          VehicleTransmissionSettings& tr) {
        eng.mMaxTorque = opts.engine.maxTorque;
        eng.mMinRPM = opts.engine.minRPM;
        eng.mMaxRPM = std::max(opts.engine.maxRPM, opts.engine.minRPM);
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
    };

    if (isTracked) {
        auto* controller = new TrackedVehicleControllerSettings();
        settings.mController = controller;  // takes ownership via Ref
        fillEngineTransmission(controller->mEngine, controller->mTransmission);
        // Tracks were validated above. Torque flows engine → gearbox → each
        // track's driven wheel (differentialRatio); no wheel differentials.
        for (int t = 0; t < 2; ++t) {
            const VehicleTrackOptions& trk = opts.tracks[t];
            VehicleTrackSettings& dst = controller->mTracks[t];
            for (int wi : trk.wheels) dst.mWheels.push_back((uint)wi);
            dst.mDrivenWheel = (uint)(trk.drivenWheel >= 0 ? trk.drivenWheel
                                                           : trk.wheels.back());
            dst.mInertia = trk.inertia;
            dst.mAngularDamping = trk.angularDamping;
            dst.mMaxBrakeTorque = trk.maxBrakeTorque;
            dst.mDifferentialRatio = trk.differentialRatio;
        }
    } else {
        WheeledVehicleControllerSettings* controller;
        if (opts.controller == VehicleOptions::ControllerMotorcycle) {
            // A motorcycle is a wheeled controller plus a lean spring that
            // torques the chassis toward the balance/turn lean angle.
            auto* mc = new MotorcycleControllerSettings();
            mc->mMaxLeanAngle =
                DegreesToRadians(std::clamp(opts.lean.maxAngle, 0.0f, 90.0f));
            // Negative = auto: scale spring strength to the chassis's roll
            // inertia (stable across chassis sizes; raw Jolt defaults are
            // tuned to the sample's offset-COM bike and blow up otherwise).
            mc->mLeanSpringConstant = opts.lean.springConstant >= 0.0f
                ? opts.lean.springConstant : 150.0f * leanInertia;
            mc->mLeanSpringDamping = opts.lean.springDamping >= 0.0f
                ? opts.lean.springDamping : 30.0f * leanInertia;
            mc->mLeanSpringIntegrationCoefficient =
                opts.lean.springIntegrationCoefficient;
            mc->mLeanSpringIntegrationCoefficientDecay =
                opts.lean.springIntegrationCoefficientDecay;
            mc->mLeanSmoothingFactor =
                std::clamp(opts.lean.smoothingFactor, 0.0f, 1.0f);
            controller = mc;
        } else {
            controller = new WheeledVehicleControllerSettings();
        }
        settings.mController = controller;  // takes ownership via Ref
        fillEngineTransmission(controller->mEngine, controller->mTransmission);
        controller->mDifferentialLimitedSlipRatio = opts.differentialLimitedSlipRatio;

        // Differentials: explicit list, or auto-derived by pairing driven
        // wheels in array order with equal torque split.
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
        // Jolt's WheeledVehicleController requires at least one driven wheel:
        // with zero differentials its clutch torque factors sum to 0, which
        // trips a Debug assert (WheeledVehicleController.cpp
        // sum_torque_factors == 1) and NaN-poisons the drivetrain math in
        // Release. Reject the config cleanly.
        if (diffs.empty()) {
            LOG_WARN("createVehicle: no driven wheels — mark at least one wheel "
                     "driven:true or pass explicit differentials");
            return 0;
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
    vehicles_[handle] = VehicleEntry{constraint, opts.controller, opts.body};
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

// Track drive ratios must never be exactly 0 — Jolt's
// TrackedVehicleController::SetDriverInput asserts on it (and 0 has no
// meaning: a stopped track is brake, not ratio).
static float nonZeroRatio(float v) {
    return v == 0.0f ? 1.0e-3f : v;
}

void PhysicsWorld::setVehicleInput(uint32_t handle, float forward, float right,
                                   float brake, float handBrake) {
    auto it = vehicles_.find(handle);
    if (it == vehicles_.end()) return;
    if (it->second.controller == VehicleOptions::ControllerTracked) {
        // Map wheeled-style steering onto differential track ratios the way
        // Jolt's tank sample does: the inside track slows linearly with steer
        // input, through zero, to a full pivot turn (ratio -1) at full lock —
        // right: 0.2 gives the sample's gentle-turn 0.6, right: 1 pivots.
        // Tracked vehicles have no separate handbrake; brake is the stronger
        // of the two pedals.
        float steer = std::clamp(right, -1.0f, 1.0f);
        float leftRatio = 1.0f, rightRatio = 1.0f;
        if (steer > 0.0f) rightRatio = 1.0f - 2.0f * steer;
        else if (steer < 0.0f) leftRatio = 1.0f + 2.0f * steer;
        auto* ctrl = static_cast<TrackedVehicleController*>(
            it->second.constraint->GetController());
        ctrl->SetDriverInput(std::clamp(forward, -1.0f, 1.0f),
                             nonZeroRatio(leftRatio), nonZeroRatio(rightRatio),
                             std::clamp(std::max(brake, handBrake), 0.0f, 1.0f));
    } else {
        auto* ctrl = static_cast<WheeledVehicleController*>(
            it->second.constraint->GetController());
        ctrl->SetDriverInput(std::clamp(forward, -1.0f, 1.0f),
                             std::clamp(right, -1.0f, 1.0f),
                             std::clamp(brake, 0.0f, 1.0f),
                             std::clamp(handBrake, 0.0f, 1.0f));
    }
    // Wake the chassis so input acts on a sleeping vehicle (constraint-motor
    // rule). Zero input doesn't wake — a parked car may sleep.
    if (forward != 0.0f || right != 0.0f || brake != 0.0f || handBrake != 0.0f)
        physicsSystem_.GetBodyInterface().ActivateBody(it->second.body);
}

void PhysicsWorld::setVehicleTrackInput(uint32_t handle, float forward,
                                        float leftRatio, float rightRatio,
                                        float brake) {
    auto it = vehicles_.find(handle);
    if (it == vehicles_.end() ||
        it->second.controller != VehicleOptions::ControllerTracked)
        return;
    auto* ctrl = static_cast<TrackedVehicleController*>(
        it->second.constraint->GetController());
    ctrl->SetDriverInput(std::clamp(forward, -1.0f, 1.0f),
                         nonZeroRatio(std::clamp(leftRatio, -1.0f, 1.0f)),
                         nonZeroRatio(std::clamp(rightRatio, -1.0f, 1.0f)),
                         std::clamp(brake, 0.0f, 1.0f));
    if (forward != 0.0f || leftRatio != 1.0f || rightRatio != 1.0f || brake != 0.0f)
        physicsSystem_.GetBodyInterface().ActivateBody(it->second.body);
}

bool PhysicsWorld::setVehicleLeanController(uint32_t handle, bool enabled) {
    auto it = vehicles_.find(handle);
    if (it == vehicles_.end() ||
        it->second.controller != VehicleOptions::ControllerMotorcycle)
        return false;
    auto* ctrl = static_cast<MotorcycleController*>(
        it->second.constraint->GetController());
    ctrl->EnableLeanController(enabled);
    // The toggle changes the balance forces immediately — wake the bike so a
    // parked one starts falling when the spring is switched off.
    physicsSystem_.GetBodyInterface().ActivateBody(it->second.body);
    return true;
}

void PhysicsWorld::setVehicleGear(uint32_t handle, int gear, float clutchFriction) {
    auto it = vehicles_.find(handle);
    if (it == vehicles_.end()) return;
    VehicleController* c = it->second.constraint->GetController();
    VehicleTransmission& tr =
        it->second.controller == VehicleOptions::ControllerTracked
            ? static_cast<TrackedVehicleController*>(c)->GetTransmission()
            : static_cast<WheeledVehicleController*>(c)->GetTransmission();
    tr.Set(gear, std::clamp(clutchFriction, 0.0f, 1.0f));
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
    // Engine/transmission accessors live on the concrete controller family
    // (tracked and wheeled don't share them; motorcycle is-a wheeled).
    const VehicleController* vc = c->GetController();
    const VehicleEngine* eng;
    const VehicleTransmission* tr;
    if (it->second.controller == VehicleOptions::ControllerTracked) {
        const auto* t = static_cast<const TrackedVehicleController*>(vc);
        eng = &t->GetEngine();
        tr = &t->GetTransmission();
    } else {
        const auto* wv = static_cast<const WheeledVehicleController*>(vc);
        eng = &wv->GetEngine();
        tr = &wv->GetTransmission();
    }
    out.rpm = eng->GetCurrentRPM();
    out.gear = tr->GetCurrentGear();
    out.isSwitchingGear = tr->IsSwitchingGear();
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

    // Ragdoll part bodies reuse body-manager indices; reset the per-index
    // listener tables (sensor bit, combine modes) so a part never inherits a
    // destroyed body's flags.
    if (listener_) {
        for (const BodyID& id : rag->GetBodyIDs()) {
            listener_->setSensorId(id, false);
            listener_->setCombineModes(id, CombineMode::Default, CombineMode::Default);
        }
    }

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

// --- Soft bodies ---
//
// Jolt soft bodies are ordinary bodies whose SoftBodyMotionProperties hold
// the XPBD vertex state; they step inside PhysicsSystem::Update like any
// dynamic body, so no per-step bookkeeping is needed here. The registry
// entry pins the SoftBodySharedSettings Ref and remembers the even mass
// split for unpinning.

// Build a cloth grid: gridX*gridZ vertices in the local XZ plane (Y up),
// centered on the origin. Vertex (x,z) lives at index z*gridX + x; faces
// wind CCW seen from +Y so the rendered front faces (and the rest normals a
// render mesh derives from them) point up.
static Ref<SoftBodySharedSettings> buildClothSettings(const SoftBodyOptions& o,
                                                      float invMass) {
    const int gx = std::max(2, o.gridX);
    const int gz = std::max(2, o.gridZ);
    const float sp = o.spacing > 0.0f ? o.spacing : 0.1f;
    const float halfX = 0.5f * (gx - 1) * sp;
    const float halfZ = 0.5f * (gz - 1) * sp;

    Ref<SoftBodySharedSettings> s = new SoftBodySharedSettings();
    s->mVertices.reserve((size_t)gx * gz);
    for (int z = 0; z < gz; z++)
        for (int x = 0; x < gx; x++)
            s->mVertices.push_back(SoftBodySharedSettings::Vertex(
                Float3(x * sp - halfX, 0.0f, z * sp - halfZ),
                Float3(0, 0, 0), invMass));

    for (int z = 0; z + 1 < gz; z++) {
        for (int x = 0; x + 1 < gx; x++) {
            uint32_t v00 = (uint32_t)(z * gx + x);
            uint32_t v10 = v00 + 1;
            uint32_t v01 = v00 + gx;
            uint32_t v11 = v01 + 1;
            s->AddFace(SoftBodySharedSettings::Face(v00, v01, v11));
            s->AddFace(SoftBodySharedSettings::Face(v00, v11, v10));
        }
    }
    return s;
}

// Build settings for an arbitrary triangle mesh. Degenerate and out-of-range
// triangles are skipped. With pressure the enclosed volume must be positive
// (CCW-outward winding); a negative signed rest volume flips all faces.
static Ref<SoftBodySharedSettings> buildMeshSettings(const SoftBodyOptions& o,
                                                     float invMass) {
    if (o.vertices.size() < 3 || o.indices.size() < 3) return nullptr;
    const uint32_t n = (uint32_t)o.vertices.size();

    Ref<SoftBodySharedSettings> s = new SoftBodySharedSettings();
    s->mVertices.reserve(n);
    for (const Vec3& v : o.vertices)
        s->mVertices.push_back(SoftBodySharedSettings::Vertex(
            Float3(v.GetX(), v.GetY(), v.GetZ()), Float3(0, 0, 0), invMass));

    for (size_t i = 0; i + 2 < o.indices.size(); i += 3) {
        uint32_t a = o.indices[i], b = o.indices[i + 1], c = o.indices[i + 2];
        if (a >= n || b >= n || c >= n) continue;
        SoftBodySharedSettings::Face f(a, b, c);
        if (f.IsDegenerate()) continue;
        s->AddFace(f);
    }
    if (s->mFaces.empty()) return nullptr;

    if (o.pressure > 0.0f) {
        // Signed rest volume (same sum ApplyPressure uses). Inside-out
        // meshes would deflate instead of inflate — flip them.
        float sixVolume = 0.0f;
        for (const auto& f : s->mFaces) {
            Vec3 x1(s->mVertices[f.mVertex[0]].mPosition);
            Vec3 x2(s->mVertices[f.mVertex[1]].mPosition);
            Vec3 x3(s->mVertices[f.mVertex[2]].mPosition);
            sixVolume += x1.Cross(x2).Dot(x3);
        }
        if (sixVolume < 0.0f)
            for (auto& f : s->mFaces) std::swap(f.mVertex[1], f.mVertex[2]);
    }
    return s;
}

uint32_t PhysicsWorld::createSoftBody(const SoftBodyOptions& opts) {
    if (!initialized_) return 0;

    // Even mass split. Pinned vertices are zeroed after the split so a few
    // pins don't change the felt weight of the rest of the body.
    size_t nVerts = opts.kind == SoftBodyOptions::Cloth
        ? (size_t)std::max(2, opts.gridX) * std::max(2, opts.gridZ)
        : opts.vertices.size();
    if (nVerts < 3 && opts.kind == SoftBodyOptions::Mesh) return 0;
    const float mass = opts.mass > 0.0f ? opts.mass : 1.0f;
    const float invMass = (float)nVerts / mass;

    Ref<SoftBodySharedSettings> settings = opts.kind == SoftBodyOptions::Cloth
        ? buildClothSettings(opts, invMass)
        : buildMeshSettings(opts, invMass);
    if (!settings) return 0;

    for (uint32_t idx : opts.pinned)
        if (idx < settings->mVertices.size())
            settings->mVertices[idx].mInvMass = 0.0f;

    // Derive edge (+ shear for quads) and optional bend constraints from the
    // faces. Compliance is XPBD's inverse stiffness: 0 = rigid spring.
    SoftBodySharedSettings::VertexAttributes attr;
    attr.mCompliance = std::max(0.0f, opts.compliance);
    attr.mShearCompliance = opts.shearCompliance >= 0.0f ? opts.shearCompliance
                                                         : attr.mCompliance;
    const bool bend = opts.bendCompliance >= 0.0f;
    attr.mBendCompliance = bend ? opts.bendCompliance : FLT_MAX;
    settings->CreateConstraints(&attr, 1,
        bend ? SoftBodySharedSettings::EBendType::Distance
             : SoftBodySharedSettings::EBendType::None);
    settings->Optimize();  // required: builds the parallel update groups

    int layer = opts.layer;
    if (layer < 0 || layer >= numLayers_) layer = 1;

    SoftBodyCreationSettings scs(settings, opts.position,
                                 opts.rotation.Normalized(),
                                 static_cast<ObjectLayer>(layer));
    scs.mNumIterations = (uint32)std::max(1, opts.numIterations);
    scs.mFriction = opts.friction;
    scs.mRestitution = opts.restitution;
    scs.mPressure = opts.kind == SoftBodyOptions::Mesh ? std::max(0.0f, opts.pressure) : 0.0f;
    scs.mLinearDamping = opts.linearDamping;
    scs.mGravityFactor = opts.gravityFactor;
    scs.mMaxLinearVelocity = opts.maxLinearVelocity;
    scs.mVertexRadius = std::max(0.0f, opts.vertexRadius);
    scs.mUpdatePosition = opts.updatePosition;
    scs.mFacesDoubleSided = opts.doubleSided;
    scs.mAllowSleeping = opts.allowSleeping;

    BodyInterface& bi = physicsSystem_.GetBodyInterface();
    BodyID id = bi.CreateAndAddSoftBody(scs, EActivation::Activate);
    if (id.IsInvalid()) return 0;
    if (listener_) {
        listener_->setSensorId(id, false);
        listener_->setCombineModes(id, CombineMode::Default, CombineMode::Default);
    }

    SoftBodyEntry entry;
    entry.settings = settings;
    entry.body = id;
    entry.defaultInvMass = invMass;

    uint32_t handle = nextSoftBodyHandle_++;
    softBodies_[handle] = std::move(entry);
    return handle;
}

void PhysicsWorld::destroySoftBody(uint32_t handle) {
    auto it = softBodies_.find(handle);
    if (it == softBodies_.end()) return;
    BodyID id = it->second.body;
    softBodies_.erase(it);
    destroyBody(id);  // entry already gone, so this is the generic path
}

BodyID PhysicsWorld::softBodyBody(uint32_t handle) const {
    auto it = softBodies_.find(handle);
    return it == softBodies_.end() ? BodyID() : it->second.body;
}

int PhysicsWorld::softBodyVertexCount(uint32_t handle) const {
    auto it = softBodies_.find(handle);
    if (it == softBodies_.end()) return -1;
    return (int)it->second.settings->mVertices.size();
}

bool PhysicsWorld::getSoftBodyVertices(uint32_t handle,
                                       std::vector<float>& outXyz) const {
    auto it = softBodies_.find(handle);
    if (it == softBodies_.end()) return false;
    BodyLockRead lock(physicsSystem_.GetBodyLockInterface(), it->second.body);
    if (!lock.Succeeded() || !lock.GetBody().IsSoftBody()) return false;
    const Body& body = lock.GetBody();
    const auto* mp =
        static_cast<const SoftBodyMotionProperties*>(body.GetMotionProperties());
    RMat44 com = body.GetCenterOfMassTransform();
    const auto& verts = mp->GetVertices();
    outXyz.resize(verts.size() * 3);
    for (size_t i = 0; i < verts.size(); i++) {
        RVec3 wp = com * verts[i].mPosition;  // vertices are COM-relative
        outXyz[i * 3 + 0] = (float)wp.GetX();
        outXyz[i * 3 + 1] = (float)wp.GetY();
        outXyz[i * 3 + 2] = (float)wp.GetZ();
    }
    return true;
}

bool PhysicsWorld::softBodyTopology(uint32_t handle, std::vector<float>& outXyz,
                                    std::vector<uint32_t>& outIndices) const {
    auto it = softBodies_.find(handle);
    if (it == softBodies_.end()) return false;
    const SoftBodySharedSettings* s = it->second.settings.GetPtr();
    outXyz.resize(s->mVertices.size() * 3);
    for (size_t i = 0; i < s->mVertices.size(); i++) {
        const Float3& p = s->mVertices[i].mPosition;
        outXyz[i * 3 + 0] = p.x;
        outXyz[i * 3 + 1] = p.y;
        outXyz[i * 3 + 2] = p.z;
    }
    outIndices.resize(s->mFaces.size() * 3);
    for (size_t i = 0; i < s->mFaces.size(); i++) {
        outIndices[i * 3 + 0] = s->mFaces[i].mVertex[0];
        outIndices[i * 3 + 1] = s->mFaces[i].mVertex[1];
        outIndices[i * 3 + 2] = s->mFaces[i].mVertex[2];
    }
    return true;
}

bool PhysicsWorld::setSoftBodyVertexPosition(uint32_t handle, int index,
                                             RVec3 pos) {
    auto it = softBodies_.find(handle);
    if (it == softBodies_.end()) return false;
    {
        BodyLockWrite lock(physicsSystem_.GetBodyLockInterface(), it->second.body);
        if (!lock.Succeeded() || !lock.GetBody().IsSoftBody()) return false;
        Body& body = lock.GetBody();
        auto* mp = static_cast<SoftBodyMotionProperties*>(body.GetMotionProperties());
        if (index < 0 || index >= (int)mp->GetVertices().size()) return false;
        auto& v = mp->GetVertex((uint)index);
        v.mPosition = Vec3(body.GetCenterOfMassTransform().Inversed() * pos);
        v.mVelocity = Vec3::sZero();
    }
    physicsSystem_.GetBodyInterface().ActivateBody(it->second.body);
    return true;
}

bool PhysicsWorld::setSoftBodyVertexVelocity(uint32_t handle, int index,
                                             Vec3 vel) {
    auto it = softBodies_.find(handle);
    if (it == softBodies_.end()) return false;
    {
        BodyLockWrite lock(physicsSystem_.GetBodyLockInterface(), it->second.body);
        if (!lock.Succeeded() || !lock.GetBody().IsSoftBody()) return false;
        auto* mp = static_cast<SoftBodyMotionProperties*>(
            lock.GetBody().GetMotionProperties());
        if (index < 0 || index >= (int)mp->GetVertices().size()) return false;
        mp->GetVertex((uint)index).mVelocity = vel;  // COM rotation is identity
    }
    physicsSystem_.GetBodyInterface().ActivateBody(it->second.body);
    return true;
}

bool PhysicsWorld::pinSoftBodyVertex(uint32_t handle, int index, bool pinned) {
    auto it = softBodies_.find(handle);
    if (it == softBodies_.end()) return false;
    {
        BodyLockWrite lock(physicsSystem_.GetBodyLockInterface(), it->second.body);
        if (!lock.Succeeded() || !lock.GetBody().IsSoftBody()) return false;
        auto* mp = static_cast<SoftBodyMotionProperties*>(
            lock.GetBody().GetMotionProperties());
        if (index < 0 || index >= (int)mp->GetVertices().size()) return false;
        auto& v = mp->GetVertex((uint)index);
        v.mInvMass = pinned ? 0.0f : it->second.defaultInvMass;
        if (pinned) v.mVelocity = Vec3::sZero();
    }
    physicsSystem_.GetBodyInterface().ActivateBody(it->second.body);
    return true;
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

std::vector<RayHit> PhysicsWorld::raycast(RVec3 origin, Vec3 direction,
                                          float maxDistance,
                                          const QueryFilter& filter) const {
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

// --- Contact events ---

std::vector<ContactEvent> PhysicsWorld::drainContactEvents(bool* overflowed) {
    if (overflowed) *overflowed = contactsOverflowedFront_;
    contactsOverflowedFront_ = false;
    std::vector<ContactEvent> out;
    out.swap(contactsFront_);
    return out;
}

} // namespace bro::physics
