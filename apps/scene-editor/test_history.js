// Tests for undo/redo integration in scene-editor.
//
// Run: bro-headless apps/scene-editor apps/scene-editor/test_history.js

'use strict';

const E   = window.__editor;
const h   = E.history;
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
function truthy(v, msg) { if (!v) throw new Error(msg || 'expected truthy'); }
function falsy(v, msg)  { if (v)  throw new Error(msg || 'expected falsy'); }

// Reset to a clean slate between tests — the app starts with one box and a
// live history; individual tests want a known baseline.
function resetState() {
    while (reg.primitives.length > 1) reg.remove(reg.primitives[reg.primitives.length - 1].id);
    h.clear();
}

resetState();

// -------------------------------------------------------------------------
// Initial state
// -------------------------------------------------------------------------

t('initial: empty history, single default primitive', () => {
    truthy(reg.primitives.length >= 1, 'at least one default primitive');
    eq(h.size(), 0);
    eq(h.canUndo(), false);
    eq(h.canRedo(), false);
});

// -------------------------------------------------------------------------
// Add / delete primitives
// -------------------------------------------------------------------------

t('add primitive records an entry', () => {
    resetState();
    const before = reg.primitives.length;
    const sphere = E.outlinerAddPrimitive('sphere');
    truthy(sphere, 'add returned a primitive');
    eq(reg.primitives.length, before + 1);
    eq(h.size(), 1);
    eq(h.canUndo(), true);
});

t('undo add removes the primitive', () => {
    resetState();
    const before = reg.primitives.length;
    const sphere = E.outlinerAddPrimitive('sphere');
    const id = sphere.id;
    h.undo();
    eq(reg.primitives.length, before);
    eq(reg.getById(id), null);
    eq(h.canRedo(), true);
});

t('redo add restores with same id', () => {
    resetState();
    const sphere = E.outlinerAddPrimitive('sphere');
    const id = sphere.id;
    const before = reg.primitives.length;
    h.undo();
    h.redo();
    eq(reg.primitives.length, before);
    const restored = reg.getById(id);
    truthy(restored, 'primitive with same id is back');
    eq(restored.name, sphere.name);
});

t('delete records an entry; undo restores mesh state', () => {
    resetState();
    const cyl = E.outlinerAddPrimitive('cylinder');
    const id = cyl.id;
    const posSample = cyl.positions.slice(0, 9);  // first 3 verts
    const name = cyl.name;
    h.clear();  // isolate: only measure the delete

    // Simulate the delete click path: snapshot + record, then remove.
    const snap = reg.snapshotPrimitive(cyl);
    h.do('Delete ' + cyl.name,
        () => reg.remove(snap.id),
        () => reg.restoreFromSnapshot(snap));

    eq(reg.getById(id), null);
    eq(h.size(), 1);

    h.undo();
    const back = reg.getById(id);
    truthy(back, 'primitive restored by undo');
    eq(back.name, name);
    // Geometry must match the pre-delete state exactly.
    const backSample = Array.from(back.positions.slice(0, 9));
    eq(backSample, Array.from(posSample));
});

t('redo delete removes again', () => {
    resetState();
    const cyl = E.outlinerAddPrimitive('cylinder');
    const id = cyl.id;
    h.clear();
    const snap = reg.snapshotPrimitive(cyl);
    h.do('Delete',
        () => reg.remove(snap.id),
        () => reg.restoreFromSnapshot(snap));
    h.undo();
    truthy(reg.getById(id));
    h.redo();
    eq(reg.getById(id), null);
});

t('delete preserves edits made before deletion', () => {
    // Add a cylinder, translate its vertices manually, then delete + undo.
    // The restored primitive should carry the edited geometry, not the
    // factory-fresh shape.
    resetState();
    const cyl = E.outlinerAddPrimitive('cylinder');
    const id = cyl.id;
    h.clear();

    // Apply a translation via updateGeometry (mimics a post-move commit).
    const edited = new Float32Array(cyl.positions);
    for (let i = 0; i < edited.length; i += 3) edited[i] += 5.0;
    cyl.updateGeometry(edited, new Uint32Array(cyl.indices),
        cyl.normals ? new Float32Array(cyl.normals) : null);
    const editedSample = Array.from(cyl.positions.slice(0, 9));

    const snap = reg.snapshotPrimitive(cyl);
    h.do('Delete',
        () => reg.remove(snap.id),
        () => reg.restoreFromSnapshot(snap));
    h.undo();

    const back = reg.getById(id);
    truthy(back);
    const backSample = Array.from(back.positions.slice(0, 9));
    eq(backSample, editedSample, 'restored geometry preserves post-edit state');
});

