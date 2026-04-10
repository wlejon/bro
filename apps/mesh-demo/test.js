// =============================================================================
// Mesh + FastNoise integration tests (run from bro-headless)
//
// Runs end-to-end "round trips" against the bromesh integration:
//
//   FastNoise (brokit)  →  Float32Array field  →  Mesh.marchingCubes
//                                              →  Mesh analysis (bbox/volume/raycast)
//                                              →  scene.createMesh + scene.raycast
//
// The point is integration coverage, not unit-test fidelity — every assertion
// crosses at least one engine boundary (JS → C++, brokit → bromesh, mesh → scene).
//
// Run:
//   bro-headless apps/mesh-demo apps/mesh-demo/test.js
// =============================================================================

var passed = 0;
var failed = 0;

function check(label, cond) {
    if (cond) {
        passed++;
        console.log("  ok  " + label);
    } else {
        failed++;
        console.log("FAIL  " + label);
    }
}

// Headless `assert` exits 1 on failure; we use `check` for soft-pass logging
// and call assert() at the end so a single failure still surfaces in CI.

console.log("\n=== Mesh + FastNoise integration tests ===\n");

// Let the host app finish loading scene 0.
advanceTime(50);

// -----------------------------------------------------------------------------
// 1. Mesh primitive sanity — proves the JS → bromesh path is wired up at all
// -----------------------------------------------------------------------------
console.log("[1] Mesh primitives");
{
    var box = Mesh.box(1, 1, 1);
    check("box has 12 triangles (6 faces × 2)", box.triangleCount === 12);
    check("box has normals", box.hasNormals);

    var bb = box.computeBBox();
    check("box bbox.min ≈ [-1,-1,-1]",
        Math.abs(bb.min[0] + 1) < 1e-4 &&
        Math.abs(bb.min[1] + 1) < 1e-4 &&
        Math.abs(bb.min[2] + 1) < 1e-4);
    check("box bbox.max ≈ [+1,+1,+1]",
        Math.abs(bb.max[0] - 1) < 1e-4 &&
        Math.abs(bb.max[1] - 1) < 1e-4 &&
        Math.abs(bb.max[2] - 1) < 1e-4);

    // 2*2*2 = 8
    check("box volume ≈ 8", Math.abs(box.computeVolume() - 8) < 1e-3);
    check("box is manifold", box.isManifold());
}

// -----------------------------------------------------------------------------
// 2. Analytic SDF → marching cubes — proves Mesh.marchingCubes itself works.
//
// Field is `R - distance`, so the iso=0 surface is a sphere of radius R.
// We then check that:
//   - the resulting mesh is in the right place
//   - its bounding box matches the analytic sphere
//   - its volume matches (4/3)πR³ within a few percent
//   - it has the inside/outside topology we expect (ray from outside hits)
// -----------------------------------------------------------------------------
console.log("\n[2] Analytic sphere SDF → marching cubes");
{
    var N = 32;
    var R = 0.6 * (N / 2);  // radius in voxel units
    var field = new Float32Array(N * N * N);
    for (var z = 0; z < N; z++) {
        for (var y = 0; y < N; y++) {
            for (var x = 0; x < N; x++) {
                var dx = x - N/2, dy = y - N/2, dz = z - N/2;
                field[z*N*N + y*N + x] = R - Math.sqrt(dx*dx + dy*dy + dz*dz);
            }
        }
    }
    var mesh = Mesh.marchingCubes(field, N, N, N, 0, 1.0);
    check("MC produced triangles", mesh.triangleCount > 100);

    // BBox in voxel coords (cellSize=1) — sphere centered at (N/2,N/2,N/2),
    // so each axis range is roughly (N/2 - R, N/2 + R), full extent ~2R.
    var bb = mesh.computeBBox();
    check("MC sphere extent X ≈ 2R (got " + bb.extentX.toFixed(2) +
          " vs " + (2*R).toFixed(2) + ")", Math.abs(bb.extentX - 2*R) < 2.0);
    check("MC sphere extent Y ≈ 2R", Math.abs(bb.extentY - 2*R) < 2.0);
    check("MC sphere extent Z ≈ 2R", Math.abs(bb.extentZ - 2*R) < 2.0);

    // Now that marching cubes welds shared-edge vertices, the output is a
    // proper closed manifold and computeVolume returns the actual signed
    // tetrahedron sum.
    check("MC sphere is manifold", mesh.isManifold());
    var expectedVol = (4/3) * Math.PI * R * R * R;
    var vol = mesh.computeVolume();
    var err = Math.abs(vol - expectedVol) / expectedVol;
    check("MC sphere volume within 5% of (4/3)πR³ (got " +
          vol.toFixed(2) + " vs " + expectedVol.toFixed(2) +
          ", err " + (err*100).toFixed(2) + "%)",
          err < 0.05);

    // Centered ray from outside should hit the mesh
    mesh.computeNormals();
    var hit = mesh.raycast([N/2, N/2, N + 10], [0, 0, -1]);
    check("ray from outside hits MC sphere", hit !== null);
    if (hit) {
        // First hit Z must be near (N/2 + R), the front face of the sphere
        check("ray hit Z ≈ front face",
            Math.abs(hit.position[2] - (N/2 + R)) < 1.0);
    }
}

