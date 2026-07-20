// =============================================================================
// bro Physics API Reference
// =============================================================================
//
// Thin JS wrapper around Jolt physics. Bodies are referenced by an integer
// "tag" (a small monotonic ID), not by JS object. Pair a tag with
// scene.createPhysicsNode({ body: tag }) for visual sync.
//
// WARNING, createPhysicsNode resolves tags against the DEFAULT WORLD ONLY.
// There is no way to tell it which world a tag came from. Every world's tag
// space starts at 1, so passing a sandbox-world tag does not fail: it
// silently binds the node to whatever unrelated default-world body happens
// to hold that number, and the visual tracks the wrong object. Only ever
// pass Physics.createBody tags to createPhysicsNode; sync sandbox bodies
// yourself from w.getTransform / w.getAllTransforms.
//
// Methods live on the global `Physics` namespace (no `bro.` prefix. This
// binding pre-dates the bro.* convention).
//
// Coordinate system: right-handed Y-up, world units (typically meters).
// Quaternions are {x, y, z, w}; passing the wrong order silently produces
// nonsense rotations.
//
// -----------------------------------------------------------------------------
// Worlds
// -----------------------------------------------------------------------------
//
// There are two flavors of physics world:
//
// 1. The DEFAULT WORLD. The engine creates it at startup, steps it once per
//    frame on the physics thread, and exposes it through the `Physics.*`
//    namespace functions (Physics.createBody, Physics.raycast, etc.). This
//    is the world your gameplay code should use.
//
// 2. SANDBOX WORLDS. Created on demand via Physics.createWorldHandle({...}).
//    Each handle is an independent Jolt world with its own bodies,
//    constraints, contact events, and collision-layer config. The engine
//    does NOT auto-step these, the caller invokes handle.step(dt) manually.
//    Use cases: trajectory previews ("ghost" balls / what-if simulations),
//    deterministic side-simulations for AI, server-authoritative replicas.
//    Sandbox worlds share the same body-creation API as the default world.
//
// Tag spaces are PER WORLD: a tag returned by w.createBody is meaningful
// only on `w`, never on the default world. The same number could refer to
// different bodies in different worlds, keep them straight.
// =============================================================================


// -----------------------------------------------------------------------------
// World configuration
// -----------------------------------------------------------------------------

/**
 * Acknowledge the physics world. The engine has already created it; this
 * call is a no-op kept for forward compat. Returns true on success.
 *
 * Takes NO options: any argument is ignored outright, including maxBodies.
 * The default world's capacity is fixed by the engine; use
 * Physics.createWorldHandle({ maxBodies }) if you need to size a world.
 */
Physics.createWorld();

/** Set world gravity (default 0, -9.81, 0). */
Physics.setGravity(x, y, z);

/** @returns {{x:number, y:number, z:number}} */
Physics.getGravity();

/** Set the fixed timestep (seconds) used by the engine when stepping the world. */
Physics.setTimeStep(dt);

/**
 * Render interpolation of body transforms (default OFF, like Godot/Unity).
 *
 * Physics steps at the fixed timestep while rendering is uncapped, so
 * anything synced from a body normally snaps at the step rate. With
 * interpolation ON, render-side consumers blend each body's previous->current
 * step transforms by the frame's accumulator fraction (position lerp +
 * rotation slerp), which makes motion smooth at any display rate at the cost
 * of the visual state lagging up to one fixed step behind the simulation.
 *
 * What is interpolated (Godot semantics: interpolate what you SEE):
 *   - PhysicsNode scene sync (scene.createPhysicsNode visuals)
 *   - getTransform(id, { interpolated: true })
 *   - getAllTransforms({ interpolated: true })
 * Physics queries: plain getTransform/getAllTransforms, raycasts, shape
 * casts, overlaps, contacts. Always return the true stepped state.
 *
 * Sleeping and static bodies always render at their true pose (no jitter).
 * Teleports via setPosition/setRotation snap: the previous-step snapshot is
 * dropped so the body never glides across the world. moveKinematic is
 * velocity-driven and interpolates smoothly.
 *
 * Default-world only (sandbox worlds are stepped manually by the caller, so
 * there is no render clock to interpolate against).
 */
Physics.setInterpolation(true);
/** @returns {boolean} */
Physics.getInterpolation();


// -----------------------------------------------------------------------------
// Collision layers
// -----------------------------------------------------------------------------
//
// Up to 16 named object layers and a row-major n*n collision matrix.
// Default config: ["static", "moving"] with [false, true, true, true]
// (static-vs-static disabled, everything else enabled).
//
// Reconfiguring layers takes effect for ALL bodies, including ones already
// created, so call this once at startup before creating gameplay bodies.

/**
 * @param {{names: string[], matrix: boolean[]}} cfg
 *  matrix is row-major n*n where matrix[i*n + j] = true means layer i
 *  collides with layer j. Should be symmetric.
 * @returns {boolean} true on success
 */
Physics.setLayers({
    names: ['static', 'moving', 'pickup', 'enemy'],
    matrix: [
        // static, moving, pickup, enemy
        false,  true,   false,   true,    // static
        true,   true,   true,    true,    // moving
        false,  true,   false,   false,   // pickup (only collides w/ moving)
        true,   true,   false,   true,    // enemy
    ],
});


// -----------------------------------------------------------------------------
// Body creation
// -----------------------------------------------------------------------------

/**
 * Create a rigid body and return its integer tag.
 *
 * @param {Object} opts
 * @param {string}  opts.shape        - "box" | "sphere" | "capsule" | "cylinder" | "convexHull" | "mesh" | "compound" | "chain" | "heightfield"
 * @param {{x,y,z}} [opts.position]   - default {0,0,0}
 * @param {{x,y,z,w}} [opts.rotation] - quaternion, default identity
 * @param {boolean} [opts.static=false]
 * @param {boolean} [opts.sensor=false] - sensors fire contact events but generate no response
 * @param {Object}  [opts.area]         - field override installed on this sensor
 *                                        (requires sensor:true), see the
 *                                        "Area field overrides" section
 * @param {boolean} [opts.ccd=false]    - LinearCast motion quality (for fast-moving bodies)
 * @param {string}  [opts.dofs]         - "all" (default) | "2d"
 *                                         "2d" locks Z translation and X/Y rotation (Plane2D).
 *                                         Or comma-separated tokens: "tx,ty,rz" etc.
 * @param {string|number} [opts.layer]  - layer name or index; defaults to "static" (0) if static, else "moving" (1)
 * @param {number|bigint} [opts.userData=0] - 64-bit user data; survives round trips via getTransform/raycast/getUserData
 * @param {number}  [opts.friction=0.5]
 * @param {number}  [opts.restitution=0.3]
 * @param {string}  [opts.frictionCombine]    - how this body's friction combines with
 *                                              the other body's on contact: 'average' |
 *                                              'min' | 'max' | 'multiply'. Omitted =
 *                                              Jolt's default sqrt(f1*f2) (geometric
 *                                              mean). If the two bodies disagree the
 *                                              higher mode wins (average < min <
 *                                              multiply < max).
 * @param {string}  [opts.restitutionCombine] - same for restitution. Omitted = Jolt's
 *                                              default max(r1, r2).
 * @param {number}  [opts.density=1000]    - kg/m³, sets mass via shape volume
 *                                           (convex shapes: box/sphere/capsule/
 *                                           cylinder/convexHull and compound parts)
 * @param {number}  [opts.mass]            - direct body mass in kg; wins over
 *                                           density when both are given (inertia
 *                                           still derives from the shape, scaled
 *                                           to this mass). Whole-body only,
 *                                           ignored on compound sub-parts.
 * @param {number}  [opts.gravityFactor=1]
 * @param {number}  [opts.linearDamping=0.05]
 * @param {number}  [opts.angularDamping=0.05]
 * @param {number}  [opts.maxLinearVelocity=500] - Jolt clamps body speed to this; raise it for pixel-unit games where 500 px/s is slow
 * @param {number}  [opts.maxAngularVelocity=47.12] - 0.25*PI*60 rad/s default
 *
 * Shape-specific:
 * @param {{x,y,z}} [opts.halfExtents]  - box (default 0.5, 0.5, 0.5)
 * @param {number}  [opts.radius]       - sphere / capsule / cylinder
 * @param {number}  [opts.halfHeight]   - capsule / cylinder
 * @param {Float32Array|number[]} [opts.points] - convexHull, flat xyz; >= 4 points
 * @param {Float32Array|number[]} [opts.positions] - mesh, flat xyz vertex list
 * @param {Uint32Array|number[]}  [opts.indices]   - mesh, triangle list (multiple of 3)
 * @param {Object[]} [opts.parts]       - compound; each part has {shape, ...subShapeProps,
 *                                          localPosition: {x,y,z}, localRotation: {x,y,z,w}}
 * @param {Float32Array|number[]} [opts.heights] - heightfield, n*n samples row-major:
 *                                          heights[z*n + x]. FLT_MAX marks a hole.
 * @param {number}  [opts.sampleCount]  - heightfield n (inferred from heights.length if omitted); n >= 4
 * @param {{x,y,z}} [opts.scale]        - heightfield cell size / height scale (default {1,1,1})
 * @param {{x,y,z}} [opts.offset]       - heightfield local offset applied before the body transform
 * @param {Float32Array|number[]} [opts.points] - chain, flat 2D [x0,y0,x1,y1,...]
 *                                          in the XY plane (same key as convexHull)
 * @param {number}  [opts.depth=20]     - chain total Z thickness of the extruded
 *                                          wall. The default is deliberately deep
 *                                          so 3D bodies can't slip past a 2D wall
 * @param {boolean} [opts.closed=false] - chain, weld the last point back to the first
 * @param {boolean} [opts.flipNormal=false] - chain, swap which side is the front face
 *
 * Notes:
 * - "heightfield" shapes are always static. The surface in body-local space is
 *   offset + scale * (x, heights[z*n + x], z) for integer x,z in [0, n-1], the
 *   grid spans scale.x*(n-1) by scale.z*(n-1) starting at the body position, NOT
 *   centered on it; use offset (or position) to center. Much cheaper than an
 *   equivalent static "mesh" for terrain (quantized samples + hierarchical grid,
 *   no triangle soup).
 * - "mesh" shapes are always static (Jolt limitation). Triangle winding determines
 *   which side collides: Jolt mesh shapes are one-sided. CCW = +Y normal in
 *   right-handed Y-up.
 * - "compound" parts can be any non-mesh, non-compound shape. Wrap moving multi-
 *   part objects (L-blocks, T-shapes, ragdoll torsos) in a compound.
 *
 * @returns {number} body tag (use everywhere else)
 */
const id = Physics.createBody({
    shape: "box",
    position: { x: 0, y: 5, z: 0 },
    halfExtents: { x: 0.5, y: 0.5, z: 0.5 },
    friction: 0.6,
    userData: 0xdeadbeefn,   // bigint or plain number both accepted
    layer: 'moving',
});

// 2D plane-locked dynamic body
Physics.createBody({
    shape: 'sphere', radius: 0.5,
    position: { x: 0, y: 5, z: 0 },
    dofs: '2d',
});

// Convex hull from a flat point list
Physics.createBody({
    shape: 'convexHull',
    points: new Float32Array([0,0,0, 1,0,0, 0,1,0, 0,0,1]),
    position: { x: 0, y: 10, z: 0 },
});

// Static mesh from a triangle list (e.g. drawn polylines, terrain heightmap tris)
Physics.createBody({
    shape: 'mesh',
    static: true,
    positions: new Float32Array([-5,0,-5,  5,0,-5,  5,0,5,  -5,0,5]),
    indices:   new Uint32Array([0,2,1, 0,3,2]),  // CCW for +Y normal
});

// Compound (two boxes glued into one rigid body)
Physics.createBody({
    shape: 'compound',
    position: { x: 0, y: 5, z: 0 },
    parts: [
        { shape: 'box', halfExtents: {x:1,y:0.5,z:0.5}, localPosition: {x:-0.5,y:0,z:0} },
        { shape: 'box', halfExtents: {x:0.5,y:1,z:0.5}, localPosition: {x:0.5,y:0.5,z:0} },
    ],
});

// 2D ground / chain (one-sided edge primitive). Polyline lives in the XY
// plane; the engine extrudes a vertical wall along ±Z by `depth/2` and
// triangulates so the in-plane left-normal of the walking direction is the
// front face, back-face culling makes the chain one-sided. Set flipNormal
// to swap. Always static.
Physics.createBody({
    shape: 'chain',
    points: [-10, 0,  0, 0,  0, 10],   // flat [x0,y0,x1,y1,...] in 2D
    depth: 4,                          // total Z thickness (default 20)
    closed: false,                     // close loop: connects last→first (default)
    flipNormal: false,                 // (default)
});

