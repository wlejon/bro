#pragma once

#include <physics/physics_types.h>

namespace bro::physics {

class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();

    /// The most bodies one world may be sized for. Jolt allocates its body
    /// tables for maxBodies up front, and its BodyID index tops out at 2^23;
    /// a million bodies is far past any scene this runtime draws.
    static constexpr int kMaxBodies = 1 << 20;

    /// Initialize the physics system. Call once before use.
    /// maxBodies is clamped to [1, kMaxBodies].
    /// contactCapacity sizes the per-step contact-event buffer (clamped to
    /// [16, 65536]); 0 = auto (4*maxBodies, min 1024). Overflow drops events
    /// and is reported via drainContactEvents().
    bool init(int maxBodies = 4096, int contactCapacity = 0);

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

    /// Wait out a step the physics thread is still running. A query walks the
    /// broad phase, which Update() rebuilds, so one made mid-step (a step
    /// overrunning its frame) read a tree being changed under it.
    void awaitStepForQuery() const;

    /// Returns true if there are dynamic bodies or characters requiring simulation.
    bool hasActiveBodies() const;

    /// Return the appropriate BodyInterface (NoLock when idle, locking otherwise).
    JPH::BodyInterface& getBodyInterface();
    const JPH::BodyInterface& getBodyInterface() const;

    /// Step the simulation synchronously on the calling thread. Use when no
    /// worker thread is active (e.g. headless mode).
    void stepInline();

    /// Shut down the physics thread and clean up.
    void shutdown();

    /// Set simulation time step (default 1/60).
    void setTimeStep(float dt) { timeStep_ = dt; }
    float timeStep() const { return timeStep_; }

    // --- Render interpolation (fixed-step physics vs uncapped rendering) ---
    //
    // When enabled, every step snapshots the pre-step transform of each active
    // body; getRenderTransform() blends previous→current by the render alpha
    // (the accumulator fraction the engine sets each frame). Render-side
    // consumers (PhysicsNode scene sync, opt-in JS reads) use it; physics
    // queries (getPosition/raycast/...) always return the true stepped state.
    // Disabled by default — matching Godot/Unity, and existing content asserts
    // exact post-step transforms through the render path.
    //
    // Sleeping bodies leave the snapshot map (no jitter); teleports via
    // setPosition/setRotation drop the snapshot so the body snaps, not glides.

    /// Enable/disable transform interpolation for render-side consumers.
    void setInterpolation(bool enabled);
    bool interpolation() const { return interpolate_; }

    /// Fraction [0,1] of the fixed step that has accumulated since the last
    /// step — set by the engine once per rendered frame.
    void setRenderAlpha(float alpha);

    /// Transform for RENDERING: interpolated between the previous and current
    /// step when interpolation is on (and a snapshot exists); the true current
    /// transform otherwise. Position lerp + quaternion slerp (normalized).
    void getRenderTransform(JPH::BodyID id, JPH::RVec3& outPos, JPH::Quat& outRot) const;

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
    /// A stale/already-destroyed id is a safe no-op (verified before Jolt's
    /// DestroyBody, which would corrupt the body-manager free list).
    /// Destroying a ragdoll part destroys the WHOLE ragdoll; onBodyDestroyed
    /// (when provided) is invoked for every body actually destroyed — one for
    /// a plain body, all part bodies for a ragdoll — so callers can evict
    /// their own per-body bookkeeping (e.g. the JS tag registries).
    void destroyBody(JPH::BodyID id,
                     const std::function<void(JPH::BodyID)>& onBodyDestroyed = {});

    /// True while `id` refers to a live body (index + sequence check). Used
    /// by callers to distinguish a destroy that was refused (e.g. a
    /// character's inner body) from one that happened.
    bool bodyExists(JPH::BodyID id) const;

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

    /// Switch a body between static and dynamic. Preserves the body's object
    /// layer, except that a body made dynamic while on layer 0 (the only
    /// layer mapped to the NON_MOVING broadphase tree) moves to the default
    /// moving layer (1).
    void setMotionType(JPH::BodyID id, bool isStatic);
    void activate(JPH::BodyID id);
    bool isActive(JPH::BodyID id) const;
    bool isSensor(JPH::BodyID id) const;

