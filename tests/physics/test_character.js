// Test the character controller — Physics.createCharacter (Jolt
// CharacterVirtual): grounding + rest stability, walking at a set velocity,
// step climb (WalkStairs), wall blocking + sliding, too-steep-slope slide,
// pushing dynamic bodies, riding a kinematic platform, jump launch, teleport,
// destroy, and the sandbox-world variant (exact step-driven walk rate).
// Exercises src/js/physics_bindings.cpp + src/physics/physics_world.cpp.
//
// Characters update inside the engine's fixed physics tick; under headless,
// advanceTime(ms) drives that tick deterministically (~60 steps/sec — the
// 16 ms virtual-time chunking makes long spans land within a step or two of
// nominal, hence the ~5% tolerances on distances).

assert(typeof Physics === 'object', 'Physics namespace exists');
assert(typeof Physics.createCharacter === 'function', 'createCharacter exists');

Physics.destroyAll();

// Floor slab: top face at y = 0.
const floor = Physics.createBody({
    shape: 'box', halfExtents: { x: 50, y: 0.5, z: 50 },
    position: { x: 0, y: -0.5, z: 0 }, static: true,
});

// Capsule r=0.3, halfHeight=0.6 → rest center height = 0.9 (+~0.02 padding).
const REST_Y = 0.9;
const ch = Physics.createCharacter({
    radius: 0.3, halfHeight: 0.6,
    position: { x: 0, y: 1.0, z: 0 },
    stepUp: 0.4, stickToFloor: 0.5, maxSlopeAngle: 45,
});
assert(typeof ch === 'object', 'createCharacter returns a handle');
assert(typeof ch.setVelocity === 'function', 'handle has setVelocity');
assert(typeof ch.getState === 'function', 'handle has getState');

// =========================================================================
// Grounding + rest stability
// =========================================================================
advanceTime(500);
let st = ch.getState();
assert(st !== null, 'getState returns state');
assert(st.isGrounded, 'character grounded on floor, got ' + st.groundState);
assert(st.groundState === 'onGround', 'groundState is onGround');
assert(Math.abs(st.position.y - REST_Y) < 0.07,
       'rest height ~' + REST_Y + ', got ' + st.position.y);
assert(st.groundNormal.y > 0.99, 'ground normal points up');
assert(st.groundBodyId === floor, 'groundBodyId is the floor');
assert(Math.abs(st.velocity.x) + Math.abs(st.velocity.z) < 0.01, 'at rest horizontally');

const restY = st.position.y;
advanceTime(500);
st = ch.getState();
assert(Math.abs(st.position.y - restY) < 0.01, 'rest position stable, drift ' +
       Math.abs(st.position.y - restY));

// =========================================================================
// Walking at a set velocity
// =========================================================================
ch.setVelocity(2, 0, 0);
const x0 = ch.getState().position.x;
advanceTime(1000);
st = ch.getState();
const walked = st.position.x - x0;
assert(Math.abs(walked - 2.0) < 0.2, 'walks ~2m in 1s at 2 m/s, got ' + walked);
assert(st.isGrounded, 'still grounded while walking');
assert(Math.abs(st.position.y - restY) < 0.02, 'height stable while walking');
assert(Math.abs(st.velocity.x - 2) < 0.05, 'actual velocity ~2, got ' + st.velocity.x);

// =========================================================================
// Step climb (step height 0.3 < stepUp 0.4)
// =========================================================================
// Plateau spanning x in [3,7], top at y = 0.3, in the walking path.
const step = Physics.createBody({
    shape: 'box', halfExtents: { x: 2, y: 0.15, z: 1 },
    position: { x: 5, y: 0.15, z: 0 }, static: true,
});
advanceTime(1500);   // keep walking: ~2.9m more → onto the plateau
st = ch.getState();
assert(st.position.x > 4.0 && st.position.x < 6.9,
       'character on the plateau span, got x=' + st.position.x);