// -----------------------------------------------------------------------------
// 3. FastNoise basics — determinism, range, single↔grid coherence.
// -----------------------------------------------------------------------------
console.log("\n[3] FastNoise — determinism and range");
{
    var N = 24;
    var noise = FastNoise.create("Simplex");

    // Same call twice → byte-identical output (no hidden state).
    var g1 = noise.genUniformGrid3D(0, 0, 0, N, N, N, 0.05, 1337);
    var g2 = noise.genUniformGrid3D(0, 0, 0, N, N, N, 0.05, 1337);
    check("genUniformGrid3D length matches request", g1.length === N*N*N);
    var sameBytes = true;
    for (var i = 0; i < g1.length; i++) {
        if (g1[i] !== g2[i]) { sameBytes = false; break; }
    }
    check("genUniformGrid3D is deterministic across calls", sameBytes);

    // Range — Simplex output should sit in roughly [-1, 1] with both signs present.
    var minV = Infinity, maxV = -Infinity;
    for (var j = 0; j < g1.length; j++) {
        if (g1[j] < minV) minV = g1[j];
        if (g1[j] > maxV) maxV = g1[j];
    }
    check("noise spans both signs (min=" + minV.toFixed(3) +
          " max=" + maxV.toFixed(3) + ")",
          minV < 0 && maxV > 0);
    check("noise range ⊂ [-1.5, 1.5]", minV > -1.5 && maxV < 1.5);

    // Different seed → different output
    var g3 = noise.genUniformGrid3D(0, 0, 0, N, N, N, 0.05, 9999);
    var anyDiff = false;
    for (var k = 0; k < g1.length; k++) {
        if (g1[k] !== g3[k]) { anyDiff = true; break; }
    }
    check("different seed produces different field", anyDiff);
}

