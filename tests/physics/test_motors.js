// Test constraint motors + the SixDOF constraint — hinge velocity/position
// motors, slider velocity/position motors, motor force limits, sixdof
// per-axis free/limited/locked config, sixdof per-axis motors, and motors
// configured at create time. Exercises PhysicsWorld::setConstraintMotor and
// the SixDOF path in createConstraint (src/physics/physics_world.cpp +
// src/js/physics_bindings.cpp).

assert(typeof Physics === 'object', 'Physics namespace exists');
assert(typeof Physics.setConstraintMotor === 'function', 'Physics.setConstraintMotor exists');

// Sandbox world, zero gravity: every motor observation is isolated from
// gravity and stepped deterministically via w.step().
function makeWorld() {
    return Physics.createWorldHandle({ maxBodies: 256, gravity: { x: 0, y: 0, z: 0 } });
}

function stepN(w, n) {
    for (let i = 0; i < n; i++) w.step(1 / 60);
}

// Rotation angle about Z from a body's quaternion (valid for pure-Z rotations).
function angleZ(w, tag) {
    const q = w.getTransform(tag).rotation;
    return 2 * Math.atan2(q.z, q.w);
}

// =========================================================================
// Hinge velocity motor — spins the body up to the target angular velocity
// =========================================================================
{
    const w = makeWorld();
    const body = w.createBody({
        shape: 'box', halfExtents: { x: 0.5, y: 0.5, z: 0.5 },
        position: { x: 0, y: 0, z: 0 }, angularDamping: 0,
    });
    const hinge = w.createConstraint({
        type: 'hinge', body1: body, body2: -1,
        point1: { x: 0, y: 0, z: 0 }, point2: { x: 0, y: 0, z: 0 },
        axis: { x: 0, y: 0, z: 1 },
    });
    assert(hinge > 0, 'hinge handle');

    const ok = w.setConstraintMotor(hinge, { type: 'velocity', target: 5, maxTorque: 1000 });
    assert(ok === true, 'setConstraintMotor returns true for hinge');

    stepN(w, 60);
    const av = w.getVelocity(body).angular;
    assert(Math.abs(Math.abs(av.z) - 5) < 0.3,
        'hinge velocity motor reaches ~5 rad/s about Z, got ' + av.z);
    assert(Math.abs(av.x) < 0.05 && Math.abs(av.y) < 0.05,
        'hinge motor spins only about the hinge axis');

    // Turning the motor off lets the spin persist (no friction configured).
    w.setConstraintMotor(hinge, { type: 'off' });
    stepN(w, 10);
    const av2 = w.getVelocity(body).angular;
    assert(Math.abs(Math.abs(av2.z) - 5) < 0.5, 'spin persists after motor off');
    w.destroy();
}

// =========================================================================
// Hinge position motor — converges to the target angle
// =========================================================================
{
    const w = makeWorld();
    const body = w.createBody({
        shape: 'box', halfExtents: { x: 0.5, y: 0.5, z: 0.5 },
        position: { x: 0, y: 0, z: 0 },
    });
    const hinge = w.createConstraint({
        type: 'hinge', body1: body, body2: -1,
        point1: { x: 0, y: 0, z: 0 }, point2: { x: 0, y: 0, z: 0 },
        axis: { x: 0, y: 0, z: 1 },
    });
    w.setConstraintMotor(hinge, {
        type: 'position', target: 0.6, maxTorque: 5000,
        frequency: 5, damping: 1,
    });
    stepN(w, 180);
    const a = angleZ(w, body);
    assert(Math.abs(Math.abs(a) - 0.6) < 0.05,
        'hinge position motor converges to |0.6| rad, got ' + a);
    w.destroy();
}

// =========================================================================
// Motor configured at create (hinge)
// =========================================================================
{
    const w = makeWorld();
    const body = w.createBody({
        shape: 'box', halfExtents: { x: 0.5, y: 0.5, z: 0.5 },
        position: { x: 0, y: 0, z: 0 }, angularDamping: 0,
    });
    const hinge = w.createConstraint({
        type: 'hinge', body1: body, body2: -1,
        point1: { x: 0, y: 0, z: 0 }, point2: { x: 0, y: 0, z: 0 },
        axis: { x: 0, y: 0, z: 1 },
        motor: { type: 'velocity', target: 3, maxTorque: 1000 },
    });
    assert(hinge > 0, 'hinge with create-time motor');
    stepN(w, 60);
    const av = w.getVelocity(body).angular;
    assert(Math.abs(Math.abs(av.z) - 3) < 0.3,
        'create-time velocity motor active, got ' + av.z);
    w.destroy();
}

