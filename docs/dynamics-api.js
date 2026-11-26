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

    /**
     * Something built (a tower, a wall, a bridge) that stands in the world and
     * that anything with enough behind it breaks (src/dynamics/structure.h).
     *
     * It is given as `members`, boxes, and `ties`, pairs of members that touch.
     * Each member is cut into cells about `chunk` metres across, joined to
     * their neighbours across the faces they share. While it stands it has no
     * bodies and costs a step nothing: a member is given a fixed body only
     * while something moving is near it.
     *
     * What strikes a standing cell with more energy than its joints take goes
     * through: the cell comes loose as a body of its own, then the others in
     * the striker's way while the blow has more to give, and the striker is
     * slowed by what that cost and by the weight it now pushes. With less, it
     * is stopped, and the cell keeps the damage. After damage the weight of
     * what stands (and of whatever rests on it) is sent to the ground again: a
     * cell asked to bear more than it holds is crushed (gone), a joint asked
     * to hang more lets go, cells further sideways from what holds them up
     * than their member's `span` let go, and cells left with no way to the
     * ground fall, each group as one body. Loose bodies break the same way
     * where they land or are struck.
     *
     * A member: { center, half, frame = 0, anchored = false,
     *   mass     kg (default: concrete, 2400 kg/m3)
     *   sigma    Pa its material holds pulled or sheared (3e6)
     *   crush    Pa it bears pressed (3e7)
     *   give     m a joint of it stretches before it lets go (0.02): the
     *            energy to part a joint is sigma x shared area x give
     *   span     m it reaches sideways from what holds it up (5) }
     * `center` and `half` are in the member's frame; `frames` are poses
     * ({position, rotation} or [x, y, z, qx, qy, qz, qw]), one at the origin
     * when left out. An `anchored` member is held by the ground and never
     * breaks.
     *
     * @param {{members: Object[], ties?: number[][], frames?: Object[], chunk?: number,
     *          safety?: number, friction?: number, restitution?: number,
     *          layers?: number, mask?: number}} o
     *   `safety` (2): a joint holds this many times what it carried as built,
     *   if that is more than its material does.
     * @returns {Structure}
     *
     * @example
     * const wall = w.addStructure({
     *     members: [
     *         { center: [0, -0.4, 0], half: [7, 0.4, 0.6], anchored: true },   // footing
     *         { center: [0, 4.8, 0], half: [6, 4.8, 0.15] },                   // 12 x 9.6 m of wall
     *     ],
     *     ties: [[0, 1]],
     * });
     * w.addSphere({ radius: 0.5, position: [2, 5, 10], mass: 5000, velocity: [0, 0, -40] });
     * for (let i = 0; i < 60; i++) { w.step(1 / 60); draw(wall.moved()); }
     */
    addStructure(o) {}
}

class Structure {
    /** Takes it, and every body of it, out of the world. */
    destroy() {}

    /**
     * The cells as built, twelve numbers each: member, position (3), rotation
     * (4), half extents (3), mass. A cell is named by its index here.
     * @type {Float32Array}
     */
    get cells() {}

    /** Per cell: -1 standing, -2 gone (crushed or dropped), else the body it is part of. @returns {Int32Array} */
    state() {}

    /**
     * The loose cells the last step moved or made, eight numbers each: cell,
     * position (3), rotation (4). A renderer that keeps one box per cell and
     * applies these rows after every step is always current
     * (broworkshop lib/kit/dynamics3d.js structureView).
     * @returns {Float32Array}
     */
    moved() {}

    /** The cells the last step crushed. @returns {Int32Array} */
    crushed() {}

    /**
     * The nearest standing cell along a ray, or null. A standing structure has
     * no bodies, so World.raycast passes through it; loose cells are bodies,
     * which World.raycast finds.
     * @returns {{cell: number, point: number[], normal: number[], distance: number}|null}
     */
    raycast(origin, direction, maxDistance) {}

    /** The loose bodies, three numbers each: body, cells, mass. @returns {Float32Array} */
    pieces() {}

    /** The cells of a loose body. @returns {Int32Array} */
    pieceCells(body) {}

    /** Takes a loose body out of the world; its cells are gone (rubble handed to something cheaper to draw). */
    drop(body) {}

    /**
     * `held` is the members with a fixed body now, `joints` the joints that
     * still hold, `solveMs` the last sending of the weight.
     * @type {{cells: number, standing: number, pieces: number, held: number, joints: number, solveMs: number}}
     */
    get counts() {}
}
