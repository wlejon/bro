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

    // A script's option: bounded before it sizes Jolt's tables, and before
    // `maxBodies * 2` below can overflow.
    maxBodies = std::clamp(maxBodies, 1, kMaxBodies);

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

    // Area field overrides: one step listener per world applies the gravity
    // overrides inside every step (no-ops instantly while no areas exist).
    areaListener_ = std::make_unique<AreaStepListener>(this);
    physicsSystem_.AddStepListener(areaListener_.get());

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
        shared_.cv.notify_all();  // a query waiting out the step (awaitStepForQuery)
    }
}

void PhysicsWorld::awaitStepForQuery() const {
    std::unique_lock<std::mutex> lk(shared_.m);
    shared_.cv.wait(lk, [this] { return shared_.state != kPhysicsStep && shared_.state != kPhysicsBusy; });
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
    collectContacts();
    checkBrokenConstraints();

    std::lock_guard<std::mutex> lk(shared_.m);
    shared_.state = kPhysicsIdle;
    return true;
}

bool PhysicsWorld::isIdle() const {
    std::lock_guard<std::mutex> lk(shared_.m);
    return shared_.state == kPhysicsIdle;
}

bool PhysicsWorld::hasActiveBodies() const {
    if (!initialized_) return false;
    return physicsSystem_.GetNumActiveBodies(JPH::EBodyType::RigidBody) > 0 || !characters_.empty();
}

JPH::BodyInterface& PhysicsWorld::getBodyInterface() {
    return isIdle() ? physicsSystem_.GetBodyInterfaceNoLock() : physicsSystem_.GetBodyInterface();
}

const JPH::BodyInterface& PhysicsWorld::getBodyInterface() const {
    return isIdle() ? physicsSystem_.GetBodyInterfaceNoLock() : physicsSystem_.GetBodyInterface();
}

void PhysicsWorld::stepInline() {
    if (!initialized_) return;
    capturePrevTransforms();
    updateCharacters(timeStep_);
    physicsSystem_.Update(timeStep_, 1, tempAllocator_.get(), jobSystem_.get());
    collectContacts();
    checkBrokenConstraints();
}

void PhysicsWorld::collectContacts() {
    if (!listener_) return;
    bool overflowed = false;
    std::vector<ContactEvent> fresh = listener_->drain(&overflowed);
    updateAreaOverlaps(fresh);
    // Accumulate until drainContactEvents(): a caller that steps several times
    // per read must still see every step's events. Capped at the listener's
    // capacity so a world nobody drains does not grow without bound.
    const size_t cap = listener_->buffer.size();
    for (const ContactEvent& e : fresh) {
        if (contactsFront_.size() >= cap) { overflowed = true; break; }
        contactsFront_.push_back(e);
    }
    contactsOverflowedFront_ = contactsOverflowedFront_ || overflowed;
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
        // Area override listener + bookkeeping (all bodies are going away).
        if (areaListener_) {
            physicsSystem_.RemoveStepListener(areaListener_.get());
            areaListener_.reset();
        }
        areas_.clear();
        bodyAreas_.clear();

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

    // Area bookkeeping: every body (areas and members alike) is gone.
    areas_.clear();
    bodyAreas_.clear();
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
