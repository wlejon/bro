// Integration test: push/pull on a flat (rectangle-tool) face.
//
// The rectangle tool produces a flat triangulated mesh (4 verts, 2 tris,
// 1 face group). Without special handling, push/pull would translate the
// whole face. The "extrude-on-first-pull" path detects the flat case and
// synthesizes a back face + side walls on begin, then drives the drag
// against the template to produce a proper 3D slab on commit.
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
// beginPushPull on a flat face builds an extruded template.
// -------------------------------------------------------------------------

t('beginPushPull on flat rect enters flatExtrude mode', () => {
    const r = rectSetup(0, 0, 2, 3);
    E.beginPushPull(hitOnFront(r));
    truthy(E.pushpull.active);
    truthy(E.pushpull.flatExtrude, 'flatExtrude flag set');
    truthy(E.pushpull.templatePositions, 'template positions cached');
    // Template: 4 front + 4 back + 4 walls × 4 verts = 24
    eq(E.pushpull.workingPositions.length, 24 * 3, 'template has 24 verts');
    // 2 front tris + 2 back tris + 4 walls × 2 tris = 12 tris
    eq(E.pushpull.workingIndices.length, 12 * 3, 'template has 12 tris');
    E.cancelPushPull();
});

t('beginPushPull on a box keeps the classic (non-flat) path', () => {
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
    falsy(E.pushpull.flatExtrude, 'flatExtrude stays false on real 3D box');
    eq(E.pushpull.templatePositions, null, 'no template for 3D face');
    E.cancelPushPull();
});

// -------------------------------------------------------------------------
// End-to-end: drag, commit, inspect resulting 3D primitive.
// -------------------------------------------------------------------------

t('pull +Y by 0.75 produces a proper 3D slab', () => {
    const r = rectSetup(0, 0, 2, 3);
    E.beginPushPull(hitOnFront(r));
    E.applyPushPull(0.75);
    E.commitPushPull();
    // The primitive should now look like a thin box.
    eq(r.positions.length / 3, 24, 'slab has 24 verts after commit');
    eq(r.indices.length / 3, 12, 'slab has 12 tris after commit');
    eq(r.faceGroups.groups.length, 6, 'slab has 6 face groups');
    // Top rim: 4 unique positions at y=0.75 (each shared by front + 2 walls
    // = 3 vertex indices per position).
    let topCount = 0, botCount = 0;
    for (let vi = 0; vi < r.positions.length / 3; vi++) {
        const y = r.positions[vi * 3 + 1];
        if (Math.abs(y - 0.75) < 1e-5) topCount++;
        if (Math.abs(y) < 1e-5)        botCount++;
    }
    eq(topCount, 12, 'top y=0.75 cluster has 12 vertex-indices');
    eq(botCount, 12, 'bottom y=0 cluster has 12 vertex-indices');
});

t('pull -Y by 0.5 (inversion) produces a valid extrusion under the plane', () => {
    const r = rectSetup(0, 0, 2, 3);
    E.beginPushPull(hitOnFront(r));
    E.applyPushPull(-0.5);
    truthy(E.pushpull.inverted, 'winding flip fires at t<0 for flat extrude');
    E.commitPushPull();
    eq(r.faceGroups.groups.length, 6, 'slab has 6 face groups after negative pull');
    // Top rim = original plane (y=0), bottom = y=-0.5.
    let atZero = 0, atNeg = 0;
    for (let vi = 0; vi < r.positions.length / 3; vi++) {
        const y = r.positions[vi * 3 + 1];
        if (Math.abs(y) < 1e-5)            atZero++;
        if (Math.abs(y - (-0.5)) < 1e-5)   atNeg++;
    }
    eq(atZero, 12, 'top (y=0) has 12 vertex-indices');
    eq(atNeg,  12, 'bottom (y=-0.5) has 12 vertex-indices');
    // Check outward-facing top by finding +Y face group and sampling its
    // stored normal.
    const topG = r.faceGroups.groups.find(g => g.normal[1] > 0.99);
    truthy(topG, '+Y face group present after inversion');
});

