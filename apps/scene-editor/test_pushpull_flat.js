// Integration test: push/pull on a flat (rectangle-tool) sketch face.
//
// SketchUp-style surgery handles this uniformly with all other push/pulls:
// the rectangle is detected as a sketch face (group covers the whole mesh)
// and the surgery emits a back-face copy in addition to the wall bridges,
// closing the slab into a manifold.
//
// Run: bro-headless apps/scene-editor apps/scene-editor/test_pushpull_flat.js

'use strict';

const E   = window.__editor;
const reg = E.registry;
const h   = E.history;

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
function falsy(v, msg)  { if (v)  throw new Error(msg || 'expected falsy'); }

// Build a fresh rectangle-only scene for each test.
function rectSetup(x1, z1, x2, z2) {
    // Cancel any leftover push/pull from a failed previous test BEFORE
    // clearing the registry — otherwise its primitive reference goes stale.
    if (E.pushpull && E.pushpull.active) E.cancelPushPull();
    reg.clear();
    h.clear();
    E.setTool('rectangle');
    E.beginRectangle([x1, 0, z1]);
    E.updateRectangleAt([x2, 0, z2]);
    E.commitRectangle();
    E.setTool('pushpull');
    const rect = reg.primitives[reg.primitives.length - 1];
    reg.setActive(rect.id);
    return rect;
}

// Synthesize a begin-push-pull hit for the flat rect's first triangle.
function hitOnFront(prim) {
    const P = prim.positions;
    const I = prim.indices;
    const t0 = 0;
    const c = [0, 0, 0];
    for (let k = 0; k < 3; k++) {
        const vi = I[t0 * 3 + k];
        c[0] += P[vi * 3 + 0];
        c[1] += P[vi * 3 + 1];
        c[2] += P[vi * 3 + 2];
    }
    c[0] /= 3; c[1] /= 3; c[2] /= 3;
    return {
        triangleIndex: t0,
        position: c,
        normal: prim.faceGroups.groups[0].normal.slice(),
        distance: 0,
    };
}

// -------------------------------------------------------------------------
// Pre-conditions: the rectangle tool really produces a flat sketch face.
// -------------------------------------------------------------------------

t('rectangle commit yields 4 verts / 2 tris / 1 face group', () => {
    const r = rectSetup(0, 0, 2, 3);
    eq(r.positions.length / 3, 4, 'verts');
    eq(r.indices.length / 3, 2, 'tris');
    eq(r.faceGroups.groups.length, 1, 'single face group');
    near(r.faceGroups.groups[0].normal[1], 1, 1e-5, 'normal is +Y');
});

// -------------------------------------------------------------------------
// beginPushPull on a flat face — surgery handles it uniformly.
// -------------------------------------------------------------------------

t('beginPushPull on flat rect arms surgery (no special flatExtrude flag)', () => {
    const r = rectSetup(0, 0, 2, 3);
    E.beginPushPull(hitOnFront(r));
    truthy(E.pushpull.active);
    eq(E.pushpull.groupIdx, 0, 'bound to the only face group');
    // The drag axis is the face normal.
    near(E.pushpull.axis[1], 1, 1e-5, 'axis is +Y');
    E.cancelPushPull();
});

t('beginPushPull on a box uses the same path (no template / no flag)', () => {
    if (E.pushpull && E.pushpull.active) E.cancelPushPull();
    reg.clear();
    h.clear();
    reg.create({ type: 'box', name: 'box',
                 params: { sx: 1, sy: 1, sz: 1 } });
    const box = reg.primitives[0];
    reg.setActive(box.id);
    E.setTool('pushpull');
    const top = box.faceGroups.groups.findIndex(g => g.normal[1] > 0.99);
    const topTri = box.faceGroups.groups[top].tris[0];
    const P = box.positions, I = box.indices;
    const c = [0, 0, 0];
    for (let k = 0; k < 3; k++) {
        const vi = I[topTri * 3 + k];
        c[0] += P[vi * 3 + 0]; c[1] += P[vi * 3 + 1]; c[2] += P[vi * 3 + 2];
    }
    c[0] /= 3; c[1] /= 3; c[2] /= 3;
    E.beginPushPull({ triangleIndex: topTri, position: c,
                      normal: box.faceGroups.groups[top].normal.slice(),
                      distance: 0 });
    truthy(E.pushpull.active);
    eq(E.pushpull.groupIdx, top, 'bound to top face group');
    E.cancelPushPull();
});

