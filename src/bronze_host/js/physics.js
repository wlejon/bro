// physics.js — the public shape of Physics, PhysicsWorldHandle, PhysicsCharacter,
// PhysicsVehicle, PhysicsRagdoll, PhysicsSoftBody, assembled over the natives
// under __bro_native.physics.

(function () {
    'use strict';

    const accessor = (obj, name, get, set) =>
        Object.defineProperty(obj, name, { get, set, enumerable: true, configurable: true });
    const fn = (obj, name, value) =>
        Object.defineProperty(obj, name, { value, writable: true, enumerable: true, configurable: true });

    // ---- Physics -------------------------------------------------------------
    const ns_Physics = typeof Physics === 'object' && Physics !== null ? Physics : {};
    globalThis.Physics = ns_Physics;

    fn(ns_Physics, "createWorldHandle", function createWorldHandle(opts) {
        return __bro_native.physics.createWorldHandle(opts === undefined ? '' : JSON.stringify(opts));
    });
    fn(ns_Physics, "createWorld", function createWorld(opts) {
        __bro_native.physics.createWorld(opts === undefined ? '' : JSON.stringify(opts));
    });
    fn(ns_Physics, "setGravity", function setGravity(x, y, z) {
        if (x === undefined) throw new TypeError("Physics.setGravity: x is required");
        if (y === undefined) throw new TypeError("Physics.setGravity: y is required");
        if (z === undefined) throw new TypeError("Physics.setGravity: z is required");
        __bro_native.physics.setGravity(x, y, z);
    });
    fn(ns_Physics, "getGravity", function getGravity() {
        return JSON.parse(__bro_native.physics.getGravity());
    });
    fn(ns_Physics, "setLayers", function setLayers(config) {
        if (config === undefined) throw new TypeError("Physics.setLayers: config is required");
        return __bro_native.physics.setLayers(JSON.stringify(config));
    });
    fn(ns_Physics, "createBody", function createBody(config) {
        if (config === undefined) throw new TypeError("Physics.createBody: config is required");
        if (config && config.area && !config.sensor) {
            throw new Error("Physics.createBody: area requires sensor: true");
        }
        let cfg = config;
        if (config && typeof config === "object") {
            const toArr = (v) => {
                if (!v) return v;
                if (Array.isArray(v)) return v;
                if (typeof v.length === 'number') {
                    const a = new Array(v.length);
                    for (let i = 0; i < v.length; i++) a[i] = v[i];
                    return a;
                }
                return v;
            };
            const normalize = (c) => {
                if (!c || typeof c !== 'object') return c;
                let res = c;
                if (c.heights && typeof c.heights.length === 'number' && !Array.isArray(c.heights)) {
                    res = Object.assign({}, res, { heights: toArr(c.heights) });
                }
                if (c.positions && typeof c.positions.length === 'number' && !Array.isArray(c.positions)) {
                    res = Object.assign({}, res, { positions: toArr(c.positions) });
                }
                if (c.points && typeof c.points.length === 'number' && !Array.isArray(c.points)) {
                    res = Object.assign({}, res, { points: toArr(c.points) });
                }
                if (c.vertices && typeof c.vertices.length === 'number' && !Array.isArray(c.vertices)) {
                    res = Object.assign({}, res, { vertices: toArr(c.vertices) });
                }
                if (c.indices && typeof c.indices.length === 'number' && !Array.isArray(c.indices)) {
                    res = Object.assign({}, res, { indices: toArr(c.indices) });
                }
                if (Array.isArray(c.parts)) {
                    res = Object.assign({}, res, { parts: c.parts.map(normalize) });
                }
                return res;
            };
            cfg = normalize(config);
        }
        return __bro_native.physics.createBody(JSON.stringify(cfg));
    });
    fn(ns_Physics, "destroyBody", function destroyBody(tag) {
        if (tag === undefined) throw new TypeError("Physics.destroyBody: tag is required");
        __bro_native.physics.destroyBody(tag);
    });
    fn(ns_Physics, "destroyAll", function destroyAll() {
        __bro_native.physics.destroyAll();
    });
    fn(ns_Physics, "getTransform", function getTransform(tag, opts) {
        if (tag === undefined) throw new TypeError("Physics.getTransform: tag is required");
        const interpolated = !!(opts && opts.interpolated);
        const raw = __bro_native.physics.getTransform(tag, interpolated);
        if (!raw || raw === "null") return undefined;
        return JSON.parse(raw);
    });
    fn(ns_Physics, "getVelocity", function getVelocity(tag) {
        if (tag === undefined) throw new TypeError("Physics.getVelocity: tag is required");
        const raw = __bro_native.physics.getVelocity(tag);
        if (!raw || raw === "null") return undefined;
        return JSON.parse(raw);
    });
    fn(ns_Physics, "setPosition", function setPosition(tag, x, y, z) {
        if (tag === undefined) throw new TypeError("Physics.setPosition: tag is required");
        if (x === undefined) throw new TypeError("Physics.setPosition: x is required");
        if (y === undefined) throw new TypeError("Physics.setPosition: y is required");
        if (z === undefined) throw new TypeError("Physics.setPosition: z is required");
        __bro_native.physics.setPosition(tag, x, y, z);
    });
    fn(ns_Physics, "setRotation", function setRotation(tag, x, y, z, w) {
        if (tag === undefined) throw new TypeError("Physics.setRotation: tag is required");
        if (x === undefined) throw new TypeError("Physics.setRotation: x is required");
        if (y === undefined) throw new TypeError("Physics.setRotation: y is required");
        if (z === undefined) throw new TypeError("Physics.setRotation: z is required");
        if (w === undefined) throw new TypeError("Physics.setRotation: w is required");
        __bro_native.physics.setRotation(tag, x, y, z, w);
    });
    fn(ns_Physics, "setTransform", function setTransform(tag, pos, rot) {
        if (tag === undefined) throw new TypeError("Physics.setTransform: tag is required");
        if (!pos) throw new TypeError("Physics.setTransform: pos is required");
        const q = rot || { x: 0, y: 0, z: 0, w: 1 };
        __bro_native.physics.setTransform(tag, pos.x || 0, pos.y || 0, pos.z || 0, q.x || 0, q.y || 0, q.z || 0, q.w !== undefined ? q.w : 1);
    });
    fn(ns_Physics, "setTransforms", function setTransforms(updates, stride) {
        if (!updates) return;
        const strideOf = (n) => {
            if (stride === 8 || stride === 17) return stride;
            if (stride !== undefined) throw new TypeError("Physics.setTransforms: stride must be 8 or 17");
            if (n % 8 === 0 && n % 17 !== 0) return 8;
            if (n % 17 === 0 && n % 8 !== 0) return 17;
            throw new TypeError("Physics.setTransforms: pass the stride (8 or 17); a length of " + n + " is ambiguous");
        };
        if (updates instanceof Float64Array) {
            __bro_native.physics.setTransforms(updates, strideOf(updates.length));
            return;
        }
        if (Array.isArray(updates) && updates.length > 0 && typeof updates[0] === 'object') {
            const mats = [], poses = [];
            for (const u of updates) {
                const tag = u.body !== undefined ? u.body : u.tag;
                if (u.matrix) {
                    mats.push(tag);
                    for (let k = 0; k < 16; k++) mats.push(u.matrix[k]);
                } else {
                    const p = u.position || u.pos || { x: 0, y: 0, z: 0 };
                    const r = u.rotation || u.rot || { x: 0, y: 0, z: 0, w: 1 };
                    poses.push(tag, p.x || 0, p.y || 0, p.z || 0, r.x || 0, r.y || 0, r.z || 0, r.w !== undefined ? r.w : 1);
                }
            }
            if (mats.length) __bro_native.physics.setTransforms(new Float64Array(mats), 17);
            if (poses.length) __bro_native.physics.setTransforms(new Float64Array(poses), 8);
            return;
        }
        __bro_native.physics.setTransforms(new Float64Array(updates), strideOf(updates.length));
    });
    fn(ns_Physics, "setLinearVelocity", function setLinearVelocity(tag, x, y, z) {
        if (tag === undefined) throw new TypeError("Physics.setLinearVelocity: tag is required");
        if (x === undefined) throw new TypeError("Physics.setLinearVelocity: x is required");
        if (y === undefined) throw new TypeError("Physics.setLinearVelocity: y is required");
        if (z === undefined) throw new TypeError("Physics.setLinearVelocity: z is required");
        __bro_native.physics.setLinearVelocity(tag, x, y, z);
    });
    fn(ns_Physics, "setAngularVelocity", function setAngularVelocity(tag, x, y, z) {
        if (tag === undefined) throw new TypeError("Physics.setAngularVelocity: tag is required");
        if (x === undefined) throw new TypeError("Physics.setAngularVelocity: x is required");
        if (y === undefined) throw new TypeError("Physics.setAngularVelocity: y is required");
        if (z === undefined) throw new TypeError("Physics.setAngularVelocity: z is required");
        __bro_native.physics.setAngularVelocity(tag, x, y, z);
    });
    fn(ns_Physics, "addForce", function addForce(tag, x, y, z) {
        if (tag === undefined) throw new TypeError("Physics.addForce: tag is required");
        if (x === undefined) throw new TypeError("Physics.addForce: x is required");
        if (y === undefined) throw new TypeError("Physics.addForce: y is required");
        if (z === undefined) throw new TypeError("Physics.addForce: z is required");
        __bro_native.physics.addForce(tag, x, y, z);
    });
    fn(ns_Physics, "addImpulse", function addImpulse(tag, x, y, z) {
        if (tag === undefined) throw new TypeError("Physics.addImpulse: tag is required");
        if (x === undefined) throw new TypeError("Physics.addImpulse: x is required");
        if (y === undefined) throw new TypeError("Physics.addImpulse: y is required");
        if (z === undefined) throw new TypeError("Physics.addImpulse: z is required");
        __bro_native.physics.addImpulse(tag, x, y, z);
    });
    fn(ns_Physics, "addTorque", function addTorque(tag, x, y, z) {
        if (tag === undefined) throw new TypeError("Physics.addTorque: tag is required");
        if (x === undefined) throw new TypeError("Physics.addTorque: x is required");
        if (y === undefined) throw new TypeError("Physics.addTorque: y is required");
        if (z === undefined) throw new TypeError("Physics.addTorque: z is required");
        __bro_native.physics.addTorque(tag, x, y, z);
    });
    fn(ns_Physics, "setUserData", function setUserData(tag, data) {
        if (tag === undefined) throw new TypeError("Physics.setUserData: tag is required");
        if (data === undefined) throw new TypeError("Physics.setUserData: data is required");
        __bro_native.physics.setUserData(tag, data);
    });
    fn(ns_Physics, "getUserData", function getUserData(tag) {
        if (tag === undefined) throw new TypeError("Physics.getUserData: tag is required");
        return __bro_native.physics.getUserData(tag);
    });
    fn(ns_Physics, "setLayer", function setLayer(tag, layer) {
        if (tag === undefined) throw new TypeError("Physics.setLayer: tag is required");
        if (layer === undefined) throw new TypeError("Physics.setLayer: layer is required");
        return __bro_native.physics.setLayer(tag, String(layer));
    });
    fn(ns_Physics, "setKinematic", function setKinematic(tag) {
        if (tag === undefined) throw new TypeError("Physics.setKinematic: tag is required");
        __bro_native.physics.setKinematic(tag);
    });
    // Two forms, as always: (tag, x, y, z, dt) keeps the body's rotation, and
    // (tag, x, y, z, qx, qy, qz, qw, dt) carries a target rotation too — the
    // one a kinematic platform or a picked-up prop turns with. The natives
    // have one arity each, so the form is chosen here by what was passed.
    fn(ns_Physics, "moveKinematic", function moveKinematic(tag, x, y, z, a4, a5, a6, a7, a8) {
        if (tag === undefined) throw new TypeError("Physics.moveKinematic: tag is required");
        if (x === undefined) throw new TypeError("Physics.moveKinematic: x is required");
        if (y === undefined) throw new TypeError("Physics.moveKinematic: y is required");
        if (z === undefined) throw new TypeError("Physics.moveKinematic: z is required");
        if (a8 !== undefined) {
            __bro_native.physics.moveKinematicRot(tag, x, y, z, +a4, +a5, +a6, +a7, +a8);
            return;
        }
        if (a4 === undefined) throw new TypeError("Physics.moveKinematic: dt is required");
        __bro_native.physics.moveKinematic(tag, x, y, z, a4);
    });
    fn(ns_Physics, "getContacts", function getContacts() {
        const raw = JSON.parse(__bro_native.physics.getContacts());
        const arr = (raw && Array.isArray(raw.events)) ? raw.events : [];
        arr.overflow = Boolean(raw && raw.overflow);
        return arr;
    });
    fn(ns_Physics, "setFrictionCombine", function setFrictionCombine(tag, mode) {
        if (tag === undefined) throw new TypeError("Physics.setFrictionCombine: tag is required");
        if (mode === undefined) throw new TypeError("Physics.setFrictionCombine: mode is required");
        __bro_native.physics.setFrictionCombine(tag, mode);
        return true;
    });
    fn(ns_Physics, "setRestitutionCombine", function setRestitutionCombine(tag, mode) {
        if (tag === undefined) throw new TypeError("Physics.setRestitutionCombine: tag is required");
        if (mode === undefined) throw new TypeError("Physics.setRestitutionCombine: mode is required");
        __bro_native.physics.setRestitutionCombine(tag, mode);
        return true;
    });
    fn(ns_Physics, "getMass", function getMass(tag) {
        if (tag === undefined) throw new TypeError("Physics.getMass: tag is required");
        return __bro_native.physics.getMass(tag);
    });
    fn(ns_Physics, "setMass", function setMass(tag, mass) {
        if (tag === undefined) throw new TypeError("Physics.setMass: tag is required");
        if (mass === undefined) throw new TypeError("Physics.setMass: mass is required");
        __bro_native.physics.setMass(tag, mass);
    });
    fn(ns_Physics, "setLinearDamping", function setLinearDamping(tag, damping) {
        if (tag === undefined) throw new TypeError("Physics.setLinearDamping: tag is required");
        if (damping === undefined) throw new TypeError("Physics.setLinearDamping: damping is required");
        __bro_native.physics.setLinearDamping(tag, damping);
    });
    fn(ns_Physics, "setAngularDamping", function setAngularDamping(tag, damping) {
        if (tag === undefined) throw new TypeError("Physics.setAngularDamping: tag is required");
        if (damping === undefined) throw new TypeError("Physics.setAngularDamping: damping is required");
        __bro_native.physics.setAngularDamping(tag, damping);
    });
    fn(ns_Physics, "setGravityFactor", function setGravityFactor(tag, factor) {
        if (tag === undefined) throw new TypeError("Physics.setGravityFactor: tag is required");
        if (factor === undefined) throw new TypeError("Physics.setGravityFactor: factor is required");
        __bro_native.physics.setGravityFactor(tag, factor);
    });
    fn(ns_Physics, "setFriction", function setFriction(tag, friction) {
        if (tag === undefined) throw new TypeError("Physics.setFriction: tag is required");
        if (friction === undefined) throw new TypeError("Physics.setFriction: friction is required");
        __bro_native.physics.setFriction(tag, friction);
    });
    fn(ns_Physics, "setRestitution", function setRestitution(tag, restitution) {
        if (tag === undefined) throw new TypeError("Physics.setRestitution: tag is required");
        if (restitution === undefined) throw new TypeError("Physics.setRestitution: restitution is required");
        __bro_native.physics.setRestitution(tag, restitution);
    });
    fn(ns_Physics, "getBodyProperties", function getBodyProperties(tag) {
        if (tag === undefined) throw new TypeError("Physics.getBodyProperties: tag is required");
        return JSON.parse(__bro_native.physics.getBodyProperties(tag));
    });
    fn(ns_Physics, "setAreaOverride", function setAreaOverride(tag, config) {
        if (tag === undefined) throw new TypeError("Physics.setAreaOverride: tag is required");
        if (config === undefined) throw new TypeError("Physics.setAreaOverride: config is required");
        const ok = __bro_native.physics.setAreaOverride(tag, JSON.stringify(config));
        if (!ok) throw new Error("Physics.setAreaOverride: target body is not a sensor or invalid");
        return true;
    });
    fn(ns_Physics, "setTimeStep", function setTimeStep(dt) {
        if (dt === undefined) throw new TypeError("Physics.setTimeStep: dt is required");
        __bro_native.physics.setTimeStep(dt);
    });
    fn(ns_Physics, "getTimeStep", function getTimeStep() {
        return __bro_native.physics.getTimeStep();
    });
    fn(ns_Physics, "step", function step(dt) {
        __bro_native.physics.step(dt === undefined ? 0 : dt);
    });
    fn(ns_Physics, "setInterpolation", function setInterpolation(enabled) {
        if (enabled === undefined) throw new TypeError("Physics.setInterpolation: enabled is required");
        __bro_native.physics.setInterpolation(enabled);
    });
    fn(ns_Physics, "getInterpolation", function getInterpolation() {
        return __bro_native.physics.getInterpolation();
    });
    fn(ns_Physics, "isActive", function isActive(tag) {
        if (tag === undefined) throw new TypeError("Physics.isActive: tag is required");
        return __bro_native.physics.isActive(tag);
    });
    fn(ns_Physics, "activate", function activate(tag) {
        if (tag === undefined) throw new TypeError("Physics.activate: tag is required");
        __bro_native.physics.activate(tag);
    });
    fn(ns_Physics, "getAllTransforms", function getAllTransforms(opts) {
        let interpolated = false;
        if (opts && typeof opts === 'object') {
            interpolated = !!opts.interpolated;
        }
        return __bro_native.physics.getAllTransforms(interpolated);
    });
    fn(ns_Physics, "createCharacter", function createCharacter(config) {
        if (config === undefined) throw new TypeError("Physics.createCharacter: config is required");
        return __bro_native.physics.createCharacter(JSON.stringify(config));
    });
    fn(ns_Physics, "createVehicle", function createVehicle(config) {
        if (config === undefined) throw new TypeError("Physics.createVehicle: config is required");
        const res = __bro_native.physics.createVehicle(JSON.stringify(config));
        if (!res) throw new Error("Physics.createVehicle: failed to create vehicle");
        return res;
    });
    fn(ns_Physics, "createRagdoll", function createRagdoll(config) {
        if (config === undefined) throw new TypeError("Physics.createRagdoll: config is required");
        if (!config || !Array.isArray(config.parts) || config.parts.length === 0) {
            throw new Error("Physics.createRagdoll: parts must be a non-empty array");
        }
        const res = __bro_native.physics.createRagdoll(JSON.stringify(config));
        if (!res) throw new Error("Physics.createRagdoll: failed to create ragdoll (invalid parts or hierarchy)");
        return res;
    });
    fn(ns_Physics, "createSoftBody", function createSoftBody(config) {
        if (config === undefined) throw new TypeError("Physics.createSoftBody: config is required");
        if (!config || typeof config !== "object") {
            throw new Error("Physics.createSoftBody: config must be an object");
        }
        if (!config.cloth && !config.mesh) {
            throw new Error("Physics.createSoftBody: cloth or mesh definition is required");
        }
        if (config.cloth) {
            if (config.cloth.gridX !== undefined && config.cloth.gridX < 2) {
                throw new Error("Physics.createSoftBody: cloth gridX must be >= 2");
            }
            if (config.cloth.gridZ !== undefined && config.cloth.gridZ < 2) {
                throw new Error("Physics.createSoftBody: cloth gridZ must be >= 2");
            }
            const gx = config.cloth.gridX !== undefined ? config.cloth.gridX : 10;
            const gz = config.cloth.gridZ !== undefined ? config.cloth.gridZ : 10;
            if (!(Math.trunc(gx) * Math.trunc(gz) <= 1048576)) {
                throw new RangeError("Physics.createSoftBody: cloth gridX*gridZ must be at most 2^20 vertices");
            }
        }
        if (config.mesh) {
            const ind = config.mesh.indices;
            if (!ind || ind.length === 0) {
                throw new Error("Physics.createSoftBody: mesh indices are required");
            }
            const verts = config.mesh.vertices !== undefined ? config.mesh.vertices : config.mesh.positions;
            if (!verts || verts.length === 0) {
                throw new Error("Physics.createSoftBody: mesh vertices are required");
            }
        }

        const toArr = (v) => {
            if (!v) return v;
            if (Array.isArray(v)) return v;
            if (typeof v.length === 'number') {
                const arr = new Array(v.length);
                for (let i = 0; i < v.length; i++) arr[i] = v[i];
                return arr;
            }
            return v;
        };

        let cfg = config;
        if (config.mesh) {
            cfg = Object.assign({}, config, {
                mesh: Object.assign({}, config.mesh, {
                    vertices: toArr(config.mesh.vertices),
                    positions: toArr(config.mesh.positions),
                    indices: toArr(config.mesh.indices),
                    pinned: toArr(config.mesh.pinned)
                })
            });
        }
        if (config.cloth && config.cloth.pinned && typeof config.cloth.pinned !== "string") {
            cfg = Object.assign({}, cfg, {
                cloth: Object.assign({}, config.cloth, {
                    pinned: toArr(config.cloth.pinned)
                })
            });
        }

        const res = __bro_native.physics.createSoftBody(JSON.stringify(cfg));
        if (!res) throw new Error("Physics.createSoftBody: failed to create soft body");
        return res;
    });
    fn(ns_Physics, "createConstraint", function createConstraint(config) {
        if (config === undefined) throw new TypeError("Physics.createConstraint: config is required");
        return __bro_native.physics.createConstraint(JSON.stringify(config));
    });
    fn(ns_Physics, "destroyConstraint", function destroyConstraint(tag) {
        if (tag === undefined) throw new TypeError("Physics.destroyConstraint: tag is required");
        __bro_native.physics.destroyConstraint(tag);
    });
    fn(ns_Physics, "setConstraintEnabled", function setConstraintEnabled(tag, enabled) {
        if (tag === undefined) throw new TypeError("Physics.setConstraintEnabled: tag is required");
        if (enabled === undefined) throw new TypeError("Physics.setConstraintEnabled: enabled is required");
        __bro_native.physics.setConstraintEnabled(tag, enabled);
    });
    fn(ns_Physics, "isConstraintEnabled", function isConstraintEnabled(tag) {
        if (tag === undefined) throw new TypeError("Physics.isConstraintEnabled: tag is required");
        return __bro_native.physics.isConstraintEnabled(tag);
    });
    fn(ns_Physics, "setWheelMotor", function setWheelMotor(handle, enabled, speed, maxTorque) {
        if (handle === undefined) throw new TypeError("Physics.setWheelMotor: handle is required");
        if (enabled === undefined) throw new TypeError("Physics.setWheelMotor: enabled is required");
        if (speed === undefined) throw new TypeError("Physics.setWheelMotor: speed is required");
        if (maxTorque === undefined) throw new TypeError("Physics.setWheelMotor: maxTorque is required");
        __bro_native.physics.setWheelMotor(handle, enabled ? 1 : 0, speed, maxTorque);
    });
    fn(ns_Physics, "setConstraintMotor", function setConstraintMotor(tag, config) {
        if (tag === undefined) throw new TypeError("Physics.setConstraintMotor: tag is required");
        if (config === undefined) throw new TypeError("Physics.setConstraintMotor: config is required");
        return __bro_native.physics.setConstraintMotor(tag, JSON.stringify(config));
    });
    fn(ns_Physics, "setConstraintBreakingImpulse", function setConstraintBreakingImpulse(tag, impulse) {
        if (tag === undefined) throw new TypeError("Physics.setConstraintBreakingImpulse: tag is required");
        if (impulse === undefined) throw new TypeError("Physics.setConstraintBreakingImpulse: impulse is required");
        __bro_native.physics.setConstraintBreakingImpulse(tag, impulse);
    });
    fn(ns_Physics, "getConstraintBreakingImpulse", function getConstraintBreakingImpulse(tag) {
        if (tag === undefined) throw new TypeError("Physics.getConstraintBreakingImpulse: tag is required");
        return __bro_native.physics.getConstraintBreakingImpulse(tag);
    });
    fn(ns_Physics, "getBrokenConstraints", function getBrokenConstraints() {
        return Array.from(__bro_native.physics.getBrokenConstraints());
    });

    // ---- Manual query helpers ------------------------------------------------
    function normalizeRayArgs(args) {
        let ox, oy, oz, dx, dy, dz, maxDist = 1000, opts = null;
        if (typeof args[0] === 'number') {
            ox = args[0]; oy = args[1]; oz = args[2];
            dx = args[3]; dy = args[4]; dz = args[5];
            if (args.length > 6) {
                if (typeof args[6] === 'number') {
                    maxDist = args[6];
                    if (args.length > 7) opts = args[7];
                } else if (typeof args[6] === 'object') {
                    opts = args[6];
                }
            }
        } else {
            const p1 = args[0], p2 = args[1];
            ox = p1.x; oy = p1.y; oz = p1.z;
            dx = p2.x - ox; dy = p2.y - oy; dz = p2.z - oz;
            const len = Math.hypot(dx, dy, dz);
            maxDist = len > 0 ? len : 1000;
            if (len > 1e-6) { dx /= len; dy /= len; dz /= len; }
            if (args.length > 2) opts = args[2];
        }
        const filterStr = (opts !== undefined && opts !== null) ? (typeof opts === 'number' ? JSON.stringify({ layerMask: opts }) : JSON.stringify(opts)) : '';
        return { ox, oy, oz, dx, dy, dz, maxDist, filterStr };
    }

    fn(ns_Physics, "raycastClosest", function raycastClosest(...args) {
        const { ox, oy, oz, dx, dy, dz, maxDist, filterStr } = normalizeRayArgs(args);
        const raw = JSON.parse(__bro_native.physics.raycastClosestJsonRaw(ox, oy, oz, dx, dy, dz, maxDist, filterStr));
        if (!raw || typeof raw !== 'object' || raw.bodyId === undefined) return null;
        if (raw.userData !== undefined) raw.userData = BigInt(raw.userData);
        return raw;
    });

    fn(ns_Physics, "raycast", function raycast(...args) {
        const { ox, oy, oz, dx, dy, dz, maxDist, filterStr } = normalizeRayArgs(args);
        const raw = JSON.parse(__bro_native.physics.raycastJsonRaw(ox, oy, oz, dx, dy, dz, maxDist, filterStr));
        if (!Array.isArray(raw)) return [];
        for (const h of raw) {
            if (h.userData !== undefined) h.userData = BigInt(h.userData);
        }
        return raw;
    });

    fn(ns_Physics, "castShape", function castShape(config) {
        if (!config || typeof config !== 'object') throw new TypeError("Physics.castShape: config object is required");
        if (!config.direction || (config.direction.x === 0 && config.direction.y === 0 && config.direction.z === 0)) {
            throw new Error("Physics.castShape: direction must be non-zero");
        }
        const raw = JSON.parse(__bro_native.physics.castShapeRaw(JSON.stringify(config)));
        if (raw && raw.error) throw new Error(raw.error);
        if (!Array.isArray(raw)) return [];
        for (const h of raw) {
            if (h.userData !== undefined) h.userData = BigInt(h.userData);
        }
        return raw;
    });

    fn(ns_Physics, "castShapeClosest", function castShapeClosest(config) {
        if (!config || typeof config !== 'object') throw new TypeError("Physics.castShapeClosest: config object is required");
        if (!config.direction || (config.direction.x === 0 && config.direction.y === 0 && config.direction.z === 0)) {
            throw new Error("Physics.castShapeClosest: direction must be non-zero");
        }
        const raw = JSON.parse(__bro_native.physics.castShapeClosestRaw(JSON.stringify(config)));
        if (raw && raw.error) throw new Error(raw.error);
        if (!raw || typeof raw !== 'object' || raw.bodyId === undefined) return null;
        if (raw.userData !== undefined) raw.userData = BigInt(raw.userData);
        return raw;
    });

    fn(ns_Physics, "overlapShape", function overlapShape(config) {
        if (!config || typeof config !== 'object') throw new TypeError("Physics.overlapShape: config object is required");
        const raw = JSON.parse(__bro_native.physics.overlapShapeJsonRaw(JSON.stringify(config)));
        if (raw && raw.error) throw new Error(raw.error);
        if (!Array.isArray(raw)) return [];
        for (const h of raw) {
            if (h.userData !== undefined) h.userData = BigInt(h.userData);
        }
        return raw;
    });

    fn(ns_Physics, "penetrations", function penetrations(config) {
        const cfg = config || {};
        if (cfg.transforms) {
            ns_Physics.setTransforms(cfg.transforms);
        }
        const opt = {
            bodies: cfg.bodies,
            minDepth: cfg.minDepth,
            maxSeparation: cfg.maxSeparation,
            layerMask: cfg.layerMask !== undefined ? cfg.layerMask : cfg.layer,
            ignorePairs: cfg.ignorePairs,
        };
        return __bro_native.physics.penetrations(JSON.stringify(opt));
    });

    fn(ns_Physics, "overlapSphere", function overlapSphere(center, radius) {
        if (!center) return [];
        return Array.from(__bro_native.physics.overlapSphereRaw(center.x, center.y, center.z, radius === undefined ? 0.5 : radius));
    });

    fn(ns_Physics, "overlapBox", function overlapBox(center, halfExtents) {
        if (!center || !halfExtents) return [];
        return Array.from(__bro_native.physics.overlapBoxRaw(center.x, center.y, center.z, halfExtents.x, halfExtents.y, halfExtents.z));
    });

    fn(ns_Physics, "overlapPoint", function overlapPoint(x, y, z, options) {
        let opts = options;
        if (typeof x === 'object' && x !== null) {
            opts = y;
            z = x.z;
            y = x.y;
            x = x.x;
        }
        const filterStr = (opts !== undefined && opts !== null) ? (typeof opts === 'number' ? JSON.stringify({ layerMask: opts }) : JSON.stringify(opts)) : '';
        const raw = JSON.parse(__bro_native.physics.overlapPointJsonRaw(x, y, z, filterStr));
        if (!Array.isArray(raw)) return [];
        for (const h of raw) {
            if (h.userData !== undefined) h.userData = BigInt(h.userData);
        }
        raw.includes = function (val) {
            return Array.prototype.includes.call(this, val) || this.some(h => h && (h.bodyId === val || h === val));
        };
        return raw;
    });

    fn(ns_Physics, "setMotionType", function setMotionType(tag, type) {
        if (tag === undefined) throw new TypeError("Physics.setMotionType: tag is required");
        if (typeof type === 'boolean') {
            __bro_native.physics.setMotionType(tag, type);
        } else if (type === 'kinematic') {
            __bro_native.physics.setKinematic(tag);
        } else if (type === 'static') {
            __bro_native.physics.setMotionType(tag, true);
        } else if (type === 'dynamic') {
            __bro_native.physics.setMotionType(tag, false);
        }
    });

    const contactListeners = new Set();
    let onContactHandler = null;

    fn(ns_Physics, "onContact", function onContact(handler) {
        onContactHandler = handler;
    });
    fn(ns_Physics, "addEventListener", function addEventListener(type, listener) {
        if (type === 'contact') contactListeners.add(listener);
    });
    fn(ns_Physics, "removeEventListener", function removeEventListener(type, listener) {
        if (type === 'contact') contactListeners.delete(listener);
    });

    // ---- PhysicsWorldHandle --------------------------------------------------
    function PhysicsWorldHandle() {
        throw new TypeError("PhysicsWorldHandle is not constructible: use Physics.createWorldHandle()");
    }
    {
        const proto = __bro_native.physics.PhysicsWorldHandleProto;
        if (proto !== undefined) Object.setPrototypeOf(proto, PhysicsWorldHandle.prototype);
    }
    globalThis.PhysicsWorldHandle = PhysicsWorldHandle;
    fn(PhysicsWorldHandle.prototype, "destroy", function destroy() {
        __bro_native.physics.PhysicsWorldHandle_destroy(this);
    });
    fn(PhysicsWorldHandle.prototype, "step", function step(dt) {
        __bro_native.physics.PhysicsWorldHandle_step(this, dt === undefined ? 0 : dt);
    });

    // Forward all Physics methods through this sandbox world handle
    for (const key of Object.getOwnPropertyNames(ns_Physics)) {
        if (key === 'createWorldHandle') continue;
        const val = ns_Physics[key];
        if (typeof val === 'function') {
            fn(PhysicsWorldHandle.prototype, key, function (...args) {
                __bro_native.physics.PhysicsWorldHandle_enter(this);
                try {
                    return val.apply(ns_Physics, args);
                } finally {
                    __bro_native.physics.PhysicsWorldHandle_exit(this);
                }
            });
        }
    }
    // Explicit push/pop of the active world, for a run of `Physics.*` calls
    // against this handle without the per-call forwarding above.
    fn(PhysicsWorldHandle.prototype, "enter", function enter() {
        __bro_native.physics.PhysicsWorldHandle_enter(this);
    });
    fn(PhysicsWorldHandle.prototype, "exit", function exit() {
        __bro_native.physics.PhysicsWorldHandle_exit(this);
    });

    // ---- PhysicsCharacter ----------------------------------------------------
    function PhysicsCharacter() {
        throw new TypeError("PhysicsCharacter is not constructible: use Physics.createCharacter()");
    }
    {
        const proto = __bro_native.physics.PhysicsCharacterProto;
        if (proto !== undefined) Object.setPrototypeOf(proto, PhysicsCharacter.prototype);
    }
    globalThis.PhysicsCharacter = PhysicsCharacter;
    fn(PhysicsCharacter.prototype, "setPosition", function setPosition(x, y, z) {
        if (x === undefined) throw new TypeError("PhysicsCharacter.prototype.setPosition: x is required");
        if (y === undefined) throw new TypeError("PhysicsCharacter.prototype.setPosition: y is required");
        if (z === undefined) throw new TypeError("PhysicsCharacter.prototype.setPosition: z is required");
        __bro_native.physics.PhysicsCharacter_setPosition(this, x, y, z);
    });
    fn(PhysicsCharacter.prototype, "setVelocity", function setVelocity(x, y, z) {
        if (x === undefined) throw new TypeError("PhysicsCharacter.prototype.setVelocity: x is required");
        if (y === undefined) throw new TypeError("PhysicsCharacter.prototype.setVelocity: y is required");
        if (z === undefined) throw new TypeError("PhysicsCharacter.prototype.setVelocity: z is required");
        __bro_native.physics.PhysicsCharacter_setVelocity(this, x, y, z);
    });
    fn(PhysicsCharacter.prototype, "setLinearVelocity", function setLinearVelocity(x, y, z) {
        if (x === undefined) throw new TypeError("PhysicsCharacter.prototype.setLinearVelocity: x is required");
        if (y === undefined) throw new TypeError("PhysicsCharacter.prototype.setLinearVelocity: y is required");
        if (z === undefined) throw new TypeError("PhysicsCharacter.prototype.setLinearVelocity: z is required");
        __bro_native.physics.PhysicsCharacter_setLinearVelocity(this, x, y, z);
    });
    fn(PhysicsCharacter.prototype, "getPosition", function getPosition() {
        return JSON.parse(__bro_native.physics.PhysicsCharacter_getPosition(this));
    });
    fn(PhysicsCharacter.prototype, "getVelocity", function getVelocity() {
        return JSON.parse(__bro_native.physics.PhysicsCharacter_getVelocity(this));
    });
    fn(PhysicsCharacter.prototype, "getLinearVelocity", function getLinearVelocity() {
        return JSON.parse(__bro_native.physics.PhysicsCharacter_getLinearVelocity(this));
    });
    fn(PhysicsCharacter.prototype, "getState", function getState() {
        return JSON.parse(__bro_native.physics.PhysicsCharacter_getState(this));
    });
    accessor(PhysicsCharacter.prototype, "innerBody", function () {
        return __bro_native.physics.PhysicsCharacter_innerBody_get(this);
    }, undefined);
    fn(PhysicsCharacter.prototype, "setShape", function setShape(shape) {
        if (!shape || typeof shape !== 'object') throw new TypeError("PhysicsCharacter.prototype.setShape: shape is required");
        return __bro_native.physics.PhysicsCharacter_setShape(this, JSON.stringify(shape));
    });
    fn(PhysicsCharacter.prototype, "update", function update(dt) {
        if (dt === undefined) throw new TypeError("PhysicsCharacter.prototype.update: dt is required");
        __bro_native.physics.PhysicsCharacter_update(this, dt);
    });
    fn(PhysicsCharacter.prototype, "destroy", function destroy() {
        __bro_native.physics.PhysicsCharacter_destroy(this);
    });

    // ---- PhysicsVehicle ------------------------------------------------------
    function PhysicsVehicle() {
        throw new TypeError("PhysicsVehicle is not constructible: use Physics.createVehicle()");
    }
    {
        const proto = __bro_native.physics.PhysicsVehicleProto;
        if (proto !== undefined) Object.setPrototypeOf(proto, PhysicsVehicle.prototype);
    }
    globalThis.PhysicsVehicle = PhysicsVehicle;
    fn(PhysicsVehicle.prototype, "setInput", function setInput(input) {
        __bro_native.physics.PhysicsVehicle_setInput(this, input ? JSON.stringify(input) : "{}");
    });
    fn(PhysicsVehicle.prototype, "setDriverInput", function setDriverInput(forward, steer, brake, handBrake) {
        __bro_native.physics.PhysicsVehicle_setDriverInput(this, forward, steer, brake, handBrake);
    });
    fn(PhysicsVehicle.prototype, "setLeanController", function setLeanController(enabled) {
        __bro_native.physics.PhysicsVehicle_setLeanController(this, Boolean(enabled));
    });
    fn(PhysicsVehicle.prototype, "setGear", function setGear(gear, clutch) {
        __bro_native.physics.PhysicsVehicle_setGear(this, gear, clutch === undefined ? 1.0 : clutch);
    });
    fn(PhysicsVehicle.prototype, "wheelState", function wheelState(index) {
        if (index === undefined) throw new TypeError("PhysicsVehicle.prototype.wheelState: index is required");
        return JSON.parse(__bro_native.physics.PhysicsVehicle_wheelState(this, index));
    });
    fn(PhysicsVehicle.prototype, "getState", function getState() {
        return JSON.parse(__bro_native.physics.PhysicsVehicle_getState(this));
    });
    accessor(PhysicsVehicle.prototype, "wheelCount", function () {
        return __bro_native.physics.PhysicsVehicle_wheelCount_get(this);
    }, undefined);
    accessor(PhysicsVehicle.prototype, "chassisBody", function () {
        return __bro_native.physics.PhysicsVehicle_chassisBody_get(this);
    }, undefined);
    accessor(PhysicsVehicle.prototype, "type", function () {
        return __bro_native.physics.PhysicsVehicle_type_get(this);
    }, undefined);
    accessor(PhysicsVehicle.prototype, "speed", function () {
        return __bro_native.physics.PhysicsVehicle_speed_get(this);
    }, undefined);
    accessor(PhysicsVehicle.prototype, "rpm", function () {
        return __bro_native.physics.PhysicsVehicle_rpm_get(this);
    }, undefined);
    accessor(PhysicsVehicle.prototype, "gear", function () {
        return __bro_native.physics.PhysicsVehicle_gear_get(this);
    }, undefined);
    fn(PhysicsVehicle.prototype, "getTransform", function getTransform() {
        return JSON.parse(__bro_native.physics.PhysicsVehicle_getTransform(this));
    });
    fn(PhysicsVehicle.prototype, "destroy", function destroy() {
        __bro_native.physics.PhysicsVehicle_destroy(this);
    });

    // ---- PhysicsRagdoll ------------------------------------------------------
    function PhysicsRagdoll() {
        throw new TypeError("PhysicsRagdoll is not constructible: use Physics.createRagdoll()");
    }
    {
        const proto = __bro_native.physics.PhysicsRagdollProto;
        if (proto !== undefined) Object.setPrototypeOf(proto, PhysicsRagdoll.prototype);
    }
    globalThis.PhysicsRagdoll = PhysicsRagdoll;
    fn(PhysicsRagdoll.prototype, "pose", function pose() {
        const raw = __bro_native.physics.PhysicsRagdoll_pose(this);
        if (!raw || raw.length === 0) return null;
        return new Float32Array(raw);
    });
    fn(PhysicsRagdoll.prototype, "localPose", function localPose() {
        const raw = __bro_native.physics.PhysicsRagdoll_localPose(this);
        if (!raw || raw.length === 0) return null;
        return new Float32Array(raw);
    });
    fn(PhysicsRagdoll.prototype, "setPose", function setPose(pose) {
        if (!pose) return false;
        return __bro_native.physics.PhysicsRagdoll_setPose(this, JSON.stringify(Array.from(pose)));
    });
    fn(PhysicsRagdoll.prototype, "driveToPose", function driveToPose(pose, motorOpts) {
        if (!pose) return false;
        return __bro_native.physics.PhysicsRagdoll_driveToPose(this, JSON.stringify(Array.from(pose)), motorOpts ? JSON.stringify(motorOpts) : "");
    });
    fn(PhysicsRagdoll.prototype, "driveToPoseKinematic", function driveToPoseKinematic(pose, dt) {
        if (!pose) return false;
        return __bro_native.physics.PhysicsRagdoll_driveToPoseKinematic(this, JSON.stringify(Array.from(pose)), dt);
    });
    fn(PhysicsRagdoll.prototype, "stopDrive", function stopDrive() {
        __bro_native.physics.PhysicsRagdoll_stopDrive(this);
    });
    fn(PhysicsRagdoll.prototype, "addImpulse", function addImpulse(x, y, z) {
        __bro_native.physics.PhysicsRagdoll_addImpulse(this, x, y, z);
    });
    fn(PhysicsRagdoll.prototype, "activate", function activate() {
        __bro_native.physics.PhysicsRagdoll_activate(this);
    });
    fn(PhysicsRagdoll.prototype, "deactivate", function deactivate() {
        __bro_native.physics.PhysicsRagdoll_deactivate(this);
    });
    accessor(PhysicsRagdoll.prototype, "isActive", function () {
        return __bro_native.physics.PhysicsRagdoll_isActive(this);
    }, undefined);
    accessor(PhysicsRagdoll.prototype, "partCount", function () {
        return __bro_native.physics.PhysicsRagdoll_partCount_get(this);
    }, undefined);
    fn(PhysicsRagdoll.prototype, "partBody", function partBody(index) {
        return __bro_native.physics.PhysicsRagdoll_partBody(this, index);
    });
    fn(PhysicsRagdoll.prototype, "partParent", function partParent(index) {
        return __bro_native.physics.PhysicsRagdoll_partParent(this, index);
    });
    fn(PhysicsRagdoll.prototype, "partIndex", function partIndex(name) {
        return __bro_native.physics.PhysicsRagdoll_partIndex(this, name);
    });
    fn(PhysicsRagdoll.prototype, "destroy", function destroy() {
        __bro_native.physics.PhysicsRagdoll_destroy(this);
    });

    // ---- PhysicsSoftBody -----------------------------------------------------
    function PhysicsSoftBody() {
        throw new TypeError("PhysicsSoftBody is not constructible: use Physics.createSoftBody()");
    }
    {
        const proto = __bro_native.physics.PhysicsSoftBodyProto;
        if (proto !== undefined) Object.setPrototypeOf(proto, PhysicsSoftBody.prototype);
    }
    globalThis.PhysicsSoftBody = PhysicsSoftBody;
    accessor(PhysicsSoftBody.prototype, "vertexCount",
        function () {
            return __bro_native.physics.PhysicsSoftBody_vertexCount_get(this);
        },
        undefined);
    accessor(PhysicsSoftBody.prototype, "body",
        function () {
            return __bro_native.physics.PhysicsSoftBody_body_get(this);
        },
        undefined);
    fn(PhysicsSoftBody.prototype, "topology", function topology() {
        const raw = JSON.parse(__bro_native.physics.PhysicsSoftBody_topology(this));
        if (!raw) return raw;
        return {
            gridX: raw.gridX || 0,
            gridZ: raw.gridZ || 0,
            positions: new Float32Array(raw.positions || []),
            indices: new Uint32Array(raw.indices || []),
        };
    });
    fn(PhysicsSoftBody.prototype, "vertices", function vertices() {
        const buf = __bro_native.physics.PhysicsSoftBody_vertices(this);
        if (!buf || buf.length === 0) return null;
        return buf;
    });
    fn(PhysicsSoftBody.prototype, "pin", function pin(index, pinned) {
        if (index === undefined) throw new TypeError("PhysicsSoftBody.prototype.pin: index is required");
        return __bro_native.physics.PhysicsSoftBody_pin(this, index, pinned === undefined ? true : pinned);
    });
    fn(PhysicsSoftBody.prototype, "setVertex", function setVertex(index, x, y, z) {
        if (index === undefined) throw new TypeError("PhysicsSoftBody.prototype.setVertex: index is required");
        return __bro_native.physics.PhysicsSoftBody_setVertex(this, index, x, y, z);
    });
    fn(PhysicsSoftBody.prototype, "setVertexVelocity", function setVertexVelocity(index, x, y, z) {
        if (index === undefined) throw new TypeError("PhysicsSoftBody.prototype.setVertexVelocity: index is required");
        return __bro_native.physics.PhysicsSoftBody_setVertexVelocity(this, index, x, y, z);
    });
    fn(PhysicsSoftBody.prototype, "getBounds", function getBounds() {
        return JSON.parse(__bro_native.physics.PhysicsSoftBody_getBounds(this));
    });
    fn(PhysicsSoftBody.prototype, "destroy", function destroy() {
        __bro_native.physics.PhysicsSoftBody_destroy(this);
    });
})();
