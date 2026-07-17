// Per-wheel tire friction (Jolt WheelSettingsWV longitudinal/lateral
// LinearCurves). Wheels take `longitudinalFriction` / `lateralFriction`
// scalar multipliers on Jolt's default curves, or full curve overrides
// (`longitudinalFrictionCurve` / `lateralFrictionCurve`, flat [x,y,...]).
//
// Two identical cars coast in from the same speed with full brakes; the
// low-friction (icy) one must slide measurably farther than the grippy one.

Physics.destroyAll();
Physics.setGravity(0, -9.81, 0);
Physics.setTimeStep(1 / 60);

// Big flat ground.
Physics.createBody({
    shape: 'box', halfExtents: { x: 200, y: 0.5, z: 200 },
    position: { x: 0, y: -0.5, z: 0 }, static: true,
    friction: 1.0,
});

function makeCar(x, frictionScale) {
    const wheel = (wx, wz, drivenWheel) => ({
        position: { x: wx, y: -0.3, z: wz },
        radius: 0.35, width: 0.25,
        suspensionMinLength: 0.2, suspensionMaxLength: 0.5,
        longitudinalFriction: frictionScale,
        lateralFriction: frictionScale,
        maxBrakeTorque: 3000,
        driven: drivenWheel,
    });
    return Physics.createVehicle({
        chassis: {
            shape: 'box', halfExtents: { x: 0.9, y: 0.4, z: 2.0 },
            position: { x: x, y: 1.0, z: 0 }, density: 260,
        },
        wheels: [
            wheel(-0.8,  1.3, false), wheel(0.8,  1.3, false),
            wheel(-0.8, -1.3, true),  wheel(0.8, -1.3, true),
        ],
    });
}

const icy = makeCar(-20, 0.1);     // 10% of default tire friction
const grippy = makeCar(20, 1.5);   // 150%

assert(icy && grippy, 'vehicles created');
assert(icy.wheelCount === 4 && grippy.wheelCount === 4, 'four wheels each');

// Settle onto the suspension.
advanceTime(1000);

// Same initial forward speed (+z), full brakes from the start.
const z0icy = Physics.getTransform(icy.chassisBody).position.z;
const z0grip = Physics.getTransform(grippy.chassisBody).position.z;
Physics.setLinearVelocity(icy.chassisBody, 0, 0, 15);
Physics.setLinearVelocity(grippy.chassisBody, 0, 0, 15);
icy.setInput({ brake: 1 });
grippy.setInput({ brake: 1 });

advanceTime(6000);

const icyDist = Physics.getTransform(icy.chassisBody).position.z - z0icy;
const gripDist = Physics.getTransform(grippy.chassisBody).position.z - z0grip;
const icySpeed = Math.abs(icy.speed);
const gripSpeed = Math.abs(grippy.speed);

assert(gripSpeed < 0.5, 'grippy car stopped, speed=' + gripSpeed);
assert(gripDist > 1, 'grippy car moved before stopping, dist=' + gripDist);
assert(icyDist > gripDist * 1.4,
       'icy car slides measurably farther under braking (' +
       icyDist.toFixed(2) + ' vs ' + gripDist.toFixed(2) + ')');

// Curve override path: a flat near-zero longitudinal curve behaves like ice
// even with the scalar left at default.
const curveIcy = Physics.createVehicle({
    chassis: {
        shape: 'box', halfExtents: { x: 0.9, y: 0.4, z: 2.0 },
        position: { x: 0, y: 1.0, z: -60 }, density: 260,
    },
    wheels: [
        { position: { x: -0.8, y: -0.3, z:  1.3 }, radius: 0.35, width: 0.25,
          longitudinalFrictionCurve: [0, 0, 1, 0.05], maxBrakeTorque: 3000 },
        { position: { x:  0.8, y: -0.3, z:  1.3 }, radius: 0.35, width: 0.25,
          longitudinalFrictionCurve: [0, 0, 1, 0.05], maxBrakeTorque: 3000 },
        { position: { x: -0.8, y: -0.3, z: -1.3 }, radius: 0.35, width: 0.25,
          longitudinalFrictionCurve: [0, 0, 1, 0.05], maxBrakeTorque: 3000,
          driven: true },
        { position: { x:  0.8, y: -0.3, z: -1.3 }, radius: 0.35, width: 0.25,
          longitudinalFrictionCurve: [0, 0, 1, 0.05], maxBrakeTorque: 3000,
          driven: true },
    ],
});
advanceTime(1000);
const z0curve = Physics.getTransform(curveIcy.chassisBody).position.z;
Physics.setLinearVelocity(curveIcy.chassisBody, 0, 0, 15);
curveIcy.setInput({ brake: 1 });
advanceTime(6000);
const curveDist = Physics.getTransform(curveIcy.chassisBody).position.z - z0curve;
assert(curveDist > gripDist * 1.4,
       'curve-override ice slides farther than grippy tires (' +
       curveDist.toFixed(2) + ' vs ' + gripDist.toFixed(2) + ')');

icy.destroy();
grippy.destroy();
curveIcy.destroy();
Physics.destroyAll();
