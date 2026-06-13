#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <Jolt/Jolt.h>
#include <Jolt/Math/Float2.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Body/AllowedDOFs.h>
#include <Jolt/Physics/Body/MotionQuality.h>
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
    bool isSensor = false;  // true when either body is a sensor (overlap event)
};

/// Raycast hit result.
struct RayHit {
    JPH::BodyID bodyID;
    float fraction;       // 0..1 along the ray
    JPH::Vec3 normal;
    JPH::RVec3 position;
};

/// Body creation options (covers all shapes & flags).
struct BodyOptions {
    enum Shape {
        ShapeBox,
        ShapeSphere,
        ShapeCapsule,
        ShapeCylinder,
        ShapeConvexHull,
        ShapeMesh,        // static only
        ShapeCompound,
        ShapeChain,       // static only — 2D polyline thickened along Z
    };

    Shape shape = ShapeBox;
    JPH::RVec3 position{0, 0, 0};
    JPH::Quat rotation = JPH::Quat::sIdentity();

    // Box
    JPH::Vec3 halfExtents{0.5f, 0.5f, 0.5f};
    // Sphere / capsule / cylinder
    float radius = 0.5f;
    float halfHeight = 0.5f;
    // ConvexHull
    std::vector<JPH::Vec3> hullPoints;
    // Mesh (static only)
    std::vector<JPH::Vec3> meshVertices;
    std::vector<uint32_t>  meshIndices;       // triangle list (multiple of 3)
    // Chain (static only): a 2D polyline in the XY plane, thickened along Z
    // into a one-sided collision strip via bromesh::sweep + Jolt MeshShape.
    // Triangle winding determines which side is "front"; flipNormal swaps it.
    std::vector<JPH::Float2> chainPoints;
    float chainDepth = 20.0f;        // total Z thickness of the strip
    bool  chainClosed = false;       // close loop (welds last segment back to first)
    bool  chainFlipNormal = false;   // flip front-face direction
    // Compound: sub-parts (each carries its own shape + local transform)
    std::vector<BodyOptions> compoundParts;
    JPH::Vec3 localPosition{0, 0, 0};         // used only for compound sub-parts
    JPH::Quat localRotation = JPH::Quat::sIdentity();

    bool isStatic = false;
    bool isSensor = false;
    bool ccd = false;                          // Linear cast for fast bodies
    JPH::EAllowedDOFs dofs = JPH::EAllowedDOFs::All;

    float friction = 0.5f;
    float restitution = 0.3f;
    float density = 1000.0f;
    float gravityFactor = 1.0f;
    float linearDamping = 0.05f;
    float angularDamping = 0.05f;
    float maxLinearVelocity = 500.0f;          // Jolt default (m/s or px/s)
    float maxAngularVelocity = 0.25f * 3.14159265f * 60.0f;

    int layer = -1;       // -1 = auto (static→non-moving, dynamic→moving)
    uint64_t userData = 0;
};

/// Constraint creation options.
struct ConstraintOptions {
    enum Type {
        Distance,
        Point,
        Hinge,
        Fixed,
        Slider,
        Wheel,    // Box2D-style: suspension axis (slider + spring) + wheel pin (hinge + motor)
        Cone,            // point + limited swing about a twist axis (e.g. ragdoll shoulder)
        SwingTwist,      // ragdoll joint: independent swing-Y/swing-Z cone + twist limits
        Pulley,          // rope over two fixed pivots: len(b1..f1) + ratio*len(b2..f2) constrained
        Gear,            // couples the rotation of two hinge constraints by a gear ratio
        RackAndPinion,   // couples a hinge (pinion) to a slider (rack) by a ratio
    };
    Type type = Distance;
    JPH::BodyID body1;
    JPH::BodyID body2;          // may be invalid → attach to world

    // Anchor points (world-space by default; for fixed/slider some use this as ref)
    JPH::RVec3 point1{0, 0, 0};
    JPH::RVec3 point2{0, 0, 0};

    // Distance
    float minDistance = -1.0f;  // <0 = use rest length
    float maxDistance = -1.0f;

    // Hinge / slider axis (world-space)
    JPH::Vec3 axis{0, 1, 0};
    float limitMin = 0.0f;
    float limitMax = 0.0f;
    bool hasLimits = false;

    // Breaking (Jolt: 0 = never break)
    float breakingImpulse = 0.0f;

