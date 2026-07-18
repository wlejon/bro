// Area field overrides (Godot Area3D analog): a sensor body carries gravity/
// damping fields that automatically affect dynamic bodies overlapping it,
// applied inside the physics step.
//
// Semantics under test (all deterministic, sandbox worlds, exact assertions):
//  - membership follows the sensor contact stream with ONE step of latency:
//    the field applies from the step after the overlap begins, and stops on
//    the step after it ends.
//  - gravity modes: replace (stops the priority walk), combine (adds),
//    scale (multiplies the world term). Priority desc, ties → earlier area.
//  - point gravity toward the sensor center, optional inverse-square falloff.
//  - damping override: set on enter / restored on exit; runtime
//    setLinearDamping while inside updates the restored BASE.
//  - runtime setAreaOverride affects bodies already inside; null clears.
// Exercises src/js/physics_bindings.cpp + src/physics/physics_world.cpp.

assert(typeof Physics === 'object', 'Physics namespace exists');
assert(typeof Physics.setAreaOverride === 'function', 'setAreaOverride exists');

const DT = 1 / 60;
function near(a, b, eps, msg) {
    assert(Math.abs(a - b) <= eps, msg + ' (got ' + a + ', want ' + b + ')');
}

// ===========================================================================
// 1. Low-gravity replace zone: falling body enters, fall rate becomes the
//    area's. Exact per-step model: Jolt integrates worldG*dt every step; the
//    area listener adds (areaG - worldG)*dt from the step after entry.
// ===========================================================================
{
    const w = Physics.createWorldHandle({ maxBodies: 32, gravity: { x: 0, y: -10, z: 0 } });
    const zone = w.createBody({
        shape: 'box', halfExtents: { x: 10, y: 10, z: 10 },
        position: { x: 0, y: 0, z: 0 }, static: true, sensor: true,
        area: { gravity: { x: 0, y: -1, z: 0 } },   // defaults to 'replace'
    });
    const ball = w.createBody({
        shape: 'sphere', radius: 0.5, position: { x: 0, y: 5, z: 0 }, linearDamping: 0,
    });

    // Step 1: overlap detected during the step → plain world gravity.
    w.step(DT);
    near(w.getVelocity(ball).linear.y, -10 * DT, 1e-4, 'step 1 (pre-membership): world g');

    // Steps 2..N: effective per-step accel is the area's -1.
    const N = 10;
    for (let i = 1; i < N; i++) w.step(DT);
    near(w.getVelocity(ball).linear.y, (-10 - (N - 1) * 1) * DT, 1e-4,
         'replace zone: -1 m/s² from the step after entry');
    w.destroy();
    console.log('PASS low-gravity replace zone');
}

// ===========================================================================
// 2. Exit restores world gravity (one step of latency after leaving)
// ===========================================================================
{
    const w = Physics.createWorldHandle({ maxBodies: 32, gravity: { x: 0, y: -10, z: 0 } });
    const zone = w.createBody({
        shape: 'box', halfExtents: { x: 2, y: 2, z: 2 },
        position: { x: 0, y: 0, z: 0 }, static: true, sensor: true,
        area: { gravity: { x: 0, y: 0, z: 0 } },   // zero-g bubble
    });
    const ball = w.createBody({
        shape: 'sphere', radius: 0.5, position: { x: 0, y: 0, z: 0 }, linearDamping: 0,
    });
    // Enter membership, then confirm zero-g holds (velocity frozen).
    w.step(DT);
    const vAfter1 = w.getVelocity(ball).linear.y;
    for (let i = 0; i < 5; i++) w.step(DT);
    near(w.getVelocity(ball).linear.y, vAfter1, 1e-4, 'zero-g zone freezes velocity');

    // Teleport far outside; the removal event fires on the next step, so one
    // more step still sees zero-g, then world gravity resumes.
    w.setPosition(ball, 100, 50, 0);
    const v0 = w.getVelocity(ball).linear.y;
    w.step(DT);   // removal detected during this step; correction still applied
    near(w.getVelocity(ball).linear.y, v0, 1e-4, 'exit latency: one step still zero-g');
    w.step(DT);
    near(w.getVelocity(ball).linear.y, v0 - 10 * DT, 1e-4, 'world gravity resumes after exit');
    w.destroy();
    console.log('PASS exit restores gravity');
}