    /// Install/replace the field override on a sensor body (see AreaOverride).
    /// Takes effect immediately for bodies already overlapping the sensor —
    /// overlap membership is tracked for every sensor, so a later
    /// setAreaOverride sees them. No-op (warn) for non-sensor bodies.
    void setAreaOverride(JPH::BodyID id, const AreaOverride& o);
    /// Remove a sensor's field override (restores damping on affected bodies).
    void clearAreaOverride(JPH::BodyID id);
    /// Read back the override; false if the body has none.
    bool getAreaOverride(JPH::BodyID id, AreaOverride& out) const;

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

    /// Per-body friction/restitution combine mode (see CombineMode). Applied
    /// in the contact listener by overriding ContactSettings' combined values
    /// whenever either body of a pair has a non-default mode; the higher mode
    /// of the two wins. Call only when idle.
    void setFrictionCombine(JPH::BodyID id, CombineMode mode);
    void setRestitutionCombine(JPH::BodyID id, CombineMode mode);

    // --- Runtime body property mutation (call only when idle) ---

    /// Set a dynamic body's mass directly (kg). Inertia is recomputed from
    /// the shape and scaled to the new mass (MassProperties::ScaleToMass),
    /// respecting the body's allowed DOFs. Wakes the body. No-op for
    /// static/kinematic/soft bodies and non-positive masses.
    void setMass(JPH::BodyID id, float massKg);
    /// A dynamic rigid body's mass in kg; 0 for static/kinematic/soft/unknown.
    float getMass(JPH::BodyID id) const;

    /// Per-body damping (fraction of velocity removed per second, Jolt
    /// semantics: v *= max(0, 1 - damping*dt) each step). Negative input is
    /// ignored. Does not wake the body.
    void setLinearDamping(JPH::BodyID id, float damping);
    float getLinearDamping(JPH::BodyID id) const;
    void setAngularDamping(JPH::BodyID id, float damping);
    float getAngularDamping(JPH::BodyID id) const;

    /// Per-body gravity multiplier (1 = normal, 0 = floats). Wakes the body
    /// so the change is immediately visible.
    void setGravityFactor(JPH::BodyID id, float factor);
    float getGravityFactor(JPH::BodyID id) const;

    /// Runtime friction/restitution (affect NEW contact solving; existing
    /// resting contacts pick the change up on their next solver update).
    void setFriction(JPH::BodyID id, float friction);
    float getFriction(JPH::BodyID id) const;
    void setRestitution(JPH::BodyID id, float restitution);
    float getRestitution(JPH::BodyID id) const;

    // --- Constraints ---

    /// Returns a non-zero handle on success, 0 on failure.
    uint32_t createConstraint(const ConstraintOptions& opts);
    void destroyConstraint(uint32_t handle);
    void setConstraintEnabled(uint32_t handle, bool enabled);
    bool isConstraintEnabled(uint32_t handle) const;
    JPH::Constraint* getConstraint(uint32_t handle) const;
    /// Adjust a wheel constraint's motor at runtime (no-op for non-wheel handles).
    void setWheelMotor(uint32_t handle, bool enabled, float speed, float maxTorque);
    /// Configure/steer a constraint motor at runtime. Works on hinge and
    /// slider constraints (axis ignored) and sixdof/wheel constraints (per
    /// axis 0..5). Wakes both bodies so the change takes effect immediately.
    /// Returns false for unknown handles or unsupported constraint types.
    bool setConstraintMotor(uint32_t handle, const MotorOptions& motor);
    /// Set/get a constraint's breaking impulse threshold (0 = never break).
    /// When the constraint's applied position impulse exceeds this in a step the
    /// constraint is auto-disabled and reported via drainBrokenConstraints().
    void setConstraintBreakingImpulse(uint32_t handle, float threshold);
    float getConstraintBreakingImpulse(uint32_t handle) const;
    /// Returns and clears any constraint-broken events accumulated since last call.
    std::vector<uint32_t> drainBrokenConstraints();

