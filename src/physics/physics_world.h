#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
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
#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>

namespace JPH { class CharacterVirtual; class VehicleConstraint; class Ragdoll; class RagdollSettings; }

namespace bro::physics {

class PairGroupFilter;  // constraint collideConnected=false pair filter (physics_world.cpp)

/// Physics thread phase. Ownership of the Jolt world follows the phase:
/// Idle means the main thread (JS) may freely access bodies; Step/Busy/Done
/// mean the physics thread owns it. Guarded by Shared::m (see below).
enum PhysicsState : uint32_t {
    kPhysicsIdle = 0,  // Physics thread waiting — JS can freely access bodies
    kPhysicsStep = 1,  // Main thread: begin simulation step
    kPhysicsBusy = 2,  // Physics thread running Update()
    kPhysicsDone = 3,  // Physics thread: step complete
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
    JPH::Vec3 normal;     // surface normal on the hit body at the hit point
    JPH::RVec3 position;  // hit point on the shape surface (world space)
};

/// Shape-cast hit result.
struct ShapeCastHit {
    JPH::BodyID bodyID;
    float fraction;       // 0..1 along direction*maxDistance
    JPH::Vec3 normal;     // surface normal on the hit body at the contact point
    JPH::RVec3 position;  // contact point on the hit body (world space)
};

/// Overlap hit result.
struct OverlapHit {
    JPH::BodyID bodyID;
    float depth;          // penetration depth
    JPH::Vec3 normal;     // contact normal on the overlapped body, toward the query shape
    JPH::RVec3 position;  // deepest contact point on the overlapped body (world space)
};

/// Filter shared by the narrow-phase spatial queries. Layer bits are
/// independent of the collision matrix — a query may see layers that never
/// collide with anything.
struct QueryFilter {
    uint32_t layerMask = 0xffffffffu;  // bit i = include bodies on layer i
    JPH::BodyID ignoreBody;            // invalid (default) = exclude nothing
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
        ShapeHeightField, // static only — square grid of n*n height samples
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
    // HeightField (static only): n*n samples, row-major (sample (x,z) at
    // z*n + x). Surface point = offset + scale * (x, heights[z*n + x], z),
    // in body-local space. n must be >= 4 (Jolt: n / block size >= 2);
    // FLT_MAX in a sample marks a hole.
    std::vector<float> heightSamples;
    uint32_t heightSampleCount = 0;
    JPH::Vec3 heightOffset{0, 0, 0};
    JPH::Vec3 heightScale{1, 1, 1};
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

/// Character controller creation options (Jolt CharacterVirtual, capsule
/// shape). `position` is the capsule center; total height = 2*(halfHeight +
/// radius). The character is not a rigid body — it moves by collision checks
/// and pushes dynamic bodies with up to `maxStrength` newtons.
struct CharacterOptions {
    JPH::RVec3 position{0, 0, 0};
    JPH::Vec3 up{0, 1, 0};
    float radius = 0.3f;
    float halfHeight = 0.6f;      // cylindrical section half-height
    float mass = 70.0f;           // kg — used when pushing dynamic bodies
    float maxSlopeAngle = 50.0f;  // degrees; steeper ground can't support the character
    float maxStrength = 100.0f;   // N — max push force against dynamic bodies
    float padding = 0.02f;        // distance kept from geometry
    float stepUp = 0.4f;          // WalkStairs step height; 0 disables
    float stickToFloor = 0.5f;    // StickToFloor snap-down distance; 0 disables
    int layer = 1;                // object layer used for collision filtering
};

/// Character ground classification (mirrors Jolt's CharacterBase::EGroundState).
enum class CharacterGround {
    OnGround = 0,   // supported, can move freely
    OnSteepGround,  // touching ground steeper than maxSlopeAngle
    NotSupported,   // touching something but not supported by it
    InAir,          // touching nothing
};

/// Character state snapshot after a fixed step.
struct CharacterState {
    JPH::RVec3 position;
    JPH::Vec3 velocity;          // actual velocity after the last update
    CharacterGround ground = CharacterGround::InAir;
    JPH::Vec3 groundNormal;      // valid when touching ground
    JPH::Vec3 groundVelocity;    // velocity of the ground body at the contact
    JPH::BodyID groundBody;      // invalid when in air
};

/// One wheel of a wheeled vehicle (see VehicleOptions). Positions and
/// directions are in the chassis body's local space.
struct VehicleWheelOptions {
    JPH::Vec3 position{0, 0, 0};             // suspension attachment point
    JPH::Vec3 suspensionDirection{0, -1, 0}; // should point down
    float radius = 0.3f;                     // m
    float width = 0.1f;                      // m
    float suspensionMinLength = 0.3f;        // m — max raised position
    float suspensionMaxLength = 0.5f;        // m — max droop position
    float suspensionFrequency = 1.5f;        // suspension spring frequency (Hz)
    float suspensionDamping = 0.5f;          // 0 = undamped, 1 = critical
    bool steerable = false;                  // responds to the `right` input
    float maxSteerAngle = 70.0f;             // degrees (steerable only)
    bool driven = false;                     // connected to the engine via a differential
    float maxBrakeTorque = 1500.0f;          // N·m — foot brake
    float maxHandBrakeTorque = 0.0f;         // N·m — hand brake (typically rear wheels only)
};

struct VehicleEngineOptions {
    float maxTorque = 500.0f;  // N·m
    float minRPM = 1000.0f;
    float maxRPM = 6000.0f;
};

struct VehicleTransmissionOptions {
    bool manual = false;                   // false = auto shifting
    std::vector<float> gearRatios;         // empty = Jolt's 5-speed defaults
    std::vector<float> reverseGearRatios;  // empty = Jolt default (single reverse)
    float switchTime = 0.5f;               // s (auto mode)
    float clutchStrength = 10.0f;
    float shiftUpRPM = 4000.0f;            // auto mode
    float shiftDownRPM = 2000.0f;          // auto mode
};

/// Splits engine torque across a left/right wheel pair. Wheel fields are
/// indices into VehicleOptions::wheels; -1 = no wheel on that side.
struct VehicleDifferentialOptions {
    int leftWheel = -1;
    int rightWheel = -1;
    float ratio = 3.42f;             // rotation ratio gearbox → wheels
    float leftRightSplit = 0.5f;     // 0 = all torque left, 1 = all right
    float limitedSlipRatio = 1.4f;   // max/min wheel speed before torque shifts to the slower wheel
    float engineTorqueRatio = 1.0f;  // fraction of engine torque for this differential
};

/// Stiff spring between two wheels to reduce body roll in corners.
struct VehicleAntiRollBarOptions {
    int leftWheel = 0;
    int rightWheel = 1;
    float stiffness = 1000.0f;  // N/m
};

/// Wheeled-vehicle creation options (Jolt VehicleConstraint +
/// WheeledVehicleController). The chassis is an existing dynamic body.
struct VehicleOptions {
    JPH::BodyID body;                 // chassis body (dynamic)
    JPH::Vec3 up{0, 1, 0};            // chassis-local up
    JPH::Vec3 forward{0, 0, 1};       // chassis-local forward
    float maxPitchRollAngle = 180.0f; // degrees; < 180 keeps the vehicle from flipping
    std::vector<VehicleWheelOptions> wheels;
    VehicleEngineOptions engine;
    VehicleTransmissionOptions transmission;
    // Empty → auto-derived: driven wheels are paired in array order into
    // differentials with equal engineTorqueRatio.
    std::vector<VehicleDifferentialOptions> differentials;
    float differentialLimitedSlipRatio = 1.4f;  // limited slip between differentials
    std::vector<VehicleAntiRollBarOptions> antiRollBars;
    // How wheel-vs-ground collision is tested. Cylinder is the most accurate
    // wheel shape; ray is cheapest (fine for flat ground); sphere in between.
    enum Tester { TesterRay, TesterCastSphere, TesterCastCylinder };
    Tester tester = TesterCastCylinder;
    int testerLayer = -1;  // object layer the wheels collide as; -1 = chassis layer
};

/// Per-wheel state snapshot for rendering.
struct VehicleWheelState {
    JPH::Vec3 position;          // wheel center, chassis-body local space
    JPH::Quat rotation;          // chassis-local; maps a Y-axis-aligned cylinder onto the wheel
    float suspensionLength = 0;  // m, in [suspensionMinLength, suspensionMaxLength]
    float steerAngle = 0;        // rad, positive = left
    float rotationAngle = 0;     // rad, [0, 2π]
    float angularVelocity = 0;   // rad/s, positive = driving forward
    bool contact = false;        // wheel touching something
    JPH::BodyID contactBody;     // invalid when no contact
    JPH::Vec3 contactNormal{0, 1, 0};
};

/// Vehicle-level state snapshot.
struct VehicleState {
    float speed = 0;             // signed speed along the chassis forward axis (m/s)
    float rpm = 0;               // current engine RPM
    int gear = 0;                // -1 reverse, 0 neutral, 1+ forward
    bool isSwitchingGear = false;
};

/// One rigid part of a ragdoll (see RagdollOptions). Bind transforms are in
/// MODEL space — the ragdoll's own rest-pose frame; RagdollOptions::position/
/// rotation place that frame in the world at creation.
struct RagdollPartOptions {
    std::string name;
    /// Index of the parent part (must appear EARLIER in the parts array), or
    /// -1 for the root. Jolt requires parents before children.
    int parentIndex = -1;