    // Collide-connected (default false — common to want no self-collision)
    bool collideConnected = false;

    // Wheel-specific (Box2D v3 surface).
    JPH::Vec3 wheelSuspensionAxis{0, 1, 0};   // suspension translation axis (world)
    JPH::Vec3 wheelHingeAxis{0, 0, 1};        // wheel rotation axis (world; 2D = +Z)
    float wheelHertz = 2.0f;                  // suspension spring frequency (Hz); 0 disables
    float wheelDampingRatio = 0.7f;           // 0 = undamped, 1 = critical
    bool  wheelHasTranslationLimits = false;
    float wheelLowerTranslation = 0.0f;
    float wheelUpperTranslation = 0.0f;
    bool  wheelEnableMotor = false;
    float wheelMotorSpeed = 0.0f;             // rad/s
    float wheelMaxMotorTorque = 0.0f;         // N·m

    // --- Cone (uses point1/point2 as the pivot, `axis` as the twist axis) ---
    float coneHalfAngle = 0.0f;               // max swing half-angle (radians)

    // --- SwingTwist (uses point1/point2 as the pivot, `axis` as the twist axis) ---
    JPH::Vec3 planeAxis{0, 1, 0};             // swing plane axis (world; ⟂ to twist axis)
    float normalHalfConeAngle = 0.0f;         // swing-Y half-angle (radians)
    float planeHalfConeAngle = 0.0f;          // swing-Z half-angle (radians)
    float twistMinAngle = 0.0f;               // radians, [-π, π]
    float twistMaxAngle = 0.0f;               // radians, [-π, π]
    float maxFrictionTorque = 0.0f;           // N·m friction when unpowered

    // --- Pulley ---
    JPH::RVec3 bodyPoint1{0, 0, 0};           // attachment on body1 (world)
    JPH::RVec3 fixedPoint1{0, 0, 0};          // fixed pivot 1 (world)
    JPH::RVec3 bodyPoint2{0, 0, 0};           // attachment on body2 (world)
    JPH::RVec3 fixedPoint2{0, 0, 0};          // fixed pivot 2 (world)
    float ratio = 1.0f;                       // pulley/gear/rack ratio
    float minLength = 0.0f;                   // pulley min total length (<0 = current)
    float maxLength = -1.0f;                  // pulley max total length (<0 = current)

    // --- Gear / RackAndPinion ---
    // Gear:  hingeAxis1/hingeAxis2 = the two gears' rotation axes (world)
    // Rack:  hingeAxis1 = pinion rotation axis, hingeAxis2 = rack slide axis (world)
    JPH::Vec3 hingeAxis1{1, 0, 0};
    JPH::Vec3 hingeAxis2{1, 0, 0};
    // Handles of the two constraints the gear/rack couples (returned by an
    // earlier createConstraint). Gear: two hinges. Rack: pinion hinge + rack slider.
    uint32_t dependentConstraint1 = 0;
    uint32_t dependentConstraint2 = 0;
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

    /// Step the simulation synchronously on the calling thread. Use when no
    /// worker thread is active (e.g. headless mode).
    void stepInline();

    /// Shut down the physics thread and clean up.
    void shutdown();

    /// Set simulation time step (default 1/60).
    void setTimeStep(float dt) { timeStep_ = dt; }
    float timeStep() const { return timeStep_; }

    /// Destroy every body and constraint in this world. Filter callback receives
    /// each BodyID before destruction; if it returns true the caller wants
    /// further bookkeeping (e.g. tag map cleanup).
    void destroyAll(const std::function<void(JPH::BodyID)>& onBodyDestroyed = {});

    /// Set gravity.
    void setGravity(float x, float y, float z);
    JPH::Vec3 gravity() const;

    // --- Layers (call only when idle, ideally just after init) ---

    /// Reset and configure named collision layers. Up to kMaxLayers names;
    /// matrix is row-major flat array of size n*n (true=collide).
    /// Default layers ("static", "moving") are reset.
    static constexpr int kMaxLayers = 16;
    bool configureLayers(const std::vector<std::string>& names,
                         const std::vector<bool>& matrix);
    int layerIndex(const std::string& name) const;
    const std::string& layerName(int idx) const;
    int numLayers() const { return numLayers_; }

    // --- Body management (call only when idle) ---

    /// Unified body creation. Returns invalid BodyID on failure.
    JPH::BodyID createBody(const BodyOptions& opts);

