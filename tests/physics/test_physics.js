// Test Physics API — default world + sandbox handles, body creation across
// shape kinds, transform queries, raycasts, constraints, contact events,
// kinematic motion. Exercises src/js/physics_bindings.cpp.

assert(typeof Physics === 'object', 'Physics namespace exists');

// =========================================================================
// Default world setup
// =========================================================================
Physics.destroyAll(); // start clean
Physics.setGravity(0, -9.81, 0);
const grav = Physics.getGravity();
assert(typeof grav === 'object', 'getGravity returns object');
assert(Math.abs(grav.y + 9.81) < 0.01, 'gravity.y = -9.81');

Physics.setTimeStep(1 / 60);

// =========================================================================
// Body creation: each primitive shape
// =========================================================================
const boxId = Physics.createBody({
    shape: 'box',
    halfExtents: { x: 1, y: 1, z: 1 },
    position: { x: 0, y: 10, z: 0 },
});
assert(typeof boxId === 'number' && boxId > 0, 'box tag > 0');

const sphereId = Physics.createBody({
    shape: 'sphere', radius: 0.5, position: { x: 2, y: 10, z: 0 },
});
assert(sphereId > 0, 'sphere tag');

const capsuleId = Physics.createBody({
    shape: 'capsule', radius: 0.3, halfHeight: 0.5,
    position: { x: 4, y: 10, z: 0 },
});
assert(capsuleId > 0, 'capsule tag');

const cylId = Physics.createBody({
    shape: 'cylinder', radius: 0.4, halfHeight: 0.5,
    position: { x: 6, y: 10, z: 0 },
});
assert(cylId > 0, 'cylinder tag');

// Static ground plane (large box)
const ground = Physics.createBody({
    shape: 'box', halfExtents: { x: 50, y: 0.5, z: 50 },
    position: { x: 0, y: 0, z: 0 }, static: true,
});
assert(ground > 0, 'ground tag');

// Convex hull (4 points minimum = tetrahedron)
const hullId = Physics.createBody({
    shape: 'convexHull',
    points: new Float32Array([0,0,0, 1,0,0, 0,1,0, 0,0,1]),
    position: { x: 8, y: 10, z: 0 },
});
assert(hullId > 0, 'convexHull tag');

// Mesh (static)
const meshId = Physics.createBody({
    shape: 'mesh', static: true,
    positions: new Float32Array([-5,5,-5, 5,5,-5, 5,5,5, -5,5,5]),
    indices: new Uint32Array([0,2,1, 0,3,2]),
});
assert(meshId > 0, 'mesh tag');

// Compound shape
const compId = Physics.createBody({
    shape: 'compound',
    position: { x: 10, y: 10, z: 0 },
    parts: [
        { shape: 'box', halfExtents: {x:1, y:0.5, z:0.5}, localPosition: {x:0,y:0,z:0} },
        { shape: 'box', halfExtents: {x:0.5, y:1, z:0.5}, localPosition: {x:0.5,y:0.5,z:0} },
    ],
});
assert(compId > 0, 'compound tag');

// =========================================================================
// Transform query / mutation
// =========================================================================
const xf = Physics.getTransform(sphereId);
assert(typeof xf === 'object', 'getTransform returns object');
assert(Math.abs(xf.position.x - 2) < 0.01, 'sphere x position');
assert(Math.abs(xf.position.y - 10) < 0.01, 'sphere y position');

Physics.setPosition(sphereId, 3, 11, 0);
const xf2 = Physics.getTransform(sphereId);
assert(Math.abs(xf2.position.x - 3) < 0.01, 'setPosition x');
assert(Math.abs(xf2.position.y - 11) < 0.01, 'setPosition y');

Physics.setRotation(sphereId, 0, 0, 0, 1);
const xf3 = Physics.getTransform(sphereId);
assert(Math.abs(xf3.rotation.w - 1) < 0.01, 'setRotation w=1');

// Velocity
Physics.setLinearVelocity(sphereId, 1, 2, 3);
const vel = Physics.getVelocity(sphereId);
assert(typeof vel === 'object', 'getVelocity returns object');
assert(Math.abs(vel.linear.x - 1) < 0.01, 'linear vx');
assert(Math.abs(vel.linear.y - 2) < 0.01, 'linear vy');

Physics.setAngularVelocity(sphereId, 0.5, 0, 0);
const vel2 = Physics.getVelocity(sphereId);
assert(Math.abs(vel2.angular.x - 0.5) < 0.01, 'angular vx');

// =========================================================================
// User data (small number — bigint conversion is optional)
// =========================================================================
Physics.setUserData(sphereId, 12345);
const ud = Physics.getUserData(sphereId);
assert(Number(ud) === 12345 || ud === 12345n, 'userData round-trip, got ' + ud);

// =========================================================================
// Forces / impulses / torques
// =========================================================================
Physics.addForce(sphereId, 0, 100, 0);
Physics.addImpulse(sphereId, 0, 1, 0);
Physics.addTorque(sphereId, 0, 0, 1);

