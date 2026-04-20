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
//
// Constraints, character controllers, and per-body material/layer
// configuration are NOT exposed to JS yet — the C++ world supports them
// internally, but no binding has been wired through.
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
// Body creation
// -----------------------------------------------------------------------------

/**
 * Create a rigid body and return its integer tag.
 *
 * @param {Object} opts
 * @param {string}  opts.shape        - "box" | "sphere" | "capsule" | "cylinder"
 * @param {{x,y,z}} [opts.position]   - default {0,0,0}
 * @param {{x,y,z,w}} [opts.rotation] - quaternion, default identity
 * @param {boolean} [opts.static=false]
 * @param {number}  [opts.friction=0.5]
 * @param {number}  [opts.restitution=0.3]
 *
 * Shape-specific:
 * @param {{x,y,z}} [opts.halfExtents] - box (default 0.5, 0.5, 0.5)
 * @param {number}  [opts.radius]      - sphere / capsule / cylinder
 * @param {number}  [opts.halfHeight]  - capsule / cylinder
 *
 * @returns {number} body tag (use everywhere else)
 */
const id = Physics.createBody({
    shape: "box",
    position: { x: 0, y: 5, z: 0 },
    halfExtents: { x: 0.5, y: 0.5, z: 0.5 },
    friction: 0.6,
});

/** Destroy a body and free its tag. Safe to call with an unknown tag. */
Physics.destroyBody(id);


// -----------------------------------------------------------------------------
// Per-body queries / mutations
// -----------------------------------------------------------------------------

/** @returns {{position:{x,y,z}, rotation:{x,y,z,w}}} or undefined if tag is unknown */
Physics.getTransform(id);

/** @returns {{linear:{x,y,z}, angular:{x,y,z}}} or undefined */
Physics.getVelocity(id);

Physics.setPosition(id, x, y, z);
Physics.setRotation(id, qx, qy, qz, qw);
Physics.setLinearVelocity(id, x, y, z);

/** Continuous force, applied per substep until next call. */
Physics.addForce(id, x, y, z);

/** One-shot impulse (instant velocity change scaled by mass). */
Physics.addImpulse(id, x, y, z);

/** Continuous torque. */
Physics.addTorque(id, x, y, z);

/** @returns {boolean} — false for sleeping bodies and unknown tags. */
Physics.isActive(id);

/** Wake a sleeping body. */
Physics.activate(id);


// -----------------------------------------------------------------------------
// Raycasting
// -----------------------------------------------------------------------------

/**
 * Cast a ray against the world; returns ALL hits (not just closest), sorted
 * by Jolt's traversal order. Pass maxDist=0 to default to 1000.
 *
 * @returns {Array<{ bodyId:number, fraction:number, position:{x,y,z} }>}
 *          bodyId is -1 if a hit body has no JS tag (engine-owned body).
 */
const hits = Physics.raycast(ox, oy, oz, dx, dy, dz, /*maxDist*/ 100);


// -----------------------------------------------------------------------------
// Contact events
// -----------------------------------------------------------------------------

/**
 * Drain pending contact events since the last call. Call this once per
 * frame from the JS update loop — events are accumulated by Jolt's
 * listener between drains.
 *
 * @returns {Array<{ type:"added"|"removed", body1:number, body2:number }>}
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