    // --- Character controllers (call only when idle) ---
    //
    // Godot move_and_slide-style: the app sets a desired velocity; every fixed
    // step (immediately before the world step, on the caller's thread) the
    // character does a CharacterVirtual::ExtendedUpdate — slide along walls,
    // walk up steps <= stepUp, snap down <= stickToFloor, push dynamic bodies,
    // ride moving platforms. Gravity integrates automatically while
    // unsupported; while supported the desired velocity applies directly (a
    // positive up component launches a jump).

    /// Returns a non-zero handle on success, 0 on failure.
    uint32_t createCharacter(const CharacterOptions& opts);
    void destroyCharacter(uint32_t handle);

    /// Set the desired velocity applied each fixed step (persists until changed).
    void setCharacterVelocity(uint32_t handle, JPH::Vec3 v);
    /// Teleport (no sweep; keeps the current velocity).
    void setCharacterPosition(uint32_t handle, JPH::RVec3 pos);
    /// Snapshot position/velocity/ground state. False for an unknown handle.
    bool getCharacterState(uint32_t handle, CharacterState& out) const;

    /// Switch the character's shape (crouch/stance — CharacterVirtual::
    /// SetShape). Takes the same descriptor as body creation, restricted to
    /// non-mesh shapes. Feet-planted semantics: the position (shape center)
    /// shifts along `up` so the new shape's bottom lands where the old one's
    /// was. Jolt checks the new shape for collision first and the switch
    /// FAILS (returns false, position restored) when there is no room — e.g.
    /// standing up under a low ceiling. On success the inner body's shape
    /// (if any) is updated too.
    bool setCharacterShape(uint32_t handle, const BodyOptions& shapeOpts);

    /// The character's inner rigid body id (invalid if none / unknown handle).
    JPH::BodyID characterInnerBody(uint32_t handle) const;

    // --- Wheeled vehicles (call only when idle) ---
    //
    // Jolt VehicleConstraint + WheeledVehicleController on an existing dynamic
    // chassis body. The constraint registers as a PhysicsStepListener, so once
    // added it steps inside PhysicsSystem::Update automatically — no per-step
    // bookkeeping here, and the phase contract is satisfied by construction.

    /// Returns a non-zero handle on success, 0 on failure.
    uint32_t createVehicle(const VehicleOptions& opts);
    void destroyVehicle(uint32_t handle);

    /// Driver input (Jolt SetDriverInput shape): forward -1..1 (auto
    /// transmission; sign is desired direction), right -1..1 (1 = steer
    /// right), brake / handBrake 0..1. Persists until changed. Any non-zero
    /// input wakes the chassis (same rule as constraint motors).
    void setVehicleInput(uint32_t handle, float forward, float right,
                         float brake, float handBrake);

    /// Tracked vehicles only (no-op otherwise): explicit per-track drive
    /// ratios, Jolt TrackedVehicleController::SetDriverInput semantics.
    /// leftRatio / rightRatio -1..1 multiply each track's rotation rate
    /// (1/1 = straight, 0.6 on the inside track = gentle turn, -1/1 = pivot
    /// turn). Ratios are clamped away from zero (Jolt asserts on 0).
    void setVehicleTrackInput(uint32_t handle, float forward, float leftRatio,
                              float rightRatio, float brake);

    /// Motorcycles only: enable/disable the lean spring that holds the bike
    /// upright (disable to let it fall over). Returns false for an unknown
    /// handle or a non-motorcycle vehicle.
    bool setVehicleLeanController(uint32_t handle, bool enabled);

    /// Manual-transmission gear select (-1 reverse, 0 neutral, 1+ forward).
    /// Only meaningful when transmission.manual is set.
    void setVehicleGear(uint32_t handle, int gear, float clutchFriction = 1.0f);

