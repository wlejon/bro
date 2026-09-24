// World-anchored constraints (body2: -1) measure body1 against the world, not
// the world against body1: asymmetric slider limits, hinge motor direction,
// and a motor-driven rack-and-pinion whose drift correction reads those
// hinge / slider values.

assert(typeof Physics === 'object', 'Physics namespace exists');

function world(g) {
    return Physics.createWorldHandle({ maxBodies: 256, gravity: g || { x: 0, y: 0, z: 0 } });
}
function stepN(w, n) { for (let i = 0; i < n; i++) w.step(1 / 60); }
function pos(w, tag) { return w.getTransform(tag).position; }

// -- Slider: asymmetric limits apply to body1's own travel ------------------
{
    // Gravity pushes up; limitMax 3.2 must stop the crate 3.2 above its start.
    const w = world({ x: 0, y: 9.81, z: 0 });
    const crate = w.createBody({ shape: 'box', halfExtents: { x: 0.5, y: 0.5, z: 0.5 },
        position: { x: 0, y: 0, z: 0 } });
    const c = w.createConstraint({ type: 'slider', body1: crate, axis: { x: 0, y: 1, z: 0 },
        limitMin: -0.9, limitMax: 3.2 });
    assert(c > 0, 'slider created');
    stepN(w, 180);
    const y = pos(w, crate).y;
    assert(Math.abs(y - 3.2) < 0.05, 'slider stops at limitMax 3.2 (y=' + y.toFixed(3) + ')');
    w.destroy();
}
{
    const w = world({ x: 0, y: -9.81, z: 0 });
    const crate = w.createBody({ shape: 'box', halfExtents: { x: 0.5, y: 0.5, z: 0.5 },
        position: { x: 0, y: 0, z: 0 } });
    w.createConstraint({ type: 'slider', body1: crate, body2: -1, axis: { x: 0, y: 1, z: 0 },
        limitMin: -0.9, limitMax: 3.2 });
    stepN(w, 180);
    const y = pos(w, crate).y;
    assert(Math.abs(y + 0.9) < 0.05, 'slider stops at limitMin -0.9 (y=' + y.toFixed(3) + ')');
    w.destroy();
}

// -- Slider position motor: target is body1's travel along +axis -------------
{
    const w = world();
    const b = w.createBody({ shape: 'box', position: { x: 0, y: 0, z: 0 } });
    w.createConstraint({ type: 'slider', body1: b, body2: -1, axis: { x: 1, y: 0, z: 0 },
        motor: { type: 'position', target: 2, maxForce: 1e6, frequency: 5, damping: 1 } });
    stepN(w, 240);
    const x = pos(w, b).x;
    assert(Math.abs(x - 2) < 0.1, 'slider position motor drives body to +2 (x=' + x.toFixed(3) + ')');
    w.destroy();
}

// -- Hinge velocity motor: positive target spins body1 +axis (right hand) ----
{
    const w = world();
    const b = w.createBody({ shape: 'box', position: { x: 0, y: 0, z: 0 }, angularDamping: 0 });
    const h = w.createConstraint({ type: 'hinge', body1: b, body2: -1,
        point1: { x: 0, y: 0, z: 0 }, point2: { x: 0, y: 0, z: 0 }, axis: { x: 0, y: 0, z: 1 },
        motor: { type: 'velocity', target: 3, maxTorque: 1000 } });
    assert(h > 0, 'hinge created');
    stepN(w, 60);
    const av = w.getVelocity(b).angular.z;
    assert(Math.abs(av - 3) < 0.2, 'hinge motor spins +3 rad/s about +Z (got ' + av.toFixed(3) + ')');
    w.destroy();
}

// -- Rack and pinion: a motor-driven pinion moves the rack smoothly ---------
{
    const w = world();
    const pinion = w.createBody({ shape: 'cylinder', radius: 0.5, halfHeight: 0.2,
        position: { x: 0, y: 1, z: 0 }, angularDamping: 0,
        rotation: { x: Math.SQRT1_2, y: 0, z: 0, w: Math.SQRT1_2 } });
    const rack = w.createBody({ shape: 'box', halfExtents: { x: 3, y: 0.2, z: 0.2 },
        position: { x: 0, y: -1, z: 0 }, linearDamping: 0 });
    const hinge = w.createConstraint({ type: 'hinge', body1: pinion, body2: -1,
        point1: { x: 0, y: 1, z: 0 }, point2: { x: 0, y: 1, z: 0 }, axis: { x: 0, y: 0, z: 1 },
        motor: { type: 'velocity', target: 1, maxTorque: 1e4 } });
    const slider = w.createConstraint({ type: 'slider', body1: rack, body2: -1,
        axis: { x: 1, y: 0, z: 0 } });
    const rp = w.createConstraint({ type: 'rackAndPinion', body1: pinion, body2: rack,
        hingeAxis1: { x: 0, y: 0, z: 1 }, sliderAxis: { x: 1, y: 0, z: 0 },
        ratio: 2, constraint1: hinge, constraint2: slider });
    assert(rp > 0, 'rack and pinion created');
    stepN(w, 30);
    let prev = pos(w, rack).x, maxJump = 0, minStep = Infinity, maxStep = -Infinity;
    for (let i = 0; i < 60; i++) {
        w.step(1 / 60);
        const x = pos(w, rack).x;
        const d = x - prev;
        minStep = Math.min(minStep, d); maxStep = Math.max(maxStep, d);
        maxJump = Math.max(maxJump, Math.abs(d));
        prev = x;
    }
    assert(maxJump > 0.001, 'rack moves');
    assert(maxStep - minStep < 0.002,
        'rack moves at a steady rate (per-step range ' + minStep.toFixed(4) + '..' + maxStep.toFixed(4) + ')');
    w.destroy();
}

console.log('test_world_anchored: OK');
