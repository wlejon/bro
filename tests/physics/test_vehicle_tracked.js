// Test tracked vehicles — Physics.createVehicle({ type: 'tracked' }) (Jolt
// VehicleConstraint + TrackedVehicleController): two skid-steered tracks,
// drive straight, differential-ratio turning (mapped `right` input and
// explicit leftRatio/rightRatio), braking, per-wheel render state, config
// rejection (missing/short tracks, empty track, unassigned/doubly-assigned
// wheels), destroy paths, and GC teardown.
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

// Test tank: ~4000 kg box chassis, forward = +Z, 5 road wheels per side.
// Left track = +X side (Jolt convention: up × forward = left), right = -X.
function tankOpts(pos) {
    const wheels = [];
    for (const x of [1.4, -1.4])            // left side first, then right
        for (let i = 0; i < 5; i++)
            wheels.push({
                position: { x: x, y: -0.3, z: 2.0 - i * 1.0 },
                radius: 0.3, width: 0.2,
                suspensionMinLength: 0.3, suspensionMaxLength: 0.5,
                suspensionFrequency: 1.0,
            });
    return {
        type: 'tracked',
        chassis: {
            shape: 'box', halfExtents: { x: 1.4, y: 0.4, z: 2.4 },
            position: pos, density: 372,    // ≈ 4000 kg
        },
        maxPitchRollAngle: 60,
        wheels: wheels,
        tracks: [
            { wheels: [0, 1, 2, 3, 4] },    // left  (+X)
            { wheels: [5, 6, 7, 8, 9] },    // right (-X)
        ],
    };
}

const tank = Physics.createVehicle(tankOpts({ x: 0, y: 1.2, z: 0 }));
assert(typeof tank === 'object', 'createVehicle type:tracked returns a handle');
assert(tank.type === 'tracked', "type getter is 'tracked', got " + tank.type);
assert(tank.wheelCount === 10, 'wheelCount is 10, got ' + tank.wheelCount);
assert(typeof tank.chassisBody === 'number' && tank.chassisBody >= 0,
       'chassisBody is a body tag');

// =========================================================================
// Settle: wheels grounded, tank at rest
// =========================================================================
advanceTime(1500);
for (let i = 0; i < 10; i++) {
    const ws = tank.wheelState(i);
    assert(ws !== null, 'wheelState(' + i + ') returns state');
    assert(ws.contact, 'wheel ' + i + ' in contact with the ground');
    assert(ws.contactBody === ground, 'wheel ' + i + ' contacts the ground body');
    assert(Math.abs(ws.steerAngle) < 1e-6, 'tracked wheel ' + i + ' never steers');
}
assert(tank.wheelState(10) === null, 'out-of-range wheel index returns null');
assert(Math.abs(tank.speed) < 0.1, 'tank at rest, speed ' + tank.speed);

const chassis = tank.chassisBody;
const restY = Physics.getTransform(chassis).position.y;

function chassisYaw() {
    const q = Physics.getTransform(chassis).rotation;
    const fx = 2 * (q.x * q.z + q.w * q.y);
    const fz = 1 - 2 * (q.x * q.x + q.y * q.y);
    return Math.atan2(fx, fz);
}

function resetTank() {
    Physics.setPosition(chassis, 0, restY + 0.1, 0);
    Physics.setRotation(chassis, 0, 0, 0, 1);
    Physics.setLinearVelocity(chassis, 0, 0, 0);
    Physics.setAngularVelocity(chassis, 0, 0, 0);
    tank.setInput({ forward: 0, right: 0, brake: 0 });
    advanceTime(500);
}

// =========================================================================
// Symmetric input drives straight along the facing (+Z)
// =========================================================================
const z0 = Physics.getTransform(chassis).position.z;
const yawStraight0 = chassisYaw();
tank.setInput({ forward: 1 });
advanceTime(2500);
let p = Physics.getTransform(chassis).position;
assert(p.z - z0 > 5, 'advanced along +Z under forward input, dz=' + (p.z - z0));
assert(Math.abs(p.x) < 1.5, 'minimal lateral drift, x=' + p.x);
assert(Math.abs(chassisYaw() - yawStraight0) < 0.15,
       'heading held while driving straight, Δyaw=' + (chassisYaw() - yawStraight0));
