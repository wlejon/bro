#pragma once

#include <cfloat>
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
#include <Jolt/Math/Float3.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Body/AllowedDOFs.h>
#include <Jolt/Physics/Body/MotionQuality.h>
#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>

namespace JPH { class CharacterVirtual; class CharacterVsCharacterCollisionSimple;
                class VehicleConstraint; class Ragdoll; class RagdollSettings; class Constraint; }

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
/// POD and bounded-size — it crosses the lock-free per-step contact ring.
/// Added events carry manifold data (world-space contact points capped at
/// kMaxPoints, the contact normal, penetration depth); Removed events carry
/// only the pair (Jolt's removal callback has no manifold).
struct ContactEvent {
    enum Type { Added, Persisted, Removed };
    static constexpr int kMaxPoints = 4;

    Type type = Added;
    JPH::BodyID body1;
    JPH::BodyID body2;
    bool isSensor = false;  // true when either body is a sensor (overlap event)

    // Manifold snapshot (Added only). `normal` is Jolt's manifold normal: the
    // world-space direction along which body2 moves out of collision (i.e. it
    // points from body1 toward body2). `penetration` may be negative for a
    // speculative contact. Points are on the surface of body2, world space.
    uint8_t numPoints = 0;
    JPH::Float3 points[kMaxPoints] = {};
    JPH::Float3 normal{0, 0, 0};
    float penetration = 0.0f;

    // Estimated collision impulse magnitude (kg·m/s), Added events only.
    // Jolt's solver doesn't expose the solved impulse, so this is Jolt's
    // standard PRE-SOLVE estimate (EstimateCollisionResponse in the contact
    // listener, summed over the manifold points): accurate for an isolated
    // two-body impact, approximate when more bodies pile into the same
    // island. 0 for sensor overlaps (no collision response).
    float impulse = 0.0f;
};

/// Per-body friction/restitution combine mode. Default keeps Jolt's built-in
/// combine functions: friction = sqrt(f1 * f2) (geometric mean), restitution
/// = max(r1, r2) — see ContactConstraintManager. When either body of a pair
/// specifies a non-default mode, the HIGHER mode of the two wins (Unity-style
/// precedence: average < min < multiply < max).
enum class CombineMode : uint8_t {
    Default  = 0,
    Average  = 1,
    Min      = 2,
    Multiply = 3,
    Max      = 4,
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
    std::vector<JPH::BodyID> ignoreBodies;  // additional exclusions (linear scan
                                            // — sized for a handful of bodies)
};