// -------------------------------------------------------------------------
// Commit / cancel edge cases.
// -------------------------------------------------------------------------

t('zero-distance commit reverts the primitive to flat', () => {
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
    // After commit: the primitive is a 3D slab.
    eq(reg.getById(id).positions.length / 3, 24);

    h.undo();
    const afterUndo = reg.getById(id);
    eq(afterUndo.positions.length / 3, 4, 'undo → flat rect (4 verts)');
    eq(afterUndo.indices.length / 3, 2,  'undo → 2 tris');
    eq(afterUndo.faceGroups.groups.length, 1, 'undo → 1 face group');

    h.redo();
    const afterRedo = reg.getById(id);
    eq(afterRedo.positions.length / 3, 24, 'redo → slab');
    eq(afterRedo.faceGroups.groups.length, 6, 'redo → 6 face groups');
});

// -------------------------------------------------------------------------
// Second pull on the now-3D slab should use the non-flat path.
// -------------------------------------------------------------------------

t('second push/pull on the extruded slab uses the classic path', () => {
    const r = rectSetup(0, 0, 2, 2);
    E.beginPushPull(hitOnFront(r));
    E.applyPushPull(0.5);
    E.commitPushPull();
    // Slab is now 3D; pulling the top again is a regular push/pull.
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
    falsy(E.pushpull.flatExtrude, 'slab is no longer a flat face');
    E.applyPushPull(0.25);
    E.commitPushPull();
    // Top is now at y = 0.5 + 0.25 = 0.75.
    let topAt75 = 0;
    for (let vi = 0; vi < r.positions.length / 3; vi++) {
        if (Math.abs(r.positions[vi * 3 + 1] - 0.75) < 1e-5) topAt75++;
    }
    eq(topAt75, 12, 'second pull raises the 12 top-cluster verts to y=0.75');
});

// -------------------------------------------------------------------------
// Wrap-up.
// -------------------------------------------------------------------------

// -------------------------------------------------------------------------
// Face-group preservation across commit
// -------------------------------------------------------------------------

t('pulling a cylinder facet until coplanar keeps facets as separate groups', () => {
    reg.clear();
    h.clear();
    // 12-segment cylinder so the math is easy to reason about; 12 side
    // facets + top cap + bottom cap = 14 face groups.
    reg.create({ type: 'cylinder', name: 'cyl',
                 params: { r: 1, h: 2, seg: 12 } });
    const cyl = reg.primitives[0];
    reg.setActive(cyl.id);
    const beforeGroupCount = cyl.faceGroups.groups.length;
    assert(beforeGroupCount === 14,
           `cylinder has 14 face groups (got ${beforeGroupCount})`);
    // Pick one side facet (first group with a horizontal-ish normal).
    const sideIdx = cyl.faceGroups.groups.findIndex(g =>
        Math.abs(g.normal[1]) < 0.01);
    assert(sideIdx >= 0, 'found a side facet');
    const sideGroup = cyl.faceGroups.groups[sideIdx];
    const tri0 = sideGroup.tris[0];
    // Centroid of that tri.
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
    // Pull outward enough that adjacent facets' rims move with it — the
    // exact distance doesn't matter for this assertion; we want to confirm
    // the commit preserves face-group identity even when geometry moves.
    E.applyPushPull(0.5);
    E.commitPushPull();
    const afterGroupCount = cyl.faceGroups.groups.length;
    assert(afterGroupCount === beforeGroupCount,
           `face-group count preserved (before ${beforeGroupCount} → ` +
           `after ${afterGroupCount})`);
});

console.log(`\n${tests - failed}/${tests} passed`);
if (failed > 0) throw new Error(`${failed} test(s) failed`);
