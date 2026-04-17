// Smoke test for the new SketchUp-style push/pull (additive surgery, no warp).
// Verifies the basic flows work without crashing and produce sane geometry.
//
// Run: bro-headless apps/scene-editor apps/scene-editor/test_pushpull_smoke.js

'use strict';

const E   = window.__editor;
const reg = E.registry;

let tests = 0, failed = 0;
function t(name, fn) {
    tests++;
    try { fn(); console.log('  ok   ' + name); }
    catch (e) {
        failed++;
        console.log('  FAIL ' + name + ': ' + (e && e.message ? e.message : e));
        if (e && e.stack) console.log(e.stack);
    }
}
function eq(a, b, msg) {
    const ja = JSON.stringify(a), jb = JSON.stringify(b);
    if (ja !== jb) throw new Error((msg || 'eq') + ': ' + ja + ' !== ' + jb);
}
function near(a, b, eps, msg) {
    if (Math.abs(a - b) > (eps || 1e-5)) {
        throw new Error((msg || 'near') + ': ' + a + ' vs ' + b);
    }
}
function truthy(v, msg) { if (!v) throw new Error(msg || 'expected truthy'); }
function hasVertAt(prim, x, y, z, eps) {
    eps = eps || 1e-3;
    const P = prim.positions;
    for (let vi = 0; vi < P.length / 3; vi++) {
        if (Math.abs(P[vi*3] - x) < eps &&
            Math.abs(P[vi*3+1] - y) < eps &&
            Math.abs(P[vi*3+2] - z) < eps) return true;
    }
    return false;
}

function pushPullGroup(prim, groupIdx, distance) {
    const g = prim.faceGroups.groups[groupIdx];
    const tri0 = g.tris[0];
    const I = prim.indices, P = prim.positions;
    const c = [0, 0, 0];
    for (let k = 0; k < 3; k++) {
        const vi = I[tri0 * 3 + k];
        c[0] += P[vi*3]; c[1] += P[vi*3+1]; c[2] += P[vi*3+2];
    }
    c[0] /= 3; c[1] /= 3; c[2] /= 3;
    E.setTool('pushpull');
    E.beginPushPullOn(prim, {
        triangleIndex: tri0,
        position: c,
        normal: g.normal.slice(),
        distance: 0,
    });
    E.applyPushPull(distance);
    E.commitPushPull();
}

// -------------------------------------------------------------------------
// Box top pull
// -------------------------------------------------------------------------

t('box: pull +Y top by 0.5 → 4 new verts at y=1.5; box still closed', () => {
    reg.clear();
    reg.create({ type: 'box', name: 'box', params: { sx: 1, sy: 1, sz: 1 } });
    const box = reg.primitives[0];
    reg.setActive(box.id);
    const topG = box.faceGroups.groups.findIndex(g => g.normal[1] > 0.99);
    pushPullGroup(box, topG, 0.5);
    // 4 new top corners at y=1.5
    truthy(hasVertAt(box, +1, 1.5, +1));
    truthy(hasVertAt(box, +1, 1.5, -1));
    truthy(hasVertAt(box, -1, 1.5, +1));
    truthy(hasVertAt(box, -1, 1.5, -1));
    // Bottom unchanged.
    truthy(hasVertAt(box, +1, -1, +1));
    truthy(hasVertAt(box, -1, -1, -1));
    // Closed manifold check via EditMesh.
    const em = EditMesh.fromMeshData(box.positions, box.indices);
    const val = EditMesh.validate(em);
    truthy(val.ok, 'manifold ok: ' + val.errors.join('; '));
    truthy(val.isClosed,
           'box stays closed (boundary=' + val.boundaryHalfEdges + ')');
});

// -------------------------------------------------------------------------
// Sketch face flat extrude — back face must be created
// -------------------------------------------------------------------------

t('rectangle pulled +Y by 0.75 → closed slab', () => {
    reg.clear();
    E.setTool('rectangle');
    E.beginRectangle([0, 0, 0]);
    E.updateRectangleAt([2, 0, 3]);
    E.commitRectangle();
    const rect = reg.primitives[reg.primitives.length - 1];
    reg.setActive(rect.id);
    pushPullGroup(rect, 0, 0.75);
    // Should have verts at y=0.75 (front) AND y=0 (back).
    truthy(hasVertAt(rect, 0, 0.75, 0));
    truthy(hasVertAt(rect, 2, 0.75, 3));
    truthy(hasVertAt(rect, 0, 0, 0));
    truthy(hasVertAt(rect, 2, 0, 3));
    const em = EditMesh.fromMeshData(rect.positions, rect.indices);
    const val = EditMesh.validate(em);
    truthy(val.ok, 'manifold ok: ' + val.errors.join('; '));
    truthy(val.isClosed,
           'slab is closed (boundary=' + val.boundaryHalfEdges + ')');
});