    /// Number of wheels, or -1 for an unknown handle.
    int vehicleWheelCount(uint32_t handle) const;
    /// Per-wheel render state. False for unknown handle / wheel index.
    bool getVehicleWheelState(uint32_t handle, int wheel, VehicleWheelState& out) const;
    /// Speed / RPM / gear snapshot. False for an unknown handle.
    bool getVehicleState(uint32_t handle, VehicleState& out) const;
    /// The chassis BodyID (invalid for an unknown handle).
    JPH::BodyID vehicleBody(uint32_t handle) const;

    // --- Ragdolls (call only when idle) ---
    //
    // Jolt Ragdoll: a tree of dynamic bodies joined by swing-twist (or fixed)
    // constraints, teleportable/drivable as a unit. Part bodies are ordinary
    // dynamic bodies in this world — every body API (impulses, velocities,
    // raycast hits, contact events) works on them. Destroying a part body via
    // destroyBody destroys the WHOLE ragdoll (bodies + constraints are a unit).
    //
    // Pose format: one RagdollPartState per part, world space, part order =
    // the parts array at creation.

    /// Returns a non-zero handle on success, 0 on failure (empty parts,
    /// parent ordering violation, bad shape, out of bodies).
    uint32_t createRagdoll(const RagdollOptions& opts);
    void destroyRagdoll(uint32_t handle,
                        const std::function<void(JPH::BodyID)>& onBodyDestroyed = nullptr);

    /// Number of parts, or -1 for an unknown handle.
    int ragdollPartCount(uint32_t handle) const;
    /// A part's BodyID (invalid for unknown handle / part index).
    JPH::BodyID ragdollPartBody(uint32_t handle, int part) const;
    /// Parent part index (-1 = root or unknown).
    int ragdollPartParent(uint32_t handle, int part) const;

    /// Snapshot every part's world transform. False for an unknown handle.
    bool getRagdollPose(uint32_t handle, std::vector<RagdollPartState>& out) const;
    /// Teleport all parts (no sweep, keeps velocities, does not wake).
    bool setRagdollPose(uint32_t handle, const std::vector<RagdollPartState>& pose);

    /// Power the swing-twist joints toward the given pose's per-joint relative
    /// rotations (Jolt DriveToPoseUsingMotors semantics: position motors on
    /// swing + twist; the ROOT is not driven — pin it kinematically if you
    /// need root tracking). dt-independent: motors persist until stopped.
    /// `motor` overrides the creation-time spring when non-null.
    bool driveRagdollToPose(uint32_t handle, const std::vector<RagdollPartState>& pose,
                            const RagdollMotorOptions* motor = nullptr);
    /// Set body velocities so every part reaches its target transform in dt
    /// seconds (Jolt DriveToPoseUsingKinematics — hard tracking, ignores
    /// joint limits' softness; re-issue each step).
    bool driveRagdollToPoseKinematic(uint32_t handle,
                                     const std::vector<RagdollPartState>& pose, float dt);
    /// Turn all joint drive motors off (go limp after driveRagdollToPose).
    void stopRagdollDrive(uint32_t handle);

    /// Impulse on every part body (center of mass of each).
    void addRagdollImpulse(uint32_t handle, JPH::Vec3 impulse);
    void activateRagdoll(uint32_t handle);
    void deactivateRagdoll(uint32_t handle);
    /// True when any part body is awake.
    bool isRagdollActive(uint32_t handle) const;

    // --- Soft bodies (call only when idle) ---
    //
    // Jolt soft bodies are REGULAR bodies (SoftBodyCreationSettings →
    // CreateAndAddSoftBody) whose motion properties hold the vertex state.
    // The body id composes with every body/query API — raycasts hit it,
    // impulses move it, contact events report it. The registry keeps the
    // shared-settings Ref + metadata under a handle; destroying the body
    // (destroyBody/destroyAll/shutdown) evicts the registry entry.

    /// Returns a non-zero handle on success, 0 on failure (bad grid/mesh,
    /// out of bodies).
    uint32_t createSoftBody(const SoftBodyOptions& opts);
    void destroySoftBody(uint32_t handle);

    /// The soft body's BodyID (invalid for an unknown handle).
    JPH::BodyID softBodyBody(uint32_t handle) const;
    /// Number of vertices, or -1 for an unknown handle.
    int softBodyVertexCount(uint32_t handle) const;

