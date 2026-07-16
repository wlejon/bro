// Test wheeled vehicles — Physics.createVehicle (Jolt VehicleConstraint +
// WheeledVehicleController): rest/suspension sanity, forward acceleration,
// steering sign symmetry, brake to a stop, reverse, handbrake wheel lock,
// per-wheel render state, sandbox-world form, destroy mid-sim (handle and
// chassis-body paths), and GC teardown without explicit destroy (the
// PhysicsCharacter lesson: worldRef needs gc_mark + ~JsWorld severing).
// Exercises src/js/physics_bindings.cpp + src/physics/physics_world.cpp.
//
// The vehicle steps inside the engine's fixed physics tick (the constraint is
// a Jolt StepListener); under headless, advanceTime(ms) drives that tick
// deterministically at ~60 steps/sec.

assert(typeof Physics === 'object', 'Physics namespace exists');
assert(typeof Physics.createVehicle === 'function', 'createVehicle exists');

Physics.destroyAll();

// Big ground slab, top face at y = 0, grippy.
const ground = Physics.createBody({
    shape: 'box', halfExtents: { x: 200, y: 0.5, z: 200 },
    position: { x: 0, y: -0.5, z: 0 }, static: true, friction: 1.0,
});

// Standard test car: ~1500 kg box chassis (density-derived), forward = +Z,
// front wheels steer, rear wheels driven + handbrake.
const WHEEL_MIN = 0.1, WHEEL_MAX = 0.5, WHEEL_R = 0.35;
function carOpts(pos) {
    return {
        chassis: {
            shape: 'box', halfExtents: { x: 0.9, y: 0.4, z: 2.0 },
            position: pos, density: 260,   // ≈ 1500 kg
        },
        wheels: [
            { position: { x: -0.8, y: -0.3, z:  1.3 }, radius: WHEEL_R, width: 0.25,
              suspensionMinLength: WHEEL_MIN, suspensionMaxLength: WHEEL_MAX,
              steerable: true, maxSteerAngle: 45 },                              // 0 FL
            { position: { x:  0.8, y: -0.3, z:  1.3 }, radius: WHEEL_R, width: 0.25,
              suspensionMinLength: WHEEL_MIN, suspensionMaxLength: WHEEL_MAX,
              steerable: true, maxSteerAngle: 45 },                              // 1 FR
            { position: { x: -0.8, y: -0.3, z: -1.3 }, radius: WHEEL_R, width: 0.25,
              suspensionMinLength: WHEEL_MIN, suspensionMaxLength: WHEEL_MAX,
              driven: true, maxHandBrakeTorque: 4000 },                          // 2 RL
            { position: { x:  0.8, y: -0.3, z: -1.3 }, radius: WHEEL_R, width: 0.25,
              suspensionMinLength: WHEEL_MIN, suspensionMaxLength: WHEEL_MAX,
              driven: true, maxHandBrakeTorque: 4000 },                          // 3 RR
        ],
        antiRollBars: [
            { leftWheel: 0, rightWheel: 1 },
            { leftWheel: 2, rightWheel: 3 },
        ],
    };
}

const car = Physics.createVehicle(carOpts({ x: 0, y: 1.2, z: 0 }));
assert(typeof car === 'object', 'createVehicle returns a handle');
assert(typeof car.setInput === 'function', 'handle has setInput');
assert(typeof car.wheelState === 'function', 'handle has wheelState');
assert(car.wheelCount === 4, 'wheelCount is 4, got ' + car.wheelCount);
assert(typeof car.chassisBody === 'number' && car.chassisBody >= 0,
       'chassisBody is a body tag');

// =========================================================================
// Settle: wheels grounded, suspension within limits, car at rest
// =========================================================================
advanceTime(1500);
for (let i = 0; i < 4; i++) {
    const ws = car.wheelState(i);
    assert(ws !== null, 'wheelState(' + i + ') returns state');
    assert(ws.contact, 'wheel ' + i + ' in contact with the ground');
    assert(ws.contactBody === ground, 'wheel ' + i + ' contacts the ground body');
    assert(ws.contactNormal.y > 0.99, 'wheel ' + i + ' contact normal points up');
    assert(ws.suspensionLength >= WHEEL_MIN - 1e-3 &&
           ws.suspensionLength <= WHEEL_MAX + 1e-3,
           'wheel ' + i + ' suspension within [min,max], got ' + ws.suspensionLength);
    assert(Math.abs(ws.angularVelocity) < 0.5,
           'wheel ' + i + ' not spinning at rest, got ' + ws.angularVelocity);
}
assert(car.wheelState(4) === null, 'out-of-range wheel index returns null');
assert(Math.abs(car.speed) < 0.1, 'car at rest, speed ' + car.speed);
assert(car.gear === 0 || car.gear === 1, 'idle gear neutral/first, got ' + car.gear);