// ===========================================================================
// 3. Point gravity: field points at the sensor center; inverse-square falloff
// ===========================================================================
{
    const w = Physics.createWorldHandle({ maxBodies: 32, gravity: { x: 0, y: 0, z: 0 } });
    const planet = w.createBody({
        shape: 'sphere', radius: 8, position: { x: 0, y: 0, z: 0 },
        static: true, sensor: true,
        area: { gravityPoint: true, gravityStrength: 4, gravityMode: 'combine' },
    });
    const ball = w.createBody({
        shape: 'sphere', radius: 0.2, position: { x: 3, y: 0, z: 0 }, linearDamping: 0,
    });
    w.step(DT);                       // membership
    assert(Math.abs(w.getVelocity(ball).linear.x) < 1e-6, 'no pull before membership');
    w.step(DT);                       // first applied step: dv = -4*dt toward center
    near(w.getVelocity(ball).linear.x, -4 * DT, 1e-4, 'constant point gravity pulls at strength');
    near(w.getVelocity(ball).linear.y, 0, 1e-6, 'pull is purely radial');

    // Falloff: strength is the acceleration AT falloffDistance; at double the
    // distance the pull is a quarter.
    const far = w.createBody({
        shape: 'sphere', radius: 0.2, position: { x: 0, y: 6, z: 0 }, linearDamping: 0,
    });
    w.setAreaOverride(planet, {
        gravityPoint: true, gravityStrength: 4, falloffDistance: 3, gravityMode: 'combine',
    });
    w.step(DT);                       // membership for `far`
    const vy0 = w.getVelocity(far).linear.y;
    w.step(DT);
    // dist 6 = 2*falloffDistance → 4 * (3/6)² = 1 m/s² (ball moved a hair, so
    // give it a loose-but-tight tolerance).
    near(w.getVelocity(far).linear.y - vy0, -1 * DT, 2e-4,
         'inverse-square falloff: quarter pull at double distance');
    w.destroy();
    console.log('PASS point gravity + falloff');
}

// ===========================================================================
// 4. Damping override: set on enter, restored on exit; setLinearDamping
//    while inside updates the BASE; getBodyProperties reports the base.
// ===========================================================================
{
    const w = Physics.createWorldHandle({ maxBodies: 32, gravity: { x: 0, y: 0, z: 0 } });
    const water = w.createBody({
        shape: 'box', halfExtents: { x: 5, y: 5, z: 5 },
        position: { x: 0, y: 0, z: 0 }, static: true, sensor: true,
        area: { linearDamping: 4.5 },              // damping-only area
    });
    const ball = w.createBody({
        shape: 'sphere', radius: 0.5, position: { x: 0, y: 0, z: 0 }, linearDamping: 0,
    });
    w.setLinearVelocity(ball, 10, 0, 0);
    w.step(DT);   // membership + damping applied at the post-step fold
    const v1 = w.getVelocity(ball).linear.x;   // step 1 ran with base damping 0
    near(v1, 10, 1e-4, 'step 1 still base damping');
    w.step(DT);
    near(w.getVelocity(ball).linear.x, v1 * (1 - 4.5 * DT), 1e-4,
         'area damping 4.5 applies exactly from step 2');

    // Runtime setter while inside → base only; live override keeps winning.
    w.setLinearDamping(ball, 1.0);
    near(w.getBodyProperties(ball).linearDamping, 1.0, 1e-6,
         'getBodyProperties reports the BASE while overridden');
    const v2 = w.getVelocity(ball).linear.x;
    w.step(DT);
    near(w.getVelocity(ball).linear.x, v2 * (1 - 4.5 * DT), 1e-4,
         'override still wins after setLinearDamping inside');

    // Exit: teleport out; after the removal step the BASE (1.0) applies.
    w.setPosition(ball, 100, 0, 0);
    w.setLinearVelocity(ball, 10, 0, 0);
    w.step(DT);   // removal event fires during this step, fold restores base
    const v3 = w.getVelocity(ball).linear.x;
    w.step(DT);
    near(w.getVelocity(ball).linear.x, v3 * (1 - 1.0 * DT), 1e-4,
         'exit restores the (updated) base damping');
    w.destroy();
    console.log('PASS damping override + base interplay');
}