assert(st.isGrounded, 'grounded on the step');
assert(st.groundBodyId === step, 'stood on the step body');
assert(Math.abs(st.position.y - (REST_Y + 0.3)) < 0.08,
       'climbed the 0.3 step, y=' + st.position.y);

// Stop: no drift once desired velocity is zero.
ch.setVelocity(0, 0, 0);
advanceTime(200);
const stopX = ch.getState().position.x;
advanceTime(300);
assert(Math.abs(ch.getState().position.x - stopX) < 0.02, 'stationary after stop');

// =========================================================================
// Jump launch (positive up component while grounded)
// =========================================================================
const jumpY0 = ch.getState().position.y;
ch.setVelocity(0, 6, 0);
advanceTime(150);
st = ch.getState();
assert(!st.isGrounded, 'airborne after jump launch');
assert(st.position.y > jumpY0 + 0.3, 'rising, y=' + st.position.y);
assert(st.velocity.y > 0, 'upward velocity during ascent');
ch.setVelocity(0, 0, 0);   // stop holding "jump" before landing
advanceTime(2000);
st = ch.getState();
assert(st.isGrounded, 'landed and re-grounded after jump');

// =========================================================================
// Wall: blocked, no penetration, slides along it
// =========================================================================
// Wall near face at x = 4.5, spanning z in [5,15].
const wall = Physics.createBody({
    shape: 'box', halfExtents: { x: 0.5, y: 2, z: 5 },
    position: { x: 5, y: 2, z: 10 }, static: true,
});
ch.setPosition(2, 0.95, 10);
ch.setVelocity(2, 0, 1);     // diagonally into the wall
advanceTime(2000);
st = ch.getState();
assert(st.position.x + 0.3 < 4.51, 'no wall penetration, surface at ' +
       (st.position.x + 0.3));
assert(st.position.x > 3.9, 'reached the wall, x=' + st.position.x);
assert(st.position.z > 11.4, 'slid along the wall in z, z=' + st.position.z);
assert(st.isGrounded, 'grounded while sliding along wall');
ch.setVelocity(0, 0, 0);

// =========================================================================
// Too-steep slope (60° face, maxSlopeAngle 45): not grounded, slides down
// =========================================================================
// Box rotated 60° about z; inclined top face normal (-sin60, cos60, 0).
const slope = Physics.createBody({
    shape: 'box', halfExtents: { x: 4, y: 0.5, z: 4 },
    position: { x: 0, y: 3, z: 20 },
    rotation: { x: 0, y: 0, z: 0.5, w: 0.8660254 },
    static: true,
});
const chS = Physics.createCharacter({
    radius: 0.3, halfHeight: 0.6,
    position: { x: -0.6, y: 4.4, z: 20 },   // above the 60° face
    maxSlopeAngle: 45,
});
advanceTime(400);   // land on the face
let s1 = chS.getState();
assert(!s1.isGrounded, 'too-steep slope does not ground, got ' + s1.groundState);
assert(s1.groundState === 'onSteepGround' || s1.groundState === 'inAir',
       'on steep ground or falling, got ' + s1.groundState);
advanceTime(300);
let s2 = chS.getState();
assert(s2.position.y < s1.position.y - 0.03,
       'slides down the slope: ' + s1.position.y + ' → ' + s2.position.y);
chS.destroy();

// =========================================================================
// Pushing a dynamic body (maxStrength)
// =========================================================================
// Frictionless light box in the path (floor/box combined friction = 0).
const crate = Physics.createBody({
    shape: 'box', halfExtents: { x: 0.25, y: 0.25, z: 0.25 },
    position: { x: 4, y: 0.25, z: 30 }, friction: 0, restitution: 0,
});
const crateX0 = Physics.getTransform(crate).position.x;
ch.setPosition(2, 0.95, 30);
ch.setVelocity(2, 0, 0);
advanceTime(2000);
const crateX = Physics.getTransform(crate).position.x;
assert(crateX > crateX0 + 0.2, 'character pushed the crate, moved ' + (crateX - crateX0));
assert(ch.getState().position.x > 3.5, 'character kept moving while pushing');
ch.setVelocity(0, 0, 0);

