// Physics.createVehicle options the port had dropped: `differentials`
// (explicit drive pairs, overriding the wheels' `driven` flags),
// `collisionTester` and `testerLayer`.
//
// Zero gravity, no ground: nothing touches the wheels, so a wheel spins only
// if the engine drives it. That makes "which wheels are driven" readable off
// wheelState().angularVelocity.

Physics.destroyAll();
Physics.setGravity(0, 0, 0);

const R = 0.35;
function wheel(x, z, extra) {
    return Object.assign({ position: { x, y: -0.3, z }, radius: R, width: 0.25,
                           suspensionMinLength: 0.1, suspensionMaxLength: 0.5 }, extra || {});
}
function car(x, extra) {
    return Object.assign({
        chassis: { shape: 'box', halfExtents: { x: 0.9, y: 0.4, z: 2.0 },
                   position: { x, y: 20, z: 0 }, density: 260 },
        wheels: [
            wheel(-0.8,  1.3, { steerable: true }),   // 0 FL
            wheel( 0.8,  1.3, { steerable: true }),   // 1 FR
            wheel(-0.8, -1.3, { driven: true }),      // 2 RL
            wheel( 0.8, -1.3, { driven: true }),      // 3 RR
        ],
    }, extra || {});
}

// Rear-drive by flags vs front-drive by explicit differentials.
const rwd = Physics.createVehicle(car(0));
const fwd = Physics.createVehicle(car(10, {
    differentials: [{ leftWheel: 0, rightWheel: 1, ratio: 3.42 }],
    collisionTester: 'ray',
}));
assert(rwd && fwd, 'both vehicles created');

for (let i = 0; i < 60; i++) {
    rwd.setInput({ forward: 1 });
    fwd.setInput({ forward: 1 });
    advanceTime(16);
}

const spin = (v, i) => Math.abs(v.wheelState(i).angularVelocity);
const r = [0, 1, 2, 3].map((i) => spin(rwd, i));
const f = [0, 1, 2, 3].map((i) => spin(fwd, i));
console.log('rwd spins ' + r.map((x) => x.toFixed(2)).join(',') + '  fwd spins ' + f.map((x) => x.toFixed(2)).join(','));

assert(r[2] > 1 && r[3] > 1, 'driven:true rear wheels spin without differentials');
assert(r[0] < 0.1 && r[1] < 0.1, 'undriven front wheels stay still');
assert(f[0] > 1 && f[1] > 1, 'explicit differential drives the front pair');
assert(f[2] < 0.1 && f[3] < 0.1, 'differentials override the driven flags (rear stays still)');

// leftRightSplit = 0 sends all torque to the left wheel (limited slip off,
// or it would hand torque back to the slower right wheel).
const left = Physics.createVehicle(car(20, {
    differentials: [{ leftWheel: 2, rightWheel: 3, leftRightSplit: 0, limitedSlipRatio: 1e30 }],
}));
for (let i = 0; i < 60; i++) { left.setInput({ forward: 1 }); advanceTime(16); }
assert(spin(left, 2) > 1, 'leftRightSplit=0 spins the left wheel: ' + spin(left, 2));
assert(spin(left, 3) < 0.1, 'leftRightSplit=0 leaves the right wheel: ' + spin(left, 3));

// collisionTester is validated.
let threw = null;
try { Physics.createVehicle(car(30, { collisionTester: 'cube' })); } catch (e) { threw = e; }
assert(threw instanceof TypeError, 'an unknown collisionTester throws TypeError, got ' + threw);

// testerLayer: a name or an index is accepted.
const byName = Physics.createVehicle(car(40, { testerLayer: 'moving' }));
const byIndex = Physics.createVehicle(car(50, { testerLayer: 1 }));
assert(byName && byIndex, 'testerLayer by name and by index both create');

rwd.destroy(); fwd.destroy(); left.destroy(); byName.destroy(); byIndex.destroy();
Physics.destroyAll();
Physics.setGravity(0, -9.81, 0);
console.log('vehicle options OK');