// -----------------------------------------------------------------------------
// 4. FastNoise → marching cubes round trip
//
// The "round trip integration" the user asked for: drive bromesh's MC straight
// from a brokit FastNoise field and verify the resulting mesh is structurally
// sane (non-empty, contained within the source grid, manifold-ish).
// -----------------------------------------------------------------------------
console.log("\n[4] FastNoise → marching cubes round trip");
{
    var N = 24;
    var noise = FastNoise.create("FractalFBm");
    noise.set("Source", FastNoise.create("Simplex"));
    noise.set("Octaves", 4);
    noise.set("Gain", 0.5);
    noise.set("Lacunarity", 2.0);

    var field = noise.genUniformGrid3D(0, 0, 0, N, N, N, 0.05, 1337);
    check("noise field length = N³", field.length === N*N*N);

    var mesh = Mesh.marchingCubes(field, N, N, N, 0.0, 1.0);
    check("noise → MC produced a non-empty mesh (" +
          mesh.triangleCount + " tris)", mesh.triangleCount > 0);
    check("noise → MC produced vertices", mesh.vertexCount > 0);
    check("noise → MC produced manifold mesh", mesh.isManifold());

    // Output bbox lies inside the grid + 1 cell of boundary-closing pad on
    // each side (allow [-1, N] in voxel coords).
    var bb = mesh.computeBBox();
    check("MC mesh fits inside padded grid X",
        bb.min[0] >= -1 && bb.max[0] <= N);
    check("MC mesh fits inside padded grid Y",
        bb.min[1] >= -1 && bb.max[1] <= N);
    check("MC mesh fits inside padded grid Z",
        bb.min[2] >= -1 && bb.max[2] <= N);

    // Determinism end-to-end: same noise + seed → same triangle count.
    var field2 = noise.genUniformGrid3D(0, 0, 0, N, N, N, 0.05, 1337);
    var mesh2 = Mesh.marchingCubes(field2, N, N, N, 0.0, 1.0);
    check("end-to-end determinism (same tri count)",
        mesh.triangleCount === mesh2.triangleCount);
}

// -----------------------------------------------------------------------------
// 5. Calculable noise → predictable mesh
//
// FastNoise.Constant produces a uniform field. The iso threshold determines
// whether the entire grid is "inside" (above iso) or "outside" (below iso).
// With closeBoundary on (the default), an all-inside field produces a closed
// skin around the grid; an all-outside field produces no surface at all.
// This is the cleanest "we know what shape this noise is, verify the mesh
// library agrees" test.
// -----------------------------------------------------------------------------
console.log("\n[5] Constant FastNoise → predictable mesh");
{
    var N = 16;
    var c = FastNoise.create("Constant");
    c.set("Value", 1.0);

    var grid = c.genUniformGrid3D(0, 0, 0, N, N, N, 1, 0);
    var allOne = true;
    for (var i = 0; i < grid.length; i++) {
        if (grid[i] !== 1.0) { allOne = false; break; }
    }
    check("Constant(1.0) yields uniform field", allOne);

    // iso=0: every cell is +1 (inside) → boundary-close produces a closed
    // skin running along all six faces of the grid (one quad per boundary
    // cell, not a single bounding box). The result should be:
    //   - non-empty
    //   - manifold (closed)
    //   - bbox spans the grid (with a half-cell sentinel offset on each side)
    //   - volume ≈ N³ (cubic skin enclosing the grid interior)
    var inside = Mesh.marchingCubes(grid, N, N, N, 0.0, 1.0);
    check("uniform-positive field produces closed skin", inside.triangleCount > 0);
    check("grid skin is manifold", inside.isManifold());

    var skinBB = inside.computeBBox();
    check("skin bbox X spans full grid",
          skinBB.min[0] < 0 && skinBB.max[0] > (N - 1));
    check("skin bbox Y spans full grid",
          skinBB.min[1] < 0 && skinBB.max[1] > (N - 1));
    check("skin bbox Z spans full grid",
          skinBB.min[2] < 0 && skinBB.max[2] > (N - 1));

    // The skin sits at the half-cell boundary, so the enclosed volume is
    // (N)³ (N+1 vertices but cells of size 1 → N units across). Allow ±5%.
    var skinVol = inside.computeVolume();
    var expectedSkinVol = N * N * N;
    check("skin volume ≈ N³ (got " + skinVol.toFixed(0) +
          " vs " + expectedSkinVol + ")",
          Math.abs(skinVol - expectedSkinVol) / expectedSkinVol < 0.05);

    // iso=2: every cell is below threshold → entire grid is "outside" → no
    // surface anywhere, including the closed boundary.
    var outside = Mesh.marchingCubes(grid, N, N, N, 2.0, 1.0);
    check("uniform-below-iso field yields 0 triangles",
        outside.triangleCount === 0);
}