// =========================================================================
// Riding a moving kinematic platform (ground velocity carry)
// =========================================================================
const plat = Physics.createBody({
    shape: 'box', halfExtents: { x: 2, y: 0.25, z: 2 },
    position: { x: 0, y: 1.75, z: 40 },   // top at y = 2
});
Physics.setKinematic(plat);
Physics.setLinearVelocity(plat, 1, 0, 0);
ch.setPosition(0, 2.95, 40);
advanceTime(300);   // land + couple to the platform
st = ch.getState();
assert(st.isGrounded, 'grounded on the platform');
assert(st.groundBodyId === plat, 'groundBodyId is the platform');
assert(Math.abs(st.groundVelocity.x - 1) < 0.2,
       'groundVelocity ~1 m/s, got ' + st.groundVelocity.x);
const rideX0 = st.position.x;
const platX0 = Physics.getTransform(plat).position.x;
advanceTime(1000);
st = ch.getState();
const rideDx = st.position.x - rideX0;
const platDx = Physics.getTransform(plat).position.x - platX0;
assert(rideDx > 0.7, 'carried by the platform, moved ' + rideDx);
assert(Math.abs(rideDx - platDx) < 0.2,
       'no slip vs platform: char ' + rideDx + ' vs plat ' + platDx);

// =========================================================================
// Teleport (setPosition) + fall
// =========================================================================
ch.setVelocity(0, 0, 0);
ch.setPosition(30, 5, 0);
let p = ch.getPosition();
assert(Math.abs(p.x - 30) < 1e-4 && Math.abs(p.y - 5) < 1e-4 && Math.abs(p.z) < 1e-4,
       'teleport reads back exactly');
advanceTime(200);
st = ch.getState();
assert(st.position.y < 5 - 0.1, 'falling after mid-air teleport, y=' + st.position.y);
assert(st.groundState === 'inAir', 'inAir while falling');
advanceTime(1500);
st = ch.getState();
assert(st.isGrounded, 're-grounded after fall');
assert(Math.abs(st.position.y - REST_Y) < 0.07, 'rest height after fall');

// =========================================================================
// destroy()
// =========================================================================
ch.destroy();
assert(ch.getState() === null, 'getState null after destroy');
ch.destroy();   // double destroy is a no-op
ch.setVelocity(1, 0, 0);   // and stale calls don't throw

// =========================================================================
// Sandbox world: caller-driven stepping → exact walk rate
// =========================================================================
const w = Physics.createWorldHandle({ maxBodies: 64 });
assert(typeof w.createCharacter === 'function', 'sandbox createCharacter exists');

w.createBody({
    shape: 'box', halfExtents: { x: 20, y: 0.5, z: 20 },
    position: { x: 0, y: -0.5, z: 0 }, static: true,
});
const wch = w.createCharacter({
    radius: 0.3, halfHeight: 0.6, position: { x: 0, y: 1.0, z: 0 },
});
for (let i = 0; i < 60; i++) w.step(1 / 60);
let ws = wch.getState();
assert(ws.isGrounded, 'sandbox character grounded');
assert(Math.abs(ws.position.y - REST_Y) < 0.07, 'sandbox rest height');

// 120 exact steps at 1.5 m/s → exactly 3.0 m.
wch.setVelocity(1.5, 0, 0);
const wx0 = wch.getState().position.x;
for (let i = 0; i < 120; i++) w.step(1 / 60);
ws = wch.getState();
const wWalked = ws.position.x - wx0;
assert(Math.abs(wWalked - 3.0) < 0.05, 'sandbox walks exactly 3m in 120 steps, got ' + wWalked);
assert(ws.isGrounded, 'sandbox still grounded after walk');
w.destroy();
assert(wch.getState() === null, 'sandbox character state null after world destroy');

// =========================================================================
// Cleanup
// =========================================================================
Physics.destroyAll();