// Heightfield terrain: 64x64 samples, 1m cells, heights from any source
// (noise, image, analytic). Collides as real terrain, much cheaper than a
// triangle mesh of the same grid.
const n = 64;
const heights = new Float32Array(n * n);
for (let z = 0; z < n; z++)
    for (let x = 0; x < n; x++)
        heights[z * n + x] = 3 * Math.sin(x * 0.2) * Math.cos(z * 0.2);
Physics.createBody({
    shape: 'heightfield',
    heights, sampleCount: n,
    scale: { x: 1, y: 1, z: 1 },
    position: { x: -n / 2, y: 0, z: -n / 2 },   // center the grid on the origin
});

/** Destroy a body and free its tag. Safely removes any constraints attached to it. */
Physics.destroyBody(id);

/**
 * Destroy EVERY body and constraint in the default world. Used at level
 * transitions; the world remains usable afterwards (just empty). Pending
 * contact events are also cleared, and tag-map state is reset (but tag
 * counters remain monotonic, so reusing old tags will not silently alias).
 */
Physics.destroyAll();


// -----------------------------------------------------------------------------
// Per-body queries / mutations
// -----------------------------------------------------------------------------

/**
 * @param {number} id
 * @param {{interpolated?: boolean}} [opts] - interpolated: true returns the
 *  render-side (interpolated) transform when Physics.setInterpolation is on;
 *  identical to the true state otherwise.
 * @returns {{position:{x,y,z}, rotation:{x,y,z,w}, userData: bigint}}
 *  or undefined if tag is unknown.
 */
Physics.getTransform(id);

/** @returns {{linear:{x,y,z}, angular:{x,y,z}}} or undefined */
Physics.getVelocity(id);

Physics.setPosition(id, x, y, z);
Physics.setRotation(id, qx, qy, qz, qw);
Physics.setLinearVelocity(id, x, y, z);
Physics.setAngularVelocity(id, x, y, z);

/** Continuous force, applied per substep until next call. */
Physics.addForce(id, x, y, z);

/** One-shot impulse (instant velocity change scaled by mass). */
Physics.addImpulse(id, x, y, z);

/** Continuous torque. */
Physics.addTorque(id, x, y, z);

/** 64-bit user data, typically used to map body→entity-id. */
Physics.setUserData(id, value);
/** @returns {bigint} */
Physics.getUserData(id);

/** @returns {boolean}: false for sleeping bodies and unknown tags. */
Physics.isActive(id);

/** Wake a sleeping body. */
Physics.activate(id);

/**
 * Change a body's collision layer at runtime.
 *
 * Layer can be a string (must already be registered via Physics.setLayers)
 * or a 0-based integer. Returns true on success. Internally calls
 * Jolt BodyInterface::SetObjectLayer, which notifies the broadphase; the
 * cost is roughly a remove+add of one body in the broadphase BVH (cheap).
 *
 * Common pattern: flip a body to a "ghost" layer that collides with
 * nothing for a brief invulnerability window, then flip back.
 */
Physics.setLayer(id, "ghost");

// -----------------------------------------------------------------------------
// Kinematic bodies
// -----------------------------------------------------------------------------
//
// A kinematic body is moved by script, gravity does not affect it, and
// dynamic bodies do not push it. But it DOES push dynamic bodies on
// contact, with stable contact forces (unlike teleporting via setPosition,
// which produces unphysical impulses). Use for moving platforms, paddles,
// elevators, swept hazards, etc.

/** Convert an existing body into a kinematic body (preserves shape/transform). */
Physics.setKinematic(id);

/**
 * Toggle a body between static (isStatic=true) and dynamic (false). The
 * body's collision layer is preserved, with one exception: a body going
 * dynamic while on layer 0: the only layer in the non-moving broadphase
 * tree: moves to the default "moving" layer (1). A body CREATED static can
 * never become dynamic (no motion state is allocated for it); create it
 * dynamic and freeze it instead. Also available on sandbox world handles.
 */
Physics.setMotionType(id, isStatic);

/**
 * Drive a kinematic body toward a target position over the next dt seconds.
 * Internally sets linear/angular velocity = (target - current) / dt so the
 * body integrates smoothly. Call once per frame with dt = your frame delta.
 *
 * Two arities:
 *   Physics.moveKinematic(id, x, y, z, dt)
 *   Physics.moveKinematic(id, x, y, z, qx, qy, qz, qw, dt)  // also rotate
 */
Physics.moveKinematic(id, x, y, z, dt);
Physics.moveKinematic(id, x, y, z, qx, qy, qz, qw, dt);


// -----------------------------------------------------------------------------
// Constraints (joints)
// -----------------------------------------------------------------------------

/**
 * Create a constraint linking two bodies (or one body to the world if body2
 * is omitted). Returns a non-zero handle on success.
 *
 * @param {Object} opts
 * @param {string} opts.type      - "distance" | "point" | "hinge" | "fixed" | "slider" | "wheel"
 *                                  | "cone" | "swingTwist" | "pulley" | "gear" | "rackAndPinion"
 *                                  | "sixdof"
 * @param {number} opts.body1     - first body tag
 * @param {number} [opts.body2]   - second body tag (omit / pass -1 to attach to world)
 * @param {{x,y,z}} [opts.point1] - world-space anchor on body1 (also: wheel hub, cone/swingTwist pivot)
 * @param {{x,y,z}} [opts.point2] - world-space anchor on body2
 * @param {number}  [opts.minDistance]   - distance: lower bound (negative = use rest length)
 * @param {number}  [opts.maxDistance]   - distance: upper bound
 * @param {{x,y,z}} [opts.axis]          - hinge / slider: world-space axis; cone / swingTwist: twist axis
 * @param {number}  [opts.limitMin]      - hinge: min angle (rad); slider: min position
 * @param {number}  [opts.limitMax]      - hinge: max angle (rad); slider: max position
 * @param {boolean} [opts.collideConnected=false] - whether the two constrained bodies
 *                                  collide with EACH OTHER. Default false: the pair is
 *                                  excluded from collision for the constraint's lifetime
 *                                  (destroying the constraint re-enables it). Pass true
 *                                  to keep normal collision between them. Ignored when
 *                                  body2 is the world, or when a body already uses
 *                                  another collision-group scheme (ragdoll parts).
 * @param {number}  [opts.breakingImpulse=0] - auto-break threshold (N·s); 0 = never break.
 *                                  When exceeded in a step the constraint is disabled and
 *                                  reported by Physics.getBrokenConstraints(). Also settable
 *                                  later via setConstraintBreakingImpulse().
 *
 * Cone-only fields (point + limited swing about the twist axis, e.g. a shoulder):
 * @param {number}  [opts.halfConeAngle=0]   - max swing half-angle (radians)
 *
 * SwingTwist-only fields (ragdoll joint: independent swing + twist limits):
 * @param {{x,y,z}} [opts.planeAxis={0,1,0}] - swing plane axis (⟂ to twist axis)
 * @param {number}  [opts.normalHalfConeAngle=0] - swing-Y half-angle (rad)
 * @param {number}  [opts.planeHalfConeAngle=0]  - swing-Z half-angle (rad)
 * @param {number}  [opts.twistMinAngle=0]       - twist lower limit (rad, [-π,π])
 * @param {number}  [opts.twistMaxAngle=0]       - twist upper limit (rad, [-π,π])
 * @param {number}  [opts.maxFrictionTorque=0]   - friction torque when unpowered (N·m)
 *
 * Pulley-only fields (rope of fixed length over two pivots):
 * @param {{x,y,z}} [opts.bodyPoint1]  - attachment on body1 (world)
 * @param {{x,y,z}} [opts.fixedPoint1] - fixed pivot 1 (world)
 * @param {{x,y,z}} [opts.bodyPoint2]  - attachment on body2 (world)
 * @param {{x,y,z}} [opts.fixedPoint2] - fixed pivot 2 (world)
 * @param {number}  [opts.ratio=1]     - len(b1..f1) + ratio·len(b2..f2) = constant
 * @param {number}  [opts.minLength=0] - min total rope length (<0 = current)
 * @param {number}  [opts.maxLength=-1]- max total rope length (<0 = current)
 *
 * Gear / RackAndPinion fields (couple two EXISTING constraints by their handles):
 * @param {{x,y,z}} [opts.hingeAxis1]  - gear: gear-1 rotation axis; rack: pinion rotation axis
 * @param {{x,y,z}} [opts.hingeAxis2]  - gear: gear-2 rotation axis (gear only)
 * @param {{x,y,z}} [opts.sliderAxis]  - rack: rack slide axis (rackAndPinion only)
 * @param {number}  [opts.ratio=1]     - gear/rack ratio
 * @param {number}  opts.constraint1   - handle of the first coupled constraint (gear: hinge; rack: pinion hinge)
 * @param {number}  opts.constraint2   - handle of the second coupled constraint (gear: hinge; rack: rack slider)
 *
 * Wheel-only fields (Box2D-style; backed by a SixDOFConstraint):
 * @param {{x,y,z}} [opts.suspensionAxis={0,1,0}] - translation axis (suspension)
 * @param {{x,y,z}} [opts.hingeAxis={0,0,1}]      - wheel rotation axis (2D = +Z)
 * @param {number}  [opts.hertz=2]                - suspension spring frequency (Hz); 0 disables
 * @param {number}  [opts.dampingRatio=0.7]       - 0=undamped, 1=critical
 * @param {number}  [opts.lowerTranslation]       - optional suspension travel min (m)
 * @param {number}  [opts.upperTranslation]       - optional suspension travel max (m)
 * @param {boolean} [opts.enableMotor=false]      - enable angular motor on the wheel pin
 * @param {number}  [opts.motorSpeed=0]           - target angular velocity (rad/s)
 * @param {number}  [opts.maxMotorTorque=0]       - motor torque cap (N·m)
 *
 * SixDOF-only fields (Godot Generic6DOFJoint3D analog: configure each of the
 * six degrees of freedom independently):
 * @param {{x,y,z}} [opts.axisX={1,0,0}]   - constraint-space X axis (world)
 * @param {{x,y,z}} [opts.axisY={0,1,0}]   - constraint-space Y axis (world; re-orthonormalized)
 * @param {string}  [opts.swingType='cone'] - rotation-Y/Z limit shape: 'cone' | 'pyramid'
 * @param {Object}  [opts.axes]            - per-axis config, keys translationX/Y/Z +
 *                                  rotationX/Y/Z. Each value is 'locked' (default),
 *                                  'free', or { min, max, frequency?, damping?, friction? }:
 *                                  min/max = limit range (m for translation; rad for
 *                                  rotation: rotationX is the twist in [-π,π],
 *                                  rotationY/Z limits are symmetric, Jolt uses max);
 *                                  frequency (default 0 = hard limits; >0 Hz) makes
 *                                  translation limits soft springs, with damping
 *                                  (default 1 = critical, NOT 0) on that spring;
 *                                  friction (default 0) = resistance (N / N·m) when
 *                                  the axis has no motor. An object without min/max
 *                                  means 'free' (useful for friction-only axes).
 * @param {Object}  [opts.motors]          - per-axis motors at create, keyed like `axes`,
 *                                  each a motor options object (see setConstraintMotor).
 *
 * Motors at create (hinge / slider only: sixdof uses `motors` above):
 * @param {Object}  [opts.motor]           - motor options object (see setConstraintMotor)
 * @returns {number} handle (truthy on success)
 */
const j = Physics.createConstraint({
    type: 'distance',
    body1: tagA, body2: tagB,
    point1: {x:0, y:5, z:0}, point2: {x:1, y:5, z:0},
    minDistance: 0.5, maxDistance: 1.5,
});

// Wheel: chassis + wheel pinned at the wheel hub, free to bounce on the
// suspension axis (with hertz/dampingRatio spring) and free to spin around
// the hinge axis (with optional motor).
const wheel = Physics.createConstraint({
    type: 'wheel',
    body1: chassis, body2: wheelBody,
    point1: {x:0, y:0, z:0},               // wheel hub (world)
    suspensionAxis: {x:0, y:1, z:0},
    hingeAxis:      {x:0, y:0, z:1},
    hertz: 2.0, dampingRatio: 0.7,
    enableMotor: true, motorSpeed: -10, maxMotorTorque: 50,
});

