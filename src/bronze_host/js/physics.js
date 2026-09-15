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
        return __bro_native.physics.createBody(JSON.stringify(config));
    });
    fn(ns_Physics, "destroyBody", function destroyBody(tag) {
        if (tag === undefined) throw new TypeError("Physics.destroyBody: tag is required");
        __bro_native.physics.destroyBody(tag);
    });
    fn(ns_Physics, "destroyAll", function destroyAll() {
        __bro_native.physics.destroyAll();
    });
    fn(ns_Physics, "getTransform", function getTransform(tag) {
        if (tag === undefined) throw new TypeError("Physics.getTransform: tag is required");
        return JSON.parse(__bro_native.physics.getTransform(tag));
    });
    fn(ns_Physics, "getVelocity", function getVelocity(tag) {
        if (tag === undefined) throw new TypeError("Physics.getVelocity: tag is required");
        return JSON.parse(__bro_native.physics.getVelocity(tag));
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
        __bro_native.physics.setLayer(tag, layer);
    });
    fn(ns_Physics, "setKinematic", function setKinematic(tag) {
        if (tag === undefined) throw new TypeError("Physics.setKinematic: tag is required");
        __bro_native.physics.setKinematic(tag);
    });
    fn(ns_Physics, "moveKinematic", function moveKinematic(tag, x, y, z, dt) {
        if (tag === undefined) throw new TypeError("Physics.moveKinematic: tag is required");
        if (x === undefined) throw new TypeError("Physics.moveKinematic: x is required");
        if (y === undefined) throw new TypeError("Physics.moveKinematic: y is required");
        if (z === undefined) throw new TypeError("Physics.moveKinematic: z is required");
        if (dt === undefined) throw new TypeError("Physics.moveKinematic: dt is required");
        __bro_native.physics.moveKinematic(tag, x, y, z, dt);
    });
    fn(ns_Physics, "getContacts", function getContacts() {
        return JSON.parse(__bro_native.physics.getContacts());
    });
    fn(ns_Physics, "setFrictionCombine", function setFrictionCombine(tag, mode) {
        if (tag === undefined) throw new TypeError("Physics.setFrictionCombine: tag is required");
        if (mode === undefined) throw new TypeError("Physics.setFrictionCombine: mode is required");
        __bro_native.physics.setFrictionCombine(tag, mode);
    });
    fn(ns_Physics, "setRestitutionCombine", function setRestitutionCombine(tag, mode) {
        if (tag === undefined) throw new TypeError("Physics.setRestitutionCombine: tag is required");
        if (mode === undefined) throw new TypeError("Physics.setRestitutionCombine: mode is required");
        __bro_native.physics.setRestitutionCombine(tag, mode);
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
        __bro_native.physics.setAreaOverride(tag, JSON.stringify(config));
    });
    fn(ns_Physics, "setTimeStep", function setTimeStep(dt) {
        if (dt === undefined) throw new TypeError("Physics.setTimeStep: dt is required");
        __bro_native.physics.setTimeStep(dt);
    });
    fn(ns_Physics, "step", function step(dt) {
        if (dt === undefined) throw new TypeError("Physics.step: dt is required");
        __bro_native.physics.step(dt);
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
    fn(ns_Physics, "getAllTransforms", function getAllTransforms(worldHandle) {
        return __bro_native.physics.getAllTransforms(worldHandle !== undefined, worldHandle === undefined ? 0 : worldHandle);
    });
    fn(ns_Physics, "createCharacter", function createCharacter(config) {
        if (config === undefined) throw new TypeError("Physics.createCharacter: config is required");
        return __bro_native.physics.createCharacter(JSON.stringify(config));
    });
    fn(ns_Physics, "createVehicle", function createVehicle(config) {
        if (config === undefined) throw new TypeError("Physics.createVehicle: config is required");
        return __bro_native.physics.createVehicle(JSON.stringify(config));
    });
    fn(ns_Physics, "createRagdoll", function createRagdoll(config) {
        if (config === undefined) throw new TypeError("Physics.createRagdoll: config is required");
        return __bro_native.physics.createRagdoll(JSON.stringify(config));
    });
    fn(ns_Physics, "createSoftBody", function createSoftBody(config) {
        if (config === undefined) throw new TypeError("Physics.createSoftBody: config is required");
        return __bro_native.physics.createSoftBody(JSON.stringify(config));
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
    fn(ns_Physics, "setWheelMotor", function setWheelMotor(vehicleTag, wheelIndex, motorTorque, brakeTorque) {
        if (vehicleTag === undefined) throw new TypeError("Physics.setWheelMotor: vehicleTag is required");
        if (wheelIndex === undefined) throw new TypeError("Physics.setWheelMotor: wheelIndex is required");
        if (motorTorque === undefined) throw new TypeError("Physics.setWheelMotor: motorTorque is required");
        if (brakeTorque === undefined) throw new TypeError("Physics.setWheelMotor: brakeTorque is required");
        __bro_native.physics.setWheelMotor(vehicleTag, wheelIndex, motorTorque, brakeTorque);
    });
    fn(ns_Physics, "setConstraintMotor", function setConstraintMotor(tag, config) {
        if (tag === undefined) throw new TypeError("Physics.setConstraintMotor: tag is required");
        if (config === undefined) throw new TypeError("Physics.setConstraintMotor: config is required");
        __bro_native.physics.setConstraintMotor(tag, JSON.stringify(config));
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
    fn(ns_Physics, "raycastClosest", function raycastClosest(p1, p2, mask) {
        if (typeof p1 === 'number') {
            const ox = arguments[0], oy = arguments[1], oz = arguments[2];
            const dx = arguments[3], dy = arguments[4], dz = arguments[5];
            const maxDist = arguments[6] === undefined ? 1000 : arguments[6];
            const m = (arguments[7] !== undefined && typeof arguments[7] === 'number') ? arguments[7] : 0;
            const res = __bro_native.physics.raycastClosestRaw(ox, oy, oz, dx, dy, dz, maxDist, m);
            return JSON.parse(res);
        }
        if (!p1 || !p2) return null;
        let ox = p1.x, oy = p1.y, oz = p1.z;
        let tx = p2.x, ty = p2.y, tz = p2.z;
        let dx = tx - ox, dy = ty - oy, dz = tz - oz;
        let len = Math.sqrt(dx * dx + dy * dy + dz * dz);
        let maxDist = len > 0 ? len : 1000;
        if (len > 1e-6) {
            dx /= len; dy /= len; dz /= len;
        } else {
            dx = 0; dy = 0; dz = 0;
        }
        const res = __bro_native.physics.raycastClosestRaw(ox, oy, oz, dx, dy, dz, maxDist, (mask !== undefined && typeof mask === 'number') ? mask : 0);
        return JSON.parse(res);
    });

    fn(ns_Physics, "raycast", function raycast(p1, p2, mask) {
        if (typeof p1 === 'number') {
            const ox = arguments[0], oy = arguments[1], oz = arguments[2];
            const dx = arguments[3], dy = arguments[4], dz = arguments[5];
            const maxDist = arguments[6] === undefined ? 1000 : arguments[6];
            const m = (arguments[7] !== undefined && typeof arguments[7] === 'number') ? arguments[7] : 0;
            const res = __bro_native.physics.raycastRaw(ox, oy, oz, dx, dy, dz, maxDist, m);
            return JSON.parse(res);
        }
        if (!p1 || !p2) return [];
        let ox = p1.x, oy = p1.y, oz = p1.z;
        let tx = p2.x, ty = p2.y, tz = p2.z;
        let dx = tx - ox, dy = ty - oy, dz = tz - oz;
        let len = Math.sqrt(dx * dx + dy * dy + dz * dz);
        let maxDist = len > 0 ? len : 1000;
        if (len > 1e-6) {
            dx /= len; dy /= len; dz /= len;
        } else {
            dx = 0; dy = 0; dz = 0;
        }
        const res = __bro_native.physics.raycastRaw(ox, oy, oz, dx, dy, dz, maxDist, (mask !== undefined && typeof mask === 'number') ? mask : 0);
        return JSON.parse(res);
    });

    fn(ns_Physics, "overlapSphere", function overlapSphere(center, radius) {
        if (!center) return [];
        return Array.from(__bro_native.physics.overlapSphereRaw(center.x, center.y, center.z, radius === undefined ? 0.5 : radius));
    });

    fn(ns_Physics, "overlapBox", function overlapBox(center, halfExtents) {
        if (!center || !halfExtents) return [];
        return Array.from(__bro_native.physics.overlapBoxRaw(center.x, center.y, center.z, halfExtents.x, halfExtents.y, halfExtents.z));
    });

    fn(ns_Physics, "overlapPoint", function overlapPoint(x, y, z, mask) {
        if (typeof x === 'object') {
            mask = y;
            z = x.z;
            y = x.y;
            x = x.x;
        }
        return Array.from(__bro_native.physics.overlapPointRaw(x, y, z, mask === undefined ? 0 : mask));
    });

    fn(ns_Physics, "setMotionType", function setMotionType(tag, type) {
        if (type === 'kinematic') __bro_native.physics.setKinematic(tag);
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
        throw new TypeError("PhysicsWorldHandle is not constructible");
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
        if (dt === undefined) throw new TypeError("PhysicsWorldHandle.prototype.step: dt is required");
        __bro_native.physics.PhysicsWorldHandle_step(this, dt);
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
    fn(PhysicsVehicle.prototype, "setDriverInput", function setDriverInput(forward, steer, brake, handBrake) {
        __bro_native.physics.PhysicsVehicle_setDriverInput(this, forward, steer, brake, handBrake);
    });
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
    fn(PhysicsRagdoll.prototype, "driveToPose", function driveToPose(pose, dt) {
        __bro_native.physics.PhysicsRagdoll_driveToPose(this, JSON.stringify(pose), dt);
    });
    fn(PhysicsRagdoll.prototype, "getPose", function getPose() {
        return JSON.parse(__bro_native.physics.PhysicsRagdoll_getPose(this));
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
    fn(PhysicsSoftBody.prototype, "topology", function topology() {
        return JSON.parse(__bro_native.physics.PhysicsSoftBody_topology(this));
    });
    fn(PhysicsSoftBody.prototype, "vertices", function vertices() {
        return __bro_native.physics.PhysicsSoftBody_vertices(this);
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
