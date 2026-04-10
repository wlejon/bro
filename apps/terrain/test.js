// =============================================================================
// Terrain headless integration test
//
// Drives the same path the windowed app uses (FastNoise FBm → voxel grid →
// Mesh.greedyMesh → scene.createMesh transfer) and verifies it actually
// produces a renderable, non-empty world. Snaps a screenshot for visual
// regression.
//
// Run:
//   bro-headless apps/terrain apps/terrain/test.js
// =============================================================================

var passed = 0;
var failed = 0;

function check(label, cond) {
    if (cond) { passed++; console.log("  ok  " + label); }
    else      { failed++; console.log("FAIL  " + label); }
}

console.log("\n=== Terrain integration tests ===\n");

// Let app.js run buildWorld(seed=1337) and the first rAF tick.
advanceTime(50);

// -----------------------------------------------------------------------------
// 1. The app populated the scene
// -----------------------------------------------------------------------------
console.log("[1] Scene was populated");
{
    var canvas = document.getElementById('c');
    check("canvas exists", canvas !== null);

    var scene = canvas.getContext('scene');
    check("scene context exists", scene !== null);

    var kids = scene.root.children;
    check("scene.root.children is an array", Array.isArray(kids));
    // Expect one MeshNode per material that produced any voxels.
    // For seed 1337 every material should be present at the default chunk
    // settings, but allow >=2 to keep the test seed-tolerant.
    check("scene has at least 2 terrain meshes (got " + kids.length + ")",
        kids.length >= 2);

    // The names should follow the "terrain-<material>" convention.
    var sawTerrain = false;
    for (var i = 0; i < kids.length; i++) {
        if (kids[i].name && kids[i].name.indexOf('terrain-') === 0) {
            sawTerrain = true;
            break;
        }
    }
    check("at least one node named 'terrain-*'", sawTerrain);
}

// -----------------------------------------------------------------------------
// 2. Re-run the generation pipeline directly and verify it's deterministic
//    and structurally sane. This duplicates a small slice of app.js but lets
//    us assert against the *output*, not just the side effects.
// -----------------------------------------------------------------------------
console.log("\n[2] Pipeline determinism + structure");
{
    var CW = 32, CD = 32, CH = 24;

    var simplex = FastNoise.create('Simplex');
    var fbm = FastNoise.create('FractalFBm');
    fbm.set('Source', simplex);
    fbm.set('Octaves', 4);

    var raw1 = fbm.genUniformGrid2D(0, 0, CW, CD, 0.05, 7);
    var raw2 = fbm.genUniformGrid2D(0, 0, CW, CD, 0.05, 7);
    check("FBm 2D grid is deterministic", raw1.length === raw2.length);
    var same = true;
    for (var i = 0; i < raw1.length; i++) {
        if (raw1[i] !== raw2[i]) { same = false; break; }
    }
    check("FBm 2D values byte-equal across calls", same);

    // Build a tiny voxel chunk: column heights derived from raw1.
    // NB: must NOT use `var voxels` — that would clobber app.js's global
    // voxel grid (var is function-scoped). Use a distinct name.
    var testVoxels = new Uint8Array(CW * CH * CD);
    var solidCount = 0;
    for (var z = 0; z < CD; z++) {
        for (var x = 0; x < CW; x++) {
            var t = (raw1[z * CW + x] + 1) * 0.5;
            if (t < 0) t = 0; else if (t > 1) t = 1;
            var h = Math.floor(2 + t * (CH - 4));
            for (var y = 0; y <= h; y++) {
                testVoxels[(z * CH + y) * CW + x] = 1;
                solidCount++;
            }
        }
    }
    check("voxel grid has solid cells", solidCount > 0);

    // greedyMesh on the full grid should produce one mesh with a non-trivial
    // triangle count and a bounding box that lies inside the chunk.
    var mesh = Mesh.greedyMesh(testVoxels, CW, CH, CD, 1.0);
    check("greedyMesh produced a mesh", mesh.triangleCount > 0);
    check("greedyMesh produced normals", mesh.hasNormals);

    var bb = mesh.computeBBox();
    check("mesh bbox X within [0," + CW + "]",
        bb.min[0] >= -0.001 && bb.max[0] <= CW + 0.001);
    check("mesh bbox Y within [0," + CH + "]",
        bb.min[1] >= -0.001 && bb.max[1] <= CH + 0.001);
    check("mesh bbox Z within [0," + CD + "]",
        bb.min[2] >= -0.001 && bb.max[2] <= CD + 0.001);

    // Determinism: build it again and confirm same triangle count.
    var raw3 = fbm.genUniformGrid2D(0, 0, CW, CD, 0.05, 7);
    var testVoxels2 = new Uint8Array(CW * CH * CD);
    for (var z2 = 0; z2 < CD; z2++) {
        for (var x2 = 0; x2 < CW; x2++) {
            var t2 = (raw3[z2 * CW + x2] + 1) * 0.5;
            if (t2 < 0) t2 = 0; else if (t2 > 1) t2 = 1;
            var h2 = Math.floor(2 + t2 * (CH - 4));
            for (var y2 = 0; y2 <= h2; y2++) {
                testVoxels2[(z2 * CH + y2) * CW + x2] = 1;
            }
        }
    }
    var mesh2 = Mesh.greedyMesh(testVoxels2, CW, CH, CD, 1.0);
    check("greedy meshing is deterministic (tri count match)",
        mesh.triangleCount === mesh2.triangleCount);
}

