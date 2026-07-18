// Runtime rigid-body property mutation + direct mass creation option.
//
// - createBody({ mass }) sets the body mass in kg directly (wins over
//   density; inertia still derives from the shape, scaled to the mass).
// - Physics.setMass / setLinearDamping / setAngularDamping /
//   setGravityFactor / setFriction / setRestitution mutate live bodies;
//   Physics.getBodyProperties round-trips them.
// - Behavioral checks are exact: impulse response dv = J/m, damping decay
//   v_N = v0*(1 - c*dt)^N (Jolt's per-step damping model), gravity-factor
//   integration dv = g*gf*dt per step. Sandbox world, deterministic.
// Exercises src/js/physics_bindings.cpp + src/physics/physics_world.cpp.

assert(typeof Physics === 'object', 'Physics namespace exists');
assert(typeof Physics.setMass === 'function', 'setMass exists');
assert(typeof Physics.getBodyProperties === 'function', 'getBodyProperties exists');

const EPS = 1e-5;
function near(a, b, eps, msg) {
    assert(Math.abs(a - b) <= (eps ?? EPS), msg + ' (got ' + a + ', want ' + b + ')');
}

// ===========================================================================
// Creation-time mass + round-trips (sandbox world, zero gravity)
// ===========================================================================
const w = Physics.createWorldHandle({ maxBodies: 64, gravity: { x: 0, y: 0, z: 0 } });

// Unit-density sphere r=0.5 → 4/3*pi*0.125*1000 ≈ 523.6 kg.
const byDensity = w.createBody({ shape: 'sphere', radius: 0.5, position: { x: 0, y: 0, z: 0 } });
near(w.getBodyProperties(byDensity).mass, 4 / 3 * Math.PI * 0.125 * 1000, 0.1,
     'density-derived mass');

// mass wins over density when both are given.
const byMass = w.createBody({
    shape: 'sphere', radius: 0.5, position: { x: 5, y: 0, z: 0 },
    density: 1, mass: 42,
});
near(w.getBodyProperties(byMass).mass, 42, 1e-3, 'direct mass wins over density');

// Full setter/getter round-trip.
w.setMass(byMass, 7);
w.setLinearDamping(byMass, 0.5);
w.setAngularDamping(byMass, 0.25);
w.setGravityFactor(byMass, 0.5);
w.setFriction(byMass, 0.9);
w.setRestitution(byMass, 0.1);
{
    const p = w.getBodyProperties(byMass);
    near(p.mass, 7, 1e-4, 'setMass round-trip');
    near(p.linearDamping, 0.5, EPS, 'setLinearDamping round-trip');
    near(p.angularDamping, 0.25, EPS, 'setAngularDamping round-trip');
    near(p.gravityFactor, 0.5, EPS, 'setGravityFactor round-trip');
    near(p.friction, 0.9, EPS, 'setFriction round-trip');
    near(p.restitution, 0.1, EPS, 'setRestitution round-trip');
}

// Static bodies: mass reads 0, setMass is a safe no-op.
const slab = w.createBody({
    shape: 'box', halfExtents: { x: 1, y: 1, z: 1 },
    position: { x: -5, y: 0, z: 0 }, static: true,
});
assert(w.getBodyProperties(slab).mass === 0, 'static body mass reads 0');
w.setMass(slab, 100);
assert(w.getBodyProperties(slab).mass === 0, 'setMass on a static body is a no-op');
console.log('PASS property round-trips');

// ===========================================================================
// Behavioral: impulse response is exactly dv = J/m after setMass
// ===========================================================================
{
    const a = w.createBody({ shape: 'sphere', radius: 0.5, position: { x: 0, y: 10, z: 0 } });
    const b = w.createBody({ shape: 'sphere', radius: 0.5, position: { x: 5, y: 10, z: 0 } });
    w.setMass(a, 2);
    w.setMass(b, 4);
    w.addImpulse(a, 10, 0, 0);   // Jolt applies impulses to velocity immediately
    w.addImpulse(b, 10, 0, 0);
    near(w.getVelocity(a).linear.x, 5.0, 1e-4, '2 kg body: dv = 10/2');
    near(w.getVelocity(b).linear.x, 2.5, 1e-4, '4 kg body: dv = 10/4');
    w.destroyBody(a); w.destroyBody(b);
    console.log('PASS impulse response scales with setMass');
}

