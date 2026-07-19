// Sandbox world handles expose the same body/constraint API as Physics.*.
//
// Regression: the world-instance method table was filled in by hand and a
// handful of default-world functions were never added to it. They were not
// no-ops on a sandbox world — they were absent, so calling one was a
// TypeError on undefined, and the documented workaround was "put it in the
// default world instead". Every one is a plain PhysicsWorld method that
// applies to any world.
//
// Each method is exercised for effect, not just existence: a present method
// that ignores its world would pass a typeof check.

function makeWorld(gravity) {
    return Physics.createWorldHandle({
        maxBodies: 256,
        gravity: gravity || { x: 0, y: 0, z: 0 },
    });
}

// --- the surfaces that were missing entirely --------------------------------
{
    const w = makeWorld();
    for (const name of ['setConstraintEnabled', 'setWheelMotor', 'setRotation',
                        'isActive', 'getGravity', 'setTimeStep', 'getTimeStep',
                        'setInterpolation', 'getInterpolation']) {
        assert(typeof w[name] === 'function',
               'world handle exposes ' + name + ', got ' + typeof w[name]);
    }
    w.destroy();
}

// --- getGravity reports THIS world, not the default one ---------------------
{
    const w = makeWorld({ x: 0, y: -3.5, z: 0 });
    const g = w.getGravity();
    assert(Math.abs(g.y + 3.5) < 1e-4,
           'sandbox getGravity reports its own gravity, got ' + g.y);

    w.setGravity(0, -1.25, 0);
    assert(Math.abs(w.getGravity().y + 1.25) < 1e-4,
           'getGravity reflects a later setGravity, got ' + w.getGravity().y);

    // The default world must be untouched by any of it.
    const dg = Physics.getGravity();
    assert(Math.abs(dg.y + 1.25) > 1e-3,
           'sandbox gravity did not leak into the default world, got ' + dg.y);
    w.destroy();
}

// --- setRotation actually rotates a sandbox body ----------------------------
{
    const w = makeWorld();
    // Zero-gravity world, so the body stays put and the only thing that can
    // change its rotation is the setRotation call.
    const tag = w.createBody({ shape: 'box', halfExtents: { x: 1, y: 1, z: 1 },
                               position: { x: 0, y: 0, z: 0 } });
    // 90° about Z.
    const h = Math.SQRT1_2;
    w.setRotation(tag, 0, 0, h, h);
    const q = w.getTransform(tag).rotation;
    assert(Math.abs(Math.abs(q.z) - h) < 1e-3 && Math.abs(Math.abs(q.w) - h) < 1e-3,
           'setRotation applied to the sandbox body, got z=' + q.z + ' w=' + q.w);
    w.destroy();
}

// --- isActive tracks a sandbox body falling asleep --------------------------
{
    const w = makeWorld({ x: 0, y: -9.81, z: 0 });
    w.createBody({ shape: 'box', halfExtents: { x: 20, y: 0.5, z: 20 },
                   position: { x: 0, y: -0.5, z: 0 }, static: true });
    const ball = w.createBody({ shape: 'sphere', radius: 0.5,
                                position: { x: 0, y: 2, z: 0 } });

    assert(w.isActive(ball) === true, 'a freshly dropped body is active');
    for (let i = 0; i < 600; i++) w.step(1 / 60);
    assert(w.isActive(ball) === false,
           'the body slept after settling on the floor');
    w.destroy();
}

// --- timeStep: a bare step() uses the world's configured step ---------------
{
    const w = makeWorld({ x: 0, y: -10, z: 0 });
    const ball = w.createBody({ shape: 'sphere', radius: 0.5,
                                position: { x: 0, y: 100, z: 0 } });

    w.setTimeStep(1 / 30);
    assert(Math.abs(w.getTimeStep() - 1 / 30) < 1e-6,
           'getTimeStep reflects setTimeStep, got ' + w.getTimeStep());

    // 30 bare steps at 1/30 == 1 second of simulated fall.
    for (let i = 0; i < 30; i++) w.step();
    const vy = w.getVelocity(ball).linear.y;
    assert(Math.abs(vy + 10) < 1.5,
           'a bare step() advanced by the configured timestep (v≈-10 after 1s), got ' + vy);
    w.destroy();
}

// --- interpolation is per-world state ---------------------------------------
{
    const a = makeWorld();
    const b = makeWorld();
    a.setInterpolation(true);
    b.setInterpolation(false);
    assert(a.getInterpolation() === true, 'world a kept interpolation on');
    assert(b.getInterpolation() === false, 'world b kept interpolation off');
    a.destroy();
    b.destroy();
}

// --- setConstraintEnabled toggles a sandbox constraint ----------------------
{
    const w = makeWorld({ x: 0, y: -9.81, z: 0 });
    const anchor = w.createBody({ shape: 'box', halfExtents: { x: 0.5, y: 0.5, z: 0.5 },
                                  position: { x: 0, y: 10, z: 0 }, static: true });
    const hung = w.createBody({ shape: 'sphere', radius: 0.5,
                                position: { x: 0, y: 8, z: 0 } });
    const c = w.createConstraint({
        type: 'distance', body1: anchor, body2: hung,
        point1: { x: 0, y: 10, z: 0 }, point2: { x: 0, y: 8, z: 0 },
        minDistance: 1.9, maxDistance: 2.1,
    });
    assert(c > 0, 'sandbox distance constraint created, got ' + c);

    for (let i = 0; i < 120; i++) w.step(1 / 60);
    const heldY = w.getTransform(hung).position.y;
    assert(heldY > 7.0, 'the constraint held the body up, y=' + heldY);

    // Disabling it must let the body fall.
    w.setConstraintEnabled(c, false);
    w.activate(hung);
    for (let i = 0; i < 120; i++) w.step(1 / 60);
    const droppedY = w.getTransform(hung).position.y;
    assert(droppedY < heldY - 1.0,
           'disabling the constraint dropped the body, ' + heldY + ' -> ' + droppedY);
    w.destroy();
}

// --- setWheelMotor drives a sandbox wheel constraint ------------------------
{
    const w = makeWorld();
    const chassis = w.createBody({ shape: 'box', halfExtents: { x: 0.5, y: 0.2, z: 0.5 },
                                   position: { x: 0, y: 2, z: 0 }, static: true });
    const wheelBody = w.createBody({ shape: 'sphere', radius: 0.35,
                                     position: { x: 0, y: 1.4, z: 0 } });
    const wheel = w.createConstraint({
        type: 'wheel', body1: chassis, body2: wheelBody,
        point1: { x: 0, y: 1.4, z: 0 },
        suspensionAxis: { x: 0, y: 1, z: 0 },
        hingeAxis: { x: 0, y: 0, z: 1 },
        hertz: 2.0, dampingRatio: 0.7,
    });
    assert(wheel > 0, 'sandbox wheel constraint created, got ' + wheel);

    // Motor off: the wheel should not spin up on its own.
    for (let i = 0; i < 60; i++) w.step(1 / 60);
    const idle = Math.abs(w.getVelocity(wheelBody).angular.z);
    assert(idle < 1.0, 'wheel is not spinning before the motor is on, got ' + idle);

    w.setWheelMotor(wheel, true, -10.0, 200);
    w.activate(wheelBody);
    for (let i = 0; i < 120; i++) w.step(1 / 60);
    const driven = w.getVelocity(wheelBody).angular.z;
    assert(Math.abs(driven) > 2.0,
           'the wheel motor spun the sandbox wheel, wz=' + driven);
    w.destroy();
}

console.log('PASS physics sandbox parity');
