// Headless tests for FollowMeTool + Sketch.sweepProfile + app wiring.
// Run: bro-headless apps/scene-editor apps/scene-editor/test_followme.js

advanceTime(100);
flush();

const E = window.__editor;
assert(E, 'app handle exists');
assert(typeof FollowMeTool === 'object', 'FollowMeTool registered');
assert(typeof Sketch.sweepProfile === 'function', 'sweepProfile exists');

// --- Sketch.sweepProfile basic geometry ------------------------------------

// Square profile (CCW viewed from +Y) swept along +Y by 1 unit. Result is
// a unit cube without caps: 4 verts × 2 rings = 8 verts, 4 quads = 8 tris.
{
    const profile = [[-0.5, 0, -0.5], [0.5, 0, -0.5], [0.5, 0, 0.5], [-0.5, 0, 0.5]];
    const path    = [[0, 0, 0], [0, 1, 0]];
    const mesh = Sketch.sweepProfile(profile, [0, 1, 0], path);
    assert(mesh, 'mesh built');
    assert(mesh.positions.length === 8 * 3,
        `8 verts (got ${mesh.positions.length / 3})`);
    assert(mesh.indices.length === 8 * 3,
        `8 tris (got ${mesh.indices.length / 3})`);

    // Verts should fall in two y-planes: 0 and 1.
    let bottomCount = 0, topCount = 0;
    for (let i = 0; i < 8; i++) {
        const y = mesh.positions[i*3 + 1];
        if (Math.abs(y - 0) < 1e-5) bottomCount++;
        else if (Math.abs(y - 1) < 1e-5) topCount++;
    }
    assert(bottomCount === 4 && topCount === 4,
        `4 verts at y=0 + 4 at y=1 (got ${bottomCount} + ${topCount})`);

    // All triangle normals should be roughly horizontal (no caps emitted).
    for (let t = 0; t < 8; t++) {
        const i0 = mesh.indices[t*3]*3;
        const i1 = mesh.indices[t*3+1]*3;
        const i2 = mesh.indices[t*3+2]*3;
        const ax = mesh.positions[i1]   - mesh.positions[i0];
        const ay = mesh.positions[i1+1] - mesh.positions[i0+1];
        const az = mesh.positions[i1+2] - mesh.positions[i0+2];
        const bx = mesh.positions[i2]   - mesh.positions[i0];
        const by = mesh.positions[i2+1] - mesh.positions[i0+1];
        const bz = mesh.positions[i2+2] - mesh.positions[i0+2];
        const ny = ax*bz*0 + ay*bz - az*by;   // cross-y component
        // Side-face normal must be perpendicular to +Y (path dir).
        // |ny| / |normal| should be near 0.
        const nx = ay*bz - az*by;
        const nyy = az*bx - ax*bz;
        const nz = ax*by - ay*bx;
        const L = Math.hypot(nx, nyy, nz);
        const ny_unit = Math.abs(nyy) / L;
        assert(ny_unit < 1e-5, `tri ${t} side normal ⊥ +Y (got |ny|=${ny_unit})`);
    }
}

// L-bend path: profile sweeps through a corner. Bisector miter at the corner
// should keep the corner ring's plane bisecting the two segments.
{
    const profile = [[-0.5, -0.5, 0], [0.5, -0.5, 0], [0.5, 0.5, 0], [-0.5, 0.5, 0]];
    const path    = [[0, 0, 0], [1, 0, 0], [1, 1, 0]];   // +X then +Y
    const mesh = Sketch.sweepProfile(profile, [0, 0, 1], path);
    assert(mesh, 'L-bend sweep built');
    // 3 rings × 4 verts = 12 verts. 2 segments × 4 quads = 8 quads = 16 tris.
    assert(mesh.positions.length === 12 * 3,
        `12 verts (got ${mesh.positions.length / 3})`);
    assert(mesh.indices.length === 16 * 3,
        `16 tris (got ${mesh.indices.length / 3})`);

    // Middle ring (verts 4..7) should have its centroid at (1, 0, 0).
    let cx = 0, cy = 0, cz = 0;
    for (let i = 4; i < 8; i++) {
        cx += mesh.positions[i*3];
        cy += mesh.positions[i*3 + 1];
        cz += mesh.positions[i*3 + 2];
    }
    cx /= 4; cy /= 4; cz /= 4;
    assert(Math.abs(cx - 1) < 1e-5 && Math.abs(cy) < 1e-5 && Math.abs(cz) < 1e-5,
        `middle ring centroid at corner (got ${cx},${cy},${cz})`);
}