    /// Legacy convenience overloads.
    JPH::BodyID createBox(JPH::RVec3 position, JPH::Quat rotation,
                          JPH::Vec3 halfExtents, bool isStatic,
                          float friction = 0.5f, float restitution = 0.3f);
    JPH::BodyID createSphere(JPH::RVec3 position, JPH::Quat rotation,
                             float radius, bool isStatic,
                             float friction = 0.5f, float restitution = 0.3f);
    JPH::BodyID createCapsule(JPH::RVec3 position, JPH::Quat rotation,
                              float halfHeight, float radius, bool isStatic,
                              float friction = 0.5f, float restitution = 0.3f);
    JPH::BodyID createCylinder(JPH::RVec3 position, JPH::Quat rotation,
                               float halfHeight, float radius, bool isStatic,
                               float friction = 0.5f, float restitution = 0.3f);

    /// Remove and destroy a body. Also destroys constraints attached to it.
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
    bool isSensor(JPH::BodyID id) const;

    /// Change a body's collision layer post-creation. Triggers a broadphase
    /// notification so future queries reflect the new layer. Cost is
    /// effectively a remove+add in the broadphase for that one body; cheap.
    void setLayer(JPH::BodyID id, int layer);

    /// Set kinematic motion type. A kinematic body is moved via velocity/
    /// MoveKinematic and pushes dynamic bodies but is not pushed back.
    void setKinematic(JPH::BodyID id);

    /// Move a kinematic body to a target transform over the given timestep.
    /// Internally sets the linear/angular velocity such that integration
    /// reaches the target in dt seconds. Use for stable kinematic bodies that
    /// need to interact with dynamic ones.
    void moveKinematic(JPH::BodyID id, JPH::RVec3 targetPos, JPH::Quat targetRot, float dt);

    void setUserData(JPH::BodyID id, uint64_t data);
    uint64_t getUserData(JPH::BodyID id) const;

    // --- Constraints ---

    /// Returns a non-zero handle on success, 0 on failure.
    uint32_t createConstraint(const ConstraintOptions& opts);
    void destroyConstraint(uint32_t handle);
    void setConstraintEnabled(uint32_t handle, bool enabled);
    /// Adjust a wheel constraint's motor at runtime (no-op for non-wheel handles).
    void setWheelMotor(uint32_t handle, bool enabled, float speed, float maxTorque);
    /// Set/get a constraint's breaking impulse threshold (0 = never break).
    /// When the constraint's applied position impulse exceeds this in a step the
    /// constraint is auto-disabled and reported via drainBrokenConstraints().
    void setConstraintBreakingImpulse(uint32_t handle, float threshold);
    float getConstraintBreakingImpulse(uint32_t handle) const;
    /// Returns and clears any constraint-broken events accumulated since last call.
    std::vector<uint32_t> drainBrokenConstraints();

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
    void rebuildLayerFilters();

    // listener_ must outlive physicsSystem_ — declare first so it's destroyed
    // last (members destroy in reverse declaration order).
    struct ListenerImpl;
    std::unique_ptr<ListenerImpl> listener_;

    JPH::PhysicsSystem physicsSystem_;
    std::unique_ptr<JPH::TempAllocatorImpl> tempAllocator_;
    std::unique_ptr<JPH::JobSystemThreadPool> jobSystem_;

    // Broadphase / collision layer config (stored as members for lifetime)
    struct Layers;
    std::unique_ptr<Layers> layers_;
    std::vector<std::string> layerNames_;
    std::vector<bool> layerMatrix_;     // flat n*n
    int numLayers_ = 0;

    // Constraint registry. Composite constraints (e.g. wheel) hold a primary
    // ref + an optional secondary ref under one handle.
    struct ConstraintEntry {
        JPH::Ref<JPH::Constraint> ref;
        JPH::Ref<JPH::Constraint> ref2;
        float breakingImpulse = 0.0f;  // 0 = never break
    };
    std::unordered_map<uint32_t, ConstraintEntry> constraints_;
    uint32_t nextConstraintHandle_ = 1;

    // Constraints that broke (exceeded breaking impulse) since the last drain.
    std::vector<uint32_t> brokenConstraints_;
    // After each Update(), disable any constraint whose applied impulse exceeded
    // its breaking threshold and record its handle. Called on the physics thread
    // (consumeStep) or inline (stepInline) — single-owner, no locking.
    void checkBrokenConstraints();

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