// ===========================================================================
// 5. Priority + replace/combine/scale stacking
// ===========================================================================
{
    const w = Physics.createWorldHandle({ maxBodies: 32, gravity: { x: 0, y: -8, z: 0 } });
    // Two co-located replace areas: higher priority wins.
    const A = w.createBody({
        shape: 'box', halfExtents: { x: 5, y: 5, z: 5 }, position: { x: 0, y: 0, z: 0 },
        static: true, sensor: true,
        area: { gravity: { x: 0, y: -2, z: 0 }, priority: 1 },
    });
    const B = w.createBody({
        shape: 'box', halfExtents: { x: 5, y: 5, z: 5 }, position: { x: 0, y: 0, z: 0 },
        static: true, sensor: true,
        area: { gravity: { x: 0, y: -7, z: 0 }, priority: 0 },
    });
    const ball = w.createBody({
        shape: 'sphere', radius: 0.5, position: { x: 0, y: 0, z: 0 }, linearDamping: 0,
    });
    w.step(DT);   // membership in both
    let v0 = w.getVelocity(ball).linear.y;
    w.step(DT);
    near(w.getVelocity(ball).linear.y - v0, -2 * DT, 1e-4,
         'higher-priority replace (A, -2) beats lower (B, -7)');

    // Raise B above A at runtime → B wins now.
    w.setAreaOverride(B, { gravity: { x: 0, y: -7, z: 0 }, priority: 2 });
    v0 = w.getVelocity(ball).linear.y;
    w.step(DT);
    near(w.getVelocity(ball).linear.y - v0, -7 * DT, 1e-4,
         'runtime priority change flips the winner (bodies already inside)');

    // A combine area above everything adds its field before B's replace stops
    // the walk: total = (1,0,0) + (0,-7,0).
    const C = w.createBody({
        shape: 'box', halfExtents: { x: 5, y: 5, z: 5 }, position: { x: 0, y: 0, z: 0 },
        static: true, sensor: true,
        area: { gravity: { x: 1, y: 0, z: 0 }, gravityMode: 'combine', priority: 3 },
    });
    w.step(DT);   // membership in C
    v0 = w.getVelocity(ball).linear.y;
    const vx0 = w.getVelocity(ball).linear.x;
    w.step(DT);
    near(w.getVelocity(ball).linear.x - vx0, 1 * DT, 1e-4, 'combine adds sideways pull');
    near(w.getVelocity(ball).linear.y - v0, -7 * DT, 1e-4, 'replace still caps the walk');

    // Clear both replaces: combine + world gravity remain.
    w.setAreaOverride(A, null);
    w.setAreaOverride(B, null);
    v0 = w.getVelocity(ball).linear.y;
    const vx1 = w.getVelocity(ball).linear.x;
    w.step(DT);
    near(w.getVelocity(ball).linear.x - vx1, 1 * DT, 1e-4, 'combine survives the clears');
    near(w.getVelocity(ball).linear.y - v0, -8 * DT, 1e-4, 'world gravity returns');

    // Scale mode: quarter world gravity (C cleared to isolate).
    w.setAreaOverride(C, { gravityScale: 0.25 });
    v0 = w.getVelocity(ball).linear.y;
    const vx2 = w.getVelocity(ball).linear.x;
    w.step(DT);
    near(w.getVelocity(ball).linear.y - v0, -2 * DT, 1e-4, 'gravityScale 0.25 of world -8');
    near(w.getVelocity(ball).linear.x - vx2, 0, 1e-6, 'scale mode adds no directional field');
    w.destroy();
    console.log('PASS priority + stacking modes');
}

// ===========================================================================
// 6. gravityFactor multiplies area fields too (gf=0 floats through a field)
// ===========================================================================
{
    const w = Physics.createWorldHandle({ maxBodies: 16, gravity: { x: 0, y: -10, z: 0 } });
    const zone = w.createBody({
        shape: 'box', halfExtents: { x: 5, y: 5, z: 5 }, position: { x: 0, y: 0, z: 0 },
        static: true, sensor: true,
        area: { gravity: { x: 0, y: -4, z: 0 } },
    });
    const floater = w.createBody({
        shape: 'sphere', radius: 0.5, position: { x: 0, y: 0, z: 0 },
        gravityFactor: 0, linearDamping: 0,
    });
    for (let i = 0; i < 6; i++) w.step(DT);
    near(w.getVelocity(floater).linear.y, 0, 1e-6, 'gravityFactor 0 ignores area gravity too');
    w.destroy();
    console.log('PASS gravityFactor scales area fields');
}

// ===========================================================================
// 7. Guards: area on a non-sensor is rejected; default world has the surface
// ===========================================================================
{
    let threw = false;
    const w = Physics.createWorldHandle({ maxBodies: 8 });
    try {
        w.createBody({
            shape: 'box', halfExtents: { x: 1, y: 1, z: 1 },
            position: { x: 0, y: 0, z: 0 },
            area: { gravity: { x: 0, y: 0, z: 0 } },   // sensor: false
        });
    } catch (e) { threw = true; }
    assert(threw, 'area on a non-sensor body throws at creation');

    const solid = w.createBody({
        shape: 'box', halfExtents: { x: 1, y: 1, z: 1 }, position: { x: 5, y: 0, z: 0 },
    });
    threw = false;
    try { w.setAreaOverride(solid, { gravityScale: 0.5 }); } catch (e) { threw = true; }
    assert(threw, 'setAreaOverride on a non-sensor throws');
    w.destroy();

    // Default world: install + clear round-trip through advanceTime.
    Physics.destroyAll();
    const zone = Physics.createBody({
        shape: 'box', halfExtents: { x: 4, y: 4, z: 4 }, position: { x: 0, y: 2, z: 0 },
        static: true, sensor: true,
        area: { gravity: { x: 0, y: 0, z: 0 } },   // zero-g bubble
    });
    const ball = Physics.createBody({
        shape: 'sphere', radius: 0.5, position: { x: 0, y: 2, z: 0 }, linearDamping: 0,
    });
    advanceTime(500);
    const tr = Physics.getTransform(ball);
    near(tr.position.y, 2, 0.15, 'default world: zero-g bubble holds the ball (± entry step)');
    assert(Physics.setAreaOverride(zone, null) === true, 'clear returns true');
    advanceTime(600);
    assert(Physics.getTransform(ball).position.y < 1.0,
           'after clearing the override the ball falls');
    Physics.destroyAll();
    console.log('PASS guards + default world');
}