// Cone: limit body2 to swing within a 30° half-angle about the twist axis.
const cone = Physics.createConstraint({
    type: 'cone',
    body1: a, body2: b,
    point1: {x:0, y:5, z:0}, point2: {x:0, y:5, z:0},
    axis: {x:0, y:1, z:0},
    halfConeAngle: Math.PI / 6,
});

// SwingTwist: ragdoll-style joint with independent swing + twist limits.
const shoulder = Physics.createConstraint({
    type: 'swingTwist',
    body1: torso, body2: upperArm,
    point1: {x:0, y:2, z:0}, point2: {x:0, y:2, z:0},
    axis: {x:1, y:0, z:0}, planeAxis: {x:0, y:1, z:0},
    normalHalfConeAngle: 0.6, planeHalfConeAngle: 0.4,
    twistMinAngle: -0.3, twistMaxAngle: 0.3,
});

// Pulley: two bodies share a rope of fixed length routed over two pivots.
const rope = Physics.createConstraint({
    type: 'pulley',
    body1: weightA, body2: weightB,
    bodyPoint1: {x:-1, y:3, z:0}, fixedPoint1: {x:-1, y:6, z:0},
    bodyPoint2: {x: 1, y:3, z:0}, fixedPoint2: {x: 1, y:6, z:0},
    ratio: 1.0,
});

// Gear: couple the rotation of two wheels (each pinned to the world by a hinge).
const hingeA = Physics.createConstraint({ type:'hinge', body1: gearA, body2:-1,
    point1:{x:-1,y:0,z:0}, point2:{x:-1,y:0,z:0}, axis:{x:0,y:0,z:1} });
const hingeB = Physics.createConstraint({ type:'hinge', body1: gearB, body2:-1,
    point1:{x: 1,y:0,z:0}, point2:{x: 1,y:0,z:0}, axis:{x:0,y:0,z:1} });
const gear = Physics.createConstraint({
    type: 'gear',
    body1: gearA, body2: gearB,
    hingeAxis1: {x:0,y:0,z:1}, hingeAxis2: {x:0,y:0,z:1},
    ratio: 2.0,                         // gearB turns half as fast as gearA
    constraint1: hingeA, constraint2: hingeB,
});

// RackAndPinion: a pinion hinge drives a rack slider.
const rack = Physics.createConstraint({
    type: 'rackAndPinion',
    body1: pinion, body2: rackBody,
    hingeAxis1: {x:0,y:0,z:1}, sliderAxis: {x:1,y:0,z:0},
    ratio: 4.0,
    constraint1: pinionHinge, constraint2: rackSlider,
});

// SixDOF: a crane arm, free vertical travel within ±2 m (soft-limited by a
// spring), yaw free, everything else locked; the yaw axis is motorized.
const arm = Physics.createConstraint({
    type: 'sixdof',
    body1: base, body2: armBody,
    point1: {x:0, y:2, z:0}, point2: {x:0, y:2, z:0},
    axes: {
        translationY: { min: -2, max: 2, frequency: 4, damping: 0.8 },
        rotationY: 'free',
        // unlisted axes stay locked
    },
    motors: {
        rotationY: { type: 'velocity', target: 1.5, maxTorque: 200 },
    },
});

// Hinge with a create-time position motor: a door that servos to 90°.
const door = Physics.createConstraint({
    type: 'hinge',
    body1: frame, body2: doorBody,
    point1: {x:0, y:1, z:0}, point2: {x:0, y:1, z:0},
    axis: {x:0, y:1, z:0},
    limitMin: 0, limitMax: Math.PI / 2,
    motor: { type: 'position', target: Math.PI / 2, maxTorque: 300, frequency: 4, damping: 1 },
});

/** Adjust a wheel constraint's motor at runtime. No-op for non-wheel handles. */
Physics.setWheelMotor(wheel, /*enabled*/ true, /*speed*/ -8.0, /*maxTorque*/ 60);

/**
 * Configure/steer a constraint motor at runtime. Works on hinge and slider
 * handles (single driven axis: `axis` is ignored) and on sixdof handles
 * (pass `axis`; wheel handles are sixdof underneath, their pin is
 * 'rotationZ'). Wakes both bodies. Returns false for unknown handles,
 * non-motorized constraint types, or a missing/invalid sixdof axis.
 *
 * @param {number} handle - constraint handle from createConstraint
 * @param {Object} motor
 * @param {string} motor.type        - 'velocity' | 'position' | 'off'
 * @param {number} [motor.target=0]  - rad/s | rad (rotation), m/s | m (translation)
 * @param {string|number} [motor.axis] - sixdof only: 'translationX'..'translationZ',
 *                                  'rotationX'..'rotationZ' (or index 0..5)
 * @param {number} [motor.maxForce]  - symmetric force limit (N, translation axes);
 *                                  omit to leave unchanged (default unlimited)
 * @param {number} [motor.maxTorque] - symmetric torque limit (N·m, rotation axes)
 * @param {number} [motor.frequency] - position-motor spring frequency (Hz, default 2)
 * @param {number} [motor.damping]   - position-motor spring damping (default 1)
 * @returns {boolean} true if the motor was applied
 */
Physics.setConstraintMotor(door, { type: 'position', target: 0.2, maxTorque: 300 });
Physics.setConstraintMotor(arm,  { axis: 'rotationY', type: 'velocity', target: -1, maxTorque: 200 });
Physics.setConstraintMotor(arm,  { axis: 'rotationY', type: 'off' });

/**
 * Breakable constraints. Set a breaking-impulse threshold (N·s) on any handle;
 * when the constraint's applied position impulse exceeds it in a step the
 * constraint is auto-disabled. Pass 0 to make it unbreakable again.
 */
Physics.setConstraintBreakingImpulse(handle, 500.0);
const threshold = Physics.getConstraintBreakingImpulse(handle);

/**
 * Drain the handles of constraints that broke since the last call (call once
 * per frame). Broken constraints are disabled, not destroyed. Call
 * destroyConstraint() if you want them gone.
 * @returns {number[]} handles that broke this frame
 */
for (const h of Physics.getBrokenConstraints()) console.log('snapped', h);

/** Disable / enable a constraint without destroying it. */
Physics.setConstraintEnabled(handle, true);

/** Remove a constraint. */
Physics.destroyConstraint(handle);


// -----------------------------------------------------------------------------
// Raycasting
// -----------------------------------------------------------------------------

/**
 * Cast a ray against the world; returns ALL hits (sorted by distance).
 * Narrow-phase precision: hits land on the actual shape surface (not the
 * broadphase AABB), `position` is the exact hit point, and `normal` is the
 * real surface normal on the hit body (pointing back toward the ray origin
 * for a ray arriving from outside). One hit per body (earliest contact).
 *
 * maxDist defaults to 1000 world units when omitted (or when the 7th argument
 * is the opts object rather than a number): the ray is never unbounded.
 *
 * An optional trailing opts object takes the same filter fields as the shape
 * queries below: `layers` (array of layer names/indices the ray can see),
 * `ignoreBody` (one body tag to exclude) and `ignoreBodies` (array of tags to
 * exclude). It may replace maxDist or follow it.
 *
 * @returns {Array<{ bodyId:number, fraction:number, position:{x,y,z},
 *                   normal:{x,y,z}, userData:bigint }>}
 *          bodyId is -1 if a hit body has no JS tag (engine-owned body).
 */
const hits = Physics.raycast(ox, oy, oz, dx, dy, dz, /*maxDist*/ 100);
const seen = Physics.raycast(ox, oy, oz, dx, dy, dz, 100,
                             { layers: ['static'], ignoreBody: myBody });

/**
 * Cast a ray and return ONLY the nearest hit (or null if nothing was hit).
 * Cheaper than raycast() for long-range line-of-sight / pick queries since it
 * collects a single closest hit instead of sorting an array. Same narrow-phase
 * precision, same maxDist=1000 default, and same optional trailing filter opts
 * as raycast().
 *
 * @returns {{ bodyId:number, fraction:number, position:{x,y,z},
 *             normal:{x,y,z}, userData:bigint } | null}
 *          bodyId is -1 if the hit body has no JS tag (engine-owned body).
 */
const hit = Physics.raycastClosest(ox, oy, oz, dx, dy, dz, /*maxDist*/ 100);
if (hit) console.log('nearest', hit.bodyId, hit.fraction, hit.normal);


// -----------------------------------------------------------------------------
// Shape casts & overlap queries
// -----------------------------------------------------------------------------
//
// Narrow-phase queries: like raycast, these test exact shape geometry and
// return real contact points and normals.
//
// All three take an opts object whose shape fields read exactly like
// createBody (shape kind + dimensions + position/rotation), restricted to
// CONVEX shapes: "box" | "sphere" | "capsule" | "cylinder" | "convexHull".
//
// Shared filter fields (also on overlapPoint's trailing opts):
//   layers:     array of layer names/indices the query can SEE. Independent
//               of the collision matrix, a query may see layers that never
//               collide with anything. Default: all layers.
//   ignoreBody: one body tag to exclude (e.g. the caster itself).
//   ignoreBodies: array of body tags to exclude (union with ignoreBody,
//               e.g. the caster plus everything it is carrying).

/**
 * Sweep a convex shape from its transform along direction*maxDistance and
 * return ALL hits: one per body (its earliest contact), sorted by fraction.
 *
 * @param {Object} opts
 * @param {string}  opts.shape        - "box" | "sphere" | "capsule" | "cylinder" | "convexHull"
 * @param {{x,y,z}} [opts.position]   - start transform (default origin)
 * @param {{x,y,z,w}} [opts.rotation] - start rotation (default identity)
 * @param {{x,y,z}} opts.direction    - cast direction (unit vector; the sweep
 *                                       is direction*maxDistance, fraction is 0..1 along it)
 * @param {number}  [opts.maxDistance=1000]
 * @param {(string|number)[]} [opts.layers] - layer filter (see above)
 * @param {number}  [opts.ignoreBody]       - body tag to exclude
 * @param {number[]} [opts.ignoreBodies]    - body tags to exclude (union)
 * @returns {Array<{ bodyId:number, fraction:number, position:{x,y,z},
 *                   normal:{x,y,z}, userData:bigint }>}
 *          position = contact point on the hit body; normal = surface normal
 *          on the hit body (points back toward the cast shape).
 *          bodyId is -1 if the hit body has no JS tag (engine-owned body).
 */
const sweeps = Physics.castShape({
    shape: 'sphere', radius: 0.5,
    position: { x: 0, y: 10, z: 0 },
    direction: { x: 0, y: -1, z: 0 },
    maxDistance: 20,
});

/**
 * Like castShape but returns ONLY the nearest hit (or null). Cheaper. Jolt
 * early-outs beyond the best fraction. The go-to for "can I move there"
 * checks, projectile sweeps, and character-controller style probes.
 * @returns {{ bodyId, fraction, position, normal, userData } | null}
 */
const sweep = Physics.castShapeClosest({
    shape: 'capsule', radius: 0.3, halfHeight: 0.6,
    position: player.pos, direction: { x: 1, y: 0, z: 0 }, maxDistance: 2,
    ignoreBody: player.body,
});
if (sweep) console.log('wall at', sweep.fraction, 'normal', sweep.normal);

/**
 * All bodies overlapping a convex shape at a transform (no sweep). One entry
 * per body with its deepest contact.
 *
 * @param {Object} opts - shape + position/rotation + layers/ignoreBody as above
 * @returns {Array<{ bodyId:number, depth:number, position:{x,y,z},
 *                   normal:{x,y,z}, userData:bigint }>}
 *          depth = penetration depth; position = contact point on the
 *          overlapped body; normal = contact normal on the overlapped body
 *          (points back toward the query shape).
 */
const around = Physics.overlapShape({
    shape: 'sphere', radius: 3,
    position: { x: 0, y: 1, z: 0 },
    layers: ['moving'],
});
for (const o of around) console.log('in blast radius:', o.bodyId, o.depth);

/**
 * All bodies containing a world-space point (shapes are treated as solid).
 * For mesh shapes this is only meaningful if the mesh is a closed manifold.
 *
 * @param {number} x
 * @param {number} y
 * @param {number} z
 * @param {Object} [opts] - { layers, ignoreBody, ignoreBodies } filter as above
 * @returns {Array<{ bodyId:number, userData:bigint }>}
 */
const under = Physics.overlapPoint(mx, my, mz);
if (under.length) console.log('picked body', under[0].bodyId);


// -----------------------------------------------------------------------------
// Contact events
// -----------------------------------------------------------------------------

