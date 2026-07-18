// Contact manifold data + friction/restitution combine modes.
//
// getContacts() "added" events now carry a manifold snapshot: up to 4
// world-space contact points (on body2's surface), the contact normal
// (direction body2 moves out of collision — from body1 toward body2), and
// the penetration depth (negative = speculative contact).
//
// Combine modes: per-body frictionCombine / restitutionCombine override
// Jolt's built-in combine functions — friction sqrt(f1*f2) (geometric mean),
// restitution max(r1, r2). When the two bodies of a pair disagree, the
// higher mode wins (average < min < multiply < max).

Physics.destroyAll();
Physics.setGravity(0, -9.81, 0);
Physics.setTimeStep(1 / 60);

// ===========================================================================
// Manifold data: a box dropped flat onto the ground.
// ===========================================================================
const ground = Physics.createBody({
    shape: 'box', halfExtents: { x: 20, y: 0.5, z: 20 },
    position: { x: 0, y: -0.5, z: 0 }, static: true,
});
const box = Physics.createBody({
    shape: 'box', halfExtents: { x: 0.5, y: 0.5, z: 0.5 },
    position: { x: 0, y: 1.2, z: 0 },
});

let hit = null;
for (let i = 0; i < 200 && hit === null; i++) {
    advanceTime(16);
    for (const e of Physics.getContacts()) {
        const pair = (e.body1 === box && e.body2 === ground) ||
                     (e.body1 === ground && e.body2 === box);
        if (pair && e.type === 'added') { hit = e; break; }
    }
}
assert(hit !== null, 'box-vs-ground added event arrived');
assert(Array.isArray(hit.points), 'added event has points array');
assert(hit.points.length >= 1 && hit.points.length <= 4,
       '1..4 contact points, got ' + hit.points.length);
assert(typeof hit.penetration === 'number', 'added event has penetration depth');
assert(typeof hit.normal === 'object', 'added event has a normal');

// The contact is on the ground plane (y = 0): every point lies near it and
// inside the box footprint.
for (const p of hit.points) {
    assert(Math.abs(p.y) < 0.15, 'contact point on the ground plane, y=' + p.y);
    assert(Math.abs(p.x) < 0.7 && Math.abs(p.z) < 0.7,
           'contact point under the box footprint (' + p.x + ',' + p.z + ')');
}
// Vertical contact: the normal is (anti)parallel to +Y. Its sign encodes the
// body1→body2 direction, so pin it with the pair order: with body1 = box
// (above) and body2 = ground (below), body2 moves out of collision downward.
assert(Math.abs(hit.normal.y) > 0.99,
       'contact normal is vertical, got ' + JSON.stringify(hit.normal));
const expectDown = hit.body1 === box;
assert(expectDown ? hit.normal.y < 0 : hit.normal.y > 0,
       'normal points from body1 toward body2');

// Removed events carry no manifold fields.
Physics.destroyAll();

// ===========================================================================
// Restitution combine: min vs max produce very different bounce heights.
// Ground restitution 1, balls restitution 0. Jolt default = max(r1,r2) = 1.
//   'min' ball  → combined 0 → dead drop
//   'max' ball  → combined 1 → lively bounce
// ===========================================================================
Physics.createBody({
    shape: 'box', halfExtents: { x: 40, y: 0.5, z: 40 },
    position: { x: 0, y: -0.5, z: 0 }, static: true,
    restitution: 1.0, friction: 0.5,
});
const deadBall = Physics.createBody({
    shape: 'sphere', radius: 0.5, position: { x: -5, y: 3, z: 0 },
    restitution: 0.0, restitutionCombine: 'min',
});
const liveBall = Physics.createBody({
    shape: 'sphere', radius: 0.5, position: { x: 5, y: 3, z: 0 },
    restitution: 0.0, restitutionCombine: 'max',
});

// Let both fall and (maybe) bounce; track the peak height after impact.
let deadPeak = -1, livePeak = -1, impacted = false;
for (let i = 0; i < 250; i++) {
    advanceTime(16);
    const dy = Physics.getTransform(deadBall).position.y;
    const ly = Physics.getTransform(liveBall).position.y;
    if (!impacted && (dy < 0.7 || ly < 0.7)) impacted = true;
    if (impacted) {
        deadPeak = Math.max(deadPeak, dy);
        livePeak = Math.max(livePeak, ly);
    }
}
assert(impacted, 'balls reached the ground');
assert(livePeak > 1.5, "'max' combine bounces high, peak=" + livePeak);
assert(deadPeak < 0.8, "'min' combine kills the bounce, peak=" + deadPeak);
assert(livePeak > deadPeak + 1.0,
       'min vs max measurably different (' + deadPeak + ' vs ' + livePeak + ')');

Physics.destroyAll();

