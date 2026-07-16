// =============================================================================
// bro Physics API Reference
// =============================================================================
//
// Thin JS wrapper around Jolt physics. Bodies are referenced by an integer
// "tag" (a small monotonic ID) — not by JS object. Pair a tag with
// scene.createPhysicsNode({ body: tag }) for visual sync.
//
// Methods live on the global `Physics` namespace (no `bro.` prefix — this
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
//    does NOT auto-step these — the caller invokes handle.step(dt) manually.
//    Use cases: trajectory previews ("ghost" balls / what-if simulations),
//    deterministic side-simulations for AI, server-authoritative replicas.
//    Sandbox worlds share the same body-creation API as the default world.
//
// Tag spaces are PER WORLD: a tag returned by w.createBody is meaningful
// only on `w`, never on the default world. The same number could refer to
// different bodies in different worlds — keep them straight.
// =============================================================================


// -----------------------------------------------------------------------------
// World configuration
// -----------------------------------------------------------------------------

/**
 * Acknowledge the physics world. The engine has already created it; this
 * call is a no-op kept for forward compat. Returns true on success.
 * @param {Object} [opts]
 * @param {number} [opts.maxBodies=4096]
 */
Physics.createWorld(opts);

/** Set world gravity (default 0, -9.81, 0). */
Physics.setGravity(x, y, z);

/** @returns {{x:number, y:number, z:number}} */
Physics.getGravity();

/** Set the fixed timestep (seconds) used by the engine when stepping the world. */
Physics.setTimeStep(dt);


// -----------------------------------------------------------------------------
// Collision layers
// -----------------------------------------------------------------------------
//
// Up to 16 named object layers and a row-major n*n collision matrix.
// Default config: ["static", "moving"] with [false, true, true, true]
// (static-vs-static disabled, everything else enabled).
//
// Reconfiguring layers takes effect for ALL bodies — including ones already
// created — so call this once at startup before creating gameplay bodies.

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
 * @param {boolean} [opts.ccd=false]    - LinearCast motion quality (for fast-moving bodies)
 * @param {string}  [opts.dofs]         - "all" (default) | "2d"
 *                                         "2d" locks Z translation and X/Y rotation (Plane2D).
 *                                         Or comma-separated tokens: "tx,ty,rz" etc.
 * @param {string|number} [opts.layer]  - layer name or index; defaults to "static" (0) if static, else "moving" (1)
 * @param {number|bigint} [opts.userData=0] - 64-bit user data; survives round trips via getTransform/raycast/getUserData
 * @param {number}  [opts.friction=0.5]
 * @param {number}  [opts.restitution=0.3]
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
 *
 * Notes:
 * - "heightfield" shapes are always static. The surface in body-local space is
 *   offset + scale * (x, heights[z*n + x], z) for integer x,z in [0, n-1] — the
 *   grid spans scale.x*(n-1) by scale.z*(n-1) starting at the body position, NOT
 *   centered on it; use offset (or position) to center. Much cheaper than an
 *   equivalent static "mesh" for terrain (quantized samples + hierarchical grid,
 *   no triangle soup).
 * - "mesh" shapes are always static (Jolt limitation). Triangle winding determines
 *   which side collides — Jolt mesh shapes are one-sided. CCW = +Y normal in
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
// front face — back-face culling makes the chain one-sided. Set flipNormal
// to swap. Always static.
Physics.createBody({
    shape: 'chain',
    points: [-10, 0,  0, 0,  0, 10],   // flat [x0,y0,x1,y1,...] in 2D
    depth: 4,                          // total Z thickness (for 2D-DOF bodies)
    closed: false,                     // close loop: connects last→first
    flipNormal: false,
});

// Heightfield terrain: 64x64 samples, 1m cells, heights from any source
// (noise, image, analytic). Collides as real terrain — much cheaper than a
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

/** 64-bit user data — typically used to map body→entity-id. */
Physics.setUserData(id, value);
/** @returns {bigint} */
Physics.getUserData(id);

/** @returns {boolean} — false for sleeping bodies and unknown tags. */
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
// A kinematic body is moved by script — gravity does not affect it, and
// dynamic bodies do not push it. But it DOES push dynamic bodies on
// contact, with stable contact forces (unlike teleporting via setPosition,
// which produces unphysical impulses). Use for moving platforms, paddles,
// elevators, swept hazards, etc.