/**
 * Drain pending contact events since the last call. Call once per frame from
 * the JS update loop. Sensor overlaps are reported here too with sensor:true.
 *
 * Event semantics (intentionally narrow):
 *   - "added"   fires ONCE when a new pair forms first contact (Jolt's
 *               OnContactAdded). It does NOT repeat while bodies remain
 *               touching.
 *   - "removed" fires ONCE when a pair separates (Jolt's OnContactRemoved).
 *   - There is NO per-step "still in contact" event. Jolt's
 *     OnContactPersisted is intentionally not surfaced: most game code
 *     wants begin/end semantics, and surfacing per-pair-per-frame events
 *     swamps the queue. If you need persistent presence, track which pairs
 *     you've seen "added" and not yet "removed" yourself.
 *
 * `sensor` is reported on removed events as well as added ones, so a trigger
 * gives you a clean enter/leave pair. (Jolt's removal callback fires from the
 * broadphase with only body IDs, and possibly for a body that has just been
 * destroyed, so the engine keeps its own per-body sensor bit to label these.)
 *
 * The returned array also carries an `overflow` property: true when the
 * fixed-capacity per-step contact buffer overflowed since the last drain and
 * events were DROPPED: possibly including sensor exits, so any enter/leave
 * bookkeeping you keep may be wedged. On overflow, re-derive presence with a
 * query (overlapShape / overlapPoint) instead of trusting the stream.
 *
 * "added" events also carry a manifold snapshot:
 *   - points:      up to 4 world-space contact points (on body2's surface)
 *   - normal:      the contact normal, the direction body2 moves out of
 *                  collision, i.e. it points from body1 toward body2
 *   - penetration: penetration depth in meters; negative = speculative
 *                  contact (bodies may not actually touch after solving)
 *   - impulse:     estimated collision impulse magnitude (kg·m/s), summed
 *                  over the contact points. This is a PRE-SOLVE estimate
 *                  (Jolt's EstimateCollisionResponse: the solver never
 *                  reports the solved impulses): accurate for an isolated
 *                  two-body impact, approximate when several bodies pile
 *                  into the same island. 0 for sensor overlaps. Scales with
 *                  impact speed and mass: the natural "how hard did we
 *                  hit" input for impact sounds / damage / particles.
 * "removed" events have none of these (Jolt's removal callback carries only
 * the body pair).
 *
 * @returns {Array<{
 *   type: "added" | "removed",
 *   body1: number, body2: number,
 *   sensor: boolean,
 *   points?: Array<{x,y,z}>,
 *   normal?: {x,y,z},
 *   penetration?: number,
 *   impulse?: number,
 * }> & { overflow: boolean }}
 */
const events = Physics.getContacts();
if (events.overflow) { /* events were dropped this drain, resync triggers */ }

/**
 * Change a body's friction/restitution combine mode at runtime (same values
 * as the frictionCombine/restitutionCombine creation options, plus 'default'
 * to restore Jolt's built-in combine). Returns true on success.
 *
 * Jolt's defaults: combined friction = sqrt(f1*f2) (geometric mean),
 * combined restitution = max(r1, r2). Precedence when the two bodies of a
 * pair specify different modes: the higher mode wins, in the order
 * average < min < multiply < max (Unity's rule).
 */
Physics.setFrictionCombine(id, 'min');
Physics.setRestitutionCombine(id, 'max');


// -----------------------------------------------------------------------------
// Area field overrides (Godot Area3D analog)
// -----------------------------------------------------------------------------
//
// A SENSOR body can carry a field override that automatically affects every
// dynamic body overlapping its volume, applied inside the physics step, no
// per-frame JS needed. Water, low-gravity zones, planet gravity wells, fans,
// drag fields.
//
// Attach at creation via createBody({ sensor:true, area:{...} }) or at
// runtime via setAreaOverride(tag, opts) on any sensor (bodies already
// inside are picked up immediately); setAreaOverride(tag, null) clears it.
//
// Stacking (a Godot space-override subset, replace + combine + scale):
// overlapping areas are walked highest `priority` first (ties: the
// earlier-created area first). Per body:
//   - gravityMode 'combine':  adds this area's field, keeps walking.
//   - gravityMode 'scale':    multiplies the world-gravity term, keeps walking.
//   - gravityMode 'replace':  final gravity = combines accumulated so far +
//                             this field; the walk STOPS (lower-priority
//                             areas and world gravity are ignored).
// If no 'replace' is hit, world gravity (times any 'scale' factors) is added.
// The body's own gravityFactor multiplies the total (gravityFactor 0 floats
// through every field). Damping is replace-only: the highest-priority
// overlapping area that specifies linearDamping/angularDamping wins; the
// body's own damping is restored on exit. setLinearDamping/setAngularDamping
// on a body inside an override update the restored BASE, not the live value.
//
// Timing + caveats (all deterministic under headless advanceTime):
//   - Membership follows the sensor contact stream, so a field takes effect
//     on the step AFTER the overlap begins, and lets go on the step after it
//     ends (one fixed step of latency).
//   - Contact-buffer overflow (getContacts().overflow) can drop enter/exit
//     events, membership may wedge until the pair re-fires.
//   - A body that falls asleep inside an area loses its sensor contact until
//     it wakes (Jolt sensors track active bodies only).
//   - Sensor-vs-sensor overlaps carry no fields; kinematic/static bodies are
//     tracked but unaffected (fields apply to DYNAMIC bodies).

/**
 * Install/replace (opts) or clear (null) the field override on a sensor.
 * Also on sandbox world handles. Returns true on success.
 *
 * @param {number} tag - a sensor body's tag
 * @param {Object|null} opts
 * @param {{x,y,z}} [opts.gravity]        - directional field (m/s²)
 * @param {string}  [opts.gravityMode]    - 'replace' | 'combine' | 'scale'.
 *                                          Default: 'replace' when gravity/
 *                                          gravityPoint given, 'scale' when
 *                                          only gravityScale is given.
 * @param {boolean} [opts.gravityPoint]   - point gravity: the field points at
 *                                          the sensor's center of mass
 * @param {number}  [opts.gravityStrength] - point-gravity acceleration (m/s²)
 * @param {number}  [opts.falloffDistance] - >0: inverse-square falloff;
 *                                          gravityStrength is the acceleration
 *                                          AT this distance. 0 = constant.
 * @param {number}  [opts.gravityScale]   - 'scale' mode: world-gravity multiplier
 * @param {number}  [opts.linearDamping]  - >=0 replaces body linear damping inside
 * @param {number}  [opts.angularDamping] - >=0 replaces body angular damping inside
 * @param {number}  [opts.priority=0]     - higher wins; ties → earlier-created
 */
Physics.setAreaOverride(zone, { gravityScale: 0.16, priority: 1 });  // moon room
Physics.setAreaOverride(zone, null);                                  // clear

// Water volume: weak upward gravity (buoyancy) + heavy drag while submerged.
const water = Physics.createBody({
    shape: 'box', halfExtents: { x: 20, y: 3, z: 20 },
    position: { x: 0, y: -3, z: 0 }, static: true, sensor: true,
    area: {
        gravity: { x: 0, y: 2.0, z: 0 }, gravityMode: 'replace',
        linearDamping: 3.0, angularDamping: 2.0,
    },
});

// Low-gravity zone: replace with a gentle pull; bodies drift while inside.
const lowGrav = Physics.createBody({
    shape: 'box', halfExtents: { x: 5, y: 5, z: 5 },
    position: { x: 30, y: 5, z: 0 }, static: true, sensor: true,
    area: { gravity: { x: 0, y: -1.0, z: 0 } },   // gravityMode defaults to 'replace'
});

// Planet: point gravity toward the sensor's center, inverse-square falloff
// (9.8 m/s² at 10 m). Combine mode lets it coexist with world gravity 0.
const planet = Physics.createBody({
    shape: 'sphere', radius: 40, position: { x: 0, y: 200, z: 0 },
    static: true, sensor: true,
    area: { gravityPoint: true, gravityStrength: 9.8, falloffDistance: 10,
            gravityMode: 'combine' },
});


// -----------------------------------------------------------------------------
// Runtime body properties
// -----------------------------------------------------------------------------
//
// Everything below also exists on sandbox world handles (w.setMass(...)).

/**
 * Set a dynamic body's mass directly (kg). Inertia is recomputed from the
 * shape and scaled to the new mass, so the body keeps tumbling plausibly.
 * Wakes the body. No-op for static/kinematic/soft bodies and masses <= 0.
 */
Physics.setMass(id, 250);

/**
 * Per-body damping: the fraction of velocity removed per second
 * (v *= max(0, 1 - damping*dt) each step. Jolt's model). Creation options
 * linearDamping/angularDamping set the initial values; these change them
 * live (drag zones, powerups, underwater state...).
 *
 * While a body is inside an area damping override (see setAreaOverride),
 * these set the body's BASE damping: the value restored when it exits.
 */
Physics.setLinearDamping(id, 0.5);
Physics.setAngularDamping(id, 0.2);

/**
 * Per-body gravity multiplier (1 = normal, 0 = floats, negative = rises).
 * Wakes the body so the change is immediately visible.
 */
Physics.setGravityFactor(id, 0.0);

/**
 * Runtime friction / restitution (values >= 0; combined per pair with the
 * usual combine rules). New contacts see the change immediately; an
 * existing resting contact picks it up on its next solver update.
 */
Physics.setFriction(id, 0.9);
Physics.setRestitution(id, 0.6);

/**
 * Snapshot of the mutable body properties: the round-trip companion to the
 * setters above. mass is 0 for static/kinematic/soft bodies.
 * @returns {{ mass:number, friction:number, restitution:number,
 *             linearDamping:number, angularDamping:number,
 *             gravityFactor:number } | undefined}
 */
const props = Physics.getBodyProperties(id);


// -----------------------------------------------------------------------------
// Bulk transform readout (perf path)
// -----------------------------------------------------------------------------

/**
 * Returns a Float32Array of [tag, px, py, pz, qx, qy, qz, qw, ...] for
 * every body, packed at stride 8. Single allocation, no per-body JS
 * objects: preferred when you need to sync many transforms each frame.
 *
 * @param {{interpolated?: boolean}} [opts] - interpolated: true packs the
 *  render-side (interpolated) transforms when Physics.setInterpolation is on.
 */
const buf = Physics.getAllTransforms();
for (let i = 0; i < buf.length; i += 8) {
    const tag = buf[i] | 0;
    const px = buf[i+1], py = buf[i+2], pz = buf[i+3];
    // ...
}


// -----------------------------------------------------------------------------
// Pairing with the scene graph
// -----------------------------------------------------------------------------
//
// PhysicsNode auto-syncs its transform from the body each frame. See
// docs/scene-api.js → SceneGraph.createPhysicsNode.
//
// DEFAULT-WORLD TAGS ONLY. createPhysicsNode has no world parameter and
// resolves the tag against the default world; a sandbox tag silently binds
// to a different body (see the warning at the top of this file).

const tag = Physics.createBody({ shape: "sphere", radius: 0.5,
                                  position: { x: 0, y: 10, z: 0 } });
const visual = scene.createMesh({ mesh: "sphere", radius: 0.5 });
const body = scene.createPhysicsNode({ body: tag });
body.add(visual);


// -----------------------------------------------------------------------------
// Sandbox worlds (Physics.createWorldHandle)
// -----------------------------------------------------------------------------
//
// Returns an opaque handle on its own Jolt world. The engine does NOT step
// it; you call .step(dt) yourself. The body/constraint API mirrors the
// default-world Physics.* functions but lives on the handle.
//
// Only world lifecycle differs: createWorld/createWorldHandle are Physics.*
// only (a world does not make worlds), and .destroy() is handle-only (the
// default world outlives the app). Stepping config, setTimeStep/getTimeStep,
// setInterpolation/getInterpolation, is per-world here rather than global,
// and the timestep is what a bare .step() with no argument uses.

/**
 * @param {Object} [opts]
 * @param {number} [opts.maxBodies=1024]
 * @param {{x,y,z}} [opts.gravity=(0,-9.81,0)]
 * @param {number} [opts.contactBufferSize=0] - per-step contact-event buffer
 *                 capacity, clamped to [16, 65536]; 0 = auto (4*maxBodies,
 *                 min 1024). When a step produces more events than fit,
 *                 the surplus is dropped and getContacts() reports
 *                 events.overflow === true.
 * @returns {PhysicsWorldHandle}
 */
const w = Physics.createWorldHandle({ maxBodies: 256, gravity: {x:0,y:-9.81,z:0} });

