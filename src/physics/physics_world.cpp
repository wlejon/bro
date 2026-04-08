#include "physics/physics_world.h"

#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>

#include "util/log.h"

#include <algorithm>

JPH_SUPPRESS_WARNINGS

using namespace JPH;

namespace bro::physics {

// --- Layer setup (simple 2-layer: static vs moving) ---

namespace LayerDefs {
    static constexpr ObjectLayer NON_MOVING = 0;
    static constexpr ObjectLayer MOVING = 1;
    static constexpr uint32_t NUM_LAYERS = 2;

    static constexpr BroadPhaseLayer BP_NON_MOVING(0);
    static constexpr BroadPhaseLayer BP_MOVING(1);
    static constexpr uint32_t BP_NUM_LAYERS = 2;
}

struct PhysicsWorld::Layers {
    std::unique_ptr<ObjectLayerPairFilterTable> objectFilter;
    std::unique_ptr<BroadPhaseLayerInterfaceTable> bpLayerInterface;
    std::unique_ptr<ObjectVsBroadPhaseLayerFilterTable> objectVsBpFilter;

    Layers() {
        objectFilter = std::make_unique<ObjectLayerPairFilterTable>(LayerDefs::NUM_LAYERS);
        objectFilter->EnableCollision(LayerDefs::NON_MOVING, LayerDefs::MOVING);
        objectFilter->EnableCollision(LayerDefs::MOVING, LayerDefs::MOVING);

        bpLayerInterface = std::make_unique<BroadPhaseLayerInterfaceTable>(
            LayerDefs::NUM_LAYERS, LayerDefs::BP_NUM_LAYERS);
        bpLayerInterface->MapObjectToBroadPhaseLayer(LayerDefs::NON_MOVING, LayerDefs::BP_NON_MOVING);
        bpLayerInterface->MapObjectToBroadPhaseLayer(LayerDefs::MOVING, LayerDefs::BP_MOVING);

        objectVsBpFilter = std::make_unique<ObjectVsBroadPhaseLayerFilterTable>(
            *bpLayerInterface, LayerDefs::BP_NUM_LAYERS,
            *objectFilter, LayerDefs::NUM_LAYERS);
    }
};

// --- Contact listener (collects events for JS) ---

class ContactListenerImpl : public ContactListener {
public:
    void OnContactAdded(const Body& b1, const Body& b2,
                        const ContactManifold&, ContactSettings&) override {
        std::lock_guard lock(mutex_);
        events_.push_back({ContactEvent::Added, b1.GetID(), b2.GetID()});
    }

    void OnContactRemoved(const SubShapeIDPair& pair) override {
        std::lock_guard lock(mutex_);
        events_.push_back({ContactEvent::Removed,
                           pair.GetBody1ID(), pair.GetBody2ID()});
    }

    std::vector<ContactEvent> drain() {
        std::lock_guard lock(mutex_);
        std::vector<ContactEvent> out;
        out.swap(events_);
        return out;
    }

private:
    std::mutex mutex_;
    std::vector<ContactEvent> events_;
};

static ContactListenerImpl* s_contactListener = nullptr;

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

    physicsSystem_.Init(
        (uint)maxBodies, 0,
        (uint)std::min(maxBodies, 4096),       // max body pairs
        (uint)std::min(maxBodies * 2, 4096),   // max contact constraints
        *layers_->bpLayerInterface,
        *layers_->objectVsBpFilter,
        *layers_->objectFilter);

    physicsSystem_.SetGravity(Vec3(0, -9.81f, 0));

    // Install contact listener
    static ContactListenerImpl contactListener;
    s_contactListener = &contactListener;
    physicsSystem_.SetContactListener(s_contactListener);

    initialized_ = true;
    return true;
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

    if (s_contactListener) {
        contactsFront_ = s_contactListener->drain();
    }

    shared_.state.store(kPhysicsIdle, std::memory_order_release);
    return true;
}

bool PhysicsWorld::isIdle() const {
    return shared_.state.load(std::memory_order_acquire) == kPhysicsIdle;
}

void PhysicsWorld::shutdown() {
    if (physicsThread_.joinable()) {
        shared_.state.store(kPhysicsShutdown, std::memory_order_release);
        shared_.state.notify_one();
        physicsThread_.join();
    }

    if (initialized_) {
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

static BodyID createBodyHelper(PhysicsSystem& system, const Shape* shape,
                               RVec3 position, Quat rotation, bool isStatic,
                               float friction, float restitution) {
    ObjectLayer layer = isStatic ? LayerDefs::NON_MOVING : LayerDefs::MOVING;
    EMotionType motion = isStatic ? EMotionType::Static : EMotionType::Dynamic;

    BodyCreationSettings settings(shape, position, rotation, motion, layer);
    settings.mFriction = friction;
    settings.mRestitution = restitution;

    BodyInterface& bi = system.GetBodyInterface();
    return bi.CreateAndAddBody(settings, isStatic ? EActivation::DontActivate : EActivation::Activate);
}

BodyID PhysicsWorld::createBox(RVec3 position, Quat rotation,
                               Vec3 halfExtents, bool isStatic,
                               float friction, float restitution) {
    BoxShapeSettings shapeSettings(halfExtents);
    auto result = shapeSettings.Create();
    if (result.HasError()) return BodyID();
    return createBodyHelper(physicsSystem_, result.Get().GetPtr(),
                            position, rotation, isStatic, friction, restitution);
}

BodyID PhysicsWorld::createSphere(RVec3 position, Quat rotation,
                                  float radius, bool isStatic,
                                  float friction, float restitution) {
    SphereShapeSettings shapeSettings(radius);
    auto result = shapeSettings.Create();
    if (result.HasError()) return BodyID();
    return createBodyHelper(physicsSystem_, result.Get().GetPtr(),
                            position, rotation, isStatic, friction, restitution);
}

BodyID PhysicsWorld::createCapsule(RVec3 position, Quat rotation,
                                   float halfHeight, float radius, bool isStatic,
                                   float friction, float restitution) {
    CapsuleShapeSettings shapeSettings(halfHeight, radius);
    auto result = shapeSettings.Create();
    if (result.HasError()) return BodyID();
    return createBodyHelper(physicsSystem_, result.Get().GetPtr(),
                            position, rotation, isStatic, friction, restitution);
}

BodyID PhysicsWorld::createCylinder(RVec3 position, Quat rotation,
                                    float halfHeight, float radius, bool isStatic,
                                    float friction, float restitution) {
    CylinderShapeSettings shapeSettings(halfHeight, radius);
    auto result = shapeSettings.Create();
    if (result.HasError()) return BodyID();
    return createBodyHelper(physicsSystem_, result.Get().GetPtr(),
                            position, rotation, isStatic, friction, restitution);
}

void PhysicsWorld::destroyBody(BodyID id) {
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

void PhysicsWorld::setMotionType(BodyID id, bool isStatic) {
    ObjectLayer layer = isStatic ? LayerDefs::NON_MOVING : LayerDefs::MOVING;
    EMotionType motion = isStatic ? EMotionType::Static : EMotionType::Dynamic;
    physicsSystem_.GetBodyInterface().SetMotionType(id, motion, EActivation::Activate);
    physicsSystem_.GetBodyInterface().SetObjectLayer(id, layer);
}

void PhysicsWorld::activate(BodyID id) {
    physicsSystem_.GetBodyInterface().ActivateBody(id);
}

bool PhysicsWorld::isActive(BodyID id) const {
    return physicsSystem_.GetBodyInterface().IsActive(id);
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