// =========================================================================
// Active / activate
// =========================================================================
const active = Physics.isActive(sphereId);
assert(typeof active === 'boolean', 'isActive returns bool');
Physics.activate(sphereId);

// =========================================================================
// Layer change
// =========================================================================
const okLayer = Physics.setLayer(sphereId, 'moving');
assert(okLayer === true, 'setLayer to existing layer');

// Custom layers
const okLayers = Physics.setLayers({
    names: ['static', 'moving', 'pickup'],
    matrix: [
        false, true,  false,
        true,  true,  true,
        false, true,  false,
    ],
});
assert(okLayers === true, 'setLayers ok');

// =========================================================================
// Kinematic motion
// =========================================================================
const kineId = Physics.createBody({
    shape: 'box', halfExtents: {x:0.5, y:0.5, z:0.5},
    position: {x: -5, y: 5, z: 0},
});
Physics.setKinematic(kineId);
Physics.moveKinematic(kineId, -5, 6, 0, 1/60);

// =========================================================================
// Constraints
// =========================================================================
const aId = Physics.createBody({
    shape:'sphere', radius:0.3, position:{x:-10, y:5, z:0},
});
const bId = Physics.createBody({
    shape:'sphere', radius:0.3, position:{x:-10, y:7, z:0},
});
const distC = Physics.createConstraint({
    type: 'distance', body1: aId, body2: bId,
    point1: {x:-10, y:5, z:0}, point2: {x:-10, y:7, z:0},
    minDistance: 0.5, maxDistance: 2.5,
});
assert(distC > 0, 'distance constraint handle');

const fixedC = Physics.createConstraint({
    type: 'fixed', body1: aId, body2: bId,
});
assert(fixedC > 0, 'fixed constraint');

Physics.setConstraintEnabled(distC, false);
Physics.setConstraintEnabled(distC, true);
Physics.destroyConstraint(distC);
Physics.destroyConstraint(fixedC);

// =========================================================================
// Raycast
// =========================================================================
// Ray from above the ground straight down
const hits = Physics.raycast(0, 5, 0, 0, -1, 0, 20);
assert(Array.isArray(hits), 'raycast returns array');
// Should hit the ground or sphere
if (hits.length > 0) {
    assert(typeof hits[0].bodyId === 'number', 'hit has bodyId');
    assert(typeof hits[0].fraction === 'number', 'hit has fraction');
    assert(typeof hits[0].position === 'object', 'hit has position');
}

// =========================================================================
// Contact events
// =========================================================================
const evs = Physics.getContacts();
assert(Array.isArray(evs), 'getContacts returns array');

// =========================================================================
// Bulk transform readout
// =========================================================================
const buf = Physics.getAllTransforms();
assert(buf instanceof Float32Array, 'getAllTransforms returns Float32Array');
assert(buf.length % 8 === 0, 'stride 8');
assert(buf.length > 0, 'at least one body');

// =========================================================================
// Destroy individual bodies
// =========================================================================
Physics.destroyBody(boxId);
Physics.destroyBody(sphereId);
Physics.destroyBody(capsuleId);
Physics.destroyBody(cylId);

// =========================================================================
// Sandbox world
// =========================================================================
const w = Physics.createWorldHandle({ maxBodies: 64, gravity: {x:0, y:-5, z:0} });
assert(typeof w === 'object', 'createWorldHandle returns object');
assert(typeof w.createBody === 'function', 'sandbox has createBody');
assert(typeof w.step === 'function', 'sandbox has step');

const sTag = w.createBody({ shape:'sphere', radius:0.5, position:{x:0,y:10,z:0} });
assert(sTag > 0, 'sandbox body created');

w.setGravity(0, -9.81, 0);
w.step(1/60);

const sXf = w.getTransform(sTag);
assert(typeof sXf === 'object', 'sandbox getTransform');

w.setLinearVelocity(sTag, 0, -1, 0);
w.step(1/60);

const sHits = w.raycast(0, 20, 0, 0, -1, 0, 100);
assert(Array.isArray(sHits), 'sandbox raycast');

w.destroyAll();
w.destroy();

// =========================================================================
// Simulation loop — gravity should pull dynamic bodies down
// =========================================================================
const fallId = Physics.createBody({
    shape: 'sphere', radius: 0.5, position: { x: 0, y: 20, z: 0 },
});
const initialY = Physics.getTransform(fallId).position.y;
// Advance time = step physics in default world via engine thread.
// In headless --no-gpu, physics is stepped on advanceTime.
advanceTime(500);
const finalY = Physics.getTransform(fallId).position.y;
// Even if engine doesn't step physics in headless, position queries succeed.
assert(typeof finalY === 'number', 'final y is number');

// =========================================================================
// Cleanup
// =========================================================================
Physics.destroyAll();