// =========================================================================
// Slider motors — velocity drives, position converges, limits respected
// =========================================================================
{
    const w = makeWorld();
    const body = w.createBody({
        shape: 'box', halfExtents: { x: 0.5, y: 0.5, z: 0.5 },
        position: { x: 0, y: 0, z: 0 }, linearDamping: 0,
    });
    const slider = w.createConstraint({
        type: 'slider', body1: body, body2: -1,
        axis: { x: 1, y: 0, z: 0 },
    });
    assert(slider > 0, 'slider handle');

    // Velocity motor.
    w.setConstraintMotor(slider, { type: 'velocity', target: 1.5, maxForce: 1e6 });
    stepN(w, 60);
    const lv = w.getVelocity(body).linear;
    assert(Math.abs(Math.abs(lv.x) - 1.5) < 0.1,
        'slider velocity motor reaches ~1.5 m/s, got ' + lv.x);
    assert(Math.abs(lv.y) < 0.01 && Math.abs(lv.z) < 0.01, 'slider motion is axis-only');

    // Position motor back to a fixed offset.
    w.setConstraintMotor(slider, {
        type: 'position', target: 2.0, maxForce: 1e6, frequency: 5, damping: 1,
    });
    stepN(w, 240);
    const px = w.getTransform(body).position.x;
    assert(Math.abs(Math.abs(px) - 2.0) < 0.05,
        'slider position motor converges to |2.0| m, got ' + px);
    w.destroy();
}

// =========================================================================
// Motor force limit caps acceleration
// =========================================================================
{
    const w = makeWorld();
    // Box: 1x1x1 m at density 1000 → 1000 kg.
    function makeSlider(x) {
        const b = w.createBody({
            shape: 'box', halfExtents: { x: 0.5, y: 0.5, z: 0.5 },
            position: { x: 0, y: 0, z: x }, linearDamping: 0,
        });
        const c = w.createConstraint({
            type: 'slider', body1: b, body2: -1, axis: { x: 1, y: 0, z: 0 },
        });
        return { b, c };
    }
    const weak = makeSlider(0);
    const strong = makeSlider(5);

    // Same 100 m/s target; weak motor limited to 1000 N (a = 1 m/s²).
    w.setConstraintMotor(weak.c,   { type: 'velocity', target: 100, maxForce: 1000 });
    w.setConstraintMotor(strong.c, { type: 'velocity', target: 100, maxForce: 1e8 });
    stepN(w, 60); // 1 second
    const vWeak = Math.abs(w.getVelocity(weak.b).linear.x);
    const vStrong = Math.abs(w.getVelocity(strong.b).linear.x);
    assert(Math.abs(vWeak - 1.0) < 0.1,
        'force-limited motor: |v| after 1s ≈ F/m = 1 m/s, got ' + vWeak);
    assert(vStrong > 10 * vWeak,
        'unlimited motor accelerates much faster (' + vStrong + ' vs ' + vWeak + ')');
    w.destroy();
}

// =========================================================================
// SixDOF — axis limits hold, free axes move, locked axes don't
// =========================================================================
{
    const w = makeWorld();
    const body = w.createBody({
        shape: 'box', halfExtents: { x: 0.5, y: 0.5, z: 0.5 },
        position: { x: 0, y: 0, z: 0 }, linearDamping: 0, angularDamping: 0,
    });
    const c = w.createConstraint({
        type: 'sixdof', body1: body, body2: -1,
        point1: { x: 0, y: 0, z: 0 }, point2: { x: 0, y: 0, z: 0 },
        axes: {
            translationX: { min: -1, max: 1 },  // limited
            translationY: 'free',
            // translationZ + all rotations default to locked
        },
    });
    assert(c > 0, 'sixdof handle');

    // Push hard along +X: must stop at the +1 limit.
    w.setLinearVelocity(body, 10, 0, 0);
    stepN(w, 60);
    let p = w.getTransform(body).position;
    assert(p.x <= 1.1 && p.x > 0.5, 'translationX clamps at +1 limit, got ' + p.x);
    assert(Math.abs(p.z) < 0.01, 'locked translationZ holds');

    // Push along -X: stops at the -1 limit.
    w.setLinearVelocity(body, -10, 0, 0);
    stepN(w, 60);
    p = w.getTransform(body).position;
    assert(p.x >= -1.1 && p.x < -0.5, 'translationX clamps at -1 limit, got ' + p.x);

    // Free axis: +Y motion passes through unhindered.
    w.setLinearVelocity(body, 0, 2, 0);
    stepN(w, 30);
    p = w.getTransform(body).position;
    assert(p.y > 0.8, 'free translationY moves, got ' + p.y);

    // Locked rotations: torque produces no spin.
    w.addTorque(body, 0, 0, 5000);
    stepN(w, 30);
    const av = w.getVelocity(body).angular;
    assert(Math.abs(av.z) < 0.05, 'locked rotationZ holds against torque');
    w.destroy();
}

