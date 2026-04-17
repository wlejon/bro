// Integration test: eraser tool.
//
// Eraser removes a single face group (or the whole primitive if that was
// the only face). Whole-primitive delete still lives on the outliner's
// trash-can button (deletePrimitive), kept around so tests can cover both
// paths without going through the DOM.
//
// Run: bro-headless apps/scene-editor apps/scene-editor/test_erase.js

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
function truthy(v, msg) { if (!v) throw new Error(msg || 'expected truthy'); }

function resetState() {
    E.setupDefaultScene();
    h.clear();
    E.setTool('select');
}

resetState();

t('deletePrimitive removes the target and records undo', () => {
    resetState();
    const p = reg.primitives[0];
    const id = p.id;
    E.deletePrimitive(p);
    eq(reg.getById(id), null, 'removed from registry');
    truthy(h.canUndo(), 'history entry recorded');
    h.undo();
    truthy(reg.getById(id), 'restored on undo');
});

t('deletePrimitive removes one primitive, leaves others (outliner path)', () => {
    resetState();
    // Add a second primitive so the scene isn't empty afterward.
    reg.create({ type: 'box', name: 'second',
                 params: { sx: 1, sy: 1, sz: 1 }, position: [3, 0, 0] });
    const target = reg.primitives[1];
    const id = target.id;
    E.deletePrimitive(target);
    eq(reg.getById(id), null, 'target deleted');
    truthy(reg.primitives.length >= 1, 'other primitives survive');
});

t('redo re-erases the primitive (round-trip)', () => {
    resetState();
    const p = reg.primitives[0];
    const id = p.id;
    E.deletePrimitive(p);
    h.undo();
    truthy(reg.getById(id), 'undo restores');
    h.redo();
    eq(reg.getById(id), null, 'redo re-erases');
});

t('erasing a primitive with an active gizmo cancels the drag', () => {
    // Select the default box, synthesize an active move drag, then erase.
    resetState();
    const box = reg.primitives[0];
    // Simulate a begin-move by calling the exposed helper.
    E.beginMove(box, { position: [0, 1, 0] });
    truthy(E.moveToolState.active, 'move active');
    E.deletePrimitive(box);
    eq(E.moveToolState.active, false, 'move cancelled before deletion');
    eq(reg.getById(box.id), null, 'box gone');
});

// -------------------------------------------------------------------------
// Face-level erase (new behavior)
// -------------------------------------------------------------------------

t('eraseFace removes one face group and keeps the primitive', () => {
    resetState();
    const box = reg.primitives[0];
    const id = box.id;
    const beforeTris = box.indices.length / 3;
    const beforeGroups = box.faceGroups.groups.length;
    // Pick the +Y (top) group.
    const topIdx = box.faceGroups.groups.findIndex(g => g.normal[1] > 0.99);
    truthy(topIdx >= 0, 'top face group present');
    const dropped = box.faceGroups.groups[topIdx].tris.length;
    E.eraseFace(box, topIdx);
    truthy(reg.getById(id), 'box survives face erase');
    eq(box.indices.length / 3, beforeTris - dropped,
       `tri count dropped by ${dropped}`);
    eq(box.faceGroups.groups.length, beforeGroups - 1, 'one fewer face group');
});

t('erasing the last face deletes the whole primitive', () => {
    resetState();
    reg.clear();
    h.clear();
    // Flat rectangle is a single-face primitive.
    E.setTool('rectangle');
    E.beginRectangle([0, 0, 0]);
    E.updateRectangleAt([1, 0, 1]);
    E.commitRectangle();
    eq(reg.primitives.length, 1);
    const rect = reg.primitives[0];
    const id = rect.id;
    E.eraseFace(rect, 0);     // only face group
    eq(reg.getById(id), null, 'rectangle removed because it had one face');
    eq(reg.primitives.length, 0);
});

t('undo restores the erased face', () => {
    resetState();
    const box = reg.primitives[0];
    const topIdx = box.faceGroups.groups.findIndex(g => g.normal[1] > 0.99);
    const prevTris = box.indices.length / 3;
    const prevGroups = box.faceGroups.groups.length;
    E.eraseFace(box, topIdx);
    eq(box.faceGroups.groups.length, prevGroups - 1);
    h.undo();
    eq(box.indices.length / 3, prevTris, 'tri count restored');
    eq(box.faceGroups.groups.length, prevGroups, 'face groups restored');
    truthy(box.faceGroups.groups.some(g => g.normal[1] > 0.99),
           '+Y face back after undo');
});

t('redo re-erases the face', () => {
    resetState();
    const box = reg.primitives[0];
    const topIdx = box.faceGroups.groups.findIndex(g => g.normal[1] > 0.99);
    E.eraseFace(box, topIdx);
    const erasedTris = box.indices.length / 3;
    const erasedGroups = box.faceGroups.groups.length;
    h.undo();
    h.redo();
    eq(box.indices.length / 3, erasedTris, 'redo drops the tris again');
    eq(box.faceGroups.groups.length, erasedGroups);
});

t('erasing a face cancels an in-flight push/pull on the same primitive', () => {
    resetState();
    const box = reg.primitives[0];
    const topIdx = box.faceGroups.groups.findIndex(g => g.normal[1] > 0.99);
    const topGroup = box.faceGroups.groups[topIdx];
    const topTri = topGroup.tris[0];
    const P = box.positions, I = box.indices;
    const c = [0, 0, 0];
    for (let k = 0; k < 3; k++) {
        const vi = I[topTri * 3 + k];
        c[0] += P[vi * 3 + 0]; c[1] += P[vi * 3 + 1]; c[2] += P[vi * 3 + 2];
    }
    c[0] /= 3; c[1] /= 3; c[2] /= 3;
    E.beginPushPull({ triangleIndex: topTri, position: c,
                      normal: topGroup.normal.slice(), distance: 0 });
    truthy(E.pushpull.active, 'push/pull active');
    E.eraseFace(box, topIdx);
    eq(E.pushpull.active, false, 'push/pull cancelled by face erase');
});

console.log(`\n${tests - failed}/${tests} passed`);
if (failed > 0) throw new Error(`${failed} test(s) failed`);