w.step(1/60);                             // advance simulation
w.setGravity(0, -19.6, 0);                // mutate after creation
w.setLayers({ names:[...], matrix:[...]}); // independent layer config

const tag = w.createBody({ shape:"sphere", radius:0.5, position:{x:0,y:5,z:0} });
const xf = w.getTransform(tag);            // {position, rotation, userData}
w.setLinearVelocity(tag, 0, -1, 0);
w.setKinematic(tag);
w.moveKinematic(tag, x, y, z, dt);
w.setLayer(tag, "ghost");
w.setMotionType(tag, true);                // BOOLEAN isStatic (not a string):
                                           // true = static, false = dynamic. A body
                                           // CREATED static can never go dynamic
                                           // (no Jolt MotionProperties), no-op.
w.activate(tag);                           // wake a sleeping body
w.setUserData(tag, 42n);                   // 64-bit; w.getUserData(tag) reads back
w.addForce(tag, fx, fy, fz);               // continuous, cleared each step
w.addImpulse(tag, ix, iy, iz);             // instantaneous
w.addTorque(tag, tx, ty, tz);
w.setAngularVelocity(tag, wx, wy, wz);
const all = w.getAllTransforms();          // packed 8 floats/body, same layout as
                                           // Physics.getAllTransforms. Takes no opts,
                                           // sandbox worlds have no interpolation.
w.destroyBody(tag);

const c = w.createConstraint({ type:'distance', body1:a, body2:b, ... });
w.setConstraintMotor(c, { type:'velocity', target: 2, maxTorque: 100 }); // hinge/slider/sixdof
w.destroyConstraint(c);
w.setConstraintBreakingImpulse(c, 500);    // auto-break threshold (N·s); 0 = never
w.getConstraintBreakingImpulse(c);
const broke = w.getBrokenConstraints();    // handles broken since last call

const hits = w.raycast(ox, oy, oz, dx, dy, dz, /*maxDist*/ 100);  // narrow-phase,
const hit  = w.raycastClosest(ox, oy, oz, dx, dy, dz, 100,        // + optional filter
                              { layers: ['moving'], ignoreBody: tag }); // nearest or null
const cs   = w.castShape({ shape:'sphere', radius:0.5, position:{...},  // narrow-phase queries,
                           direction:{x:0,y:-1,z:0}, maxDistance:20 }); // same opts as Physics.*
const c1   = w.castShapeClosest({ ... });   // nearest or null
const ov   = w.overlapShape({ shape:'box', halfExtents:{...}, position:{...} });
const pts  = w.overlapPoint(x, y, z, { layers:['moving'] });
const evs  = w.getContacts();              // independent event queue

w.destroyAll();      // wipe contents but keep the world (level-restart pattern)
w.destroy();         // tear down the world entirely; do NOT use the handle after

// Trajectory-prediction pattern (typical use):
//   const ghostWorld = Physics.createWorldHandle({ maxBodies: 64 });
//   ghostWorld.setGravity(0, -gameGravity, 0);
//   // each frame, before rendering the prediction line:
//   ghostWorld.destroyAll();   // reset
//   const ghost = ghostWorld.createBody({ shape:'sphere', ... initial state ... });
//   for (let i = 0; i < N; i++) { ghostWorld.step(1/60);
//                                  points.push(ghostWorld.getTransform(ghost).position); }
//
// Reusing one world via destroyAll is much cheaper than creating/destroying
// the handle every frame, the JobSystem and Jolt's allocators stay warm.


// -----------------------------------------------------------------------------
// Character controller (Physics.createCharacter)
// -----------------------------------------------------------------------------
//
// Godot move_and_slide-style kinematic character on Jolt's CharacterVirtual.
// The character is NOT a rigid body: it moves by collision sweeps, slides
// along walls, walks up steps (<= stepUp), snaps down to floors (<=
// stickToFloor), pushes dynamic bodies (up to maxStrength newtons), and rides
// moving kinematic platforms.
//
// There is no per-frame move() call. The engine updates every character
// inside its fixed-timestep physics tick, immediately before the world step
// (headless: advanceTime drives the same tick deterministically). Your loop
// just sets a desired velocity and reads state:
//
//   character.setVelocity(vx, vy, vz), persists until changed
//   character.getState(), position/velocity/ground info
//
// Velocity semantics per fixed step:
//   - SUPPORTED (groundState "onGround"): the character moves at
//     groundVelocity + desired velocity. No gravity accumulates; a positive
//     up component is a jump launch.
//   - UNSUPPORTED ("onSteepGround" | "notSupported" | "inAir"): gravity
//     integrates into the vertical velocity; only the horizontal part of the
//     desired velocity steers (holding a positive vy can't fly). Too-steep
//     ground therefore slides the character down automatically.

/**
 * Create a character controller (capsule shape, upright).
 *
 * @param {Object} opts
 * @param {{x,y,z}} [opts.position]        - capsule CENTER (not the feet);
 *                                           total height = 2*(halfHeight+radius)
 * @param {number}  [opts.radius=0.3]
 * @param {number}  [opts.halfHeight=0.6]  - cylindrical section half-height
 * @param {{x,y,z}} [opts.up={0,1,0}]      - character up axis
 * @param {number}  [opts.mass=70]         - kg; used when pushing dynamic bodies
 * @param {number}  [opts.maxSlopeAngle=50]- degrees; steeper ground can't support
 * @param {number}  [opts.maxStrength=100] - N; max push force vs dynamic bodies
 * @param {number}  [opts.padding=0.02]    - distance kept from geometry
 * @param {number}  [opts.stepUp=0.4]      - max step height climbed while walking; 0 disables
 * @param {number}  [opts.stickToFloor=0.5]- max snap-down distance (stairs/slopes); 0 disables
 * @param {string|number} [opts.layer]     - collision layer (name or index),
 *                                           default "moving"; the character
 *                                           collides with whatever that layer
 *                                           collides with in the matrix
 * @param {boolean} [opts.innerBody=false] - create an inner rigid body (a
 *                                           kinematic body that shadows the
 *                                           character) so sensors, raycasts,
 *                                           shape queries, CCD bodies and
 *                                           contact events can SEE the
 *                                           character. Without it a character
 *                                           is invisible to all of those (it
 *                                           never enters the broadphase). The
 *                                           body's tag is `character.innerBody`.
 *                                           This governs DETECTION only:
 *                                           how hard the character shoves
 *                                           dynamic bodies is maxStrength,
 *                                           which works with or without it.
 * @param {string|number} [opts.innerBodyLayer] - inner body's collision layer;
 *                                           default = the character's layer
 * @returns {PhysicsCharacter}
 */
const player = Physics.createCharacter({
    radius: 0.3, halfHeight: 0.6,
    position: { x: 0, y: 1, z: 0 },
});

// Characters COLLIDE WITH EACH OTHER: every character is registered with a
// world-level character-vs-character checker, so two characters walked into
// each other stop and slide instead of ghosting through.

// Per-frame control: set desired velocity, read state.
player.setVelocity(runX, 0, runZ);         // walk
if (wantJump && player.getState().isGrounded)
    player.setVelocity(runX, 6, runZ);     // jump launch (one-shot vy > 0)

/**
 * State snapshot (after the last fixed step).
 *
 * @returns {{
 *   position: {x,y,z},        // capsule center
 *   velocity: {x,y,z},        // actual velocity (post-slide/collide)
 *   groundState: "onGround" | "onSteepGround" | "notSupported" | "inAir",
 *   isGrounded: boolean,      // groundState === "onGround"
 *   groundNormal: {x,y,z},    // surface normal under the character
 *   groundVelocity: {x,y,z},  // moving-platform velocity at the contact
 *   groundBodyId: number,     // body tag stood on, -1 if none
 * }}
 */
const st = player.getState();

player.getPosition();          // {x,y,z} shorthand
player.getVelocity();          // {x,y,z} shorthand
player.setPosition(x, y, z);   // teleport (no sweep; keeps velocity)
player.destroy();              // remove from the world; handle is dead after

/**
 * Crouch / stance: switch the character's shape at runtime. Takes the same
 * descriptor syntax as createBody (non-mesh shapes only: capsule, box,
 * sphere, cylinder, convexHull, compound). FEET-PLANTED: the position (shape
 * center) is shifted along `up` so the new shape's bottom lands exactly where
 * the old one's was: crouching pulls the center down, standing pushes it up.
 *
 * Jolt collision-checks the new shape first: returns FALSE and changes
 * nothing when there is no room: e.g. standing up under a low ceiling. Keep
 * polling while the crouch key is released to stand up as soon as there's
 * clearance. Updates the inner body's shape too (if innerBody was set).
 *
 * @returns {boolean} true if the shape was switched
 */
const crouched = player.setShape({ shape: 'capsule', radius: 0.3, halfHeight: 0.1 });

/**
 * Body tag of the inner rigid body (created with innerBody: true), or -1.
 * It's a real body: contact events report it, raycasts hit it, and
 * Physics.getTransform works on it. It is OWNED by the character.
 * Physics.destroyBody on it is refused (destroy the character instead).
 * @type {number}
 */
player.innerBody;

// Sandbox worlds have the same API; their characters update inside w.step(dt):
//   const w = Physics.createWorldHandle({ maxBodies: 64 });
//   const npc = w.createCharacter({ position: { x: 0, y: 1, z: 0 } });
//   npc.setVelocity(1.5, 0, 0);
//   w.step(1/60);   // character + world advance together


// -----------------------------------------------------------------------------
// Wheeled vehicles (Physics.createVehicle)
// -----------------------------------------------------------------------------
//
// A full Jolt vehicle simulation (VehicleConstraint + WheeledVehicleController)
// on a dynamic chassis body: sprung suspension per wheel, engine torque curve,
// clutch + gearbox (auto or manual), differentials with limited slip, tire
// slip-based friction, anti-roll bars. Strictly stronger than a raycast car,
// wheels are real collision shapes (cylinder-cast by default), the drivetrain
// has state (RPM, gear), and brake/handbrake are torque-based.
//
// The vehicle steps inside the engine's fixed physics tick automatically
// (the constraint registers as a Jolt step listener). There is no per-frame
// vehicle.update() call. Your loop sets driver input and reads state:
//
//   vehicle.setInput({ forward, right, brake, handBrake }), persists
//   vehicle.speed / .rpm / .gear, drivetrain state
//   vehicle.wheelState(i), per-wheel render state
//
// `type` picks the controller family: 'wheeled' (default, this section),
// 'tracked' (tanks, two skid-steered tracks), or 'motorcycle' (two-wheeler
// + lean spring): see their sections below; both build on everything here.
// vehicle.type reports it back.
//
// Conventions: chassis-local forward defaults to +Z, up to +Y. Wheel
// positions are in chassis-body local space; suspension extends along
// suspensionDirection (default straight down).

