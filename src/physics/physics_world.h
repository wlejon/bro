#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>

namespace bro::physics {

/// Physics thread state machine (atomics, same pattern as raster/layout threads).
enum PhysicsState : uint32_t {
    kPhysicsIdle     = 0,  // Physics thread waiting — JS can freely access bodies
    kPhysicsStep     = 1,  // Main thread: begin simulation step
    kPhysicsBusy     = 2,  // Physics thread running Update()
    kPhysicsDone     = 3,  // Physics thread: step complete
    kPhysicsShutdown = 4,  // Main thread: terminate physics thread
};

/// Contact event recorded during a physics step (thread-safe collection).
struct ContactEvent {
    enum Type { Added, Persisted, Removed };
    Type type;
    JPH::BodyID body1;
    JPH::BodyID body2;
};

/// Raycast hit result.
struct RayHit {
    JPH::BodyID bodyID;
    float fraction;       // 0..1 along the ray
    JPH::Vec3 normal;
    JPH::RVec3 position;
};

class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();

    /// Initialize the physics system. Call once before use.
    bool init(int maxBodies = 4096);

    /// Start the physics thread.
    void startThread();

    /// Signal the physics thread to run one step.
    /// Only call when state is kPhysicsIdle.
    void signalStep();

    /// Check if the physics step is done and transition to idle.
    /// Returns true if a step completed this call.
    bool consumeStep();

    /// Returns true if the physics thread is idle (JS can access bodies).
    bool isIdle() const;

    /// Shut down the physics thread and clean up.
    void shutdown();

    /// Set simulation time step (default 1/60).
    void setTimeStep(float dt) { timeStep_ = dt; }
    float timeStep() const { return timeStep_; }

    /// Set gravity.
    void setGravity(float x, float y, float z);
    JPH::Vec3 gravity() const;

    // --- Body management (call only when idle) ---

    /// Create a box body. Returns BodyID.
    JPH::BodyID createBox(JPH::RVec3 position, JPH::Quat rotation,
                          JPH::Vec3 halfExtents, bool isStatic,
                          float friction = 0.5f, float restitution = 0.3f);

    /// Create a sphere body.
    JPH::BodyID createSphere(JPH::RVec3 position, JPH::Quat rotation,
                             float radius, bool isStatic,
                             float friction = 0.5f, float restitution = 0.3f);

    /// Create a capsule body.
    JPH::BodyID createCapsule(JPH::RVec3 position, JPH::Quat rotation,
                              float halfHeight, float radius, bool isStatic,
                              float friction = 0.5f, float restitution = 0.3f);

    /// Create a cylinder body.
    JPH::BodyID createCylinder(JPH::RVec3 position, JPH::Quat rotation,
                               float halfHeight, float radius, bool isStatic,
                               float friction = 0.5f, float restitution = 0.3f);

    /// Remove and destroy a body.
    void destroyBody(JPH::BodyID id);

    // --- Body state (call only when idle) ---

    JPH::RVec3 getPosition(JPH::BodyID id) const;
    JPH::Quat getRotation(JPH::BodyID id) const;
    JPH::Vec3 getLinearVelocity(JPH::BodyID id) const;
    JPH::Vec3 getAngularVelocity(JPH::BodyID id) const;

    void setPosition(JPH::BodyID id, JPH::RVec3 pos);
    void setRotation(JPH::BodyID id, JPH::Quat rot);
    void setLinearVelocity(JPH::BodyID id, JPH::Vec3 vel);
    void setAngularVelocity(JPH::BodyID id, JPH::Vec3 vel);

    void addForce(JPH::BodyID id, JPH::Vec3 force);
    void addImpulse(JPH::BodyID id, JPH::Vec3 impulse);
    void addTorque(JPH::BodyID id, JPH::Vec3 torque);

    void setMotionType(JPH::BodyID id, bool isStatic);
    void activate(JPH::BodyID id);
    bool isActive(JPH::BodyID id) const;

    // --- Queries (call only when idle) ---

    /// Cast a ray. Returns hits sorted by distance.
    std::vector<RayHit> raycast(JPH::RVec3 origin, JPH::Vec3 direction,
                                float maxDistance = 1000.0f) const;

    /// Get the closest hit along a ray, or empty if none.
    bool raycastClosest(JPH::RVec3 origin, JPH::Vec3 direction,
                        RayHit& outHit, float maxDistance = 1000.0f) const;

    // --- Contact events ---

    /// Swap and return contact events from the last step. Clears the buffer.
    std::vector<ContactEvent> drainContactEvents();

    /// Access the Jolt physics system directly (advanced use).
    JPH::PhysicsSystem& system() { return physicsSystem_; }

private:
    void physicsThreadFunc();

    JPH::PhysicsSystem physicsSystem_;
    std::unique_ptr<JPH::TempAllocatorImpl> tempAllocator_;
    std::unique_ptr<JPH::JobSystemThreadPool> jobSystem_;

    // Broadphase / collision layer config (stored as members for lifetime)
    struct Layers;
    std::unique_ptr<Layers> layers_;

    // Thread state
    struct Shared {
        std::atomic<uint32_t> state{kPhysicsIdle};
    };
    Shared shared_;
    std::thread physicsThread_;

    float timeStep_ = 1.0f / 60.0f;

    // Contact events double-buffer: physics thread writes to back_, main drains front_.
    std::vector<ContactEvent> contactsFront_;
    std::vector<ContactEvent> contactsBack_;

    bool initialized_ = false;
};

} // namespace bro::physics