// -------------------------------------------------------------------------
// End-to-end: drag, commit, inspect resulting 3D primitive.
// -------------------------------------------------------------------------

t('pull +Y by 0.75 on a sketch face produces a closed slab', () => {
    const r = rectSetup(0, 0, 2, 3);
    E.beginPushPull(hitOnFront(r));
    E.applyPushPull(0.75);
    E.commitPushPull();
    // 4 corners at y=0.75 (the moved front face), 4 at y=0 (original / back).
    // Each corner position has 3 distinct vertex objects in the surgery
    // output (front face, back face, side wall) due to per-face dedup.
    let topAt075 = 0, botAt0 = 0;
    for (let vi = 0; vi < r.positions.length / 3; vi++) {
        const y = r.positions[vi * 3 + 1];
        if (Math.abs(y - 0.75) < 1e-5) topAt075++;
        if (Math.abs(y) < 1e-5) botAt0++;
    }
    truthy(topAt075 >= 4, '≥4 verts at y=0.75 (got ' + topAt075 + ')');
    truthy(botAt0 >= 4, '≥4 verts at y=0 (got ' + botAt0 + ')');
    // Closed manifold check.
    const em = EditMesh.fromMeshData(r.positions, r.indices);
    const val = EditMesh.validate(em);
    truthy(val.ok, 'manifold ok: ' + val.errors.join('; '));
    truthy(val.isClosed, 'slab closed (boundary=' + val.boundaryHalfEdges + ')');
    // 1 front + 1 back + 4 walls = 6 face groups.
    eq(r.faceGroups.groups.length, 6, '6 face groups');
});

t('pull -Y by 0.5 on a sketch face produces a closed slab below the plane', () => {
    const r = rectSetup(0, 0, 2, 3);
    E.beginPushPull(hitOnFront(r));
    E.applyPushPull(-0.5);
    E.commitPushPull();
    eq(r.faceGroups.groups.length, 6, '6 face groups after negative pull');
    let atZero = 0, atNeg = 0;
    for (let vi = 0; vi < r.positions.length / 3; vi++) {
        const y = r.positions[vi * 3 + 1];
        if (Math.abs(y) < 1e-5) atZero++;
        if (Math.abs(y - (-0.5)) < 1e-5) atNeg++;
    }
    truthy(atZero >= 4, '≥4 verts at y=0 (got ' + atZero + ')');
    truthy(atNeg >= 4, '≥4 verts at y=-0.5 (got ' + atNeg + ')');
});

// -------------------------------------------------------------------------
// Commit / cancel edge cases.
// -------------------------------------------------------------------------

t('zero-distance commit leaves the primitive flat (no slab baked)', () => {
    const r = rectSetup(0, 0, 1, 1);
    const beforeHist = h.size();
    E.beginPushPull(hitOnFront(r));
    E.commitPushPull();   // no applyPushPull → distance = 0
    falsy(E.pushpull.active);
    eq(r.positions.length / 3, 4, 'still a flat rect after zero-dist commit');
    eq(r.indices.length / 3, 2);
    eq(h.size(), beforeHist, 'no history entry recorded for zero-dist commit');
});

t('cancel reverts the render without mutating the primitive', () => {
    const r = rectSetup(0, 0, 1, 1);
    const posBefore = Array.from(r.positions);
    E.beginPushPull(hitOnFront(r));
    E.applyPushPull(0.9);
    E.cancelPushPull();
    eq(r.positions.length, posBefore.length,
       'committed positions buffer unchanged');
    for (let i = 0; i < posBefore.length; i++) {
        near(r.positions[i], posBefore[i], 1e-6,
             'primitive positions[' + i + '] unchanged after cancel');
    }
});

// -------------------------------------------------------------------------
// Undo / redo round-trip.
// -------------------------------------------------------------------------

t('undo restores the flat rect; redo re-applies the extrusion', () => {
    const r = rectSetup(0, 0, 2, 2);
    const id = r.id;
    E.beginPushPull(hitOnFront(r));
    E.applyPushPull(0.5);
    E.commitPushPull();
    eq(reg.getById(id).faceGroups.groups.length, 6, 'commit → slab (6 groups)');

    h.undo();
    const afterUndo = reg.getById(id);
    eq(afterUndo.positions.length / 3, 4, 'undo → flat rect (4 verts)');
    eq(afterUndo.indices.length / 3, 2,  'undo → 2 tris');
    eq(afterUndo.faceGroups.groups.length, 1, 'undo → 1 face group');

    h.redo();
    const afterRedo = reg.getById(id);
    eq(afterRedo.faceGroups.groups.length, 6, 'redo → slab');
});