const chassis = car.chassisBody;
const restY = Physics.getTransform(chassis).position.y;
assert(restY > 0.4 && restY < 1.2, 'chassis riding on suspension, y=' + restY);

// =========================================================================
// Forward input accelerates along the facing (+Z)
// =========================================================================
const z0 = Physics.getTransform(chassis).position.z;
car.setInput({ forward: 1 });
advanceTime(2000);
let xf = Physics.getTransform(chassis).position;
assert(xf.z - z0 > 5, 'advanced along +Z under forward input, dz=' + (xf.z - z0));
assert(Math.abs(xf.x) < 2, 'no significant sideways drift, x=' + xf.x);
assert(car.speed > 3, 'forward speed built up, got ' + car.speed);
assert(car.rpm > 900, 'engine spinning, rpm=' + car.rpm);
assert(car.gear >= 1, 'in a forward gear, got ' + car.gear);
let st = car.getState();
assert(Math.abs(st.speed - car.speed) < 0.5, 'getState().speed matches .speed');
const drivenWs = car.wheelState(2);
assert(drivenWs.angularVelocity > 2, 'driven wheel spinning forward, ω=' +
       drivenWs.angularVelocity);

// =========================================================================
// Brake decelerates to near-stop
// =========================================================================
car.setInput({ brake: 1 });
advanceTime(3000);
assert(Math.abs(car.speed) < 0.3, 'braked to near-stop, speed ' + car.speed);

// =========================================================================
// Reverse (auto transmission: negative forward input)
// =========================================================================
const zRev0 = Physics.getTransform(chassis).position.z;
car.setInput({ forward: -1 });
advanceTime(1500);
assert(car.speed < -0.5, 'reversing, speed ' + car.speed);
assert(Physics.getTransform(chassis).position.z < zRev0 - 0.5, 'moved backwards');
assert(car.gear === -1, 'reverse gear, got ' + car.gear);
car.setInput({ forward: 0, brake: 1 });
advanceTime(2000);

// =========================================================================
// Steering + forward curves the path; heading change flips with steer sign
// =========================================================================
// Heading (yaw) of the chassis forward axis from its quaternion.
function chassisYaw() {
    const q = Physics.getTransform(chassis).rotation;
    // Rotate local +Z by q, take atan2(x, z) — yaw around +Y.
    const fx = 2 * (q.x * q.z + q.w * q.y);
    const fz = 1 - 2 * (q.x * q.x + q.y * q.y);
    return Math.atan2(fx, fz);
}

function resetCar() {
    Physics.setPosition(chassis, 0, restY + 0.1, 0);
    Physics.setRotation(chassis, 0, 0, 0, 1);
    Physics.setLinearVelocity(chassis, 0, 0, 0);
    Physics.setAngularVelocity(chassis, 0, 0, 0);
    car.setInput({ forward: 0, right: 0, brake: 0, handBrake: 0 });
    advanceTime(500);
}

function steeredYawDelta(rightInput) {
    resetCar();
    const yaw0 = chassisYaw();
    car.setInput({ forward: 1, right: rightInput });
    advanceTime(2500);
    return chassisYaw() - yaw0;
}

const yawRight = steeredYawDelta(1);
const yawLeft = steeredYawDelta(-1);
assert(Math.abs(yawRight) > 0.15, 'steering right turns the car, Δyaw=' + yawRight);
assert(Math.abs(yawLeft) > 0.15, 'steering left turns the car, Δyaw=' + yawLeft);
assert(yawRight * yawLeft < 0, 'heading change flips with steer sign: right=' +
       yawRight + ' left=' + yawLeft);
// Steered wheel reports a steer angle while turning.
resetCar();
car.setInput({ forward: 1, right: 1 });
advanceTime(400);
assert(Math.abs(car.wheelState(0).steerAngle) > 0.1,
       'front wheel steered, angle=' + car.wheelState(0).steerAngle);
assert(Math.abs(car.wheelState(2).steerAngle) < 1e-3,
       'rear wheel does not steer');