// -----------------------------------------------------------------------------
// 6. Noise-perturbed sphere — analytic shape + small noise → still ≈ sphere
//
// Mirrors the pattern from mesh-demo scene 6 (which uses Math.sin) but uses
// FastNoise as the perturbation. Volume should still be close to the
// unperturbed sphere; this exercises the full pipeline with both noise libs
// and the analysis code together.
// -----------------------------------------------------------------------------
console.log("\n[6] Noise-perturbed sphere SDF");
{
    var N = 32;
    var R = 0.55 * N;  // big enough for noise perturbation to matter

    var noise = FastNoise.create("Simplex");
    var noiseGrid = noise.genUniformGrid3D(0, 0, 0, N, N, N, 0.08, 7);

    var field = new Float32Array(N*N*N);
    for (var z = 0; z < N; z++) {
        for (var y = 0; y < N; y++) {
            for (var x = 0; x < N; x++) {
                var dx = x - N/2, dy = y - N/2, dz = z - N/2;
                var sdf = R - Math.sqrt(dx*dx + dy*dy + dz*dz);
                var i = z*N*N + y*N + x;
                field[i] = sdf + noiseGrid[i] * 0.6;  // small bumps
            }
        }
    }

    var mesh = Mesh.marchingCubes(field, N, N, N, 0, 1.0);
    check("perturbed sphere has triangles", mesh.triangleCount > 200);
    check("perturbed sphere is manifold", mesh.isManifold());

    // Full extent should still be near 2R — small bumps don't push the bbox
    // far past the underlying sphere.
    var bb = mesh.computeBBox();
    check("perturbed extent X within 30% of 2R (got " +
          bb.extentX.toFixed(2) + ")",
          Math.abs(bb.extentX - 2*R) / (2*R) < 0.30);
    check("perturbed extent Y within 30% of 2R",
          Math.abs(bb.extentY - 2*R) / (2*R) < 0.30);
    check("perturbed extent Z within 30% of 2R",
          Math.abs(bb.extentZ - 2*R) / (2*R) < 0.30);

    var refVol = (4/3) * Math.PI * R * R * R;
    var vol = mesh.computeVolume();
    var err = Math.abs(vol - refVol) / refVol;
    check("perturbed volume within 25% of clean sphere (" +
          vol.toFixed(2) + " vs " + refVol.toFixed(2) +
          ", err " + (err*100).toFixed(1) + "%)",
          err < 0.25);
}

