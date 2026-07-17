// Physics correctness batch — regression tests for lifetime + filter fixes:
//  1. Ragdoll sibling double-destroy: destroying any part body destroys the
//     WHOLE ragdoll and evicts every sibling part tag; a later destroy through
//     a sibling tag must be a safe no-op (previously reached Jolt's
//     DestroyBody with a dead id — Debug assert, Release free-list corruption).
//  2. Constraint collideConnected: default false = the two constrained bodies
//     don't collide with each other; true = they do; destroying the
//     constraint re-enables collision.
//  3. setMotionType preserves the body's collision layer (custom layers must
//     survive a static/dynamic toggle; only layer 0 → dynamic swaps to the
//     moving layer).
//  4. vehicle.destroy() destroys an inline-created chassis ({chassis:...})
//     but leaves an app-provided one ({body: tag}) alone.
//  5. getContacts() overflow flag: dropped events (fixed-capacity buffer) are
//     reported via events.overflow instead of vanishing silently.
// Exercises src/js/physics_bindings.cpp + src/physics/physics_world.cpp.
// Run under Debug too — the Jolt asserts + QuickJS leak assert are the gates.

assert(typeof Physics === 'object', 'Physics namespace exists');
Physics.destroyAll();

const DEG = Math.PI / 180;

// ---------------------------------------------------------------------------
// 1. Ragdoll sibling double-destroy (default world, hostile destroy order)
// ---------------------------------------------------------------------------

function ragdollOpts(pos) {
    return {
        position: pos,
        parts: [
            { name: 'pelvis', shape: 'capsule', halfHeight: 0.10, radius: 0.12,
              position: { x: 0, y: 1.00, z: 0 } },
            { name: 'spine', parent: 'pelvis', shape: 'capsule', halfHeight: 0.12, radius: 0.11,
              position: { x: 0, y: 1.35, z: 0 },
              joint: { point: { x: 0, y: 1.15, z: 0 }, normalHalfConeAngle: 25 * DEG,
                       twistMin: -20 * DEG, twistMax: 20 * DEG } },
            { name: 'head', parent: 'spine', shape: 'sphere', radius: 0.11,
              position: { x: 0, y: 1.72, z: 0 },
              joint: { point: { x: 0, y: 1.55, z: 0 }, normalHalfConeAngle: 35 * DEG,
                       twistMin: -45 * DEG, twistMax: 45 * DEG } },
        ],
    };
}

{
    const base = Physics.getAllTransforms().length / 8;
    const rd = Physics.createRagdoll(ragdollOpts({ x: 0, y: 2, z: 0 }));
    const tags = [rd.partBody(0), rd.partBody(1), rd.partBody(2)];
    assert(tags.every(t => typeof t === 'number' && t >= 0), 'ragdoll part tags valid');
    assert(Physics.getAllTransforms().length / 8 === base + 3, 'ragdoll adds 3 bodies');

    advanceTime(100);

    // Destroy the whole ragdoll THROUGH one part body tag...
    Physics.destroyBody(tags[1]);
    assert(Physics.getAllTransforms().length / 8 === base,
           'destroying one part destroyed the whole ragdoll AND evicted all part tags');
    assert(Physics.getTransform(tags[0]) === undefined, 'sibling tag 0 evicted');
    assert(Physics.getTransform(tags[2]) === undefined, 'sibling tag 2 evicted');

    // ...then hit the SIBLING tags. Must be safe no-ops (this is the sequence
    // that corrupted Jolt's body-manager free list in Release).
    Physics.destroyBody(tags[0]);
    Physics.destroyBody(tags[2]);
    Physics.destroyBody(tags[1]);   // and a straight double-destroy
    rd.destroy();                    // handle destroy after the fact: no-op

    // The world must still be fully usable: allocate new bodies (re-uses the
    // freed slots), step, and read back sane transforms.
    advanceTime(100);
    const probes = [];
    for (let i = 0; i < 8; i++) {
        probes.push(Physics.createBody({
            shape: 'sphere', radius: 0.2, position: { x: i, y: 5, z: 0 },
        }));
    }
    advanceTime(250);
    for (const t of probes) {
        const tr = Physics.getTransform(t);
        assert(tr && isFinite(tr.position.y) && tr.position.y < 5,
               'world still simulates after sibling double-destroy');
        Physics.destroyBody(t);
    }
    console.log('PASS ragdoll sibling double-destroy');
}

// ---------------------------------------------------------------------------
// 2. collideConnected (sandbox world for deterministic inline stepping)
// ---------------------------------------------------------------------------

