// dynamics.js — the public shape of bro.dynamics (docs/dynamics-api.js), assembled over the
// natives under __bro_native.dynamics (native_dynamics_decl.h states each C entry point,
// native_dynamics_register.cpp registers them, both in natives/dynamics/; the bodies are
// native_dynamics.cpp's). Hand-maintained: a native's signature changes in all four together.
//
// Every native is spelled by its full dotted path at the point of use: that is the
// spelling the compiler lowers to a direct call. The roots this module reads as bare
// identifiers are listed in module.globals beside it.
(function () {
    'use strict';

    const fn = (obj, name, value) =>
        Object.defineProperty(obj, name, { value, writable: true, enumerable: true, configurable: true });
    const mount = (root, name) => root[name] !== undefined ? root[name] : (root[name] = {});

    const ns = mount(globalThis.bro, 'dynamics');

    const KINDS = { weld: 0, hinge: 1, ball: 2 };

    const put3 = (out, at, v, d) => {
        out[at] = v ? +v[0] : d[0];
        out[at + 1] = v ? +v[1] : d[1];
        out[at + 2] = v ? +v[2] : d[2];
    };
    const ZERO = [0, 0, 0];

    const flagsOf = (o) => (o.kinematic ? 1 : 0) | (o.bullet ? 2 : 0) | (o.planar ? 4 : 0);
    const layersOf = (o) => o.layers === undefined ? 1 : o.layers | 0;
    const maskOf = (o) => o.mask === undefined ? -1 : o.mask | 0;
    const made = (what, id) => {
        if (id < 0) throw new RangeError(`bro.dynamics: ${what}: the shape came to nothing`);
        return id;
    };

    // position 3, rotation 4, size 3, mass, friction, restitution, velocity 3, angular 3,
    // linear damping, angular damping, gravity scale
    const bodyDesc = (what, o, size) => {
        if (o === null || typeof o !== 'object') throw new TypeError(`bro.dynamics: ${what} takes an object`);
        const d = new Float32Array(22);
        d[19] = o.linearDamping === undefined ? 0 : +o.linearDamping;
        d[20] = o.angularDamping === undefined ? 0 : +o.angularDamping;
        d[21] = o.gravityScale === undefined ? 1 : +o.gravityScale;
        put3(d, 0, o.position, ZERO);
        const r = o.rotation;
        d[3] = r ? +r[0] : 0; d[4] = r ? +r[1] : 0; d[5] = r ? +r[2] : 0; d[6] = r ? +r[3] : 1;
        d[7] = size[0]; d[8] = size[1]; d[9] = size[2];
        d[10] = o.mass === undefined ? 1 : +o.mass;
        d[11] = o.friction === undefined ? 0.6 : +o.friction;
        d[12] = o.restitution === undefined ? 0 : +o.restitution;
        put3(d, 13, o.velocity, ZERO);
        put3(d, 16, o.angular, ZERO);
        for (let i = 0; i < 22; i++) {
            if (!Number.isFinite(d[i])) throw new TypeError(`bro.dynamics: ${what}: every number must be finite`);
        }
        if (d[10] < 0) throw new RangeError(`bro.dynamics: ${what}: mass must be 0 (fixed) or more`);
        return d;
    };

    class World {
        constructor(options) {
            this._id = __bro_native.dynamics.create();
            const g = options && options.gravity;
            if (g) __bro_native.dynamics.setGravity(this._id, +g[0], +g[1], +g[2]);
        }

        destroy() {
            if (this._id < 0) return;
            __bro_native.dynamics.destroy(this._id);
            this._id = -1;
        }

        setGravity(g) { __bro_native.dynamics.setGravity(this._id, +g[0], +g[1], +g[2]); }

        addBox(o) {
            const h = o && o.half;
            if (!h || !(h[0] > 0 && h[1] > 0 && h[2] > 0)) throw new RangeError('bro.dynamics: addBox: half must be three lengths over 0');
            return __bro_native.dynamics.addBody(this._id, 0, bodyDesc('addBox', o, [+h[0], +h[1], +h[2]]),
                flagsOf(o), layersOf(o), maskOf(o));
        }

        addSphere(o) {
            const r = o && +o.radius;
            if (!(r > 0)) throw new RangeError('bro.dynamics: addSphere: radius must be over 0');
            return __bro_native.dynamics.addBody(this._id, 1, bodyDesc('addSphere', o, [r, 0, 0]),
                flagsOf(o), layersOf(o), maskOf(o));
        }

        addCapsule(o) {
            const r = o && +o.radius, h = o && +o.halfHeight;
            if (!(r > 0 && h > 0)) throw new RangeError('bro.dynamics: addCapsule: radius and halfHeight must be over 0');
            return __bro_native.dynamics.addBody(this._id, 2, bodyDesc('addCapsule', o, [r, h, 0]),
                flagsOf(o), layersOf(o), maskOf(o));
        }

        addHull(o) {
            const d = bodyDesc('addHull', o, ZERO);
            const pts = Float32Array.from(o.points || []);
            if (pts.length < 12 || pts.length % 3) throw new RangeError('bro.dynamics: addHull: points is x y z of four points or more');
            return made('addHull', __bro_native.dynamics.addHull(this._id, d, flagsOf(o), layersOf(o), maskOf(o), pts));
        }

        addCylinder(o) {
            const r = o && +o.radius, h = o && +o.halfHeight;
            if (!(r > 0 && h > 0)) throw new RangeError('bro.dynamics: addCylinder: radius and halfHeight must be over 0');
            const n = Math.max(6, Math.min(32, o.segments === undefined ? 16 : o.segments | 0));
            const pts = new Float32Array(n * 6);
            for (let i = 0; i < n; i++) {
                const a = i / n * 2 * Math.PI, x = Math.cos(a) * r, z = Math.sin(a) * r;
                pts[i * 6] = x; pts[i * 6 + 1] = -h; pts[i * 6 + 2] = z;
                pts[i * 6 + 3] = x; pts[i * 6 + 4] = h; pts[i * 6 + 5] = z;
            }
            return made('addCylinder', __bro_native.dynamics.addHull(this._id, bodyDesc('addCylinder', o, ZERO),
                flagsOf(o), layersOf(o), maskOf(o), pts));
        }

        addMesh(o) {
            const d = bodyDesc('addMesh', o, ZERO);
            const xyz = Float32Array.from(o.positions || []);
            const idx = Int32Array.from(o.indices || []);
            if (xyz.length < 9 || xyz.length % 3 || idx.length < 3 || idx.length % 3) {
                throw new RangeError('bro.dynamics: addMesh: positions is x y z per vertex, indices three per triangle');
            }
            return made('addMesh', __bro_native.dynamics.addMesh(this._id, d, layersOf(o), maskOf(o), xyz, idx));
        }

        removeBody(body) { __bro_native.dynamics.removeBody(this._id, body | 0); }
        hasBody(body) { return __bro_native.dynamics.hasBody(this._id, body | 0); }

        set maxSpeed(v) { __bro_native.dynamics.setMaxSpeed(this._id, +v); }
        set hitSpeed(v) { __bro_native.dynamics.setHitSpeed(this._id, +v); }

        addJoint(o) {
            if (o === null || typeof o !== 'object') throw new TypeError('bro.dynamics: addJoint takes an object');
            const kind = KINDS[o.kind === undefined ? 'weld' : o.kind];
            if (kind === undefined) throw new TypeError("bro.dynamics: addJoint: kind is 'weld', 'hinge' or 'ball'");
            const d = new Float32Array(8);
            put3(d, 0, o.anchor, ZERO);
            put3(d, 3, o.axis, [0, 1, 0]);
            d[6] = o.breakForce === undefined ? 0 : +o.breakForce;
            d[7] = o.breakTorque === undefined ? 0 : +o.breakTorque;
            const a = o.a === undefined || o.a === null ? -1 : o.a | 0;
            const b = o.b === undefined || o.b === null ? -1 : o.b | 0;
            const id = __bro_native.dynamics.addJoint(this._id, kind, a, b, d, !!o.collide);
            if (id < 0) throw new RangeError('bro.dynamics: addJoint: a and b must name bodies of this world, one of them at least');
            return id;
        }

        removeJoint(joint) { __bro_native.dynamics.removeJoint(this._id, joint | 0); }
        hasJoint(joint) { return __bro_native.dynamics.hasJoint(this._id, joint | 0); }

        jointLoad(joint) {
            const v = __bro_native.dynamics.jointLoad(this._id, joint | 0);
            return { force: [v[0], v[1], v[2]], torque: [v[3], v[4], v[5]] };
        }

        step(dt, substeps) {
            __bro_native.dynamics.step(this._id, dt === undefined ? 1 / 60 : +dt, substeps === undefined ? 4 : substeps | 0);
        }

        pose(body) { return __bro_native.dynamics.pose(this._id, body | 0); }
        setPose(body, pose) { __bro_native.dynamics.setPose(this._id, body | 0, Float32Array.from(pose)); }
        velocity(body) { return __bro_native.dynamics.velocity(this._id, body | 0); }
        setVelocity(body, linear, angular) {
            const a = angular || ZERO;
            __bro_native.dynamics.setVelocity(this._id, body | 0,
                Float32Array.of(+linear[0], +linear[1], +linear[2], +a[0], +a[1], +a[2]));
        }
        applyImpulse(body, impulse, at) {
            __bro_native.dynamics.applyImpulse(this._id, body | 0,
                Float32Array.of(+impulse[0], +impulse[1], +impulse[2], +at[0], +at[1], +at[2]));
        }
        mass(body) { return __bro_native.dynamics.mass(this._id, body | 0); }
        moveTo(body, pose, dt) {
            __bro_native.dynamics.moveTo(this._id, body | 0, Float32Array.from(pose), dt === undefined ? 1 / 60 : +dt);
        }
        wake(body) { __bro_native.dynamics.wake(this._id, body | 0); }

        moved() { return __bro_native.dynamics.moved(this._id); }
        broken() { return __bro_native.dynamics.broken(this._id); }
        touched() { return __bro_native.dynamics.touched(this._id); }
        hits() { return __bro_native.dynamics.hits(this._id); }

        raycast(origin, direction, maxDistance, mask) {
            const v = __bro_native.dynamics.raycast(this._id,
                Float32Array.of(+origin[0], +origin[1], +origin[2], +direction[0], +direction[1], +direction[2]),
                maxDistance === undefined ? 1000 : +maxDistance, mask === undefined ? -1 : mask | 0);
            if (!v.length) return null;
            return { body: v[0], point: [v[1], v[2], v[3]], normal: [v[4], v[5], v[6]], distance: v[7] };
        }

        get counts() {
            const c = __bro_native.dynamics.counts(this._id);
            return { bodies: c[0], joints: c[1], awake: c[2], contacts: c[3], stepMs: c[4] };
        }
    }

    fn(ns, 'World', World);
    fn(ns, 'createWorld', function createWorld(options) { return new World(options); });
})();