// ===========================================================================
// Behavioral: setLinearDamping visibly (and exactly) slows a body
// ===========================================================================
{
    const free = w.createBody({
        shape: 'sphere', radius: 0.5, position: { x: 0, y: 20, z: 0 }, linearDamping: 0,
    });
    const damped = w.createBody({
        shape: 'sphere', radius: 0.5, position: { x: 10, y: 20, z: 0 }, linearDamping: 0,
    });
    w.setLinearDamping(damped, 2.0);
    w.setLinearVelocity(free, 10, 0, 0);
    w.setLinearVelocity(damped, 10, 0, 0);
    const N = 30, dt = 1 / 60;
    for (let i = 0; i < N; i++) w.step(dt);
    near(w.getVelocity(free).linear.x, 10, 1e-3, 'undamped body keeps its velocity');
    // Jolt damping: v *= max(0, 1 - c*dt) per step, exactly.
    const expect = 10 * Math.pow(1 - 2.0 * dt, N);
    near(w.getVelocity(damped).linear.x, expect, 1e-3,
         'damped body decays exactly per Jolt model');
    w.destroyBody(free); w.destroyBody(damped);
    console.log('PASS damping change slows a body');
}

// ===========================================================================
// Behavioral: setGravityFactor halves the fall rate
// ===========================================================================
{
    const g = Physics.createWorldHandle({ maxBodies: 8, gravity: { x: 0, y: -10, z: 0 } });
    const full = g.createBody({ shape: 'sphere', radius: 0.5, position: { x: 0, y: 50, z: 0 }, linearDamping: 0 });
    const half = g.createBody({ shape: 'sphere', radius: 0.5, position: { x: 5, y: 50, z: 0 }, linearDamping: 0 });
    g.setGravityFactor(half, 0.5);
    const N = 12, dt = 1 / 60;
    for (let i = 0; i < N; i++) g.step(dt);
    near(g.getVelocity(full).linear.y, -10 * N * dt, 1e-3, 'gf=1 integrates g*dt per step');
    near(g.getVelocity(half).linear.y, -5 * N * dt, 1e-3, 'gf=0.5 integrates g/2*dt per step');
    g.destroy();
    console.log('PASS gravity factor');
}

// ===========================================================================
// Behavioral: runtime restitution change (bouncy vs dead drop)
// ===========================================================================
{
    const g = Physics.createWorldHandle({ maxBodies: 8, gravity: { x: 0, y: -10, z: 0 } });
    g.createBody({
        shape: 'box', halfExtents: { x: 20, y: 0.5, z: 20 },
        position: { x: 0, y: -0.5, z: 0 }, static: true, restitution: 0,
    });
    function peakAfterBounce(rest) {
        const ball = g.createBody({
            shape: 'sphere', radius: 0.5, position: { x: 0, y: 3, z: 0 },
            restitution: 0, linearDamping: 0,
        });
        g.setRestitution(ball, rest);   // runtime change, not creation-time
        let peak = -Infinity, bounced = false;
        for (let i = 0; i < 240; i++) {
            g.step(1 / 60);
            const v = g.getVelocity(ball).linear.y;
            const y = g.getTransform(ball).position.y;
            if (v > 0.5) bounced = true;
            if (bounced) peak = Math.max(peak, y);
        }
        g.destroyBody(ball);
        return { peak, bounced };
    }
    const dead = peakAfterBounce(0.0);
    const lively = peakAfterBounce(0.8);
    assert(!dead.bounced, 'restitution 0: no bounce');
    assert(lively.bounced && lively.peak > 1.0,
           'runtime restitution 0.8 bounces back up, peak=' + lively.peak.toFixed(2));
    g.destroy();
    console.log('PASS runtime restitution');
}

// ===========================================================================
// Default world: the same surface exists and round-trips
// ===========================================================================
Physics.destroyAll();
{
    const t = Physics.createBody({ shape: 'sphere', radius: 0.5, position: { x: 0, y: 5, z: 0 }, mass: 12 });
    near(Physics.getBodyProperties(t).mass, 12, 1e-3, 'default world: creation mass');
    Physics.setMass(t, 3);
    Physics.setGravityFactor(t, 0.25);
    const p = Physics.getBodyProperties(t);
    near(p.mass, 3, 1e-4, 'default world: setMass round-trip');
    near(p.gravityFactor, 0.25, EPS, 'default world: setGravityFactor round-trip');
    Physics.destroyBody(t);
    console.log('PASS default-world surface');
}

w.destroy();
Physics.destroyAll();