/** Convert an existing body into a kinematic body (preserves shape/transform). */
Physics.setKinematic(id);

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
 * @param {boolean} [opts.collideConnected=false]
 * @param {number}  [opts.breakingImpulse=0] - auto-break threshold (N·s); 0 = never break.
 *                                  When exceeded in a step the constraint is disabled and
 *                                  reported by Physics.getBrokenConstraints(). Also settable
 *                                  later via setConstraintBreakingImpulse().
 *
 * Cone-only fields (point + limited swing about the twist axis — e.g. a shoulder):
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
 * SixDOF-only fields (Godot Generic6DOFJoint3D analog — configure each of the
 * six degrees of freedom independently):
 * @param {{x,y,z}} [opts.axisX={1,0,0}]   - constraint-space X axis (world)
 * @param {{x,y,z}} [opts.axisY={0,1,0}]   - constraint-space Y axis (world; re-orthonormalized)
 * @param {string}  [opts.swingType='cone'] - rotation-Y/Z limit shape: 'cone' | 'pyramid'
 * @param {Object}  [opts.axes]            - per-axis config, keys translationX/Y/Z +
 *                                  rotationX/Y/Z. Each value is 'locked' (default),
 *                                  'free', or { min, max, frequency?, damping?, friction? }:
 *                                  min/max = limit range (m for translation; rad for
 *                                  rotation — rotationX is the twist in [-π,π],
 *                                  rotationY/Z limits are symmetric, Jolt uses max);
 *                                  frequency/damping (>0 Hz) make translation limits
 *                                  soft springs; friction = resistance (N / N·m) when
 *                                  the axis has no motor. An object without min/max
 *                                  means 'free' (useful for friction-only axes).
 * @param {Object}  [opts.motors]          - per-axis motors at create, keyed like `axes`,
 *                                  each a motor options object (see setConstraintMotor).
 *
 * Motors at create (hinge / slider only — sixdof uses `motors` above):
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

// SixDOF: a crane arm — free vertical travel within ±2 m (soft-limited by a
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
 * handles (single driven axis — `axis` is ignored) and on sixdof handles
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
 * per frame). Broken constraints are disabled, not destroyed — call
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
 * An optional trailing opts object takes the same filter fields as the shape
 * queries below: `layers` (array of layer names/indices the ray can see) and
 * `ignoreBody` (one body tag to exclude). It may replace maxDist or follow it.
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
 * precision and optional trailing filter opts as raycast().
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
//               of the collision matrix — a query may see layers that never
//               collide with anything. Default: all layers.
//   ignoreBody: one body tag to exclude (e.g. the caster itself).

/**
 * Sweep a convex shape from its transform along direction*maxDistance and
 * return ALL hits — one per body (its earliest contact), sorted by fraction.
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
 * Like castShape but returns ONLY the nearest hit (or null). Cheaper — Jolt
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
 * @param {Object} [opts] - { layers, ignoreBody } filter as above
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
 *     OnContactPersisted is intentionally not surfaced — most game code
 *     wants begin/end semantics, and surfacing per-pair-per-frame events
 *     swamps the queue. If you need persistent presence, track which pairs
 *     you've seen "added" and not yet "removed" yourself.
 *
 * `sensor` is reported on removed events as well as added ones, so a trigger
 * gives you a clean enter/leave pair. (Jolt's removal callback fires from the
 * broadphase with only body IDs — and possibly for a body that has just been
 * destroyed — so the engine keeps its own per-body sensor bit to label these.)
 *
 * @returns {Array<{
 *   type: "added" | "removed",
 *   body1: number, body2: number,
 *   sensor: boolean,
 * }>}
 */
const events = Physics.getContacts();


// -----------------------------------------------------------------------------
// Bulk transform readout (perf path)
// -----------------------------------------------------------------------------

/**
 * Returns a Float32Array of [tag, px, py, pz, qx, qy, qz, qw, ...] for
 * every body, packed at stride 8. Single allocation, no per-body JS
 * objects — preferred when you need to sync many transforms each frame.
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
// it; you call .step(dt) yourself. Body API mirrors the default-world
// Physics.* functions, but lives on the handle.

/**
 * @param {Object} [opts]
 * @param {number} [opts.maxBodies=1024]
 * @param {{x,y,z}} [opts.gravity=(0,-9.81,0)]
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
// the handle every frame — the JobSystem and Jolt's allocators stay warm.


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
//   character.setVelocity(vx, vy, vz)   — persists until changed
//   character.getState()                — position/velocity/ground info
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
 * @returns {PhysicsCharacter}
 */
const player = Physics.createCharacter({
    radius: 0.3, halfHeight: 0.6,
    position: { x: 0, y: 1, z: 0 },
});

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

// Sandbox worlds have the same API; their characters update inside w.step(dt):
//   const w = Physics.createWorldHandle({ maxBodies: 64 });
//   const npc = w.createCharacter({ position: { x: 0, y: 1, z: 0 } });
//   npc.setVelocity(1.5, 0, 0);
//   w.step(1/60);   // character + world advance together
