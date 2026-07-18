// Test motorcycles — Physics.createVehicle({ type: 'motorcycle' }) (Jolt
// VehicleConstraint + MotorcycleController = WheeledVehicleController + lean
// spring): stays upright while driving, moves forward, steers both ways,
// brakes, falls over when the lean controller is disabled (and recovers from
// the same kick when it is enabled), destroy paths, GC teardown.
// Exercises src/js/physics_bindings.cpp + src/physics/physics_world.cpp.
//
// The vehicle steps inside the engine's fixed physics tick; under headless,
// advanceTime(ms) drives that tick deterministically at ~60 steps/sec.

assert(typeof Physics === 'object', 'Physics namespace exists');

Physics.destroyAll();

// Big ground slab, top face at y = 0, grippy.
const ground = Physics.createBody({
    shape: 'box', halfExtents: { x: 200, y: 0.5, z: 200 },
    position: { x: 0, y: -0.5, z: 0 }, static: true, friction: 1.0,
});

// Test bike, numbers from Jolt's MotorcycleTest (Yamaha XJ 900 flavored):
// ~240 kg box chassis, forward = +Z, steerable front wheel on a 30° caster,
// driven rear wheel.
function bikeOpts(pos, extra) {
    return Object.assign({
        type: 'motorcycle',
        chassis: {
            shape: 'box', halfExtents: { x: 0.2, y: 0.3, z: 0.4 },
            position: pos, density: 1250,   // ≈ 240 kg
        },
        maxPitchRollAngle: 60,
        wheels: [
            { position: { x: 0, y: -0.27, z: 0.75 }, radius: 0.31, width: 0.05,
              suspensionMinLength: 0.3, suspensionMaxLength: 0.5,
              suspensionFrequency: 1.5, suspensionDirection: { x: 0, y: -1, z: 0.577 },
              steerable: true, maxSteerAngle: 30, maxBrakeTorque: 500 },   // 0 front
            { position: { x: 0, y: -0.27, z: -0.75 }, radius: 0.31, width: 0.05,
              suspensionMinLength: 0.3, suspensionMaxLength: 0.5,
              suspensionFrequency: 2.0, driven: true, maxBrakeTorque: 250 }, // 1 rear
        ],
        engine: { maxTorque: 150, minRPM: 1000, maxRPM: 10000 },
        transmission: {
            clutchStrength: 2, shiftUpRPM: 8000, shiftDownRPM: 2000,
            gearRatios: [2.27, 1.63, 1.3, 1.09, 0.96, 0.88],
            reverseGearRatios: [-4],
        },
    }, extra || {});
}

// Up-axis alignment with world up: 1 = upright, ~0 = on its side.
function upDot(tag) {
    const q = Physics.getTransform(tag).rotation;
    return 1 - 2 * (q.x * q.x + q.z * q.z);   // (rotate (0,1,0) by q).y
}

const bike = Physics.createVehicle(bikeOpts({ x: 0, y: 1, z: 0 }));
assert(typeof bike === 'object', 'createVehicle type:motorcycle returns a handle');
assert(bike.type === 'motorcycle', "type getter is 'motorcycle', got " + bike.type);
assert(bike.wheelCount === 2, 'wheelCount is 2, got ' + bike.wheelCount);
assert(typeof bike.setLeanController === 'function', 'setLeanController exists');
const chassis = bike.chassisBody;

// =========================================================================
// Settle upright: lean spring balances a standing bike
// =========================================================================
advanceTime(2000);
assert(bike.wheelState(0).contact, 'front wheel grounded');
assert(bike.wheelState(1).contact, 'rear wheel grounded');
assert(upDot(chassis) > 0.95, 'standing bike balanced upright, up·Y=' + upDot(chassis));
assert(Math.abs(bike.speed) < 0.2, 'bike at rest, speed ' + bike.speed);

// =========================================================================
// Drives forward and STAYS UPRIGHT the whole way
// =========================================================================
const z0 = Physics.getTransform(chassis).position.z;
bike.setInput({ forward: 1 });
let minUp = 1;
for (let i = 0; i < 12; i++) {          // 3 s, sampled every 250 ms
    advanceTime(250);
    minUp = Math.min(minUp, upDot(chassis));
}
const p = Physics.getTransform(chassis).position;
assert(p.z - z0 > 5, 'advanced along +Z, dz=' + (p.z - z0));
assert(Math.abs(p.x) < 2, 'no significant sideways drift, x=' + p.x);
assert(minUp > 0.9, 'stayed upright while driving, worst up·Y=' + minUp);
assert(bike.speed > 3, 'forward speed built up, got ' + bike.speed);
assert(bike.rpm > 900, 'engine spinning, rpm=' + bike.rpm);
assert(bike.gear >= 1, 'in a forward gear, got ' + bike.gear);
const rearWs = bike.wheelState(1);
assert(rearWs.angularVelocity > 2, 'rear wheel driving, ω=' + rearWs.angularVelocity);

// =========================================================================
// Brakes to a stop (still upright)
// =========================================================================
bike.setInput({ forward: 0, brake: 1 });
advanceTime(3000);
assert(Math.abs(bike.speed) < 0.5, 'braked to near-stop, speed ' + bike.speed);
assert(upDot(chassis) > 0.9, 'upright after braking, up·Y=' + upDot(chassis));

// =========================================================================
// Steering: heading change flips with steer sign (fresh bike per run —
// motorcycles are not trivially resettable mid-lean)
// =========================================================================
function chassisYawOf(tag) {
    const q = Physics.getTransform(tag).rotation;
    const fx = 2 * (q.x * q.z + q.w * q.y);
    const fz = 1 - 2 * (q.x * q.x + q.y * q.y);
    return Math.atan2(fx, fz);
}