// ===========================================================================
// Friction combine via the runtime setters: sliding boxes, min vs max.
// Ground friction 1.0, boxes friction 0.04. Jolt default sqrt(0.04) = 0.2.
//   'min' → 0.04 (slides far)     'max' → 1.0 (stops fast)
// ===========================================================================
Physics.createBody({
    shape: 'box', halfExtents: { x: 60, y: 0.5, z: 60 },
    position: { x: 0, y: -0.5, z: 0 }, static: true,
    friction: 1.0, restitution: 0,
});
const slick = Physics.createBody({
    shape: 'box', halfExtents: { x: 0.5, y: 0.5, z: 0.5 },
    position: { x: 0, y: 0.51, z: -5 },
    friction: 0.04, restitution: 0,
});
const grippy = Physics.createBody({
    shape: 'box', halfExtents: { x: 0.5, y: 0.5, z: 0.5 },
    position: { x: 0, y: 0.51, z: 5 },
    friction: 0.04, restitution: 0,
});
assert(Physics.setFrictionCombine(slick, 'min') === true, 'setFrictionCombine ok');
assert(Physics.setFrictionCombine(grippy, 'max') === true, 'setFrictionCombine ok');

advanceTime(300); // settle onto the ground
const slick0 = Physics.getTransform(slick).position.x;
const grippy0 = Physics.getTransform(grippy).position.x;
Physics.setLinearVelocity(slick, 5, 0, 0);
Physics.setLinearVelocity(grippy, 5, 0, 0);
advanceTime(2000);
const slickDist = Physics.getTransform(slick).position.x - slick0;
const grippyDist = Physics.getTransform(grippy).position.x - grippy0;
assert(grippyDist < slickDist - 1.5,
       'max-friction box stops far sooner than min-friction box (' +
       grippyDist.toFixed(2) + ' vs ' + slickDist.toFixed(2) + ')');
assert(grippyDist < 2.5, 'combined friction 1.0 stops quickly, got ' + grippyDist);
assert(slickDist > 4.0, 'combined friction 0.04 keeps sliding, got ' + slickDist);

Physics.destroyAll();

// ===========================================================================
// Contact impulse estimate: "added" events carry `impulse` (kg·m/s), a
// pre-solve estimate (Jolt EstimateCollisionResponse). A body arriving
// faster must report a proportionally larger impulse; sensor overlaps
// report 0. Sandbox world for deterministic inline stepping.
// ===========================================================================
{
    const w = Physics.createWorldHandle({ maxBodies: 32, gravity: { x: 0, y: 0, z: 0 } });
    const floor = w.createBody({
        shape: 'box', halfExtents: { x: 20, y: 0.5, z: 20 },
        position: { x: 0, y: -0.5, z: 0 }, static: true,
        restitution: 0,   // combined restitution is max(r1,r2) — keep J ≈ m*v exact
    });

    // Fire a unit-density sphere (r=0.5 → m ≈ 523.6 kg) straight down at a
    // known speed from just above the floor; capture its contact impulse.
    // Restitution 0 → expected impulse ≈ m * v (all normal momentum killed).
    function impactImpulse(speed) {
        const ball = w.createBody({
            shape: 'sphere', radius: 0.5, position: { x: 0, y: 0.7, z: 0 },
            restitution: 0, friction: 0,
        });
        w.setLinearVelocity(ball, 0, -speed, 0);
        let imp = null;
        for (let i = 0; i < 60 && imp === null; i++) {
            w.step(1 / 60);
            for (const e of w.getContacts()) {
                if (e.type !== 'added') continue;
                const pair = (e.body1 === ball && e.body2 === floor) ||
                             (e.body1 === floor && e.body2 === ball);
                if (pair) { imp = e.impulse; break; }
            }
        }
        w.destroyBody(ball);
        w.step(1 / 60); w.getContacts();   // flush the removal event
        return imp;
    }

    const mass = 4 / 3 * Math.PI * 0.5 ** 3 * 1000;  // unit-density sphere
    const i1 = impactImpulse(2);
    const i2 = impactImpulse(6);
    assert(i1 !== null && i2 !== null, 'both impacts reported an impulse');
    assert(typeof i1 === 'number' && i1 > 0, 'impulse present and positive on added events');
    // Speculative contact fires just BEFORE the surfaces touch, so the
    // estimate sees the full approach speed: J ≈ m*v within a few percent.
    assert(Math.abs(i1 - mass * 2) / (mass * 2) < 0.1,
           'impulse ≈ m*v at 2 m/s, got ' + i1 + ' expected ~' + (mass * 2));
    assert(Math.abs(i2 - mass * 6) / (mass * 6) < 0.1,
           'impulse ≈ m*v at 6 m/s, got ' + i2 + ' expected ~' + (mass * 6));
    assert(i2 > 2 * i1, 'faster impact reports a larger impulse (' + i1 + ' → ' + i2 + ')');

    // Sensor overlap: impulse must be 0 (no collision response).
    const zone = w.createBody({
        shape: 'box', halfExtents: { x: 1, y: 1, z: 1 },
        position: { x: 10, y: 0, z: 0 }, static: true, sensor: true,
    });
    const probe = w.createBody({
        shape: 'sphere', radius: 0.3, position: { x: 10, y: 3, z: 0 },
    });
    w.setLinearVelocity(probe, 0, -5, 0);
    let sensorEv = null;
    for (let i = 0; i < 60 && sensorEv === null; i++) {
        w.step(1 / 60);
        for (const e of w.getContacts()) {
            if (e.type === 'added' && e.sensor) { sensorEv = e; break; }
        }
    }
    assert(sensorEv !== null, 'sensor overlap event arrived');
    assert(sensorEv.impulse === 0, 'sensor overlap reports impulse 0, got ' + sensorEv.impulse);

    w.destroy();
    console.log('PASS contact impulse estimate');
}