// -------------------------------------------------------------------------
// Cylinder cap pull (height grows, side facets stretch)
// -------------------------------------------------------------------------

t('cylinder top cap pulled +Y by 0.5 → side facets stretch, manifold stays closed', () => {
    reg.clear();
    reg.create({ type: 'cylinder', name: 'cyl',
                 params: { r: 1, h: 1, seg: 16 } });
    const cyl = reg.primitives[0];
    reg.setActive(cyl.id);
    const topG = cyl.faceGroups.groups.findIndex(g => g.normal[1] > 0.99);
    pushPullGroup(cyl, topG, 0.5);
    // Top rim moves to y = halfHeight + 0.5 = 1.5; original rim verts stay
    // at y=1 (now used by stretched side-facet bridges, not the moved cap).
    truthy(hasVertAt(cyl, 1, 1.5, 0), 'rim vert at (1, 1.5, 0) exists');
    // Side facets should extend up to 1.5 (vertical geometry between -1 and 1.5).
    const em = EditMesh.fromMeshData(cyl.positions, cyl.indices);
    const val = EditMesh.validate(em);
    truthy(val.ok, 'manifold ok: ' + val.errors.join('; '));
    truthy(val.isClosed,
           'cylinder stays closed (boundary=' + val.boundaryHalfEdges + ')');
});

// -------------------------------------------------------------------------
// Cylinder side facet pull — the bug-fix scenario
// -------------------------------------------------------------------------

t('cylinder side facet pulled +X by 0.5 → cap stays flat, no flipped tris', () => {
    reg.clear();
    reg.create({ type: 'cylinder', name: 'cyl',
                 params: { r: 1, h: 1, seg: 16 } });
    const cyl = reg.primitives[0];
    reg.setActive(cyl.id);
    // Pick the side facet whose normal is most +X.
    let sideG = -1, bestX = -Infinity;
    for (let i = 0; i < cyl.faceGroups.groups.length; i++) {
        const n = cyl.faceGroups.groups[i].normal;
        if (Math.abs(n[1]) > 0.1) continue;
        if (n[0] > bestX) { bestX = n[0]; sideG = i; }
    }
    truthy(sideG >= 0);
    pushPullGroup(cyl, sideG, 0.5);

    // Cap stays flat — every cap-vert at y=1 should still be at y=1 (we
    // never moved cap verts). The pulled facet's NEW corners are at y=1
    // too (only x/z shifted), but the cap tris also still use the originals.
    let capVertsBelowOne = 0, capVertsAboveOne = 0;
    for (let vi = 0; vi < cyl.positions.length / 3; vi++) {
        const y = cyl.positions[vi * 3 + 1];
        if (y > 1.0001) capVertsAboveOne++;
        else if (y < 0.9999 && y > 0.5) capVertsBelowOne++;
    }
    eq(capVertsAboveOne, 0, 'no verts above y=1 (cap flat)');

    // No flipped cap triangles — every top-cap tri's winding-normal Y > 0.
    const topG = cyl.faceGroups.groups.findIndex(g => g.normal[1] > 0.99);
    let flipped = 0;
    for (const ti of cyl.faceGroups.groups[topG].tris) {
        const i0 = cyl.indices[ti*3], i1 = cyl.indices[ti*3+1], i2 = cyl.indices[ti*3+2];
        const ax = cyl.positions[i0*3], az = cyl.positions[i0*3+2];
        const bx = cyl.positions[i1*3], bz = cyl.positions[i1*3+2];
        const cx = cyl.positions[i2*3], cz = cyl.positions[i2*3+2];
        const ex1 = bx - ax, ez1 = bz - az;
        const ex2 = cx - ax, ez2 = cz - az;
        const cy = ez1 * ex2 - ex1 * ez2;
        if (cy < -1e-5) flipped++;
    }
    eq(flipped, 0, 'no flipped cap tris (was the bug)');

    // Manifold check.
    const em = EditMesh.fromMeshData(cyl.positions, cyl.indices);
    const val = EditMesh.validate(em);
    truthy(val.ok, 'manifold ok: ' + val.errors.join('; '));
    truthy(val.isClosed,
           'cylinder closed after side pull (boundary=' +
           val.boundaryHalfEdges + ')');
});

console.log(`\n${tests - failed}/${tests} passed`);
if (failed > 0) throw new Error(`${failed} test(s) failed`);
