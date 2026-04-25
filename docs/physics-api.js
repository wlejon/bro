// =============================================================================
// bro Physics API Reference
// =============================================================================
//
// Thin JS wrapper around the engine's Jolt physics world (singleton; the
// engine owns it and JS just talks to it). Bodies are referenced by an
// integer "tag" (a small monotonic ID) — not by JS object. Pair a tag with
// scene.createPhysicsNode({ body: tag }) for visual sync.
//
// All methods live on the global `Physics` namespace (no `bro.` prefix —
// this binding pre-dates the bro.* convention). The world is created and
// stepped by the engine; JS only configures it and creates/destroys bodies.
//
// Coordinate system: right-handed Y-up, world units (typically meters).
// Quaternions are {x, y, z, w}; passing the wrong order silently
// produces nonsense rotations.
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
 * @param {string}  opts.shape        - "box" | "sphere" | "capsule" | "cylinder" | "convexHull" | "mesh" | "compound"
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
 *
 * Notes:
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

/** Destroy a body and free its tag. Safely removes any constraints attached to it. */
Physics.destroyBody(id);


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


// -----------------------------------------------------------------------------
// Constraints (joints)
// -----------------------------------------------------------------------------

/**
 * Create a constraint linking two bodies (or one body to the world if body2
 * is omitted). Returns a non-zero handle on success.
 *
 * @param {Object} opts
 * @param {string} opts.type      - "distance" | "point" | "hinge" | "fixed" | "slider"
 * @param {number} opts.body1     - first body tag
 * @param {number} [opts.body2]   - second body tag (omit / pass -1 to attach to world)
 * @param {{x,y,z}} [opts.point1] - world-space anchor on body1
 * @param {{x,y,z}} [opts.point2] - world-space anchor on body2
 * @param {number}  [opts.minDistance]   - distance: lower bound (negative = use rest length)
 * @param {number}  [opts.maxDistance]   - distance: upper bound
 * @param {{x,y,z}} [opts.axis]          - hinge / slider: world-space axis
 * @param {number}  [opts.limitMin]      - hinge: min angle (rad); slider: min position
 * @param {number}  [opts.limitMax]      - hinge: max angle (rad); slider: max position
 * @param {boolean} [opts.collideConnected=false]
 * @returns {number} handle (truthy on success)
 */
const j = Physics.createConstraint({
    type: 'distance',
    body1: tagA, body2: tagB,
    point1: {x:0, y:5, z:0}, point2: {x:1, y:5, z:0},
    minDistance: 0.5, maxDistance: 1.5,
});

/** Disable / enable a constraint without destroying it. */
Physics.setConstraintEnabled(handle, true);

/** Remove a constraint. */
Physics.destroyConstraint(handle);


// -----------------------------------------------------------------------------
// Raycasting
// -----------------------------------------------------------------------------

/**
 * Cast a ray against the world; returns ALL hits (sorted by distance).
 *
 * @returns {Array<{ bodyId:number, fraction:number, position:{x,y,z}, userData:bigint }>}
 *          bodyId is -1 if a hit body has no JS tag (engine-owned body).
 */
const hits = Physics.raycast(ox, oy, oz, dx, dy, dz, /*maxDist*/ 100);


// -----------------------------------------------------------------------------
// Contact events
// -----------------------------------------------------------------------------

/**
 * Drain pending contact events since the last call. Call once per frame from
 * the JS update loop. Sensor overlaps are reported here too with sensor:true.
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
