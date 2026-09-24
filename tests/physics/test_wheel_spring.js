// The wheel constraint's suspension spring (hertz / dampingRatio) holds the
// chassis at its rest height with no translation limits given, and hertz 0
// leaves the suspension free.

assert(typeof Physics === 'object', 'Physics namespace exists');

function stepN(w, n) { for (let i = 0; i < n; i++) w.step(1 / 60); }
function pos(w, tag) { return w.getTransform(tag).position; }

// Chassis and wheel may only move vertically (the wheel may also spin), so the
// suspension is all that is measured.
function rig(extra) {
    const w = Physics.createWorldHandle({ maxBodies: 64, gravity: { x: 0, y: -9.81, z: 0 } });
    w.createBody({ shape: 'box', halfExtents: { x: 20, y: 0.5, z: 20 },
        position: { x: 0, y: -0.5, z: 0 }, static: true });
    const chassis = w.createBody({ shape: 'box', halfExtents: { x: 1, y: 0.3, z: 0.5 },
        position: { x: 0, y: 5, z: 0 }, mass: 20, dofs: 'ty' });
    const wheel = w.createBody({ shape: 'sphere', radius: 0.5, mass: 5,
        position: { x: 0, y: 4, z: 0 }, dofs: 'ty,rz' });
    const c = w.createConstraint(Object.assign({ type: 'wheel', body1: chassis, body2: wheel,
        point1: { x: 0, y: 4, z: 0 } }, extra));
    assert(c > 0, 'wheel created');
    stepN(w, 240);
    const r = { gap: pos(w, chassis).y - pos(w, wheel).y, wheelY: pos(w, wheel).y };
    w.destroy();
    return r;
}

// As in Jolt and Box2D, the frequency is of the joint's effective mass
// (1 / (1/20 + 1/5) = 4 kg), so the stiffness is k = 4 (2 pi hertz)^2 and the
// 20 kg chassis sags 20 g / k below the rest gap of 1.
function sag(hertz) { return 20 * 9.81 / (4 * Math.pow(2 * Math.PI * hertz, 2)); }
{
    const r = rig({ hertz: 4, dampingRatio: 0.9 });
    assert(Math.abs(r.gap - (1 - sag(4))) < 0.02,
        'a 4 Hz spring holds the chassis at 1 - ' + sag(4).toFixed(3) + ' above the wheel (gap ' + r.gap.toFixed(3) + ')');
    assert(Math.abs(r.wheelY - 0.5) < 0.05, 'the wheel sits on the floor (y ' + r.wheelY.toFixed(3) + ')');
}
{
    const soft = rig({ hertz: 2, dampingRatio: 1 });
    assert(Math.abs(soft.gap - (1 - sag(2))) < 0.03,
        'a 2 Hz spring sags ' + sag(2).toFixed(3) + ' (gap ' + soft.gap.toFixed(3) + ')');
}
{
    const free = rig({ hertz: 0 });
    assert(free.gap < 0.5, 'hertz 0 leaves the suspension free (gap ' + free.gap.toFixed(3) + ')');
}

console.log('test_wheel_spring: OK');