{
    const w = Physics.createWorldHandle({ maxBodies: 64, gravity: { x: 0, y: 0, z: 0 } });

    function pairContacts(evs, a, b) {
        return evs.filter(e =>
            (e.body1 === a && e.body2 === b) || (e.body1 === b && e.body2 === a));
    }
    function makeOverlappingPair() {
        const a = w.createBody({ shape: 'sphere', radius: 0.5, position: { x: 0, y: 0, z: 0 } });
        const b = w.createBody({ shape: 'sphere', radius: 0.5, position: { x: 0.6, y: 0, z: 0 } });
        return [a, b];
    }

    // Default (collideConnected omitted = false): overlapping constrained
    // bodies produce NO contact between each other.
    const [a1, b1] = makeOverlappingPair();
    const c1 = w.createConstraint({
        type: 'distance', body1: a1, body2: b1,
        point1: { x: 0, y: 0, z: 0 }, point2: { x: 0.6, y: 0, z: 0 },
        minDistance: 0, maxDistance: 2,
    });
    assert(c1 > 0, 'constraint created (default collideConnected)');
    let evs = [];
    for (let i = 0; i < 30; i++) { w.step(1 / 60); evs = evs.concat(Array.from(w.getContacts())); }
    assert(pairContacts(evs, a1, b1).length === 0,
           'collideConnected=false (default): no contact between constrained bodies');
    const sep1 = Math.abs(w.getTransform(b1).position.x - w.getTransform(a1).position.x);
    assert(sep1 < 0.75, 'no contact push-out while filtered, sep=' + sep1);

    // Destroying the constraint re-enables collision between the pair.
    w.destroyConstraint(c1);
    w.activate(a1); w.activate(b1);
    evs = [];
    for (let i = 0; i < 30; i++) { w.step(1 / 60); evs = evs.concat(Array.from(w.getContacts())); }
    assert(pairContacts(evs, a1, b1).some(e => e.type === 'added'),
           'destroying the constraint re-enables pair collision');
    w.destroyBody(a1); w.destroyBody(b1);

    // collideConnected: true — the pair collides while constrained.
    const [a2, b2] = makeOverlappingPair();
    const c2 = w.createConstraint({
        type: 'distance', body1: a2, body2: b2,
        point1: { x: 0, y: 0, z: 0 }, point2: { x: 0.6, y: 0, z: 0 },
        minDistance: 0, maxDistance: 2,
        collideConnected: true,
    });
    assert(c2 > 0, 'constraint created (collideConnected: true)');
    evs = [];
    for (let i = 0; i < 30; i++) { w.step(1 / 60); evs = evs.concat(Array.from(w.getContacts())); }
    assert(pairContacts(evs, a2, b2).some(e => e.type === 'added'),
           'collideConnected=true: constrained bodies do collide');
    const sep2 = Math.abs(w.getTransform(b2).position.x - w.getTransform(a2).position.x);
    assert(sep2 > 0.75, 'contact resolution pushed the pair apart, sep=' + sep2);

    w.destroy();
    console.log('PASS collideConnected');
}

// ---------------------------------------------------------------------------
// 3. setMotionType preserves the collision layer
// ---------------------------------------------------------------------------

{
    const w = Physics.createWorldHandle({ maxBodies: 64 });  // default gravity -9.81
    w.setLayers({
        names: ['static', 'moving', 'custom'],
        matrix: [
            false, true,  true,
            true,  true,  true,
            true,  true,  true,
        ],
    });

    // Custom layer survives a dynamic → static → dynamic round trip.
    const b = w.createBody({ shape: 'box', halfExtents: { x: 0.5, y: 0.5, z: 0.5 },
                             position: { x: 0, y: 5, z: 0 }, layer: 'custom' });
    w.setMotionType(b, true);
    for (let i = 0; i < 30; i++) w.step(1 / 60);
    const yFrozen = w.getTransform(b).position.y;
    assert(Math.abs(yFrozen - 5) < 1e-3, 'static body does not fall, y=' + yFrozen);

    w.setMotionType(b, false);
    for (let i = 0; i < 30; i++) w.step(1 / 60);
    const p = w.getTransform(b).position;
    assert(p.y < 4.5, 'body dynamic again after toggle, y=' + p.y);
    assert(w.overlapPoint(p.x, p.y, p.z, { layers: ['custom'] }).length === 1,
           'custom layer PRESERVED across motion-type toggle');
    assert(w.overlapPoint(p.x, p.y, p.z, { layers: ['moving'] }).length === 0,
           'body was not silently moved to the moving layer');
    w.destroyBody(b);

    // The one required swap: a body going dynamic while on layer 0 (the only
    // NON_MOVING-broadphase layer) moves to the default moving layer.
    const b0 = w.createBody({ shape: 'box', halfExtents: { x: 0.5, y: 0.5, z: 0.5 },
                              position: { x: 10, y: 5, z: 0 }, layer: 'static' });
    w.setMotionType(b0, false);
    for (let i = 0; i < 10; i++) w.step(1 / 60);
    const p0 = w.getTransform(b0).position;
    assert(p0.y < 5, 'layer-0 body became dynamic and falls');
    assert(w.overlapPoint(p0.x, p0.y, p0.z, { layers: ['moving'] }).length === 1,
           'dynamic body left layer 0 for the moving layer');
    w.destroyBody(b0);

    // A body CREATED static has no motion state — asking for dynamic is a
    // guarded no-op, not a Jolt assert.
    const bs = w.createBody({ shape: 'box', halfExtents: { x: 0.5, y: 0.5, z: 0.5 },
                              position: { x: 20, y: 5, z: 0 }, static: true });
    w.setMotionType(bs, false);
    for (let i = 0; i < 10; i++) w.step(1 / 60);
    assert(Math.abs(w.getTransform(bs).position.y - 5) < 1e-3,
           'static-created body cannot become dynamic (safe no-op)');

    w.destroy();
    console.log('PASS setMotionType layer preservation');
}

