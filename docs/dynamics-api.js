/**
 * =============================================================================
 * bro.dynamics — rigid bodies, joints, and what goes through a joint
 * =============================================================================
 *
 * Worlds of rigid bodies over Box3D (wlejon/box3d, the fork of Erin Catto's
 * engine; src/dynamics/world.h). A world is stepped by the code that made it:
 * nothing steps it for you, and it is not tied to the scene or to bro.time.
 * It runs on the CPU, the same in bro, bro-headless and bro-server, and in a
 * Worker (a world belongs to the thread that made it). The same calls give
 * the same bodies bit for bit on Windows, Linux and macOS, x64 and arm64, so
 * peers can send inputs and step alike (tests/dynamics/test_same_everywhere.js).
 *
 * Bodies and joints are small integers, reused once removed. Vectors are
 * arrays ([x, y, z]); a rotation is a quaternion [x, y, z, w]. Units are SI.
 *
 * `bro.dynamics.available` is false in a build without BRO_WITH_DYNAMICS.
 */

/**
 * @param {{gravity?: number[]}} [options]  gravity defaults to [0, -9.81, 0]
 * @returns {World}
 *
 * @example
 * const w = bro.dynamics.createWorld();
 * const ground = w.addBox({ position: [0, -1, 0], half: [50, 1, 50], mass: 0 });
 * const crate = w.addBox({ position: [0, 3, 0], half: [0.5, 0.5, 0.5], mass: 80 });
 * for (let i = 0; i < 120; i++) w.step(1 / 60);
 * w.pose(crate);        // Float32Array [x, y, z, qx, qy, qz, qw]
 */
bro.dynamics.createWorld = function (options) {};

class World {
    /** Frees the world. Its bodies and joints go with it. */
    destroy() {}

    /** @param {number[]} g */
    setGravity(g) {}

    /** The fastest a body moves (m/s, 400 unless set). @type {number} write only */
    maxSpeed;

    /** The closing speed past which two bodies meeting is a hit (m/s, 1 unless set). @type {number} write only */
    hitSpeed;

    /**
     * A box. `mass` 0 fixes it in place; otherwise it is the body's mass in kg
     * (default 1), spread evenly through the shape. Every add* takes these:
     *
     *   position, rotation     where it is
     *   mass, friction, restitution
     *   velocity, angular      what it starts with
     *   linearDamping, angularDamping, gravityScale
     *   kinematic   moved by its velocity or by moveTo, pushed by nothing
     *   bullet      swept against what moves, for the small and fast
     *   planar      held to the plane z = its z, turning about z only
     *   layers      bit set of the layers it is on (default 1)
     *   mask        bit set of the layers it touches (default all): two bodies
     *               touch when each one's mask holds a layer of the other
     *
     * @param {{half: number[], position?: number[], rotation?: number[], mass?: number,
     *          friction?: number, restitution?: number, velocity?: number[], angular?: number[],
     *          linearDamping?: number, angularDamping?: number, gravityScale?: number,
     *          kinematic?: boolean, bullet?: boolean, planar?: boolean,
     *          layers?: number, mask?: number}} o
     * @returns {number} the body
     */
    addBox(o) {}

    /** A sphere of `radius`. @returns {number} the body */
    addSphere(o) {}

    /** A capsule along the body's y: `radius`, and `halfHeight` of its straight part. */
    addCapsule(o) {}

    /** A cylinder along the body's y: `radius`, `halfHeight`, `segments` (16) round it. */
    addCylinder(o) {}

    /**
     * The convex hull of `points` ([x, y, z, x, y, z, ...], body space, four or
     * more). Throws when they make none (all in one plane).
     */
    addHull(o) {}

    /**
     * Triangles, fixed in place: `positions` (x y z per vertex, body space) and
     * `indices` (three per triangle, counter-clockwise seen from outside). Only
     * bodies that move touch it.
     *
     * @example
     * const floor = w.addMesh({ positions: [-9, 0, -9, 9, 0, -9, 9, 0, 9, -9, 0, 9], indices: [0, 2, 1, 0, 3, 2] });
     */
    addMesh(o) {}