// -------------------------------------------------------------------------
// Second push/pull on the now-3D slab.
// -------------------------------------------------------------------------

t('second push/pull on the extruded slab extends it further', () => {
    const r = rectSetup(0, 0, 2, 2);
    E.beginPushPull(hitOnFront(r));
    E.applyPushPull(0.5);
    E.commitPushPull();
    const top = r.faceGroups.groups.findIndex(g => g.normal[1] > 0.99);
    const topTri = r.faceGroups.groups[top].tris[0];
    const P = r.positions, I = r.indices;
    const c = [0, 0, 0];
    for (let k = 0; k < 3; k++) {
        const vi = I[topTri * 3 + k];
        c[0] += P[vi * 3 + 0]; c[1] += P[vi * 3 + 1]; c[2] += P[vi * 3 + 2];
    }
    c[0] /= 3; c[1] /= 3; c[2] /= 3;
    E.beginPushPull({ triangleIndex: topTri, position: c,
                      normal: r.faceGroups.groups[top].normal.slice(),
                      distance: 0 });
    E.applyPushPull(0.25);
    E.commitPushPull();
    let topAt075 = 0;
    for (let vi = 0; vi < r.positions.length / 3; vi++) {
        if (Math.abs(r.positions[vi * 3 + 1] - 0.75) < 1e-5) topAt075++;
    }
    truthy(topAt075 >= 4, '≥4 verts at y=0.75 after second pull (got ' + topAt075 + ')');
});

// -------------------------------------------------------------------------
// Face-group preservation: pulling a cylinder facet deforms adjacent faces
// in place (vertex-substitution surgery). No new bridge groups are added —
// the neighbour facets tilt to a new plane through the moved edge.
// -------------------------------------------------------------------------

t('pulling a cylinder facet deforms neighbours in place (no new groups)', () => {
    if (E.pushpull && E.pushpull.active) E.cancelPushPull();
    reg.clear();
    h.clear();
    // 12-segment cylinder: 12 sides + 2 caps = 14 face groups.
    reg.create({ type: 'cylinder', name: 'cyl',
                 params: { r: 1, h: 2, seg: 12 } });
    const cyl = reg.primitives[0];
    reg.setActive(cyl.id);
    const beforeGroupCount = cyl.faceGroups.groups.length;
    assert(beforeGroupCount === 14,
           `cylinder has 14 face groups (got ${beforeGroupCount})`);
    const sideIdx = cyl.faceGroups.groups.findIndex(g =>
        Math.abs(g.normal[1]) < 0.01);
    assert(sideIdx >= 0, 'found a side facet');
    const sideGroup = cyl.faceGroups.groups[sideIdx];
    const tri0 = sideGroup.tris[0];
    const P = cyl.positions, I = cyl.indices;
    const c = [0, 0, 0];
    for (let k = 0; k < 3; k++) {
        const vi = I[tri0 * 3 + k];
        c[0] += P[vi * 3 + 0]; c[1] += P[vi * 3 + 1]; c[2] += P[vi * 3 + 2];
    }
    c[0] /= 3; c[1] /= 3; c[2] /= 3;
    E.setTool('pushpull');
    E.beginPushPull({ triangleIndex: tri0, position: c,
                      normal: sideGroup.normal.slice(), distance: 0 });
    E.applyPushPull(0.5);
    E.commitPushPull();
    const afterGroupCount = cyl.faceGroups.groups.length;
    eq(afterGroupCount, beforeGroupCount,
       `no new groups (got ${afterGroupCount - beforeGroupCount} new)`);
    // Manifold preserved — no edges opened up.
    const em = EditMesh.fromMeshData(cyl.positions, cyl.indices);
    const val = EditMesh.validate(em);
    truthy(val.isClosed,
           'cylinder closed after side pull (boundary=' +
           val.boundaryHalfEdges + ')');
});

// -------------------------------------------------------------------------
// Wrap-up.
// -------------------------------------------------------------------------

console.log(`\n${tests - failed}/${tests} passed`);
if (failed > 0) throw new Error(`${failed} test(s) failed`);