assert(tank.speed > 2, 'forward speed built up, got ' + tank.speed);
assert(tank.rpm > 400, 'engine spinning, rpm=' + tank.rpm);
assert(tank.gear >= 1, 'in a forward gear, got ' + tank.gear);
const drivingWs = tank.wheelState(0);
assert(drivingWs.angularVelocity > 1,
       'track wheel spinning forward, ω=' + drivingWs.angularVelocity);

// =========================================================================
// Brake stops it
// =========================================================================
tank.setInput({ forward: 0, brake: 1 });
advanceTime(3000);
assert(Math.abs(tank.speed) < 0.3, 'braked to near-stop, speed ' + tank.speed);

// handBrake maps onto the same track brakes (tanks have one brake).
resetTank();
tank.setInput({ forward: 1 });
advanceTime(2000);
assert(tank.speed > 2, 'up to speed before handBrake, got ' + tank.speed);
tank.setInput({ forward: 0, handBrake: 1 });
advanceTime(3000);
assert(Math.abs(tank.speed) < 0.3, 'handBrake input brakes a tank too');

// =========================================================================
// Reverse (auto transmission: negative forward input)
// =========================================================================
resetTank();
const zRev0 = Physics.getTransform(chassis).position.z;
tank.setInput({ forward: -1 });
advanceTime(2000);
assert(tank.speed < -0.5, 'reversing, speed ' + tank.speed);
assert(Physics.getTransform(chassis).position.z < zRev0 - 0.5, 'moved backwards');
assert(tank.gear <= -1, 'reverse gear (tank default has two), got ' + tank.gear);
tank.setInput({ forward: 0, brake: 1 });
advanceTime(2000);

// =========================================================================
// Differential steering: `right` input skid-steers; sign flips with input
// =========================================================================
// A full pivot turn covers more than π rad in 2 s — accumulate unwrapped yaw
// instead of differencing endpoint angles.
function steeredYawDelta(input) {
    resetTank();
    let prev = chassisYaw(), acc = 0;
    tank.setInput(Object.assign({ forward: 1 }, input));
    for (let i = 0; i < 8; i++) {
        advanceTime(250);
        const y = chassisYaw();
        let d = y - prev;
        if (d > Math.PI) d -= 2 * Math.PI;
        if (d < -Math.PI) d += 2 * Math.PI;
        acc += d;
        prev = y;
    }
    return acc;
}

// Sign convention matches the wheeled controller: right input → negative yaw
// (verified against a WheeledVehicleController car under the same axes).
const yawRight = steeredYawDelta({ right: 1 });
const yawLeft = steeredYawDelta({ right: -1 });
assert(yawRight < -0.3, 'full right input pivots the tank right, Δyaw=' + yawRight);
assert(yawLeft > 0.3, 'full left input pivots the tank left, Δyaw=' + yawLeft);

// Gentle turn: partial input turns less than full lock over the same time.
const yawGentle = steeredYawDelta({ right: 0.2 });
assert(Math.abs(yawGentle) > 0.05, 'partial input still turns, Δyaw=' + yawGentle);
assert(yawGentle * yawRight > 0, 'partial input turns the same way');
assert(Math.abs(yawGentle) < Math.abs(yawRight),
       'partial input turns slower than full lock');

// Explicit per-track ratios: leftRatio/rightRatio bypass the steering map.
// left=1/right=-1 is exactly the full-right-input pivot.
const yawExplicit = steeredYawDelta({ leftRatio: 1, rightRatio: -1 });
assert(yawExplicit * yawRight > 0,
       'explicit ratios (1,-1) turn like right:1, Δyaw=' + yawExplicit);
const yawExplicitL = steeredYawDelta({ leftRatio: -1, rightRatio: 1 });
assert(yawExplicitL * yawLeft > 0,
       'explicit ratios (-1,1) turn like right:-1, Δyaw=' + yawExplicitL);