// =========================================================================
// SixDOF — rotation limits
// =========================================================================
{
    const w = makeWorld();
    const body = w.createBody({
        shape: 'box', halfExtents: { x: 0.5, y: 0.5, z: 0.5 },
        position: { x: 0, y: 0, z: 0 }, angularDamping: 0,
    });
    w.createConstraint({
        type: 'sixdof', body1: body, body2: -1,
        point1: { x: 0, y: 0, z: 0 }, point2: { x: 0, y: 0, z: 0 },
        axes: {
            rotationX: { min: -0.4, max: 0.4 },   // twist limit
        },
    });
    w.setAngularVelocity(body, 6, 0, 0);
    stepN(w, 60);
    const q = w.getTransform(body).rotation;
    const twist = 2 * Math.atan2(q.x, q.w);
    assert(Math.abs(twist) <= 0.45, 'rotationX twist limited to ±0.4, got ' + twist);
    w.destroy();
}

// =========================================================================
// SixDOF — per-axis motors (runtime + create-time)
// =========================================================================
{
    const w = makeWorld();
    const body = w.createBody({
        shape: 'box', halfExtents: { x: 0.5, y: 0.5, z: 0.5 },
        position: { x: 0, y: 0, z: 0 }, linearDamping: 0, angularDamping: 0,
    });
    const c = w.createConstraint({
        type: 'sixdof', body1: body, body2: -1,
        point1: { x: 0, y: 0, z: 0 }, point2: { x: 0, y: 0, z: 0 },
        axes: {
            translationX: { min: -3, max: 3 },
            rotationY: 'free',
        },
        motors: {
            rotationY: { type: 'velocity', target: 3, maxTorque: 5000 },
        },
    });

    // Create-time rotationY velocity motor.
    stepN(w, 60);
    const av = w.getVelocity(body).angular;
    assert(Math.abs(Math.abs(av.y) - 3) < 0.3,
        'sixdof create-time rotationY motor reaches ~3 rad/s, got ' + av.y);

    // Runtime per-axis position motor on translationX — separate body so the
    // spin above doesn't rotate the constraint frame (constraint space is
    // attached to body1).
    const body2 = w.createBody({
        shape: 'box', halfExtents: { x: 0.5, y: 0.5, z: 0.5 },
        position: { x: 0, y: 0, z: 5 }, linearDamping: 0,
    });
    const c2 = w.createConstraint({
        type: 'sixdof', body1: body2, body2: -1,
        point1: { x: 0, y: 0, z: 5 }, point2: { x: 0, y: 0, z: 5 },
        axes: { translationX: { min: -3, max: 3 } },
    });
    const ok = w.setConstraintMotor(c2, {
        axis: 'translationX', type: 'position', target: 1.5,
        maxForce: 1e6, frequency: 5, damping: 1,
    });
    assert(ok === true, 'sixdof per-axis setConstraintMotor ok');
    stepN(w, 240);
    const px = w.getTransform(body2).position.x;
    assert(Math.abs(Math.abs(px) - 1.5) < 0.1,
        'sixdof translationX position motor converges to |1.5|, got ' + px);

    // Bad axis is rejected.
    assert(w.setConstraintMotor(c, { type: 'velocity', target: 1 }) === false,
        'sixdof motor without axis returns false');
    w.destroy();
}

// =========================================================================
// Motors are rejected on non-motorized constraint types
// =========================================================================
{
    const w = makeWorld();
    const a = w.createBody({ shape: 'sphere', radius: 0.5, position: { x: 0, y: 0, z: 0 } });
    const b = w.createBody({ shape: 'sphere', radius: 0.5, position: { x: 2, y: 0, z: 0 } });
    const dist = w.createConstraint({
        type: 'distance', body1: a, body2: b,
        point1: { x: 0, y: 0, z: 0 }, point2: { x: 2, y: 0, z: 0 },
    });
    assert(w.setConstraintMotor(dist, { type: 'velocity', target: 1 }) === false,
        'distance constraint has no motor');
    assert(w.setConstraintMotor(9999, { type: 'velocity', target: 1 }) === false,
        'unknown handle returns false');
    w.destroy();
}

console.log('test_motors: OK');
