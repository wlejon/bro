// Test: Inverse Procedural Generation Solver (bro.scene.solveProcedural, bro.gpu.solveProcedural, bro.mesh.solveSDF)
// Verifies automatic differentiation + damped Gauss-Newton / Levenberg-Marquardt synthesis of
// procedural terrain heightfields and 3D signed distance fields from spatial constraints.

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

console.log("=== Test 1: Exact recovery of procedural terrain parameters ===");
{
    // True parameters: amp = 4.2, freqX = 0.35, offset = 1.8
    const trueAmp = 4.2;
    const trueFreqX = 0.35;
    const trueOffset = 1.8;

    const terrainFn = (x, z, amp, freqX, offset) =>
        amp * Math.sin(x * freqX) * Math.cos(z * 0.25) + offset;

    const sampleCoords = [
        [1.5, 0.8],
        [3.2, -1.2],
        [-2.4, 2.0],
        [4.0, 1.5],
        [-1.0, -2.5],
        [2.2, 3.1]
    ];

    const targets = sampleCoords.map(([x, z]) => ({
        x: x,
        z: z,
        value: terrainFn(x, z, trueAmp, trueFreqX, trueOffset)
    }));

    const solved = bro.scene.solveProcedural(terrainFn, {
        targets: targets,
        initialParams: [3.0, 0.28, 0.5],
        paramBounds: [[0.5, 10.0], [0.1, 0.8], [-5.0, 5.0]],
        maxIterations: 60,
        tolerance: 1e-4
    });

    assert(solved && solved.ok === true, "solveProcedural returned ok");
    assert(solved.converged === true, "solveProcedural converged on terrain parameter recovery");
    assert(solved.lossFinal < solved.lossInitial * 1e-4, "lossFinal reduced by > 4 orders of magnitude");
    assert(solved.maxError < 1e-3, "maxError < 1e-3, got " + solved.maxError);
    assert(solved.provenSafe === true, "SPIR-V kernel proven safe");
    assert(solved.spirvWordCount > 20, "SPIR-V words emitted");

    assertNear(solved.params[0], trueAmp, 1e-2, "recovered amplitude");
    assertNear(solved.params[1], trueFreqX, 1e-2, "recovered frequency");
    assertNear(solved.params[2], trueOffset, 1e-2, "recovered offset");

    // Verify returned evaluate(x, z) closure matches target heights
    for (const t of targets) {
        const h = solved.evaluate(t.x, t.z);
        assertNear(h, t.value, 1e-3, "evaluatedFn at (" + t.x + ", " + t.z + ")");
    }
    console.log("  Recovered params:", JSON.stringify(solved.params), "maxError:", solved.maxError);
}

console.log("=== Test 2: Terrain synthesis with flat landing pad (maxSlope) & inequality bounds ===");
{
    // Procedural terrain with controllable ridge amplitude, basin depth, and base elevation:
    // At landing pad around (0, 0), we want height = 10.0 and maxSlope <= 0.05,
    // while at outer rim (4, 0) we want minValue >= 14.0.
    const padTerrain = (x, z, ridgeAmp, baseHeight) =>
        baseHeight + ridgeAmp * (x * x + z * z) * 0.25;

    const solved = bro.gpu.solveProcedural(padTerrain, {
        targets: [
            { x: 0.0, z: 0.0, value: 10.0, maxSlope: 0.02 },
            { x: 0.05, z: -0.05, maxSlope: 0.05 },
            { x: 4.0, z: 0.0, minValue: 14.0, maxValue: 16.0 }
        ],
        initialParams: [0.1, 2.0],
        paramBounds: [[0.5, 3.0], [5.0, 15.0]],
        maxIterations: 50,
        tolerance: 1e-4
    });

    assert(solved && solved.ok === true, "pad terrain solve ok");
    assert(solved.converged === true, "pad terrain solve converged");
    const hCenter = solved.evaluate(0.0, 0.0);
    const hRim = solved.evaluate(4.0, 0.0);
    const gradCenter = solved.evalGrad(0.0, 0.0);
    const slopeCenter = Math.hypot(gradCenter.dx, gradCenter.dy);

    assertNear(hCenter, 10.0, 1e-2, "landing pad height at (0,0)");
    assert(slopeCenter <= 0.02 + 1e-3, "landing pad slope <= 0.02, got " + slopeCenter);
    assert(hRim >= 14.0 - 1e-3 && hRim <= 16.0 + 1e-3, "rim height within [14, 16], got " + hRim);
    console.log("  Landing pad height:", hCenter, "slope:", slopeCenter, "rim height:", hRim);
}