// =========================================================================
// Handbrake locks the rear wheels while the car is still sliding
// =========================================================================
resetCar();
car.setInput({ forward: 1 });
advanceTime(2500);
assert(car.speed > 5, 'up to speed before handbrake, got ' + car.speed);
car.setInput({ handBrake: 1 });
advanceTime(400);
const rearW = car.wheelState(2);
const frontW = car.wheelState(0);
assert(car.speed > 1, 'still sliding under handbrake, speed ' + car.speed);
assert(Math.abs(rearW.angularVelocity) < 0.5,
       'rear wheel locked by handbrake, ω=' + rearW.angularVelocity);
assert(Math.abs(frontW.angularVelocity) > 2,
       'front wheel still rolling, ω=' + frontW.angularVelocity);
advanceTime(3000);
assert(Math.abs(car.speed) < 0.3, 'handbrake eventually stops the car');

// =========================================================================
// Sandbox world: caller-driven stepping
// =========================================================================
const w = Physics.createWorldHandle({ maxBodies: 64 });
assert(typeof w.createVehicle === 'function', 'sandbox createVehicle exists');

w.createBody({
    shape: 'box', halfExtents: { x: 100, y: 0.5, z: 100 },
    position: { x: 0, y: -0.5, z: 0 }, static: true, friction: 1.0,
});
const wcar = w.createVehicle(carOpts({ x: 0, y: 1.2, z: 0 }));
assert(wcar.wheelCount === 4, 'sandbox vehicle has 4 wheels');
for (let i = 0; i < 90; i++) w.step(1 / 60);
assert(wcar.wheelState(0).contact, 'sandbox wheels grounded');

const wz0 = w.getTransform(wcar.chassisBody).position.z;
wcar.setInput({ forward: 1 });
for (let i = 0; i < 180; i++) w.step(1 / 60);
const wdz = w.getTransform(wcar.chassisBody).position.z - wz0;
assert(wdz > 3, 'sandbox vehicle drives forward, dz=' + wdz);
assert(wcar.speed > 2, 'sandbox vehicle speed, got ' + wcar.speed);

// Manual transmission on a sandbox car: gear stays where it's put.
const mcar = w.createVehicle(Object.assign(carOpts({ x: 20, y: 1.2, z: 0 }), {
    transmission: { mode: 'manual' },
}));
for (let i = 0; i < 60; i++) w.step(1 / 60);
mcar.setGear(1);
mcar.setInput({ forward: 1 });
for (let i = 0; i < 120; i++) w.step(1 / 60);
assert(mcar.gear === 1, 'manual gear held at 1, got ' + mcar.gear);
assert(mcar.speed > 1, 'manual car drives in 1st, speed ' + mcar.speed);

// =========================================================================
// Destroy mid-sim: handle destroy and chassis-body destroy both safe
// =========================================================================
wcar.setInput({ forward: 1 });
wcar.destroy();                      // vehicle gone, chassis body remains
for (let i = 0; i < 30; i++) w.step(1 / 60);   // must not crash
assert(wcar.wheelState(0) === null, 'wheelState null after destroy');
assert(wcar.speed === 0, 'speed 0 after destroy');
wcar.destroy();                      // double destroy is a no-op
assert(w.getTransform(wcar.chassisBody) !== undefined,
       'chassis body survives vehicle destroy');

// Destroying the chassis body removes the vehicle (constraint + step
// listener) without crashing the next steps.
w.destroyBody(mcar.chassisBody);
for (let i = 0; i < 30; i++) w.step(1 / 60);
w.destroy();

// Default-world vehicle destroyed mid-drive.
const car2 = Physics.createVehicle(carOpts({ x: 30, y: 1.2, z: 0 }));
car2.setInput({ forward: 1 });
advanceTime(500);
car2.destroy();
advanceTime(500);                    // must not crash

// =========================================================================
// Teardown/GC: leave live handles in globals, no explicit destroy. The
// Debug-build QuickJS leak assert is the real gate here (worldRef gc_mark +
// ~JsWorld back-pointer severing).
// =========================================================================
globalThis.__vehicleGcWorld = Physics.createWorldHandle({ maxBodies: 32 });
globalThis.__vehicleGcWorld.createBody({
    shape: 'box', halfExtents: { x: 10, y: 0.5, z: 10 },
    position: { x: 0, y: -0.5, z: 0 }, static: true,
});
globalThis.__vehicleGcVehicle =
    globalThis.__vehicleGcWorld.createVehicle(carOpts({ x: 0, y: 1.2, z: 0 }));
globalThis.__vehicleGcWorld.step(1 / 60);
globalThis.__vehicleGcDefault = car;   // default-world handle stays live too

// No Physics.destroyAll() here on purpose: teardown must clean up the live
// vehicle constraints/step listeners and JS handles in arbitrary GC order.