    /// Snapshot every vertex position in WORLD space, packed xyz. False for
    /// an unknown handle.
    bool getSoftBodyVertices(uint32_t handle, std::vector<float>& outXyz) const;

    /// Rest-pose topology: local-space vertex positions (xyz triples, the
    /// creation-time rest shape) + face indices (triangle list). Stable for
    /// the body's lifetime — the render-mesh blueprint for scene sync.
    bool softBodyTopology(uint32_t handle, std::vector<float>& outXyz,
                          std::vector<uint32_t>& outIndices) const;

    /// Move one vertex to a world-space position (grab interactions). Resets
    /// the vertex velocity and wakes the body. Note Jolt's caveat: directly
    /// placed vertices can tunnel — prefer velocities for fast drags.
    bool setSoftBodyVertexPosition(uint32_t handle, int index, JPH::RVec3 pos);
    /// Set one vertex's velocity (world space) and wake the body.
    bool setSoftBodyVertexVelocity(uint32_t handle, int index, JPH::Vec3 vel);
    /// Freeze / release one vertex (invMass 0 ↔ the even mass split).
    /// Pinning zeroes the vertex velocity; both directions wake the body.
    bool pinSoftBodyVertex(uint32_t handle, int index, bool pinned);

    // --- Queries (call only when idle) ---

    /// Cast a ray (narrow phase: exact shape geometry, real hit positions and
    /// surface normals). One hit per body (earliest contact), sorted by
    /// fraction.
    std::vector<RayHit> raycast(JPH::RVec3 origin, JPH::Vec3 direction,
                                float maxDistance = 1000.0f,
                                const QueryFilter& filter = {}) const;

    /// Get the closest narrow-phase hit along a ray; false if nothing was hit.
    bool raycastClosest(JPH::RVec3 origin, JPH::Vec3 direction,
                        RayHit& outHit, float maxDistance = 1000.0f,
                        const QueryFilter& filter = {}) const;

    /// Sweep a convex shape (the shape/position/rotation fields of `shape`)
    /// along direction*maxDistance. One hit per body (earliest contact),
    /// sorted by fraction. Non-convex query shapes return no hits.
    std::vector<ShapeCastHit> castShape(const BodyOptions& shape, JPH::Vec3 direction,
                                        float maxDistance,
                                        const QueryFilter& filter = {}) const;

    /// Closest shape-cast hit only; returns false if nothing was hit.
    bool castShapeClosest(const BodyOptions& shape, JPH::Vec3 direction,
                          float maxDistance, ShapeCastHit& outHit,
                          const QueryFilter& filter = {}) const;

    /// All bodies overlapping a convex shape at its transform. One hit per
    /// body — the deepest contact.
    std::vector<OverlapHit> overlapShape(const BodyOptions& shape,
                                         const QueryFilter& filter = {}) const;

    /// All bodies containing a point (shapes are treated as solid).
    std::vector<JPH::BodyID> overlapPoint(JPH::RVec3 point,
                                          const QueryFilter& filter = {}) const;

    /// One colliding sub-shape pair result from a penetration query.
    struct PenetrationHit {
        JPH::BodyID body1;
        uint32_t subShape1 = 0;
        JPH::BodyID body2;
        uint32_t subShape2 = 0;
        float depth = 0.0f;
        JPH::Vec3 position{0, 0, 0};
    };

    struct PenetrationOptions {
        PenetrationOptions()
            : minDepth(-FLT_MAX), maxSeparation(0.0f), layerMask(0) {}
        std::vector<JPH::BodyID> bodies;
        std::vector<std::pair<JPH::BodyID, JPH::BodyID>> ignorePairs;
        float minDepth;      // unbounded: depth > 0 unless maxSeparation > 0
        float maxSeparation;
        int layerMask;
    };

    struct BodyTransformUpdate {
        JPH::BodyID id;
        JPH::RVec3 position;
        JPH::Quat rotation;
        JPH::Vec3 scale{1, 1, 1};   // query scale, applied by penetrations() only
    };