// ---------------------------------------------------------------------------
// 4. Vehicle destroy: inline chassis destroyed, app-provided chassis kept
// ---------------------------------------------------------------------------

{
    Physics.destroyAll();
    const wheels = [
        { position: { x: -0.8, y: -0.3, z:  1.4 }, steerable: true,  driven: true,  radius: 0.35 },
        { position: { x:  0.8, y: -0.3, z:  1.4 }, steerable: true,  driven: true,  radius: 0.35 },
        { position: { x: -0.8, y: -0.3, z: -1.4 }, radius: 0.35 },
        { position: { x:  0.8, y: -0.3, z: -1.4 }, radius: 0.35 },
    ];

    // Inline chassis: destroy() must take the chassis body with it.
    const base = Physics.getAllTransforms().length / 8;
    const car = Physics.createVehicle({
        chassis: { shape: 'box', halfExtents: { x: 0.9, y: 0.4, z: 2 },
                   position: { x: 0, y: 3, z: 0 }, density: 500 },
        wheels,
    });
    const chassisTag = car.chassisBody;
    assert(Physics.getAllTransforms().length / 8 === base + 1, 'inline chassis body created');
    advanceTime(200);
    car.destroy();
    assert(Physics.getAllTransforms().length / 8 === base,
           'destroy() destroyed the inline-created chassis body');
    assert(Physics.getTransform(chassisTag) === undefined, 'chassis tag evicted');
    car.destroy();  // double-destroy: safe no-op
    advanceTime(100);

    // App-provided chassis: destroy() leaves the body alone.
    const myBody = Physics.createBody({ shape: 'box', halfExtents: { x: 0.9, y: 0.4, z: 2 },
                                        position: { x: 10, y: 3, z: 0 }, density: 500 });
    const car2 = Physics.createVehicle({ body: myBody, wheels });
    advanceTime(200);
    car2.destroy();
    assert(Physics.getTransform(myBody) !== undefined,
           'app-provided chassis body survives vehicle destroy');
    Physics.destroyBody(myBody);

    // Hostile order: destroy the chassis BODY first (removes the vehicle),
    // then the vehicle handle — must not double-destroy the chassis.
    const car3 = Physics.createVehicle({
        chassis: { shape: 'box', halfExtents: { x: 0.9, y: 0.4, z: 2 },
                   position: { x: 20, y: 3, z: 0 }, density: 500 },
        wheels,
    });
    Physics.destroyBody(car3.chassisBody);
    car3.destroy();
    advanceTime(100);
    console.log('PASS vehicle chassis lifetime');
}

// ---------------------------------------------------------------------------
// 5. Contact-buffer overflow flag
// ---------------------------------------------------------------------------

{
    // Deterministic overflow: shrink the per-step contact buffer to 16 and
    // drop 12 mutually-overlapping spheres into one spot — 66 pairs form in
    // the first step, so events past 16 are dropped and the drain must say so.
    const w = Physics.createWorldHandle({
        maxBodies: 64, contactBufferSize: 16, gravity: { x: 0, y: 0, z: 0 },
    });

    const balls = [];
    for (let i = 0; i < 12; i++) {
        balls.push(w.createBody({
            shape: 'sphere', radius: 0.5,
            position: { x: (i % 4) * 0.01, y: Math.floor(i / 4) * 0.01, z: 0 },
        }));
    }
    w.step(1 / 60);
    const evs = w.getContacts();
    assert(evs.overflow === true,
           'overflow flag set when contact events were dropped (got ' + evs.length + ' events)');
    assert(evs.length === 16, 'buffer-capacity events delivered, got ' + evs.length);

    // The flag is consumed by the drain: after the removal-event burst from
    // destroying the pile is flushed and drained, a quiet step reports clean.
    for (const b of balls) w.destroyBody(b);
    w.step(1 / 60);
    w.getContacts();     // drain the 'removed' burst (may overflow again)
    w.step(1 / 60);
    const evs2 = w.getContacts();
    assert(evs2.overflow === false, 'overflow flag clears after a drain');

    w.destroy();
    console.log('PASS contact overflow flag');
}

Physics.destroyAll();
console.log('test_correctness: all sections passed');