/**
 * Create a wheeled vehicle on a chassis body.
 *
 * Pass EITHER an existing dynamic body's tag as `body`, OR inline chassis
 * creation opts as `chassis` (same schema as createBody, forced dynamic).
 * Give the chassis a realistic mass via `density`: a 1.8×0.8×4 m box at
 * density 260 is ≈1500 kg; the default 1000 kg/m³ makes a very heavy car
 * that the default 500 N·m engine will barely move.
 *
 * @param {Object} opts
 * @param {number}  [opts.body]           - existing dynamic body tag (chassis)
 * @param {Object}  [opts.chassis]        - or createBody opts for a new chassis
 * @param {{x,y,z}} [opts.up={0,1,0}]     - chassis-local up
 * @param {{x,y,z}} [opts.forward={0,0,1}]- chassis-local forward
 * @param {number}  [opts.maxPitchRollAngle=180] - degrees; < 180 applies a
 *                                          righting torque that keeps the car
 *                                          from flipping past that angle
 *
 * @param {Object[]} opts.wheels          - at least one wheel:
 * @param {{x,y,z}} opts.wheels[].position           - suspension attachment
 *                                                     point, chassis-local
 * @param {{x,y,z}} [opts.wheels[].suspensionDirection={0,-1,0}]
 * @param {number}  [opts.wheels[].radius=0.3]       - m
 * @param {number}  [opts.wheels[].width=0.1]        - m
 * @param {number}  [opts.wheels[].suspensionMinLength=0.3] - m, max raised
 * @param {number}  [opts.wheels[].suspensionMaxLength=0.5] - m, max droop
 * @param {number}  [opts.wheels[].suspensionFrequency=1.5] - spring Hz
 * @param {number}  [opts.wheels[].suspensionDamping=0.5]   - 0..1 (1 = critical)
 * @param {boolean} [opts.wheels[].steerable=false]  - responds to `right` input
 * @param {number}  [opts.wheels[].maxSteerAngle=70] - degrees (steerable only)
 * @param {boolean} [opts.wheels[].driven=false]     - engine-connected
 * @param {number}  [opts.wheels[].maxBrakeTorque=1500]     - N·m, foot brake
 * @param {number}  [opts.wheels[].maxHandBrakeTorque=0]    - N·m; set ~4000 on
 *                                                     the REAR wheels for a
 *                                                     classic handbrake
 * @param {number}  [opts.wheels[].longitudinalFriction=1] - per-wheel tire grip in
 *                                                     the rolling direction: scalar
 *                                                     multiplier on Jolt's default
 *                                                     slip-ratio friction curve
 *                                                     ((0,0)(0.06,1.2)(0.2,1.0)).
 *                                                     ~0.1 = ice, 1 = tarmac,
 *                                                     >1 = racing slicks
 * @param {number}  [opts.wheels[].lateralFriction=1]  - same for sideways grip
 *                                                     (slip-angle curve
 *                                                     (0,0)(3,1.2)(20,1.0)); lower
 *                                                     it on rear wheels for a
 *                                                     drift setup
 * @param {number[]} [opts.wheels[].longitudinalFrictionCurve] - full curve override,
 *                                                     flat [slipRatio, friction, ...]
 *                                                     pairs; replaces the default
 *                                                     curve (scalar not applied)
 * @param {number[]} [opts.wheels[].lateralFrictionCurve] - full override, flat
 *                                                     [slipAngleDeg, friction, ...]
 *
 * @param {Object}  [opts.engine]
 * @param {number}  [opts.engine.maxTorque=500]  - N·m
 * @param {number}  [opts.engine.minRPM=1000]
 * @param {number}  [opts.engine.maxRPM=6000]
 *
 * @param {Object}  [opts.transmission]
 * @param {string}  [opts.transmission.mode='auto']    - 'auto' | 'manual'
 * @param {number[]} [opts.transmission.gearRatios]    - default 5-speed
 *                                                       [2.66,1.78,1.3,1,0.74]
 * @param {number[]} [opts.transmission.reverseGearRatios] - default [-2.90]
 * @param {number}  [opts.transmission.switchTime=0.5] - s (auto)
 * @param {number}  [opts.transmission.clutchStrength=10]
 * @param {number}  [opts.transmission.shiftUpRPM=4000]   - auto
 * @param {number}  [opts.transmission.shiftDownRPM=2000] - auto
 *
 * @param {Object[]} [opts.differentials] - omit to auto-derive: driven wheels
 *                                          are paired in array order, torque
 *                                          split equally. At least one wheel
 *                                          must be driven (or one explicit
 *                                          differential given): a drivetrain
 *                                          with no driven wheels is rejected
 *                                          (createVehicle throws). Explicit form:
 * @param {number}  [opts.differentials[].leftWheel=-1]  - wheel index or -1
 * @param {number}  [opts.differentials[].rightWheel=-1]
 * @param {number}  [opts.differentials[].ratio=3.42]    - gearbox→wheel ratio
 * @param {number}  [opts.differentials[].leftRightSplit=0.5] - 0=left, 1=right
 * @param {number}  [opts.differentials[].limitedSlipRatio=1.4]
 * @param {number}  [opts.differentials[].engineTorqueRatio=1] - sum to 1 across diffs
 * @param {number}  [opts.differentialLimitedSlipRatio=1.4] - between differentials
 *
 * @param {Object[]} [opts.antiRollBars]  - stiff spring between a wheel pair:
 *                                          {leftWheel, rightWheel, stiffness=1000}
 *
 * @param {string}  [opts.collisionTester='cylinder'] - wheel-vs-ground test:
 *                                          'cylinder' (accurate wheel shape) |
 *                                          'sphere' | 'ray' (cheapest; flat ground)
 * @param {string|number} [opts.testerLayer] - object layer the wheels collide
 *                                          as; default = chassis layer
 * @returns {PhysicsVehicle}
 */
const car = Physics.createVehicle({
    chassis: {
        shape: 'box', halfExtents: { x: 0.9, y: 0.4, z: 2.0 },
        position: { x: 0, y: 1.2, z: 0 }, density: 260,   // ≈ 1500 kg
    },
    wheels: [   // fronts steer, rears drive + handbrake
        { position: { x: -0.8, y: -0.3, z:  1.3 }, radius: 0.35, width: 0.25, steerable: true },
        { position: { x:  0.8, y: -0.3, z:  1.3 }, radius: 0.35, width: 0.25, steerable: true },
        { position: { x: -0.8, y: -0.3, z: -1.3 }, radius: 0.35, width: 0.25, driven: true, maxHandBrakeTorque: 4000 },
        { position: { x:  0.8, y: -0.3, z: -1.3 }, radius: 0.35, width: 0.25, driven: true, maxHandBrakeTorque: 4000 },
    ],
    antiRollBars: [ { leftWheel: 0, rightWheel: 1 }, { leftWheel: 2, rightWheel: 3 } ],
});

/**
 * Driver input (persists until changed; any non-zero input wakes the car).
 * @param {Object} input
 * @param {number} [input.forward=0]   - -1..1. Auto transmission: sign picks
 *                                       direction (car shifts into reverse on
 *                                       its own); manual: 0..1 gas pedal
 * @param {number} [input.right=0]     - -1..1 steering, 1 = right
 * @param {number} [input.brake=0]     - 0..1 foot brake
 * @param {number} [input.handBrake=0] - 0..1 hand brake
 */
car.setInput({ forward: 1, right: 0.3 });

car.speed;        // signed speed along chassis forward (m/s)
car.rpm;          // current engine RPM
car.gear;         // -1 reverse, 0 neutral, 1+ forward
car.wheelCount;   // number of wheels
car.chassisBody;  // the chassis body TAG. Use with Physics.* body functions,
                  // raycast filters, and scene.createPhysicsNode
car.getState();   // { speed, rpm, gear, isSwitchingGear } in one call

/**
 * Manual-transmission gear select (transmission.mode 'manual' only).
 * @param {number} gear - -1 reverse, 0 neutral, 1+ forward
 * @param {number} [clutchFriction=1] - 0 = clutch in, 1 = fully engaged
 */
car.setGear(1);

/**
 * Per-wheel state for rendering. Returns null for a bad index or after
 * destroy.
 *
 * @param {number} i - wheel index (order = the wheels array at create)
 * @returns {{
 *   position: {x,y,z},        // wheel center, CHASSIS-LOCAL space
 *   rotation: {x,y,z,w},      // chassis-local; includes steer + suspension +
 *                             // spin. Maps a Y-axis-aligned cylinder onto the
 *                             // wheel: exactly what a bro.mesh cylinder needs
 *   suspensionLength: number, // m, within [suspensionMinLength, suspensionMaxLength]
 *   steerAngle: number,       // rad, positive = left
 *   rotationAngle: number,    // rad [0, 2π], wheel spin
 *   angularVelocity: number,  // rad/s, positive = driving the car forward
 *   contact: boolean,         // touching anything?
 *   contactBody: number,      // body tag under the wheel, -1 if none
 *   contactNormal: {x,y,z},   // ground normal at the contact
 * }}
 */
const ws = car.wheelState(0);

car.destroy();    // remove the vehicle (constraint + drivetrain). A chassis
                  // created inline via `chassis:` is destroyed with it; a
                  // chassis passed as an existing `body:` tag survives.
                  // It stays yours to manage.
                  //
                  // Destroying the chassis body directly (Physics.destroyBody
                  // (car.chassisBody)) also removes the vehicle in the engine,
                  // but it does NOT notify this JS wrapper. `car` still looks
                  // alive: .chassisBody keeps returning the now-dead tag, and
                  // setInput/getState/wheelState quietly do nothing. Prefer
                  // car.destroy(), and drop your reference to `car` whenever
                  // you destroy the chassis yourself.

// Rendering recipe, chassis under a PhysicsNode, wheels as chassis children
// driven from wheelState each frame (no new engine machinery needed):
//
//   const chassisNode = scene.createPhysicsNode({ body: car.chassisBody });
//   chassisNode.add(scene.createMesh({ mesh: 'box', ... }));   // body visual
//   const wheelNodes = [];
//   for (let i = 0; i < car.wheelCount; i++) {
//       const n = scene.createMesh({ mesh: 'cylinder',         // Y-axis cylinder
//                                    radius: 0.35, height: 0.25 });
//       chassisNode.add(n);                                    // chassis-local
//       wheelNodes.push(n);
//   }
//   // per frame (node transform props take arrays):
//   for (let i = 0; i < car.wheelCount; i++) {
//       const ws = car.wheelState(i);
//       wheelNodes[i].position = [ws.position.x, ws.position.y, ws.position.z];
//       wheelNodes[i].quaternion = [ws.rotation.x, ws.rotation.y,
//                                   ws.rotation.z, ws.rotation.w];
//   }
//
// Transmission notes: in 'auto' mode the gearbox shifts itself using
// shiftUpRPM/shiftDownRPM and needs only the sign of `forward` to pick a
// direction, hold forward: -1 from a stop and it engages reverse. In
// 'manual' mode the gearbox stays in the gear set by setGear(); forward is
// just the gas pedal. While isSwitchingGear is true the clutch is open and
// no engine torque reaches the wheels.
//
// Sandbox worlds have the same API; the vehicle advances inside w.step(dt):
//   const w = Physics.createWorldHandle({ maxBodies: 64 });
//   const kart = w.createVehicle({ chassis: {...}, wheels: [...] });
//   kart.setInput({ forward: 1 });
//   w.step(1/60);


// -----------------------------------------------------------------------------
// Tracked vehicles (Physics.createVehicle({ type: 'tracked' }))
// -----------------------------------------------------------------------------
//
// A tank: Jolt's TrackedVehicleController on the same VehicleConstraint.
// Instead of steered wheels and differentials, the road wheels belong to two
// TRACKS ([left, right], left is the +X side when forward is +Z and up +Y),
// each with its own drivetrain connection and brake; steering is
// skid-steering (the tracks run at different rates). Everything else from
// the wheeled section carries over: chassis creation, up/forward, wheel
// suspension geometry, engine/transmission (drivetrain defaults switch to
// Jolt's tank numbers: minRPM 500, maxRPM 4000, shiftUp 3500, shiftDown
// 1000, but NOT the gear ratios: the binding only writes gearRatios /
// reverseGearRatios when JS supplies a non-empty array, so an unconfigured
// tank keeps Jolt's car ratios [2.66, 1.78, 1.3, 1.0, 0.74] / [-2.90].
// Pass transmission.gearRatios explicitly if you want tank ratios),
// antiRollBars, collision
// tester, wheelState, getState/speed/rpm/gear, setGear, destroy semantics,
// scene.createPhysicsNode on .chassisBody.
//
// Differences from wheeled:
//  - `tracks` is REQUIRED: exactly two entries, and together they must list
//    every wheel index exactly once (a track with zero wheels, an unassigned
//    wheel, or a doubly-assigned wheel is rejected, createVehicle throws).
//  - Per-wheel steerable/driven/brake fields are ignored (steer, drive, and
//    brake are per track); `differentials` is ignored.
//  - Per-wheel friction is a SCALAR on tracked vehicles (Jolt models the
//    track's terrain grip per road wheel, not slip curves): the
//    longitudinalFriction / lateralFriction multipliers apply to Jolt's
//    track defaults (4.0 longitudinal, 2.0 lateral) and the *FrictionCurve
//    overrides do not apply.
//  - setInput maps `right` onto differential track ratios the way Jolt's
//    tank sample does: the inside track slows linearly with steer input,
//    reaching a full pivot turn (inside track running backwards) at full
//    lock. right: 0.2 is a gentle turn, right: 1 spins in place. Sign
//    convention matches the wheeled controller (right > 0 turns right).
//    `handBrake` is folded into `brake` (a tank has one brake).
//  - setInput also takes explicit `leftRatio` / `rightRatio` (-1..1, each
//    multiplies that track's rotation rate) which BYPASS the steering map
//    for direct skid control: { forward: 1, leftRatio: 1, rightRatio: -1 }
//    pivots right. Ratios are clamped away from exactly 0.