    JPH::Vec3 position{0, 0, 0};              // bind position (part center), model space
    JPH::Quat rotation = JPH::Quat::sIdentity();  // bind rotation, model space

    enum Shape { ShapeCapsule, ShapeBox, ShapeSphere };
    Shape shape = ShapeCapsule;
    float halfHeight = 0.15f;                 // capsule cylindrical half-height
    float radius = 0.08f;                     // capsule/sphere radius
    JPH::Vec3 halfExtents{0.1f, 0.1f, 0.1f};  // box

    float density = 1000.0f;                  // kg/m^3 (mass derives from shape)
    float mass = 0.0f;                        // > 0 = override mass in kg (inertia
                                              // recomputed from the shape for this mass)
    float friction = 0.5f;
    float restitution = 0.0f;

    /// Joint connecting this part to its parent (ignored for the root).
    enum Joint { JointSwingTwist, JointFixed };
    Joint joint = JointSwingTwist;
    bool hasJointPoint = false;
    JPH::Vec3 jointPoint{0, 0, 0};            // pivot, model space; default = part position
    bool hasTwistAxis = false;
    JPH::Vec3 twistAxis{0, 1, 0};             // model space; default = parent→child direction
    bool hasPlaneAxis = false;
    JPH::Vec3 planeAxis{0, 0, 0};             // model space; default = auto perpendicular
    float normalHalfConeAngle = 0.0f;         // swing cone half-angle about planeAxis (rad)
    float planeHalfConeAngle = 0.0f;          // swing cone half-angle in the plane (rad)
    float twistMinAngle = 0.0f;               // rad, [-π, π]
    float twistMaxAngle = 0.0f;               // rad, [-π, π]
    float maxFrictionTorque = 0.0f;           // N·m friction when the joint is unpowered
};

/// Position-motor spring used by driveRagdollToPose (per swing-twist joint).
struct RagdollMotorOptions {
    float frequency = 10.0f;   // spring frequency (Hz)
    float damping = 1.0f;      // 0 = undamped, 1 = critical
    float maxTorque = -1.0f;   // symmetric torque limit (N·m); < 0 = unlimited
};

/// Ragdoll creation options (Jolt RagdollSettings + Ragdoll). Parts form a
/// tree via parentIndex; each non-root part gets a swing-twist (or fixed)
/// constraint toward its parent. Parent/child part pairs — and any parts that
/// overlap in the bind pose — don't collide with each other (Jolt
/// GroupFilterTable); everything else self-collides normally.
struct RagdollOptions {
    std::vector<RagdollPartOptions> parts;
    JPH::RVec3 position{0, 0, 0};             // world placement of the model-space origin
    JPH::Quat rotation = JPH::Quat::sIdentity();
    int layer = 1;                            // object layer for all parts
    float gravityFactor = 1.0f;
    float linearDamping = 0.05f;
    float angularDamping = 0.05f;
    bool stabilize = true;                    // Jolt Stabilize(): clamp parent/child
                                              // mass ratios + grow parent inertia
    bool activate = true;                     // wake the bodies at creation
    RagdollMotorOptions motor;                // drive-motor spring baked into the joints
};

/// World-space transform of one ragdoll part.
struct RagdollPartState {
    JPH::RVec3 position;
    JPH::Quat rotation;
};

/// Soft-body creation options (Jolt SoftBody: XPBD cloth / pressurized
/// volumes). Two creation paths:
///  - Cloth: a gridX*gridZ vertex grid in the local XZ plane (Y up), centered
///    on the local origin. Vertex (x,z) is at index z*gridX + x; faces wind
///    counter-clockwise viewed from +Y. Edge/shear/bend constraints are
///    derived from the faces (Jolt CreateConstraints).
///  - Mesh: an arbitrary triangle mesh (vertices + indices). With
///    pressure > 0 the mesh should be CLOSED with outward (CCW-from-outside)
///    winding — the enclosed volume is what the pressure inflates. If the
///    signed rest volume comes out negative the winding is flipped
///    automatically.
struct SoftBodyOptions {
    enum Kind { Cloth, Mesh };
    Kind kind = Cloth;

