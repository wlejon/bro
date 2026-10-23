// Test: Differentiable Physics & Inverse Trajectory Solver (bro.physics.solveTrajectory)
// Verifies shooting-method + Velocity-Verlet sensitivity propagation for:
// 1. Projectile launch in uniform gravity + harmonic wind potential field
// 2. Trajectory through a compiled autodiff conservative potential field U(x, y, z)
// 3. Obstacle avoidance with guaranteed minimum clearance and conservative energy invariance

function assert(cond, msg) {
    if (!cond) {
        console.error("FAIL: " + msg);
        process.exit(1);
    }
}

function assertNear(actual, expected, tol, msg) {
    const diff = Math.abs(actual - expected);
    if (!(diff <= tol)) {
        console.error("FAIL: " + msg + " (expected " + expected + " +/- " + tol + ", got " + actual + ")");
        process.exit(1);
    }
}

function vec3Dist(a, b) {
    return Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

console.log("=== Test 1: Inverse ballistic launch in gravity + harmonic potential ===");
{
    const startPos = [0.0, 2.0, 0.0];
    const targetPos = [12.0, 5.0, -4.0];

    const sol = bro.physics.solveTrajectory({
        startPos: startPos,
        targetPos: targetPos,
        gravity: [0.0, -9.81, 0.0],
        harmonicK: 0.15,
        harmonicCenter: [6.0, 4.0, -2.0],
        mass: 1.5,
        flightTime: 1.6,
        steps: 160,
        tolerance: 1e-4
    });

    assert(sol && sol.ok === true, "solveTrajectory returned ok");
    assert(sol.converged === true, "solveTrajectory converged");
    assert(sol.missDistance < 1e-4, "missDistance < 1e-4, got " + sol.missDistance);
    assert(Array.isArray(sol.initialVelocity) && sol.initialVelocity.length === 3, "initialVelocity is [vx,vy,vz]");
    assert(Array.isArray(sol.trajectory) && sol.trajectory.length === 161, "trajectory has 161 samples");
    assert(sol.energyInvariantVerified === true, "Symplectic energy invariant verified");
    assert(sol.maxEnergyDrift < 1e-3, "maxEnergyDrift < 1e-3, got " + sol.maxEnergyDrift);

    const endSample = sol.trajectory[sol.trajectory.length - 1];
    assertNear( vec3Dist(endSample.pos, targetPos), 0.0, 1e-4, "final trajectory position hits targetPos");
    console.log("  Initial velocity:", JSON.stringify(sol.initialVelocity),
                "missDistance:", sol.missDistance,
                "maxEnergyDrift:", sol.maxEnergyDrift);
}

console.log("=== Test 2: Inverse trajectory in compiled autodiff potential field ===");
{
    // Compile a 3D conservative potential field U(x, y, z) with gravity + sinusoidal ridge potential
    const field = bro.physics.compilePotentialField((x, y, z) => {
        return 9.81 * y + 2.5 * Math.sin(0.25 * x) * Math.cos(0.25 * z) + 0.08 * (x * x + z * z);
    });

    assert(field && field.provenSafe === true, "compiled potential field provenSafe");

    const startPos = [-4.0, 1.0, -2.0];
    const targetPos = [6.0, 3.5, 4.0];

    const sol = field.solveTrajectory({
        startPos: startPos,
        targetPos: targetPos,
        mass: 1.0,
        flightTime: 1.5,
        steps: 150,
        tolerance: 1e-3
    });

    assert(sol && sol.ok === true, "field.solveTrajectory returned ok");
    assert(sol.converged === true, "field.solveTrajectory converged");
    assert(sol.missDistance < 1e-3, "field.solveTrajectory missDistance < 1e-3, got " + sol.missDistance);
    assert(sol.energyInvariantVerified === true, "energyInvariantVerified on compiled potential field");

    // Verify independent forward Velocity-Verlet rollout using field.evalAt matches targetPos
    let rx = startPos[0], ry = startPos[1], rz = startPos[2];
    let rvx = sol.initialVelocity[0], rvy = sol.initialVelocity[1], rvz = sol.initialVelocity[2];
    const dt = 1.5 / 150;
    let f = field.evalAt(rx, ry, rz);
    for (let i = 0; i < 150; ++i) {
        rx += rvx * dt + 0.5 * f.fx * dt * dt;
        ry += rvy * dt + 0.5 * f.fy * dt * dt;
        rz += rvz * dt + 0.5 * f.fz * dt * dt;
        const fNext = field.evalAt(rx, ry, rz);
        rvx += 0.5 * (f.fx + fNext.fx) * dt;
        rvy += 0.5 * (f.fy + fNext.fy) * dt;
        rvz += 0.5 * (f.fz + fNext.fz) * dt;
        f = fNext;
    }
    const verletDist = vec3Dist([rx, ry, rz], targetPos);
    assert(verletDist < 1e-3, "independent Velocity-Verlet rollout hits target within 1e-3, got " + verletDist);

    // Also verify SIMD particle integrator rollout stays within O(dt) of Velocity-Verlet
    const pos = new Float32Array(startPos);
    const vel = new Float32Array(sol.initialVelocity);
    for (let i = 0; i < 150; ++i) {
        field.applyToParticles(pos, vel, dt, 1.0);
    }
    const rolloutDist = vec3Dist([pos[0], pos[1], pos[2]], targetPos);
    assert(rolloutDist < 0.1, "independent Float32 applyToParticles rollout hits target within O(dt), got " + rolloutDist);
    console.log("  Compiled potential field trajectory missDistance:", sol.missDistance,
                "verletDist:", verletDist, "eulerDist:", rolloutDist);
}

console.log("=== Test 3: Obstacle avoidance with guaranteed clearance ===");
{
    // Place a spherical obstacle directly obstructing the free-space path between startPos and targetPos
    const startPos = [0.0, 0.0, 0.0];
    const targetPos = [10.0, 0.0, 0.0];
    const obstacleCenter = [5.0, 0.0, 0.0];
    const obstacleRadius = 1.5;

    const sol = bro.physics.solveTrajectory({
        startPos: startPos,
        targetPos: targetPos,
        gravity: [0.0, 0.0, 0.0],
        obstacles: [
            { center: obstacleCenter, radius: obstacleRadius, margin: 0.8, weight: 80.0 }
        ],
        flightTime: 2.0,
        steps: 160,
        tolerance: 1e-3
    });

    assert(sol && sol.ok === true, "obstacle trajectory solve returned ok");
    assert(sol.converged === true, "obstacle trajectory solve converged");
    assert(sol.missDistance < 1e-3, "obstacle trajectory hits target within 1e-3, got " + sol.missDistance);
    assert(sol.obstacleFree === true, "obstacleFree === true");
    assert(sol.minObstacleClearance > 0.05,
           "minObstacleClearance > 0.05 (strictly outside sphere), got " + sol.minObstacleClearance);

    // Double-check every point along the returned trajectory is outside the obstacle sphere
    let minSurfaceDist = Infinity;
    for (const pt of sol.trajectory) {
        const d = vec3Dist(pt.pos, obstacleCenter) - obstacleRadius;
        if (d < minSurfaceDist) minSurfaceDist = d;
    }
    assert(minSurfaceDist > 0.05, "verified all trajectory samples clear obstacle by > 0.05, min=" + minSurfaceDist);
    console.log("  Obstacle trajectory minClearance:", sol.minObstacleClearance,
                "missDistance:", sol.missDistance,
                "initialVelocity:", JSON.stringify(sol.initialVelocity));
}

console.log("ALL TRAJECTORY SOLVER TESTS PASSED");
process.exit(0);