// -----------------------------------------------------------------------------
// 3. Scene-graph round trip with the same chunk
// -----------------------------------------------------------------------------
console.log("\n[3] Scene-graph round trip");
{
    var canvas = document.getElementById('c');
    var scene = canvas.getContext('scene');

    // Build a small mesh and add it as a fresh node — verifies transfer:true
    // still works for greedyMesh output (this is the whole point of the
    // recently-fixed pipeline).
    //
    // NB: rename the local — `var voxels` would clobber app.js's global
    // voxel grid (JS var is function-scoped, not block-scoped) and the
    // next rebuildMeshes() would read garbage past the end.
    var smallVoxels = new Uint8Array(8 * 8 * 8);
    for (var z = 0; z < 8; z++)
        for (var x = 0; x < 8; x++)
            smallVoxels[(z * 8 + 0) * 8 + x] = 1;  // single ground layer

    var mesh = Mesh.greedyMesh(smallVoxels, 8, 8, 8, 1.0);
    check("small chunk meshed", mesh.triangleCount > 0);
    var triBefore = mesh.triangleCount;

    var node = scene.createMesh({
        mesh: mesh,
        transfer: true,
        x: 100, y: 100, z: 0,    // out of view, just verifying creation
        color: '#ff00ff',
        name: 'test-island'
    });
    check("createMesh returned a node", node && typeof node === 'object');
    check("source mesh neutered after transfer", mesh.triangleCount === 0);
    check("source mesh reads as empty", mesh.empty === true);

    var found = scene.findByName('test-island');
    check("scene.findByName located the new node", found !== null);

    if (node && node.destroy) node.destroy();
}

// -----------------------------------------------------------------------------
// 4. Block picking — mine + place via the camera-aligned raycast
//
// The app exposes pickAndEdit() on globalThis so headless tests can drive
// the same code path as a left/right click. We mine a block, verify the
// triangle count drops, place a stone block, verify it comes back, and
// confirm scene.raycast still hits the modified terrain.
// -----------------------------------------------------------------------------
console.log("\n[4] Block picking (mine + place)");
{
    var canvas = document.getElementById('c');
    var scene = canvas.getContext('scene');

    // Aim the camera straight down at the middle of the chunk so the
    // raycast lands on a known column. The 0.5 offsets keep the ray inside
    // a voxel cell instead of grazing corners (which precision-trips
    // bromesh::raycast on the voxel grid boundaries).
    cam.pos = [0.5, CHUNK_H + 5, 0.5];
    cam.yaw = 0;
    cam.pitch = -Math.PI / 2 + 0.001;   // straight down (avoid singular up)

    var trisBefore = lastStats.tris;
    check("scene has triangles before mining (" + trisBefore + ")", trisBefore > 0);

    // Mine: should remove the highest block in the center column.
    var mined = pickAndEdit('mine');
    check("mining returned true", mined === true);
    check("triangle count changed after mining",
        lastStats.tris !== trisBefore);

    // Mine a few more so the change is visible in the screenshot.
    for (var i = 0; i < 5; i++) pickAndEdit('mine');

    var trisAfterMine = lastStats.tris;
    check("multiple mines kept the world non-empty", trisAfterMine > 0);

    // Count solid voxels before placing — placing should add exactly one
    // (the tris count is an unreliable proxy here because greedy merging
    // can absorb a single new block by hiding neighbor faces).
    var solidBefore = 0;
    for (var i = 0; i < voxels.length; i++) if (voxels[i] !== 0) solidBefore++;

    // Place a block back. After mining straight-down, the new ray hits the
    // floor of the hole; placing puts a block on top of that floor.
    var placed = pickAndEdit('place', STONE);
    check("placing returned true", placed === true);

    var solidAfter = 0;
    for (var j = 0; j < voxels.length; j++) if (voxels[j] !== 0) solidAfter++;
    check("place added exactly one solid voxel (" + solidBefore + " → " + solidAfter + ")",
        solidAfter === solidBefore + 1);

    // Sanity: scene.raycast still hits the (modified) terrain.
    var hit = scene.raycast([0, CHUNK_H + 20, 0], [0, -1, 0], 200);
    check("scene.raycast still hits terrain after edits", hit !== null);
}

// -----------------------------------------------------------------------------
// 5. Visual smoke test
// -----------------------------------------------------------------------------
console.log("\n[5] Visual screenshot");
{
    // Pull the camera back to the default isometric framing for the shot.
    cam.pos = [50, 42, 50];
    cam.yaw = -Math.PI / 4;
    cam.pitch = -0.30;

    advanceTime(32);
    flush();
    screenshot('terrain.png');
    console.log("  screenshot: terrain.png");
}

// -----------------------------------------------------------------------------
// Summary
// -----------------------------------------------------------------------------
console.log("\n=== " + passed + " passed, " + failed + " failed ===\n");
assert(failed === 0, failed + " terrain integration test(s) failed");