    /// Update transforms for a batch of bodies in one operation without waking them.
    void setTransforms(const std::vector<BodyTransformUpdate>& updates);

    /// Set position and rotation together for one body without waking it.
    void setTransform(JPH::BodyID id, JPH::RVec3 pos, JPH::Quat rot);

    /// Query-only narrow-phase penetration query across a set of bodies (or all).
    /// Returns every penetrating sub-shape pair with exact depth and contact position.
    std::vector<PenetrationHit> penetrations(const PenetrationOptions& options = {}) const;

    /// Snapshot of one static body for nav-grid baking.
    struct StaticBodyInfo {
        JPH::BodyID id;
        JPH::Vec3 min, max;   // world-space AABB
        int layer = 0;        // object layer index
        bool isSensor = false;
    };

    /// Enumerate every static body with its world-space AABB (call only when
    /// idle). Used to derive navigation obstacles from collision geometry.
    std::vector<StaticBodyInfo> collectStaticBodies() const;

    /// Append the world-space triangle geometry of every static, non-sensor
    /// body whose layer bit is in `layerMask` (call only when idle). Vertices
    /// are appended to `outXyz` as xyz triples and `outIndices` gets matching
    /// sequential triangle indices (offset past any existing soup), so the
    /// output concatenates cleanly with other sources. Mesh and heightfield
    /// shapes yield their exact triangles; primitive/convex shapes yield
    /// Jolt's coarse triangulation (a box is 12 triangles, spheres/capsules a
    /// low-LOD tessellation) — good enough for navmesh baking, not rendering.
    /// Used to bake polygon navmeshes from collision geometry.
    void collectStaticTriangles(std::vector<float>& outXyz,
                                std::vector<uint32_t>& outIndices,
                                uint32_t layerMask = 0xffffffffu) const;

    // --- Contact events ---