/**
 * Tracked-vehicle creation: wheeled opts plus `tracks`, minus the ignored
 * fields listed above.
 *
 * @param {Object[]} opts.tracks - exactly two: [leftTrack, rightTrack]
 * @param {number[]} opts.tracks[].wheels       - wheel indices in this track
 * @param {number}  [opts.tracks[].drivenWheel=-1] - engine-connected wheel
 *                                          (index into opts.wheels, must be
 *                                          in this track); -1 = last listed
 * @param {number}  [opts.tracks[].inertia=10]        - kg·m² of track+wheels
 *                                          as seen on the driven wheel
 * @param {number}  [opts.tracks[].angularDamping=0.5]
 * @param {number}  [opts.tracks[].maxBrakeTorque=15000] - N·m on the driven wheel
 * @param {number}  [opts.tracks[].differentialRatio=6]  - gearbox → driven wheel
 * @returns {PhysicsVehicle}
 */
const tank = Physics.createVehicle({
    type: 'tracked',
    chassis: {
        shape: 'box', halfExtents: { x: 1.4, y: 0.4, z: 2.4 },
        position: { x: 0, y: 1.2, z: 0 }, density: 372,   // ≈ 4000 kg
    },
    maxPitchRollAngle: 60,
    wheels: [   // 5 road wheels per side; steer/brake fields not needed
        ...[0, 1, 2, 3, 4].map(i => ({
            position: { x:  1.4, y: -0.3, z: 2.0 - i },   // left track
            radius: 0.3, width: 0.2, suspensionFrequency: 1.0 })),
        ...[0, 1, 2, 3, 4].map(i => ({
            position: { x: -1.4, y: -0.3, z: 2.0 - i },   // right track
            radius: 0.3, width: 0.2, suspensionFrequency: 1.0 })),
    ],
    tracks: [
        { wheels: [0, 1, 2, 3, 4] },    // left
        { wheels: [5, 6, 7, 8, 9] },    // right
    ],
});

tank.type;                                        // 'tracked'
tank.setInput({ forward: 1 });                    // straight ahead
tank.setInput({ forward: 1, right: 0.2 });        // gentle right turn
tank.setInput({ forward: 1, right: 1 });          // pivot turn in place
tank.setInput({ forward: 1, leftRatio: 1, rightRatio: -1 });  // same pivot, explicit
tank.setInput({ forward: 0, brake: 1 });          // stop


// -----------------------------------------------------------------------------
// Motorcycles (Physics.createVehicle({ type: 'motorcycle' }))
// -----------------------------------------------------------------------------
//
// A two-wheeler: Jolt's MotorcycleController, the wheeled controller plus a
// lean spring that torques the chassis toward the balance/turn lean angle,
// so the bike stays upright at rest and leans into corners on its own. The
// ENTIRE wheeled section applies (wheels, engine, transmission,
// differentials, the driven rear wheel auto-derives one, setInput with
// forward/right/brake/handBrake, wheelState, destroy semantics); `lean` and
// setLeanController() are the additions.
//
// Lean spring strength: by default springConstant/springDamping are AUTO,
// scaled to the chassis's actual roll inertia (k = 150·I, c = 30·I about the
// forward axis, the stiffness-to-inertia ratio of Jolt's tuned sample bike).
// Pass explicit values to override; note Jolt's raw sample numbers
// (5000/1000) assume that sample's offset-center-of-mass chassis and will
// violently destabilize a typical uniform-density chassis.
//
// A steering-angle limit derived from maxAngle is applied at speed (Jolt's
// lean steering limit) so full-lock input can't order a lean the spring
// couldn't hold.

/**
 * Motorcycle creation: wheeled opts plus optional `lean`.
 *
 * @param {Object}  [opts.lean]
 * @param {number}  [opts.lean.maxAngle=45]       - deg the bike may lean in turns
 * @param {number}  [opts.lean.springConstant=-1] - -1 = auto (150·I_roll)
 * @param {number}  [opts.lean.springDamping=-1]  - -1 = auto (30·I_roll)
 * @param {number}  [opts.lean.springIntegrationCoefficient=0] - PID integral term
 * @param {number}  [opts.lean.springIntegrationCoefficientDecay=4] - integral
 *                                          decay while airborne
 * @param {number}  [opts.lean.smoothingFactor=0.8] - lean-target smoothing,
 *                                          0 = none, 1 = frozen
 * @returns {PhysicsVehicle}
 */
const bike = Physics.createVehicle({
    type: 'motorcycle',
    chassis: {
        shape: 'box', halfExtents: { x: 0.2, y: 0.3, z: 0.4 },
        position: { x: 0, y: 1, z: 0 }, density: 1250,   // ≈ 240 kg
    },
    maxPitchRollAngle: 60,
    wheels: [   // front steers on a 30° caster, rear drives
        { position: { x: 0, y: -0.27, z: 0.75 }, radius: 0.31, width: 0.05,
          suspensionDirection: { x: 0, y: -1, z: 0.577 },   // tan(30°) caster
          suspensionFrequency: 1.5, steerable: true, maxSteerAngle: 30,
          maxBrakeTorque: 500 },
        { position: { x: 0, y: -0.27, z: -0.75 }, radius: 0.31, width: 0.05,
          suspensionFrequency: 2.0, driven: true, maxBrakeTorque: 250 },
    ],
    engine: { maxTorque: 150, minRPM: 1000, maxRPM: 10000 },
    transmission: {
        clutchStrength: 2, shiftUpRPM: 8000, shiftDownRPM: 2000,
        gearRatios: [2.27, 1.63, 1.3, 1.09, 0.96, 0.88],
        reverseGearRatios: [-4],
    },
});

bike.type;                              // 'motorcycle'
bike.setInput({ forward: 1 });          // ride; the lean spring keeps it up
bike.setInput({ forward: 1, right: 0.5 });  // lean into a right turn

/**
 * Enable/disable the lean spring (motorcycles only; a safe no-op on other
 * vehicle types). Disable it to let the bike fall over, e.g. when the
 * rider is knocked off.
 * @param {boolean} enabled
 */
bike.setLeanController(false);


// -----------------------------------------------------------------------------
// Ragdolls (Physics.createRagdoll)
// -----------------------------------------------------------------------------
//
// A Jolt Ragdoll: a tree of dynamic rigid parts joined by swing-twist (or
// fixed) constraints, the PhysicalBone3D analog. Parent/child part pairs,
// and any parts that overlap in the bind pose, never collide with each
// other; everything else self-collides normally, and the whole ragdoll
// collides with the rest of the world.
//
// Part bodies are ORDINARY bodies in their world: partBody(i) returns a
// regular body tag, so every body API works on them, addImpulse to shove a
// limb, getVelocity, contact events, and raycasts report them like any other
// body. The flip side: destroying a part body (Physics.destroyBody) destroys
// the WHOLE ragdoll, because bodies + joints live and die as one unit.
//
// Pose format (used by pose()/localPose()/setPose/driveToPose*): a
// Float32Array, 7 floats per part, [px,py,pz, qx,qy,qz,qw], in the parts
// array order. pose() is world space; localPose() is relative to each
// part's parent (root = world). setPose and the two drive calls also accept
// 16 floats per part (column-major rigid mat4s, exactly what
// AnimationPlayer.getBoneWorldMatrix returns, packed per part).

/**
 * Create a ragdoll. Part bind transforms are MODEL space (the rest-pose
 * frame); opts.position/rotation place that frame in the world. Parents must
 * appear EARLIER in the parts array than their children (Jolt requirement).
 *
 * @param {Object} opts
 * @param {{x,y,z}}   [opts.position]        - world placement of the model origin
 * @param {{x,y,z,w}} [opts.rotation]        - world orientation
 * @param {string|number} [opts.layer=1]     - object layer for all parts
 * @param {number}  [opts.gravityFactor=1]
 * @param {number}  [opts.linearDamping=0.05]
 * @param {number}  [opts.angularDamping=0.05]
 * @param {boolean} [opts.stabilize=true]    - Jolt Stabilize(): clamps
 *                                             parent/child mass ratios and
 *                                             grows parent inertia so long
 *                                             chains don't oscillate
 * @param {boolean} [opts.activate=true]     - wake the bodies at creation
 * @param {Object}  [opts.motor]             - driveToPose motor spring:
 *                                             { frequency=10 (Hz), damping=1,
 *                                               maxTorque=-1 (N·m; <0 unlimited) }
 *
 * @param {Object[]} opts.parts              - the part tree, parents first:
 * @param {string}  [opts.parts[].name]      - for partIndex()/parent-by-name
 * @param {number|string} [opts.parts[].parent=-1] - EARLIER part index or
 *                                             name; -1 = root
 * @param {{x,y,z}}   opts.parts[].position  - bind position (part center), model space
 * @param {{x,y,z,w}} [opts.parts[].rotation]- bind rotation, model space
 * @param {string}  [opts.parts[].shape='capsule'] - 'capsule' | 'box' | 'sphere'.
 *                                             Capsules are Y-axis-aligned in
 *                                             part-local space: use rotation
 *                                             to lay a limb along X/Z
 * @param {number}  [opts.parts[].halfHeight=0.15] - capsule cylinder half-height
 * @param {number}  [opts.parts[].radius=0.08]    - capsule/sphere radius
 * @param {{x,y,z}} [opts.parts[].halfExtents={0.1,0.1,0.1}] - box half-extents
 * @param {number}  [opts.parts[].density=1000]   - kg/m³ (mass from shape volume)
 * @param {number}  [opts.parts[].mass=0]         - > 0 overrides the mass in kg
 *                                             (inertia recomputed from the
 *                                             shape for that mass)
 * @param {number}  [opts.parts[].friction=0.5]
 * @param {number}  [opts.parts[].restitution=0]
 *
 * @param {Object}  [opts.parts[].joint]     - joint to the PARENT (non-root only):
 * @param {string}  [opts.parts[].joint.type='swingTwist'] - 'swingTwist' | 'fixed'
 * @param {{x,y,z}} [opts.parts[].joint.point]     - pivot, model space;
 *                                             default = this part's position
 * @param {{x,y,z}} [opts.parts[].joint.twistAxis] - model space; default =
 *                                             parent→child bind direction
 * @param {{x,y,z}} [opts.parts[].joint.planeAxis] - model space, ⟂ to twist;
 *                                             default = auto perpendicular
 * @param {number}  [opts.parts[].joint.normalHalfConeAngle=0] - swing limit (rad)
 * @param {number}  [opts.parts[].joint.planeHalfConeAngle]    - swing limit in
 *                                             the plane axis direction (rad);
 *                                             default = normalHalfConeAngle
 *                                             (a circular cone)
 * @param {number}  [opts.parts[].joint.twistMin=0] - rad, [-π, π]
 * @param {number}  [opts.parts[].joint.twistMax=0] - rad, [-π, π]
 * @param {number}  [opts.parts[].joint.frictionTorque=0] - N·m joint friction
 *                                             when unpowered (cheap "muscle tone")
 * @returns {PhysicsRagdoll}
 */
const DEG = Math.PI / 180;
const rd = Physics.createRagdoll({
    position: { x: 0, y: 0, z: 0 },
    parts: [
        { name: 'pelvis', shape: 'capsule', halfHeight: 0.10, radius: 0.12,
          position: { x: 0, y: 1.00, z: 0 } },
        { name: 'spine', parent: 'pelvis', shape: 'capsule', halfHeight: 0.12, radius: 0.11,
          position: { x: 0, y: 1.35, z: 0 },
          joint: { point: { x: 0, y: 1.15, z: 0 }, normalHalfConeAngle: 25 * DEG,
                   twistMin: -20 * DEG, twistMax: 20 * DEG } },
        { name: 'head', parent: 'spine', shape: 'sphere', radius: 0.11,
          position: { x: 0, y: 1.72, z: 0 },
          joint: { point: { x: 0, y: 1.55, z: 0 }, normalHalfConeAngle: 35 * DEG,
                   twistMin: -45 * DEG, twistMax: 45 * DEG } },
        // Limbs: capsules are Y-aligned, so rotate the part to lay it along X
        // and give the shoulder an X twist axis.
        { name: 'upperArmR', parent: 'spine', shape: 'capsule', halfHeight: 0.10, radius: 0.05,
          position: { x: 0.36, y: 1.42, z: 0 },
          rotation: { x: 0, y: 0, z: -Math.SQRT1_2, w: Math.SQRT1_2 },
          joint: { point: { x: 0.24, y: 1.42, z: 0 }, twistAxis: { x: 1, y: 0, z: 0 },
                   normalHalfConeAngle: 60 * DEG, twistMin: -30 * DEG, twistMax: 30 * DEG } },
    ],
});

