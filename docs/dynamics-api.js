/**
 * =============================================================================
 * bro.dynamics — rigid bodies, joints, and what goes through a joint
 * =============================================================================
 *
 * Worlds of rigid bodies over Box3D (wlejon/box3d, the fork of Erin Catto's
 * engine; src/dynamics/world.h). A world is stepped by the code that made it:
 * nothing steps it for you, and it is not tied to the scene or to bro.time.
 * It runs on the CPU, the same in bro, bro-headless and bro-server, and in a
 * Worker (a world belongs to the thread that made it).
 *
 * Bodies and joints are small integers, reused once removed. Vectors are
 * arrays ([x, y, z]); a rotation is a quaternion [x, y, z, w]. Units are SI.
 *
 * `bro.dynamics.available` is false in a build without BRO_WITH_PHYSICS.
 *
 * This is not `Physics` (docs/physics-api.js, Jolt): the two do not see each
 * other's bodies.
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

    /**
     * A box. `mass` 0 fixes it in place; otherwise it is the body's mass in kg
     * (default 1), spread evenly through the box.
     *
     * @param {{half: number[], position?: number[], rotation?: number[], mass?: number,
     *          friction?: number, restitution?: number, velocity?: number[], angular?: number[]}} o
     * @returns {number} the body
     */
    addBox(o) {}

    /**
     * A sphere; as addBox, with `radius` for `half`.
     * @returns {number} the body
     */
    addSphere(o) {}

    /** Removes a body and the joints on it. */
    removeBody(body) {}

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

    /** The joints the last step broke. @returns {Int32Array} */
    broken() {}

    /** @type {{bodies: number, joints: number, awake: number, contacts: number, stepMs: number}} */
    get counts() {}
}