    /// Swap and return contact events from the last step. Clears the buffer.
    /// If `overflowed` is non-null it is set to true when the fixed-capacity
    /// per-step contact buffer overflowed during any step since the last
    /// drain — events (including sensor exits) were dropped, so cached
    /// contact/trigger state derived from the stream may be stale.
    std::vector<ContactEvent> drainContactEvents(bool* overflowed = nullptr);

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
        // SixDOF position-motor rotation targets (rad, per rotation axis).
        // Jolt stores the target as a quaternion; we keep the per-axis angles
        // so single-axis updates can rebuild it without decomposition.
        float sixDofRotTarget[3] = {0, 0, 0};
        // collideConnected=false bookkeeping: the disabled body pair in
        // pairFilter_ (removed when the constraint goes away).
        uint64_t pairKey = 0;
        bool hasPair = false;
    };
    std::unordered_map<uint32_t, ConstraintEntry> constraints_;
    uint32_t nextConstraintHandle_ = 1;

    // Pairwise no-collide filter backing ConstraintOptions::collideConnected
    // (default false = constrained bodies don't collide with each other).
    JPH::Ref<PairGroupFilter> pairFilter_;
    // Register/unregister the constraint's body pair with pairFilter_.
    void applyCollideConnected(const ConstraintOptions& opts, ConstraintEntry& entry);
    void evictConstraintPair(const ConstraintEntry& entry);

    // Character registry. CharacterVirtuals are not tracked by the Jolt
    // system — updateCharacters() steps them just before each world step,
    // always on the thread that owns the world at that moment (main thread
    // while idle), so the registry needs no locking.
    struct CharacterEntry {
        JPH::Ref<JPH::CharacterVirtual> character;
        JPH::Vec3 desiredVelocity{0, 0, 0};
        float stepUp = 0.4f;
        float stickToFloor = 0.5f;
        int layer = 1;
    };
    // Ordered map: updateCharacters iterates it, and character-vs-body push
    // order must be deterministic across runs (unordered_map iteration order
    // varies with the allocator, breaking cross-run determinism).
    std::map<uint32_t, CharacterEntry> characters_;
    uint32_t nextCharacterHandle_ = 1;
    // Character-vs-character collision (Jolt CharacterVsCharacterCollisionSimple):
    // every CharacterVirtual registers here at creation so characters collide
    // with each other instead of ghosting through. Jolt's Simple checker is
    // not thread-safe, which is fine — updateCharacters steps them serially
    // on the phase-owning thread. Entries are removed on character destroy.
    std::unique_ptr<JPH::CharacterVsCharacterCollisionSimple> charVsChar_;
    // Runs each character's velocity update + ExtendedUpdate with the fixed
    // dt. Called from signalStep (main thread, before the phase flip) and
    // stepInline — never concurrently with a world step.
    void updateCharacters(float dt);

    // Vehicle registry. Each VehicleConstraint is registered with the system
    // both as a constraint and as a step listener; removeVehicleFromSystem
    // detaches both together so the listener can never outlive the constraint.
    struct VehicleEntry {
        JPH::Ref<JPH::VehicleConstraint> constraint;
        VehicleOptions::Controller controller = VehicleOptions::ControllerWheeled;
        JPH::BodyID body;  // chassis
    };
    std::unordered_map<uint32_t, VehicleEntry> vehicles_;
    uint32_t nextVehicleHandle_ = 1;
    void removeVehicleFromSystem(VehicleEntry& e);

    // Ragdoll registry. The Ragdoll owns its bodies and constraints as a
    // unit: RemoveFromPhysicsSystem detaches everything together, and the
    // final Ref release (~Ragdoll) destroys the part bodies — so ragdoll
    // bodies must never reach the generic body sweeps (destroyAll/shutdown
    // remove ragdolls FIRST), and their constraints never live in constraints_.
    struct RagdollEntry {
        JPH::Ref<JPH::Ragdoll> ragdoll;
        JPH::Ref<JPH::RagdollSettings> settings;
        std::vector<int> parentIndex;   // per part; -1 = root
    };
    std::unordered_map<uint32_t, RagdollEntry> ragdolls_;
    uint32_t nextRagdollHandle_ = 1;
    uint32_t nextRagdollGroup_ = 1;     // unique CollisionGroup ID per ragdoll
    // Detach a ragdoll (and any user constraints attached to its parts) from
    // the system. The caller erases the entry, which destroys the bodies.
    void removeRagdollFromSystem(RagdollEntry& e);
    // Remove + erase every registry constraint referencing `id` (shared by
    // destroyBody and removeRagdollFromSystem).
    void removeConstraintsReferencing(JPH::BodyID id);

    // Soft-body registry. The BODY is owned by the body manager like any
    // other body (the generic sweeps destroy it); the entry only pins the
    // shared settings Ref + metadata, so eviction (destroyBody/destroyAll/
    // shutdown) is pure bookkeeping with no ordering constraint.
    struct SoftBodyEntry {
        JPH::Ref<JPH::SoftBodySharedSettings> settings;
        JPH::BodyID body;
        float defaultInvMass = 1.0f;  // per-vertex invMass for unpinning
    };
    std::unordered_map<uint32_t, SoftBodyEntry> softBodies_;
    uint32_t nextSoftBodyHandle_ = 1;

    // --- Area field overrides (see AreaOverride) ---
    //
    // Threading contract (the sensorFlags pattern): all maps below are
    // mutated ONLY between steps on the phase-owning thread — overlap
    // membership from the drained contact stream (updateAreaOverlaps), the
    // override table from the idle-only setters. During a step they are
    // immutable, so the step listener reads them lock-free from the physics
    // job that runs OnStep. Membership is tracked for EVERY sensor (not just
    // ones with overrides) so setAreaOverride can take effect on bodies
    // already inside. Keys are BodyID index+sequence numbers.
    struct AreaStepListener;
    std::unique_ptr<AreaStepListener> areaListener_;
    std::unordered_map<uint32_t, AreaOverride> areas_;  // sensor → override
    struct BodyAreaState {
        // Overlapping sensor keys, sorted highest priority first (ties:
        // lower key). Sensors without an override sort as priority 0 and are
        // skipped when fields are computed. Refcounted: Jolt reports contact
        // add/remove per SUB-SHAPE pair, so a compound sensor can hold
        // several concurrent contacts with one body — membership ends when
        // the last one goes.
        struct AreaRef { uint32_t key; int refs; };
        std::vector<AreaRef> areas;
        // Body's own damping, saved when an area override first replaces it
        // (restored on exit; runtime damping setters update the base).
        float baseLinearDamping = 0.0f;
        float baseAngularDamping = 0.0f;
        bool linOverridden = false;
        bool angOverridden = false;
    };
    std::unordered_map<uint32_t, BodyAreaState> bodyAreas_;  // body → state
    // Fold one step's contact events into the membership maps + damping.
    // Called from consumeStep/stepInline, phase-owning thread.
    void updateAreaOverlaps(const std::vector<ContactEvent>& events);
    // Re-sort a body's area list (priority desc, key asc).
    void sortBodyAreas(BodyAreaState& st);
    // Re-sort + re-apply damping for every body overlapping `areaKey`
    // (override installed/replaced/cleared while bodies are inside).
    void refreshBodiesTouchingArea(uint32_t areaKey);
    // Recompute + apply the body's effective damping from its area list;
    // restores the saved base when no area overrides a channel.
    void applyAreaDamping(JPH::BodyID id, BodyAreaState& st);
    // Gravity deltas for all overlapped dynamic bodies; runs INSIDE the step
    // (AreaStepListener::OnStep) with dt = the fixed step.
    void applyAreaGravity(float dt);
    // Body/area destruction bookkeeping (destroyBody/destroyAll).
    void evictAreaBookkeeping(JPH::BodyID id);

    // Constraints that broke (exceeded breaking impulse) since the last drain.
    std::vector<uint32_t> brokenConstraints_;
    // After each Update(), disable any constraint whose applied impulse exceeded
    // its breaking threshold and record its handle. Called on the physics thread
    // (consumeStep) or inline (stepInline) — single-owner, no locking.
    void checkBrokenConstraints();

    // Thread handshake. The mutex guards only the phase word and the shutdown
    // flag — Jolt's Update() always runs outside it, and body access needs no
    // lock because the phase grants exclusive ownership. shutdownRequested is
    // separate from the phase so a Busy→Done transition can never erase a
    // shutdown request, and the worker's wait predicate can block in both
    // resting phases (Idle and Done) instead of spinning through one of them.
    struct Shared {
        mutable std::mutex m;
        mutable std::condition_variable cv;
        uint32_t state = kPhysicsIdle;
        bool shutdownRequested = false;
    };
    Shared shared_;
    std::thread physicsThread_;

    float timeStep_ = 1.0f / 60.0f;

    // Render interpolation state. prevTransforms_ is rebuilt at the START of
    // every step from the active-body list (pre-step transforms), so a body
    // that goes to sleep simply stops appearing and renders at its true pose.
    // Only touched on the phase-owning thread (capture in signalStep/
    // stepInline before the phase flip, reads while idle) — no locking.
    struct PrevTransform {
        JPH::RVec3 pos;
        JPH::Quat rot;
    };
    bool interpolate_ = false;
    float renderAlpha_ = 1.0f;   // 1 = render the current stepped state
    std::unordered_map<uint64_t, PrevTransform> prevTransforms_;
    void capturePrevTransforms();

    // Per-body shape scale set by setTransforms() and honoured by the
    // penetrations() query (not by the simulation). Keyed by the body id's
    // index+sequence; absent means unit scale.
    std::unordered_map<uint32_t, JPH::Vec3> queryScale_;

    // Holds the events drained from ListenerImpl's lock-free buffer for every
    // step since the last drainContactEvents(), which hands them to the caller.
    std::vector<ContactEvent> contactsFront_;
    void collectContacts();
    // True when any step since the last drainContactEvents() overflowed the
    // contact buffer (events were dropped). Sticky across steps so a caller
    // polling getContacts() every few frames still sees it.
    bool contactsOverflowedFront_ = false;

    bool initialized_ = false;
};

} // namespace bro::physics