rd.partCount;            // number of parts
rd.partIndex('head');    // name → part index (-1 unknown)
rd.partParent(i);        // parent part index (-1 = root)
rd.partBody(i);          // the part's regular body TAG. Physics.addImpulse,
                         // getVelocity, raycast hits, contact events all work
rd.isActive;             // true while any part body is awake

rd.pose();               // Float32Array partCount*7, WORLD space
rd.localPose();          // Float32Array partCount*7, relative to parent part
                         // (root = world), drops into a bromesh Pose
rd.setPose(pose);        // teleport all parts (7- or 16-stride)
rd.activate();           // wake / sleep the whole body set
rd.deactivate();
rd.addImpulse(x, y, z);  // impulse on every part (N·s each)

/**
 * Power the swing-twist joints toward the target pose's parent-relative
 * rotations (Jolt DriveToPoseUsingMotors: position motors on swing + twist).
 * Motors PERSIST until stopDrive(): call once, not per frame (re-call to
 * change the target). The root is not driven; positions in the pose are
 * ignored (only relative rotations matter). Great for "get up", stagger,
 * or animation-following that stays physical.
 *
 * @param {Float32Array|number[]} pose - partCount*7 or partCount*16 floats
 * @param {Object} [motor] - override the creation-time spring:
 *                           { frequency (Hz), damping, maxTorque (N·m; <0 unlimited) }
 * @returns {boolean}
 */
rd.driveToPose(targetPose, { frequency: 15, damping: 1 });
rd.stopDrive();          // motors off → limp ragdoll again

/**
 * Hard tracking: set part velocities so every part reaches its target
 * transform in dt seconds (Jolt DriveToPoseUsingKinematics). Positions ARE
 * used. Re-issue every step while tracking, large jumps are clamped by the
 * max velocity caps, so treat it as incremental pursuit, not teleport.
 *
 * @param {Float32Array|number[]} pose - partCount*7 or partCount*16 floats
 * @param {number} dt - seconds to reach the target (typically the step size)
 * @returns {boolean}
 */
rd.driveToPoseKinematic(targetPose, 1 / 60);

rd.destroy();            // remove bodies + joints; handle is dead after.
                         // (Destroying any part body via Physics.destroyBody
                         // also destroys the whole ragdoll.)

// Sandbox worlds have the same API; the ragdoll steps inside w.step(dt):
//   const w = Physics.createWorldHandle({ maxBodies: 64 });
//   const dummy = w.createRagdoll({ parts: [...] });
//   w.step(1/60);

// -----------------------------------------------------------------------------
// Ragdoll ↔ skinned mesh recipes (Godot PhysicalBone3D flow)
// -----------------------------------------------------------------------------
//
// Author the skeleton so bone i mirrors part i, same order, same parents,
// bones AT the part bind transforms (inverseBind = inverse of the part's
// model-space bind matrix), and keep the skinned node's own transform at
// identity (the ragdoll's world transforms then ARE the mesh's model space).
// Both recipes are verified end-to-end in tests/physics/test_ragdoll.js.
//
// RECIPE 1, ragdoll drives the mesh (limp / knocked out). localPose() is
// parent-relative, exactly what the bromesh Pose's local joint slots hold:
//
//   const pose = skel.bindPose();                // bromesh rigging objects
//   function syncMeshToRagdoll() {
//       const rp = rd.localPose();               // stride 7: [t3, q4] per part
//       const pd = pose.data;                    // stride 10: [t3, q4, s3] per bone
//       for (let i = 0; i < rd.partCount; i++)
//           for (let k = 0; k < 7; k++) pd[i * 10 + k] = rp[i * 7 + k];
//       pose.data = pd;
//       node.setSkinningMatrices(pose.computeSkinningMatrices(skel));
//   }
//   // per frame while the ragdoll is active:
//   syncMeshToRagdoll();
//
// If the mesh has MORE bones than the ragdoll has parts (fingers, jaw...),
// map ragdoll parts onto their bone indices and leave the rest at bind:
// pd[boneOf[i] * 10 + k] = rp[i * 7 + k].
//
// RECIPE 2, animation drives the ragdoll (powered / getting up). Sample the
// AnimationPlayer's bone matrices (model space, column-major, accepted
// directly as a 16-stride pose) and set them as the motor target:
//
//   const boneOfPart = ['pelvis', 'spine', 'head', 'upperArmR'];  // part i → bone name
//   const target = new Float32Array(rd.partCount * 16);
//   function driveRagdollToAnimation() {
//       const player = node.player;              // AnimationPlayer (play() first)
//       for (let i = 0; i < rd.partCount; i++)
//           target.set(player.getBoneWorldMatrix(boneOfPart[i]), i * 16);
//       rd.driveToPose(target);                  // motors chase the clip
//   }
//   // call when the target should change (e.g. each frame during a getup);
//   // rd.stopDrive() to go limp again. driveToPoseKinematic(target, dt) is
//   // the hard-tracking variant (call every step, parts remain real bodies
//   // that push whatever is in the way).

// -----------------------------------------------------------------------------
// Soft bodies (Physics.createSoftBody)
// -----------------------------------------------------------------------------
//
// A Jolt SoftBody (XPBD): a particle cloud held together by edge / shear /
// bend constraints, colliding with the rest of the world, the SoftBody3D
// analog. Two creation paths:
//
//   cloth: a gridX*gridZ vertex grid in the LOCAL XZ plane (Y up), centered
//     on the local origin. Vertex (x, z) lives at index z*gridX + x, so the
//     corners are 0, gridX-1, (gridZ-1)*gridX and gridX*gridZ-1. Faces wind
//     counter-clockwise seen from +Y (rest normals point up).
//   mesh: an arbitrary triangle mesh. With pressure > 0 it should be CLOSED
//     with outward (CCW-from-outside) winding, the enclosed gas volume is
//     what inflates it (an inside-out mesh is flipped automatically). A
//     pressurized ball is mesh + pressure; no extra constraints needed.
//
// The soft body IS a regular body in its world: `sb.body` is an ordinary
// body tag, so raycasts hit it and report that tag, contact events name it,
// Physics.addImpulse / addForce on the tag move the whole body (spread over
// the vertices), and Physics.destroyBody(sb.body) destroys it. Per-vertex
// control (setVertex / setVertexVelocity / pin) is the grab surface.

/**
 * Create a soft body. Exactly one of `cloth` / `mesh` selects the path;
 * everything else is shared tuning.
 *
 * @param {Object} opts
 * @param {Object} [opts.cloth]           - cloth grid:
 * @param {number} [opts.cloth.gridX=10]  - vertices along local X (>= 2)
 * @param {number} [opts.cloth.gridZ=10]  - vertices along local Z (>= 2)
 * @param {number} [opts.cloth.spacing=0.1] - rest edge length (m)
 * @param {number} [opts.cloth.mass=1]    - TOTAL mass (kg), split evenly
 * @param {number[]|string} [opts.cloth.pinned] - vertex indices frozen in
 *                                          place (invMass 0), or 'corners'
 *                                          for all four grid corners
 * @param {Object} [opts.mesh]            - arbitrary mesh:
 * @param {Float32Array|number[]} opts.mesh.vertices - local xyz triples
 * @param {Uint32Array|number[]}  opts.mesh.indices  - triangle list
 * @param {number} [opts.mesh.mass=1]     - TOTAL mass (kg), split evenly
 * @param {number} [opts.mesh.pressure=0] - n*R*T gas coefficient; > 0 keeps
 *                                          a closed mesh inflated (try
 *                                          ~1000-5000 for a beach ball)
 * @param {number[]} [opts.mesh.pinned]   - vertex indices frozen in place
 *
 * @param {number}  [opts.compliance=0]      - edge stretch compliance
 *                                             (1/stiffness; 0 = rigid edges,
 *                                             larger = stretchier: 1e-4 is
 *                                             already noticeably soft)
 * @param {number}  [opts.shearCompliance]   - cloth shear edges; default =
 *                                             compliance
 * @param {number}  [opts.bendCompliance]    - OMIT for no bend constraints
 *                                             (crumply cloth); >= 0 adds
 *                                             distance-bend constraints
 *                                             (0 = stiff sheet)
 * @param {number}  [opts.numIterations=5]   - XPBD solver iterations
 * @param {number}  [opts.friction=0.2]
 * @param {number}  [opts.restitution=0]
 * @param {number}  [opts.linearDamping=0.1] - corner-pinned "hammocks" swing
 *                                             forever at low damping
 * @param {number}  [opts.gravityFactor=1]
 * @param {number}  [opts.vertexRadius=0]    - particle radius; a small value
 *                                             (~0.01) keeps the surface off
 *                                             other bodies (fights z-fights)
 * @param {boolean} [opts.updatePosition=true] - body position follows the
 *                                             vertices (false for something
 *                                             welded to the static world)
 * @param {boolean} [opts.doubleSided=true]  - queries (raycast etc.) hit the
 *                                             faces from both sides
 * @param {boolean} [opts.allowSleeping=true]
 * @param {string|number} [opts.layer=1]     - object layer
 * @param {{x,y,z}}   [opts.position]        - world placement of the local origin
 * @param {{x,y,z,w}} [opts.rotation]        - baked into the vertices (the
 *                                             body itself keeps identity
 *                                             rotation: Jolt simulates soft
 *                                             bodies more accurately that way)
 * @returns {PhysicsSoftBody}
 */
const sb = Physics.createSoftBody({
    cloth: { gridX: 20, gridZ: 20, spacing: 0.1, mass: 1, pinned: 'corners' },
    position: { x: 0, y: 2, z: 0 },
});

sb.vertexCount;          // number of particles
sb.body;                 // the soft body's regular body TAG (-1 after destroy)
sb.vertices();           // Float32Array vertexCount*3, WORLD-space positions,
                         // one snapshot per call (stream this into a mesh)
sb.topology();           // { positions, indices, gridX, gridZ }, the REST
                         // shape: local positions (Float32Array), triangle
                         // list (Uint32Array), and the cloth grid (0/0 for
                         // mesh bodies). Vertex order matches vertices()
                         // one-to-one and is stable for the body's lifetime.

sb.setVertex(i, x, y, z);         // teleport one vertex (world space, zeroes
                                  // its velocity), grab interactions. Fast
                                  // drags prefer setVertexVelocity (a placed
                                  // vertex can tunnel).
sb.setVertexVelocity(i, x, y, z); // set one vertex's velocity (world space)
sb.pin(i, pinned = true);         // freeze / release a vertex at runtime
sb.destroy();                     // remove the body; handle is dead after.
                                  // (Physics.destroyBody(sb.body) is the same
                                  // teardown through the generic body API.)

// A pressurized ball that bounces:
//   const ball = Physics.createSoftBody({
//       mesh: { vertices, indices, pressure: 2000, mass: 2 },  // closed mesh
//       position: { x: 0, y: 3, z: 0 }, restitution: 0.6,
//   });
//   Physics.addImpulse(ball.body, 20, 0, 0);   // regular body API. It rolls

// Sandbox worlds have the same API; the soft body steps inside w.step(dt):
//   const w = Physics.createWorldHandle({ maxBodies: 64 });
//   const cloth = w.createSoftBody({ cloth: { gridX: 8, gridZ: 8 } });
//   w.step(1/60);

// -----------------------------------------------------------------------------
// Soft body ↔ scene mesh recipe (SoftBody3D rendering flow)
// -----------------------------------------------------------------------------
//
// topology() is the render-mesh blueprint (same vertex order as vertices());
// build a MeshNode from it once, keep the node at IDENTITY transform (the
// soft body streams WORLD-space positions), and per frame push vertices()
// through updateMesh with recomputeNormals so the deforming surface stays
// lit. Verified end-to-end in tests/physics/test_softbody.js.
//
//   const sb = Physics.createSoftBody({
//       cloth: { gridX: 16, gridZ: 16, spacing: 0.1, pinned: 'corners' },
//       position: { x: 0, y: 2, z: 0 },
//   });
//   const topo = sb.topology();
//   const node = scene.createMesh({
//       positions: sb.vertices(), indices: topo.indices,
//       recomputeNormals: true,               // no normals in the stream,
//       color: 'crimson', roughness: 0.9,     // derive smooth ones
//   });
//   // per frame:
//   node.updateMesh({ positions: sb.vertices(), indices: topo.indices },
//                   { recomputeNormals: true });
//
// A cloth seen from both sides wants `twoSided: true` on createMesh (the
// mesh pass backface-culls otherwise). For a pressurized ball pass the SAME
// vertices/indices you gave the physics mesh, vertex order is preserved.