    // Cloth
    int gridX = 10;                 // vertices along local X (>= 2)
    int gridZ = 10;                 // vertices along local Z (>= 2)
    float spacing = 0.1f;           // rest distance between grid neighbors (m)

    // Mesh
    std::vector<JPH::Vec3> vertices;   // local-space vertex positions
    std::vector<uint32_t> indices;     // triangle list (multiple of 3)

    // Common
    float mass = 1.0f;              // total mass (kg), spread evenly over vertices
    std::vector<uint32_t> pinned;   // vertex indices frozen in place (invMass 0)
    float pressure = 0.0f;          // n*R*T gas coefficient; > 0 inflates a closed mesh
    float compliance = 0.0f;        // edge constraint compliance (1/stiffness; 0 = rigid)
    float shearCompliance = -1.0f;  // cloth shear edges; < 0 = same as compliance
    float bendCompliance = -1.0f;   // bend constraints; < 0 = no bend constraints
    int numIterations = 5;          // XPBD solver iterations
    float friction = 0.2f;
    float restitution = 0.0f;
    float linearDamping = 0.1f;
    float gravityFactor = 1.0f;
    float maxLinearVelocity = 500.0f;
    float vertexRadius = 0.0f;      // particle radius (pushes verts off surfaces)
    bool updatePosition = true;     // body position follows the vertices
    bool doubleSided = true;        // faces hit by queries from both sides
    bool allowSleeping = true;
    int layer = -1;                 // -1 = moving (1)
    JPH::RVec3 position{0, 0, 0};
    JPH::Quat rotation = JPH::Quat::sIdentity();
};

/// Per-axis configuration for a SixDOF constraint. Axis order follows Jolt's
/// SixDOFConstraintSettings::EAxis: 0..2 = translation X/Y/Z, 3..5 = rotation
/// X/Y/Z (all in constraint space — see ConstraintOptions::sixDofAxisX/Y).
struct SixDofAxis {
    enum Mode { Locked, Free, Limited };
    Mode mode = Locked;
    float min = 0.0f;             // Limited only. Rotation X: twist [-π, π].
    float max = 0.0f;             // Rotation Y/Z limits are symmetric (Jolt uses max).
    float springFrequency = 0.0f; // >0 = soft limits (translation axes only), Hz
    float springDamping = 1.0f;   // 0 = undamped, 1 = critical
    float maxFriction = 0.0f;     // friction force (N) / torque (N·m) when unpowered
};

/// Motor configuration for a motorized constraint (hinge, slider, sixdof —
/// wheel handles are sixdof underneath and accept axis 5 / RotationZ).
/// Units: hinge/rotation targets are rad/s (velocity) or rad (position);
/// slider/translation targets are m/s or m.
struct MotorOptions {
    enum State { Off, Velocity, Position };
    State state = Off;
    int axis = -1;            // SixDOF only: 0..5 (tx,ty,tz,rx,ry,rz); ignored otherwise
    float target = 0.0f;
    float maxForce = -1.0f;   // symmetric force limit (N); <0 = leave unlimited/unchanged
    float maxTorque = -1.0f;  // symmetric torque limit (N·m); <0 = leave unlimited/unchanged
    float frequency = -1.0f;  // position-motor spring frequency (Hz); <0 = keep current (Jolt default 2)
    float damping = -1.0f;    // position-motor spring damping; <0 = keep current (default 1)
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
        SixDOF,          // per-axis free/limited/locked on all 6 DOFs (Godot Generic6DOF analog)
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

