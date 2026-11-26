// bro.dynamics mechanical joints & compound bodies:
//   - addCompound: multi-box rigid cluster with combined mass and inertia
//   - hinge: limits, motor, and clamped implicit spring servo (torque cap under overload)
//   - slider (prismatic): limits, spring servo, and force cap under load
//   - wheel: suspension spring/limits and motorized rolling wheel
//   - distance: cable/winch/spring tension & limits
//   - filter: disables collision between linked bodies without changing layers
//   - applyForce / applyTorque / jointState

function near(a, b, eps, msg) {
    assert(Math.abs(a - b) <= eps, msg + ' (got ' + a + ', want ' + b + ')');
}

// --- addCompound + applyTorque --------------------------------------------------
{
    const w = bro.dynamics.createWorld({ gravity: [0, 0, 0] });
    const c = w.addCompound({
        position: [0, 2, 0],
        parts: [
            { center: [-1, 0, 0], half: [0.5, 0.25, 0.25], mass: 12 },
            { center: [1, 0, 0], half: [0.5, 0.25, 0.25], mass: 18 },
        ],
    });
    near(w.mass(c), 30, 1e-2, 'compound mass sums part masses');
    w.applyTorque(c, [0, 300, 0]);
    w.step(1 / 60, 4);
    const vel = w.velocity(c);
    assert(vel[4] > 0.05, 'applyTorque spins compound body about Y (wy=' + vel[4] + ')');
    w.destroy();
    console.log('PASS compound + applyTorque');
}

// --- hinge clamped servo + limits ----------------------------------------------
{
    const w = bro.dynamics.createWorld();
    const base = w.addBox({ position: [0, 5, 0], half: [0.5, 0.5, 0.5], mass: 0 });
    const arm = w.addBox({ position: [1, 5, 0], half: [1, 0.1, 0.1], mass: 20 });
    // Torque needed to hold 20 kg at 1 m horizontally against g=9.81 is ~196.2 N*m.
    // A servo capped at 400 N*m holds near target 0.3 rad; capped at 40 N*m yields/droops!
    const hStrong = w.addJoint({
        kind: 'hinge', a: base, b: arm, anchor: [0, 5, 0], axis: [0, 0, 1],
        lower: -1.2, upper: 1.2, target: 0.3, hertz: 12, damping: 1.0, maxSpring: 600,
    });
    for (let i = 0; i < 60; i++) w.step(1 / 60, 4);
    const st1 = w.jointState(hStrong);
    near(st1.position, 0.3, 0.06, 'clamped hinge servo reaches target when within torque budget');

    // Now drop torque limit below static gravity torque (196 N*m) -> arm must yield downward.
    w.setJointTarget(hStrong, 0.3, 12, 1.0, 40, true);
    for (let i = 0; i < 60; i++) w.step(1 / 60, 4);
    const st2 = w.jointState(hStrong);
    assert(st2.position < -0.5, 'overloaded clamped hinge servo yields under gravity (angle=' + st2.position.toFixed(3) + ')');
    assert(st2.position >= -1.28, 'and respects lower angle limit');
    w.destroy();
    console.log('PASS hinge clamped servo + limits');
}

// --- slider (prismatic) clamped servo -------------------------------------------
{
    const w = bro.dynamics.createWorld();
    const base = w.addBox({ position: [0, 0, 0], half: [1, 0.5, 1], mass: 0 });
    const sled = w.addBox({ position: [0, 2, 0], half: [0.4, 0.4, 0.4], mass: 100 });
    // Vertical slider along +Y: weight is 981 N. Servo with maxSpring 3000 N lifts to +0.8 m.
    const s = w.addJoint({
        kind: 'slider', a: base, b: sled, anchor: [0, 2, 0], axis: [0, 1, 0],
        lower: -0.5, upper: 1.5, target: 0.8, hertz: 10, damping: 1.0, maxSpring: 3000,
    });
    for (let i = 0; i < 90; i++) w.step(1 / 60, 4);
    near(w.jointState(s).position, 0.8, 0.06, 'clamped slider lifts payload to target');

    // Clamp maxSpring below weight (981 N) -> payload drops to lower limit -0.5 m.
    w.setJointTarget(s, 0.8, 10, 1.0, 200, true);
    for (let i = 0; i < 90; i++) w.step(1 / 60, 4);
    assert(w.jointState(s).position < -0.35, 'underpowered clamped slider settles to lower stop');
    w.destroy();
    console.log('PASS slider clamped servo');
}

// --- wheel suspension + motorized drive -----------------------------------------
{
    const w = bro.dynamics.createWorld();
    w.addBox({ position: [0, -0.5, 0], half: [100, 0.5, 100], mass: 0, friction: 1.0 });
    const chassis = w.addBox({ position: [0, 1.2, 0], half: [1.5, 0.3, 1.0], mass: 400 });
    const wheels = [];
    for (const [wx, wz] of [[-1.1, -1.2], [1.1, -1.2], [-1.1, 1.2], [1.1, 1.2]]) {
        const wheel = w.addSphere({ position: [wx, 0.6, wz], radius: 0.55, mass: 30, friction: 1.2 });
        w.addJoint({ kind: 'filter', a: chassis, b: wheel });
        const j = w.addJoint({
            kind: 'wheel', a: chassis, b: wheel,
            anchor: [wx, 0.6, wz], axis: [0, 1, 0], axisB: [0, 0, 1],
            lower: -0.35, upper: 0.25, hertz: 8, damping: 0.9,
            motorSpeed: -10, maxMotor: 1500,
        });
        wheels.push(j);
    }
    for (let i = 0; i < 120; i++) w.step(1 / 60, 4);
    const p = w.pose(chassis);
    assert(p[0] > 3.0 && p[1] > 0.5 && p[1] < 2.0, 'wheeled vehicle drives cleanly forward (x=' + p[0].toFixed(2) + ', y=' + p[1].toFixed(2) + ')');
    w.destroy();
    console.log('PASS wheel + filter');
}

// --- distance cable / winch -----------------------------------------------------
{
    const w = bro.dynamics.createWorld();
    const ceiling = w.addBox({ position: [0, 10, 0], half: [0.5, 0.5, 0.5], mass: 0 });
    const load = w.addBox({ position: [0, 6, 0], half: [0.4, 0.4, 0.4], mass: 20 });
    const cable = w.addJoint({
        kind: 'distance', a: ceiling, b: load,
        anchor: [0, 10, 0], anchorB: [0, 6, 0],
        lower: 1.0, upper: 4.5, target: 4.0, hertz: 8, damping: 0.9,
    });
    for (let i = 0; i < 60; i++) w.step(1 / 60, 4);
    assert(w.pose(load)[1] > 5.0, 'distance spring holds suspended mass');
    w.setJointTarget(cable, 2.0, 8, 0.9, 2000, true);
    for (let i = 0; i < 90; i++) w.step(1 / 60, 4);
    near(w.jointState(cable).position, 2.0, 0.3, 'distance target winches load up');
    w.destroy();
    console.log('PASS distance cable');
}