    /** Removes a body and the joints on it. */
    removeBody(body) {}

    /** @returns {boolean} */
    hasBody(body) {}

    /**
     * A joint between two bodies, or between one body and the world itself
     * (leave `b` out). `anchor` and `axis` are in the world, at the bodies'
     * poses when the joint is made.
     *
     *   'weld'   nothing moves
     *   'hinge'  turns about `axis` through `anchor`
     *   'ball'   turns any way about `anchor`
     *
     * `breakForce` (N) and `breakTorque` (N m) are what the joint can take: the
     * step in which more goes through it removes it and names it in broken().
     * Left out, it takes anything. The two bodies of a joint do not touch each
     * other unless `collide` is true.
     *
     * @param {{kind?: 'weld'|'hinge'|'ball', a: number, b?: number, anchor?: number[],
     *          axis?: number[], breakForce?: number, breakTorque?: number, collide?: boolean}} o
     * @returns {number} the joint
     *
     * @example
     * // A beam welded to the world that lets go past 50 kN.
     * const beam = w.addBox({ position: [0, 5, 0], half: [2, 0.2, 0.2], mass: 800 });
     * const weld = w.addJoint({ a: beam, anchor: [-2, 5, 0], breakForce: 50000 });
     * w.step();
     * w.jointLoad(weld).force;   // about [0, 7848, 0]
     */
    addJoint(o) {}

    removeJoint(joint) {}

    /** @returns {boolean} false once removed or broken */
    hasJoint(joint) {}

    /**
     * What went through a joint in the last step, world axes.
     * @returns {{force: number[], torque: number[]}}
     */
    jointLoad(joint) {}

    /**
     * Advances the world. More substeps hold joints and stacks tighter.
     * @param {number} [dt=1/60] seconds
     * @param {number} [substeps=4]
     */
    step(dt, substeps) {}

    /** @returns {Float32Array} [x, y, z, qx, qy, qz, qw] */
    pose(body) {}
    setPose(body, pose) {}

    /** @returns {Float32Array} linear 3, angular 3 */
    velocity(body) {}
    setVelocity(body, linear, angular) {}

    /** An impulse (N s) at a point of the world; wakes the body. */
    applyImpulse(body, impulse, at) {}

    /** @returns {number} kg; 0 for a fixed body */
    mass(body) {}

    /**
     * The bodies the last step moved, nine numbers each: the body, its position
     * (3) and rotation (4), and 1 if it came to rest in that step. A body at
     * rest is not listed again until something wakes it, so a renderer that
     * applies these rows every step is always current.
     * @returns {Float32Array}
     */
    moved() {}

    /**
     * Sends a kinematic body toward a pose ([x, y, z, qx, qy, qz, qw]): it is
     * given the velocity that has it there after a step of `dt`, so what rests
     * on it or stands in its way is carried and pushed.
     * @param {number} [dt=1/60]
     */
    moveTo(body, pose, dt) {}

    /** Wakes a body at rest. */
    wake(body) {}

    /** The joints the last step broke. @returns {Int32Array} */
    broken() {}

    /**
     * The pairs of bodies that began to touch in the last step, two numbers a
     * pair.
     * @returns {Int32Array}
     *
     * @example
     * w.step(dt);
     * const t = w.touched();
     * for (let i = 0; i < t.length; i += 2) onTouch(t[i], t[i + 1]);
     */
    touched() {}

    /**
     * The pairs that met faster than hitSpeed in the last step, nine numbers
     * each: the two bodies, the point (3), the normal from the first to the
     * second (3), and the closing speed.
     * @returns {Float32Array}
     */
    hits() {}

    /**
     * The nearest body along a ray, or null. `direction` need not be unit;
     * `mask` is the layers to hit (default all).
     * @param {number[]} origin
     * @param {number[]} direction
     * @param {number} [maxDistance=1000]
     * @param {number} [mask]
     * @returns {{body: number, point: number[], normal: number[], distance: number}|null}
     */
    raycast(origin, direction, maxDistance, mask) {}

    /** @type {{bodies: number, joints: number, awake: number, contacts: number, stepMs: number}} */
    get counts() {}
}
