// Tests for Issue #134: bro.physics.compileForceField / compilePotentialField
// Analytical -grad U Potential & Native SIMD Parallel Force Compiler

assert(typeof bro === 'object', 'bro global exists');
assert(typeof bro.physics === 'object', 'bro.physics exists');
assert(typeof bro.physics.compileForceField === 'function', 'bro.physics.compileForceField exists');
assert(typeof bro.physics.compilePotentialField === 'function', 'bro.physics.compilePotentialField exists');
assert(typeof Physics.compileForceField === 'function', 'Physics.compileForceField exists');

// Test 1: Potential Field -grad U Accuracy
// U(x, y, z) = 0.5 * k * (x^2 + y^2 + z^2) - G * M / sqrt(x^2 + y^2 + z^2 + eps^2)
{
    const field = bro.physics.compilePotentialField(function harmonicGravPotential(x, y, z) {
        let k = 2.5;
        let gm = 10.0;
        let eps2 = 0.25;
        let r2 = x * x + y * y + z * z;
        return 0.5 * k * r2 - gm / Math.sqrt(r2 + eps2);
    });

    assert(field && field.ok === true, 'harmonicGravPotential compiled ok');
    assert(field.isPotential === true, 'field isPotential is true');
    assert(field.provenSafe === true, 'field provenSafe is true');
    assert(field.cpuGpuProvenEquivalent === true, 'field cpuGpuProvenEquivalent is true');

    const k = 2.5;
    const gm = 10.0;
    const eps2 = 0.25;
    const coords = [
        [1.0, 2.0, -1.5],
        [-2.5, 0.75, 3.25],
        [0.0, 0.0, 0.0],
        [4.0, -3.0, 1.25],
        [-0.5, -1.25, -2.0]
    ];

    for (const [x, y, z] of coords) {
        const r2 = x * x + y * y + z * z;
        const rEps = Math.sqrt(r2 + eps2);
        const rEps3 = rEps * rEps * rEps;

        const expectedU = 0.5 * k * r2 - gm / rEps;
        // Analytical F = -grad U:
        // dU/dx = k * x + gm * x / (r^2 + eps^2)^(3/2)  =>  Fx = -dU/dx
        const expectedFx = -(k * x + gm * x / rEps3);
        const expectedFy = -(k * y + gm * y / rEps3);
        const expectedFz = -(k * z + gm * z / rEps3);

        const s = field.evalAt(x, y, z);
        const denomU = Math.max(Math.abs(expectedU), 1.0);
        const denomF = Math.max(Math.hypot(expectedFx, expectedFy, expectedFz), 1.0);

        assert(Math.abs(s.energy - expectedU) / denomU < 1e-5,
               'potential U accuracy at (' + x + ',' + y + ',' + z + ')');
        assert(Math.abs(s.fx - expectedFx) / denomF < 1e-5,
               'force Fx accuracy at (' + x + ',' + y + ',' + z + ')');
        assert(Math.abs(s.fy - expectedFy) / denomF < 1e-5,
               'force Fy accuracy at (' + x + ',' + y + ',' + z + ')');
        assert(Math.abs(s.fz - expectedFz) / denomF < 1e-5,
               'force Fz accuracy at (' + x + ',' + y + ',' + z + ')');
    }
}

// Test 2: Symplectic Energy Conservation over 1,000 timesteps (dt = 0.001)
// U(x, y, z) = 0.5 * k * (x^2 + y^2 + z^2)
{
    const harmonic = bro.physics.compileForceField(function isotropicHarmonic(x, y, z) {
        let k = 1.5;
        return 0.5 * k * (x * x + y * y + z * z);
    });
    assert(harmonic && harmonic.ok === true, 'isotropicHarmonic compiled ok');

    const report = harmonic.verifyConservation({
        x: 2.0, y: -1.0, z: 0.5,
        vx: 0.25, vy: 1.5, vz: -0.75,
        dt: 0.001,
        steps: 1000,
        mass: 1.0,
        tolerance: 1e-3
    });

    assert(report.steps === 1000, 'integrated 1000 steps');
    assert(report.initialEnergy > 0.0, 'positive initial mechanical energy');
    assert(report.maxRelativeDrift < 1e-3,
           'relative energy drift |E_final - E_initial| / E_initial < 1e-3, got ' + report.maxRelativeDrift);
    assert(report.conserved === true, 'symplectic energy conservation verified');
}

// Test 3: Parallel Batch Throughput (10,000 particles in Float32Array in < 5ms)
// and Jolt PhysicsWorld integration
{
    const vortexWell = bro.physics.compileForceField(function radialWell(x, y, z, t) {
        let r2 = x * x + y * y + z * z;
        return 0.5 * 2.0 * r2 + 0.1 * Math.sin(x + t);
    });
    assert(vortexWell && vortexWell.ok === true, 'vortexWell compiled ok');

    const N = 10000;
    const positions = new Float32Array(N * 3);
    const velocities = new Float32Array(N * 3);
    for (let i = 0; i < N; i++) {
        positions[i * 3 + 0] = (i % 100) * 0.1 - 5.0;
        positions[i * 3 + 1] = Math.floor(i / 100) * 0.1 - 5.0;
        positions[i * 3 + 2] = 1.0;
        velocities[i * 3 + 0] = 0.0;
        velocities[i * 3 + 1] = 0.0;
        velocities[i * 3 + 2] = 0.0;
    }

    // Warmup call
    vortexWell.applyToParticles(positions, velocities, 0.001, 1.0, 0.0);

    const t0 = performance.now();
    const updated = vortexWell.applyToParticles(positions, velocities, 0.001, 1.0, 0.25);
    const elapsedMs = performance.now() - t0;

    assert(updated === N, 'applyToParticles updated all 10,000 particles');
    assert(elapsedMs < 5.0, '10,000 particles updated in < 5ms, got ' + elapsedMs.toFixed(3) + 'ms');
    assert(!Number.isNaN(positions[0]) && !Number.isNaN(velocities[0]), 'particles have finite state');

    // Verify Jolt PhysicsWorld integration via applyToPhysicsWorld
    Physics.createWorld({ gravity: [0, 0, 0] });
    const b1 = Physics.createBody({ shape: 'sphere', position: [3, 0, 0], radius: 0.5, mass: 1.0, linearDamping: 0 });
    const b2 = Physics.createBody({ shape: 'sphere', position: [0, 4, 0], radius: 0.5, mass: 1.0, linearDamping: 0 });

    const bodiesUpdated = vortexWell.applyToPhysicsWorld(0.0);
    assert(bodiesUpdated >= 2, 'applyToPhysicsWorld applied forces to active dynamic bodies');
    Physics.step(1 / 60);

    const t1 = Physics.getTransform(b1);
    const t2 = Physics.getTransform(b2);
    const x1 = Array.isArray(t1.position) ? t1.position[0] : t1.position.x;
    const y2 = Array.isArray(t2.position) ? t2.position[1] : t2.position.y;
    assert(x1 < 3.0, 'body 1 pulled toward origin along -x by conservative force field');
    assert(y2 < 4.0, 'body 2 pulled toward origin along -y by conservative force field');
    Physics.destroyAll();
}

console.log('test_force_field_jit OK');