/// Per-sensor field override (Godot Area3D analog). Attached to a SENSOR
/// body, it automatically affects every dynamic body overlapping the sensor
/// volume, applied inside the physics step:
///
///  - gravity: directional replace/combine, point gravity toward the sensor's
///    center (with optional inverse-square falloff), or a world-gravity scale.
///  - damping: linear/angular damping replaced while inside.
///
/// Stacking (Godot space-override subset — replace + combine + scale):
/// overlapping areas are walked highest priority first (ties: the area whose
/// body has the lower index+sequence — i.e. created earlier — wins the
/// earlier slot). Per body, gravity accumulates over the walk:
///   - Combine: adds this area's field, keep walking.
///   - Scale:   multiplies the world-gravity term, keep walking.
///   - Replace: final gravity = accumulated combines + this area's field;
///              the walk STOPS (lower-priority areas and the world-gravity
///              term are ignored).
/// If no Replace is hit, the world term (scaled by any Scale areas) is added.
/// The body's own gravityFactor multiplies the total, so gf=0 still floats.
/// Damping is replace-only: the highest-priority overlapping area that
/// specifies a damping value wins; the body's own damping is restored on
/// exit (setLinearDamping/setAngularDamping while inside update the restored
/// base, not the override).
///
/// Mechanics: overlap membership is maintained on the phase-owning thread
/// from the sensor contact stream (between steps — no new locks); gravity is
/// applied INSIDE the step by a PhysicsStepListener reading the membership
/// maps, which are immutable for the duration of a step. Membership therefore
/// takes effect on the step AFTER the overlap begins/ends (one fixed step of
/// latency — deterministic). Caveats: contact-buffer overflow can drop
/// enter/exit events (membership may wedge — same caveat as JS trigger
/// bookkeeping); a body that falls asleep inside an area loses its sensor
/// contact until it wakes (Jolt sensors track active bodies only).
struct AreaOverride {
    enum GravityMode : uint8_t {
        GravityNone = 0,   // no gravity override (damping-only area)
        GravityReplace,    // field replaces gravity; stops the priority walk
        GravityCombine,    // field adds to gravity; walk continues
        GravityScale,      // gravityScale multiplies the world term; walk continues
    };
    GravityMode gravityMode = GravityNone;
    JPH::Vec3 gravity{0, 0, 0};    // directional field (m/s²), replace/combine
    bool pointGravity = false;     // field points at the sensor's center of mass
    float strength = 0.0f;         // point-gravity acceleration (m/s²)
    float falloffDistance = 0.0f;  // > 0: inverse-square falloff — strength is
                                   // the acceleration AT this distance; 0 = constant
    float gravityScale = 1.0f;     // GravityScale mode: world-gravity multiplier
    float linearDamping = -1.0f;   // >= 0 replaces body linear damping while inside
    float angularDamping = -1.0f;  // >= 0 replaces body angular damping while inside
    int priority = 0;              // higher wins; ties → earlier-created area
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
        ShapeDecomposedMesh,
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
    // < 0 keeps Jolt's default convex radius; 0 keeps the hull's sharp corners
    // (exact geometry for queries on small parts).
    float convexRadius = -1.0f;
    // Mesh (static only) / DecomposedMesh
    std::vector<JPH::Vec3> meshVertices;
    std::vector<uint32_t>  meshIndices;       // triangle list (multiple of 3)
    int maxHulls = 16;
    int maxVerticesPerHull = 64;
    float decompResolution = 100000.0f;
    float minVolumePerHull = 0.001f;
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

    enum MotionType { MotionDynamic = 0, MotionStatic = 1, MotionKinematic = 2 };
    MotionType motionType = MotionDynamic;
    bool isKinematic = false;
    bool isStatic = false;
    bool isSensor = false;
    // Field override installed at creation (sensors only; see AreaOverride).
    bool hasArea = false;
    AreaOverride area;
    bool ccd = false;                          // Linear cast for fast bodies
    JPH::EAllowedDOFs dofs = JPH::EAllowedDOFs::All;

    float friction = 0.5f;
    float restitution = 0.3f;
    CombineMode frictionCombine = CombineMode::Default;
    CombineMode restitutionCombine = CombineMode::Default;
    float density = 1000.0f;
    /// > 0 = direct body mass in kg — wins over density (inertia is still
    /// derived from the shape, scaled to this mass). Whole-body only; ignored
    /// on compound sub-parts. Dynamic bodies only.
    float mass = 0.0f;
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
    // Inner rigid body (CharacterVirtualSettings::mInnerBodyShape): a
    // kinematic body that follows the character so sensors, raycasts, CCD and
    // ordinary bodies can SEE it — a bare CharacterVirtual is invisible to
    // them (it never enters the broadphase).
    bool innerBody = false;
    int innerBodyLayer = -1;      // -1 = same as `layer`
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