function steeredYawDelta(rightInput, x) {
    const b = Physics.createVehicle(bikeOpts({ x: x, y: 1, z: 0 }));
    advanceTime(1500);
    b.setInput({ forward: 1 });
    advanceTime(1000);                   // build speed going straight
    const yaw0 = chassisYawOf(b.chassisBody);
    b.setInput({ forward: 1, right: rightInput });
    advanceTime(2000);
    const dYaw = chassisYawOf(b.chassisBody) - yaw0;
    const up = upDot(b.chassisBody);
    b.destroy();
    return { dYaw: dYaw, up: up };
}

const turnR = steeredYawDelta(1, 30);
const turnL = steeredYawDelta(-1, 60);
assert(Math.abs(turnR.dYaw) > 0.1, 'steering right turns the bike, Δyaw=' + turnR.dYaw);
assert(Math.abs(turnL.dYaw) > 0.1, 'steering left turns the bike, Δyaw=' + turnL.dYaw);
assert(turnR.dYaw * turnL.dYaw < 0, 'heading change flips with steer sign: right=' +
       turnR.dYaw + ' left=' + turnL.dYaw);
assert(turnR.up > 0.7 && turnL.up > 0.7,
       'bike leans but does not fall in turns, up·Y right=' + turnR.up +
       ' left=' + turnL.up);

// =========================================================================
// Lean controller is what keeps it up: same roll kick, spring off → falls,
// spring on → recovers. (Fall bikes omit maxPitchRollAngle so nothing else
// rights them.)
// =========================================================================
function kickedUpDot(leanEnabled, x) {
    const b = Physics.createVehicle(bikeOpts({ x: x, y: 1, z: 0 },
                                             { maxPitchRollAngle: 180 }));
    advanceTime(1500);
    b.setLeanController(leanEnabled);
    // Roll kick around the forward (+Z) axis.
    Physics.setAngularVelocity(b.chassisBody, 0, 0, 2);
    advanceTime(2500);
    const up = upDot(b.chassisBody);
    b.destroy();
    return up;
}

const upSpringOff = kickedUpDot(false, 90);
const upSpringOn = kickedUpDot(true, 120);
assert(upSpringOff < 0.5, 'lean controller off: kicked bike falls over, up·Y=' +
       upSpringOff);
assert(upSpringOn > 0.85, 'lean controller on: kicked bike recovers, up·Y=' +
       upSpringOn);

// setLeanController is motorcycle-only sugar; on other vehicles it must be a
// safe no-op at the world level (C++ returns false) — nothing to observe
// here beyond "does not throw".
bike.setLeanController(true);

// =========================================================================
// Sandbox world + destroy paths (mirrors the wheeled destroy tests)
// =========================================================================
const w = Physics.createWorldHandle({ maxBodies: 64 });
w.createBody({
    shape: 'box', halfExtents: { x: 100, y: 0.5, z: 100 },
    position: { x: 0, y: -0.5, z: 0 }, static: true, friction: 1.0,
});
const wbike = w.createVehicle(bikeOpts({ x: 0, y: 1, z: 0 }));
assert(wbike.type === 'motorcycle', 'sandbox motorcycle');
for (let i = 0; i < 120; i++) w.step(1 / 60);
assert(wbike.wheelState(0).contact, 'sandbox bike wheels grounded');
const wz0 = w.getTransform(wbike.chassisBody).position.z;
wbike.setInput({ forward: 1 });
for (let i = 0; i < 180; i++) w.step(1 / 60);
assert(w.getTransform(wbike.chassisBody).position.z - wz0 > 2,
       'sandbox bike drives forward');

// Handle destroy mid-drive: inline chassis goes with it.
const wbikeChassis = wbike.chassisBody;
wbike.destroy();
for (let i = 0; i < 30; i++) w.step(1 / 60);      // must not crash
assert(wbike.wheelState(0) === null, 'wheelState null after destroy');
wbike.destroy();                                   // double destroy is a no-op
assert(w.getTransform(wbikeChassis) === undefined,
       'inline-created chassis destroyed with the bike');

// Destroying the chassis body removes the vehicle (constraint + listener).
const wbike2 = w.createVehicle(bikeOpts({ x: 20, y: 1, z: 0 }));
wbike2.setInput({ forward: 1 });
for (let i = 0; i < 30; i++) w.step(1 / 60);
w.destroyBody(wbike2.chassisBody);
for (let i = 0; i < 30; i++) w.step(1 / 60);      // must not crash
w.destroy();

// Default-world bike destroyed mid-drive.
const bike2 = Physics.createVehicle(bikeOpts({ x: 150, y: 1, z: 0 }));
bike2.setInput({ forward: 1 });
advanceTime(500);
bike2.destroy();
advanceTime(500);                                  // must not crash

// =========================================================================
// Teardown/GC: leave live handles in globals, no explicit destroy (Debug
// QuickJS leak assert is the real gate).
// =========================================================================
globalThis.__motoGcWorld = Physics.createWorldHandle({ maxBodies: 32 });
globalThis.__motoGcWorld.createBody({
    shape: 'box', halfExtents: { x: 10, y: 0.5, z: 10 },
    position: { x: 0, y: -0.5, z: 0 }, static: true,
});
globalThis.__motoGcVehicle =
    globalThis.__motoGcWorld.createVehicle(bikeOpts({ x: 0, y: 1, z: 0 }));
globalThis.__motoGcWorld.step(1 / 60);
globalThis.__motoGcDefault = bike;      // default-world handle stays live too