// -------------------------------------------------------------------------
// Rename
// -------------------------------------------------------------------------

t('rename: undo reverts, redo re-applies', () => {
    resetState();
    const prim = reg.primitives[0];
    const original = prim.name;
    h.clear();
    const next = 'Renamed';
    h.do('Rename',
        () => reg.setName(prim.id, next),
        () => reg.setName(prim.id, original));
    eq(prim.name, next);
    h.undo();
    eq(prim.name, original);
    h.redo();
    eq(prim.name, next);
});

// -------------------------------------------------------------------------
// Mesh transforms (move / rotate / scale / pushpull) all use the shared
// captureMesh/applyMesh pattern, so one test per the pattern covers them.
// -------------------------------------------------------------------------

t('mesh transform: captureMesh + record round-trips positions', () => {
    resetState();
    const prim = reg.primitives[0];
    const prevMesh = E.captureMesh(prim);
    // Translate every vertex by +2 on X — simulates a Move commit.
    const next = new Float32Array(prim.positions);
    for (let i = 0; i < next.length; i += 3) next[i] += 2.0;
    prim.updateGeometry(next, new Uint32Array(prim.indices),
        prim.normals ? new Float32Array(prim.normals) : null);
    const nextMesh = E.captureMesh(prim);
    h.clear();
    h.record('Move',
        () => E.applyMesh(prim, nextMesh),
        () => E.applyMesh(prim, prevMesh));

    // Snapshot the post-move buffer to verify undo/redo bring it back.
    const postX = prim.positions[0];
    h.undo();
    const undoneX = prim.positions[0];
    eq(undoneX, postX - 2.0, 'undo reverts X by -2');
    h.redo();
    eq(prim.positions[0], postX, 'redo re-applies X shift');
});

t('pushpull integration: direct app API, undo reverts, redo restores', () => {
    resetState();
    const prim = reg.primitives[0];
    // Find a face group on the default box and push it +1 along its normal.
    const fg = prim.faceGroups.groups[0];
    const tri = fg.tris[0];
    const pre = new Float32Array(prim.positions);
    h.clear();

    E.beginPushPullOn(prim, {
        triangleIndex: tri,
        position: prim.faceGroupCentroid(0),
        normal: fg.normal.slice(),
        distance: 0,
    });
    E.applyPushPull(1.0);
    E.commitPushPull();

    eq(h.size(), 1, 'one entry for push/pull commit');
    truthy(h.canUndo());
    const postPositions = Array.from(prim.positions);

    h.undo();
    const back = Array.from(prim.positions);
    eq(back.length, pre.length, 'positions length back to pre-state');
    for (let i = 0; i < back.length; i++) {
        if (Math.abs(back[i] - pre[i]) > 1e-9) {
            throw new Error(`undo differs at ${i}: ${back[i]} vs ${pre[i]}`);
        }
    }

    h.redo();
    eq(Array.from(prim.positions), postPositions, 'redo restores post-pushpull');
});

// -------------------------------------------------------------------------
// Sequencing & limit
// -------------------------------------------------------------------------

t('multi-step: add → rename → undo twice → redo twice', () => {
    resetState();
    const base = reg.primitives[0];
    h.clear();
    const newName = 'A';
    const prev = base.name;
    h.do('Rename', () => reg.setName(base.id, newName), () => reg.setName(base.id, prev));
    const sphere = E.outlinerAddPrimitive('sphere');
    const sid = sphere.id;
    eq(h.size(), 2);

    h.undo();  // undo add
    eq(reg.getById(sid), null);
    eq(base.name, newName);  // rename still applied

    h.undo();  // undo rename
    eq(base.name, prev);

    h.redo();  // redo rename
    eq(base.name, newName);

    h.redo();  // redo add
    truthy(reg.getById(sid));
});

t('new record clears redo stack', () => {
    resetState();
    const base = reg.primitives[0];
    h.clear();
    const prev = base.name;
    h.do('Rename', () => reg.setName(base.id, 'X'), () => reg.setName(base.id, prev));
    h.undo();
    truthy(h.canRedo());
    // Any new recorded action should invalidate the redo branch.
    const sphere = E.outlinerAddPrimitive('sphere');
    falsy(h.canRedo(), 'redo cleared by new action');
    // Sanity: undo the add.
    h.undo();
    eq(reg.getById(sphere.id), null);
});

// -------------------------------------------------------------------------
// End
// -------------------------------------------------------------------------

console.log(`\n${tests - failed}/${tests} passed`);
if (failed > 0) throw new Error(`${failed} test(s) failed`);