    // --- SixDOF (uses point1/point2 as the two anchors) ---
    // Constraint-space frame (world-space axes; Y is re-orthonormalized
    // against X). Translation/rotation limits apply along/about these.
    JPH::Vec3 sixDofAxisX{1, 0, 0};
    JPH::Vec3 sixDofAxisY{0, 1, 0};
    bool sixDofSwingPyramid = false;   // rotation-Y/Z limit shape: cone (default) or pyramid
    SixDofAxis sixDofAxes[6];          // tx, ty, tz, rx, ry, rz — default all Locked

    // Motors applied right after creation (hinge/slider: one entry, axis
    // ignored; sixdof: one entry per driven axis). Same semantics as
    // setConstraintMotor().
    std::vector<MotorOptions> motors;
};

class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();

    /// Initialize the physics system. Call once before use.
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
    /// A stale/already-destroyed id is a safe no-op (verified before Jolt's
    /// DestroyBody, which would corrupt the body-manager free list).
    /// Destroying a ragdoll part destroys the WHOLE ragdoll; onBodyDestroyed
    /// (when provided) is invoked for every body actually destroyed — one for
    /// a plain body, all part bodies for a ragdoll — so callers can evict
    /// their own per-body bookkeeping (e.g. the JS tag registries).
    void destroyBody(JPH::BodyID id,
                     const std::function<void(JPH::BodyID)>& onBodyDestroyed = {});

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
    void destroyRagdoll(uint32_t handle);

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
    // Runs each character's velocity update + ExtendedUpdate with the fixed
    // dt. Called from signalStep (main thread, before the phase flip) and
    // stepInline — never concurrently with a world step.
    void updateCharacters(float dt);

    // Vehicle registry. Each VehicleConstraint is registered with the system
    // both as a constraint and as a step listener; removeVehicleFromSystem
    // detaches both together so the listener can never outlive the constraint.
    struct VehicleEntry {
        JPH::Ref<JPH::VehicleConstraint> constraint;
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
        std::condition_variable cv;
        uint32_t state = kPhysicsIdle;
        bool shutdownRequested = false;
    };
    Shared shared_;
    std::thread physicsThread_;

    float timeStep_ = 1.0f / 60.0f;

    // Holds the events drained from ListenerImpl's lock-free buffer for the
    // last completed step, until drainContactEvents() hands them to the caller.
    std::vector<ContactEvent> contactsFront_;
    // True when any step since the last drainContactEvents() overflowed the
    // contact buffer (events were dropped). Sticky across steps so a caller
    // polling getContacts() every few frames still sees it.
    bool contactsOverflowedFront_ = false;

    bool initialized_ = false;
};

} // namespace bro::physics
