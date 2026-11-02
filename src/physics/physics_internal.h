#pragma once
#include "physics/physics_world.h"

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/GroupFilter.h>
#include <Jolt/Physics/Collision/CollisionGroup.h>
#include <Jolt/Physics/Collision/EstimateCollisionResponse.h>
#include <Jolt/Physics/PhysicsStepListener.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>

JPH_SUPPRESS_WARNINGS

namespace bro::physics {

using namespace JPH;

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

// --- Area field overrides: step listener ---
//
// Applies the gravity part of AreaOverride INSIDE the physics step (Jolt
// PhysicsStepListener — the same hook vehicles use). There is exactly ONE of
// these per world, so its OnStep body is single-threaded even though Jolt may
// run different listeners in parallel; it reads the membership/override maps
// (immutable during a step — mutated only between steps on the phase-owning
// thread) and writes only body velocities, which step listeners are licensed
// to do. Damping overrides are NOT applied here — they're set/restored on the
// bodies' MotionProperties at enter/exit (updateAreaOverlaps).
struct PhysicsWorld::AreaStepListener final : public PhysicsStepListener {
    explicit AreaStepListener(PhysicsWorld* w) : world(w) {}
    PhysicsWorld* world;
    void OnStep(const PhysicsStepListenerContext& ctx) override {
        world->applyAreaGravity(ctx.mDeltaTime);
    }
};


JPH::RefConst<JPH::Shape> buildShape(const BodyOptions& opts);

} // namespace bro::physics