console.log("=== Test 3: 3D Signed Distance Field (bro.mesh.solveSDF) surface fitting ===");
{
    // Ellipsoidal / rounded SDF with controllable radii (rx, ry, rz)
    const sdfFn = (x, y, z, rx, ry, rz) =>
        Math.sqrt((x / rx) * (x / rx) + (y / ry) * (y / ry) + (z / rz) * (z / rz)) - 1.0;

    // Fit surface (sdf == 0) through (2.5, 0, 0), (0, 1.5, 0), (0, 0, 3.5)
    // and interior point (0, 0, 0) with sdf == -1.0
    const solved = bro.mesh.solveSDF(sdfFn, {
        surfaceConstraints: [
            { pos: [2.5, 0.0, 0.0], value: 0.0 },
            { pos: [0.0, 1.5, 0.0], value: 0.0 },
            { pos: [0.0, 0.0, 3.5], value: 0.0 },
            { pos: [0.0, 0.0, 0.0], value: -1.0 }
        ],
        initialParams: [1.0, 1.0, 1.0],
        paramBounds: [[0.5, 5.0], [0.5, 5.0], [0.5, 5.0]],
        maxIterations: 50,
        tolerance: 1e-4
    });

    assert(solved && solved.ok === true, "solveSDF returned ok");
    assert(solved.converged === true, "solveSDF converged");
    assertNear(solved.params[0], 2.5, 1e-2, "SDF rx == 2.5");
    assertNear(solved.params[1], 1.5, 1e-2, "SDF ry == 1.5");
    assertNear(solved.params[2], 3.5, 1e-2, "SDF rz == 3.5");
    assertNear(solved.evaluate(2.5, 0.0, 0.0), 0.0, 1e-3, "SDF zero-level at (2.5, 0, 0)");
    assertNear(solved.evaluate(0.0, 1.5, 0.0), 0.0, 1e-3, "SDF zero-level at (0, 1.5, 0)");
    assertNear(solved.evaluate(0.0, 0.0, 3.5), 0.0, 1e-3, "SDF zero-level at (0, 0, 3.5)");
    console.log("  Fitted SDF radii:", JSON.stringify(solved.params), "maxError:", solved.maxError);
}

console.log("=== Test 4: Discrete seed candidate selection + continuous parameter solve ===");
{
    const seededTerrain = (x, z, scale, seed) =>
        scale * Math.sin(x * 0.5 + seed * 1.1) + Math.cos(z * 0.5 + seed * 0.7);

    const trueScale = 2.75;
    const trueSeed = 3;

    const pts = [
        [0.5, 1.0],
        [1.8, -0.5],
        [-1.2, 2.1],
        [2.5, 1.4]
    ];
    const targets = pts.map(([x, z]) => ({
        x,
        z,
        value: seededTerrain(x, z, trueScale, trueSeed)
    }));

    const solved = bro.scene.solveProcedural(seededTerrain, {
        targets,
        initialParams: [1.0],
        paramBounds: [[0.5, 5.0]],
        seedRange: [1, 5],
        maxIterations: 40,
        tolerance: 1e-4
    });

    assert(solved && solved.ok === true, "seeded solveProcedural returned ok");
    assert(solved.converged === true, "seeded solveProcedural converged");
    assert(solved.bestSeed === trueSeed, "recovered bestSeed == 3, got " + solved.bestSeed);
    assertNear(solved.params[0], trueScale, 1e-2, "recovered scale with discrete seed");
    console.log("  Recovered seed:", solved.bestSeed, "scale:", solved.params[0]);
}

console.log("ALL INVERSE PROCEDURAL SOLVER TESTS PASSED");
process.exit(0);