    // Tire friction. Jolt models per-wheel friction as two LinearCurves:
    // longitudinal (Y = friction vs X = slip ratio) and lateral (Y = friction
    // vs X = slip angle in degrees). The scale factors multiply the Y values
    // of Jolt's default curves — (0,0)(0.06,1.2)(0.2,1.0) longitudinal,
    // (0,0)(3,1.2)(20,1.0) lateral — the simple "ice vs tarmac" knob. A
    // non-empty curve REPLACES the default curve entirely (scale then not
    // applied to that curve).
    float longitudinalFrictionScale = 1.0f;
    float lateralFrictionScale = 1.0f;
    std::vector<JPH::Float2> longitudinalFrictionCurve;  // (slipRatio, friction)
    std::vector<JPH::Float2> lateralFrictionCurve;       // (slipAngleDeg, friction)
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

/// One tank track (tracked vehicles only). Wheel fields are indices into
/// VehicleOptions::wheels; every wheel must belong to exactly one track.
/// Defaults follow Jolt's VehicleTrackSettings (M1-Abrams-flavored, scaled).
struct VehicleTrackOptions {
    std::vector<int> wheels;         // wheels inside this track (non-empty)
    int drivenWheel = -1;            // engine-connected wheel; -1 = last in `wheels`
    float inertia = 10.0f;           // kg·m² of track+wheels seen on the driven wheel
    float angularDamping = 0.5f;     // dw/dt = -c*w on the driven wheel
    float maxBrakeTorque = 15000.0f; // N·m the brake applies on the driven wheel
    float differentialRatio = 6.0f;  // gearbox → driven wheel rotation ratio
};

/// Lean-spring configuration (motorcycles only). Angle/smoothing defaults
/// follow Jolt's MotorcycleControllerSettings. Spring constant/damping
/// default to AUTO (-1): k = 150·I, c = 30·I where I is the chassis's roll
/// inertia about the forward axis — the stiffness-to-inertia ratio of Jolt's
/// tuned sample bike. (Jolt's raw defaults 5000/1000 assume that sample's
/// offset-center-of-mass chassis and destabilize a typical uniform-density
/// chassis, pumping energy through the ground contacts until the bike
/// launches.) Explicit non-negative values pass through raw.
struct VehicleLeanOptions {
    float maxAngle = 45.0f;                    // degrees the bike may lean in turns
    float springConstant = -1.0f;              // lean spring stiffness; -1 = 150·I(roll)
    float springDamping = -1.0f;               // lean spring damping;   -1 = 30·I(roll)
    float springIntegrationCoefficient = 0.0f; // integral term (PID-style)
    float springIntegrationCoefficientDecay = 4.0f;  // integral decay in air
    float smoothingFactor = 0.8f;              // 0 = no smoothing, 1 = frozen
};

/// Vehicle creation options (Jolt VehicleConstraint + a controller picked by
/// `controller`: WheeledVehicleController, TrackedVehicleController, or
/// MotorcycleController). The chassis is an existing dynamic body.
struct VehicleOptions {
    enum Controller { ControllerWheeled, ControllerTracked, ControllerMotorcycle };
    Controller controller = ControllerWheeled;
    JPH::BodyID body;                 // chassis body (dynamic)
    JPH::Vec3 up{0, 1, 0};            // chassis-local up
    JPH::Vec3 forward{0, 0, 1};       // chassis-local forward
    float maxPitchRollAngle = 180.0f; // degrees; < 180 keeps the vehicle from flipping
    std::vector<VehicleWheelOptions> wheels;
    VehicleEngineOptions engine;
    VehicleTransmissionOptions transmission;
    // Tracked only: exactly two tracks, [left, right]. Together they must
    // cover every wheel exactly once. Wheeled/motorcycle ignore this.
    std::vector<VehicleTrackOptions> tracks;
    // Motorcycle only: lean-spring configuration.
    VehicleLeanOptions lean;
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

    void applyControllerDefaults() {
        if (controller == ControllerTracked) {
            engine.minRPM = 500.0f;
            engine.maxRPM = 4000.0f;
            transmission.shiftUpRPM = 3500.0f;
            transmission.shiftDownRPM = 1000.0f;
        }
    }
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
    JPH::BodyID body2;          // may be invalid → attach body1 to the world;
                                // limits/motors then measure body1 vs the world

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
    float wheelHertz = 2.0f;                  // suspension spring toward rest (Hz); 0 = free
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


} // namespace bro::physics