// -----------------------------------------------------------------------------
// 7. Scene graph integration — Mesh → scene.createMesh → scene.raycast
//
// The final integration hop: drop a Mesh into the scene graph (via the new
// transfer:true zero-copy path) and confirm scene.raycast can find it. This
// is the path the deleted terrain app was using; if this round-trips, the
// engine-side wiring is sound.
// -----------------------------------------------------------------------------
console.log("\n[7] Scene graph integration");
{
    var canvas = document.getElementById('canvas');
    var scene = canvas.getContext('scene');

    // Camera so the test isn't dependent on whatever scene mesh-demo loaded.
    scene.setCamera({
        fov: 50,
        position: [0, 0, 10],
        target: [0, 0, 0],
        aspect: canvas.clientWidth / canvas.clientHeight
    });

    // Build a sphere via Mesh, then transfer it to a scene node.
    var sphere = Mesh.sphere(1.0, 24, 16);
    var triCount = sphere.triangleCount;
    check("source sphere has triangles", triCount > 0);

    var node = scene.createMesh({
        mesh: sphere,
        transfer: true,
        x: 0, y: 0, z: 0,
        color: 'red',
        name: 'integration-sphere'
    });
    check("scene.createMesh returned a node", node && typeof node === 'object');
    // After transfer, the source Mesh wrapper has its MeshData moved out.
    // The neutered Mesh reads as logically empty (triangleCount === 0),
    // matching postMessage transferList semantics.
    check("transferred Mesh has 0 triangles", sphere.triangleCount === 0);
    check("transferred Mesh reads as empty", sphere.empty === true);

    // scene.raycast returns { hit, distance, position, point, normal, node }.
    // `position` is the natural name (matches Mesh.raycast); `point` is kept
    // as a backwards-compatible alias.
    var hit = scene.raycast([0, 0, 5], [0, 0, -1]);
    check("scene.raycast hits the transferred sphere", hit !== null);
    if (hit) {
        check("scene.raycast position z ≈ 1.0 (got " +
              hit.position[2].toFixed(3) + ")",
            Math.abs(hit.position[2] - 1.0) < 0.1);
        check("scene.raycast hit normal points +Z",
            hit.normal && hit.normal[2] > 0.5);
    }

    // updateMesh round-trip with a fresh primitive.
    var newGeom = Mesh.box(0.5, 0.5, 0.5);
    node.updateMesh(newGeom, { transfer: true });
    check("updateMesh consumed the source Mesh (now 0 tris)",
        newGeom.triangleCount === 0);

    // Raycast again — now the box should be the hit target.
    var hit2 = scene.raycast([0, 0, 5], [0, 0, -1]);
    check("scene.raycast hits after updateMesh", hit2 !== null);
    if (hit2) {
        check("updated box hit z ≈ 0.5 (got " + hit2.position[2].toFixed(3) + ")",
            Math.abs(hit2.position[2] - 0.5) < 0.1);
    }

    node.destroy();
}

// -----------------------------------------------------------------------------
// 8. End-to-end visual: render a noise → MC mesh and screenshot it
// -----------------------------------------------------------------------------
console.log("\n[8] Visual smoke test");
{
    var canvas = document.getElementById('canvas');
    var scene = canvas.getContext('scene');

    // Clear whatever the host app put in the scene so the screenshot only
    // shows our noise blob.
    var root = scene.root;
    var kidsBefore = root.children;
    check("scene.root.children is a real array", Array.isArray(kidsBefore));
    for (var i = 0; i < kidsBefore.length; i++) {
        if (kidsBefore[i] && kidsBefore[i].destroy) kidsBefore[i].destroy();
    }
    check("scene was cleared", root.childCount === 0);

    var N = 32;
    var noise = FastNoise.create("FractalFBm");
    noise.set("Source", FastNoise.create("Simplex"));
    noise.set("Octaves", 4);
    var field = noise.genUniformGrid3D(0, 0, 0, N, N, N, 0.06, 1337);

    // Threshold around the noise mean so we get a non-empty surface.
    var sum = 0;
    for (var k = 0; k < field.length; k++) sum += field[k];
    var mean = sum / field.length;

    var blob = Mesh.marchingCubes(field, N, N, N, mean, 1.0);
    blob.center();
    blob.computeNormals();
    check("rendered noise blob has triangles", blob.triangleCount > 0);
    check("rendered noise blob is manifold", blob.isManifold());

    scene.setCamera({
        fov: 50,
        position: [0, 0, N * 1.4],
        target: [0, 0, 0],
        aspect: canvas.clientWidth / canvas.clientHeight
    });
    var node = scene.createMesh({ mesh: blob, transfer: true, color: '#3498db' });

    advanceTime(32);
    flush();
    screenshot('mesh-noise-integration.png');
    console.log("  screenshot: mesh-noise-integration.png");

    node.destroy();
}

// -----------------------------------------------------------------------------
// Summary
// -----------------------------------------------------------------------------
console.log("\n=== " + passed + " passed, " + failed + " failed ===\n");
assert(failed === 0, failed + " mesh integration test(s) failed");