// Degenerate inputs.
assert(Sketch.sweepProfile([[0,0,0]], [0,1,0], [[0,0,0],[0,1,0]]) === null,
    'profile <3 verts ⇒ null');
assert(Sketch.sweepProfile(
    [[0,0,0],[1,0,0],[1,1,0]], [0,0,1], [[0,0,0]]) === null,
    'path <2 pts ⇒ null');

// --- App wiring -------------------------------------------------------------

{
    // Build a path: a 3-point polyline along +Y on the side of the cube.
    const positions = new Float32Array([
        2, 0, 0,
        2, 1, 0,
        3, 1, 0,
    ]);
    const edges = [{ a: 0, b: 1 }, { a: 1, b: 2 }];
    const pathPrim = E.registry.createEdgePrimitive(
        { name: 'TestPath' }, { positions, edges });

    // edgePrimitiveToPath should yield 3 ordered points.
    const pts = E.edgePrimitiveToPath(pathPrim);
    assert(pts && pts.length === 3, `path has 3 pts (got ${pts ? pts.length : 'null'})`);

    // Use the cube top face as the profile.
    const cube = E.registry.primitives[0];
    const top = cube.faceGroups.groups.findIndex(g => g.normal[1] > 0.999);

    const before = E.registry.primitives.length;
    const id = E.runFollowMe(cube, top, pathPrim);
    assert(id != null, 'runFollowMe returned id');
    const after = E.registry.primitives.length;
    assert(after === before + 1,
        `sweep added one primitive (was ${before}, now ${after})`);

    const newPrim = E.registry.getById(id);
    assert(newPrim && newPrim.kind === 'primitive', 'new primitive present');
    // 3 rings × 4 verts = 12 verts.
    assert(newPrim.positions.length === 12 * 3,
        `12 verts on swept primitive (got ${newPrim.positions.length / 3})`);

    // Undo + redo.
    E.history.undo();
    assert(!E.registry.getById(id), 'undo removes sweep primitive');
    E.history.redo();
    assert(E.registry.getById(id), 'redo restores sweep primitive');

    // Cleanup.
    E.registry.remove(id);
    E.registry.remove(pathPrim.id);
}

// --- edgePrimitiveToPath handles unordered edges ----------------------------

{
    // Polyline 0 → 1 → 2 → 3 supplied as edges in shuffled order.
    const positions = new Float32Array([0,0,0, 1,0,0, 2,0,0, 3,0,0]);
    const edges = [{ a: 2, b: 3 }, { a: 0, b: 1 }, { a: 1, b: 2 }];
    const ep = E.registry.createEdgePrimitive(
        { name: 'shuffled' }, { positions, edges });
    const path = E.edgePrimitiveToPath(ep);
    assert(path.length === 4, `walked all 4 verts (got ${path.length})`);
    // Endpoints must be (0,0,0) and (3,0,0).
    const startX = path[0][0];
    const endX   = path[3][0];
    assert((startX === 0 && endX === 3) || (startX === 3 && endX === 0),
        `path endpoints are 0 and 3 (got ${startX} and ${endX})`);
    E.registry.remove(ep.id);
}

console.log('OK — Follow-Me: sweepProfile straight + L-bend, edge-prim-to-path, ' +
            'app wiring, undo/redo, unordered edge walk');