// =========================================================================
// Invalid configs are rejected cleanly (no NaN/assert deaths)
// =========================================================================
function rejects(mutate, label) {
    const opts = tankOpts({ x: 50, y: 1.2, z: 0 });
    mutate(opts);
    let threw = false;
    try { Physics.createVehicle(opts); } catch (e) { threw = true; }
    assert(threw, 'rejected: ' + label);
}
rejects(o => { delete o.tracks; }, 'tracked without tracks');
rejects(o => { o.tracks = [{ wheels: [0, 1, 2, 3, 4] }]; }, 'only one track');
rejects(o => { o.tracks[1].wheels = []; }, 'track with zero wheels');
rejects(o => { o.tracks[1].wheels = [5, 6, 7, 8]; }, 'wheel assigned to no track');
rejects(o => { o.tracks[1].wheels = [4, 5, 6, 7, 8, 9]; }, 'wheel in both tracks');
rejects(o => { o.tracks[1].wheels = [5, 6, 7, 8, 42]; }, 'wheel index out of range');
rejects(o => { o.tracks[1].drivenWheel = 0; }, 'drivenWheel not in the track');
// A valid config still creates after all those rejections.
const okTank = Physics.createVehicle(tankOpts({ x: 50, y: 1.2, z: 0 }));
assert(okTank.wheelCount === 10, 'valid tracked config still creates');
okTank.destroy();

// =========================================================================
// Sandbox world + destroy paths (mirrors the wheeled destroy tests)
// =========================================================================
const w = Physics.createWorldHandle({ maxBodies: 64 });
w.createBody({
    shape: 'box', halfExtents: { x: 100, y: 0.5, z: 100 },
    position: { x: 0, y: -0.5, z: 0 }, static: true, friction: 1.0,
});
const wtank = w.createVehicle(tankOpts({ x: 0, y: 1.2, z: 0 }));
assert(wtank.type === 'tracked', 'sandbox tracked vehicle');
for (let i = 0; i < 90; i++) w.step(1 / 60);
assert(wtank.wheelState(0).contact, 'sandbox tank wheels grounded');
const wz0 = w.getTransform(wtank.chassisBody).position.z;
wtank.setInput({ forward: 1 });
for (let i = 0; i < 180; i++) w.step(1 / 60);
assert(w.getTransform(wtank.chassisBody).position.z - wz0 > 2,
       'sandbox tank drives forward');

// Handle destroy mid-drive: inline chassis goes with it.
wtank.setInput({ forward: 1 });
const wtankChassis = wtank.chassisBody;
wtank.destroy();
for (let i = 0; i < 30; i++) w.step(1 / 60);      // must not crash
assert(wtank.wheelState(0) === null, 'wheelState null after destroy');
assert(wtank.speed === 0, 'speed 0 after destroy');
wtank.destroy();                                   // double destroy is a no-op
assert(w.getTransform(wtankChassis) === undefined,
       'inline-created chassis destroyed with the tank');

// Destroying the chassis body removes the vehicle (constraint + listener).
const wtank2 = w.createVehicle(tankOpts({ x: 20, y: 1.2, z: 0 }));
wtank2.setInput({ forward: 1 });
for (let i = 0; i < 30; i++) w.step(1 / 60);
w.destroyBody(wtank2.chassisBody);
for (let i = 0; i < 30; i++) w.step(1 / 60);      // must not crash
w.destroy();

// Default-world tank destroyed mid-drive.
const tank2 = Physics.createVehicle(tankOpts({ x: 80, y: 1.2, z: 0 }));
tank2.setInput({ forward: 1 });
advanceTime(500);
tank2.destroy();
advanceTime(500);                                  // must not crash

// =========================================================================
// Teardown/GC: leave live handles in globals, no explicit destroy (Debug
// QuickJS leak assert is the real gate).
// =========================================================================
globalThis.__trackedGcWorld = Physics.createWorldHandle({ maxBodies: 32 });
globalThis.__trackedGcWorld.createBody({
    shape: 'box', halfExtents: { x: 10, y: 0.5, z: 10 },
    position: { x: 0, y: -0.5, z: 0 }, static: true,
});
globalThis.__trackedGcVehicle =
    globalThis.__trackedGcWorld.createVehicle(tankOpts({ x: 0, y: 1.2, z: 0 }));
globalThis.__trackedGcWorld.step(1 / 60);
globalThis.__trackedGcDefault = tank;   // default-world handle stays live too
