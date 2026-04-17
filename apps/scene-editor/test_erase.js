// Integration test: eraser tool.
//
// The tool hooks into the same pick → delete path the outliner's trash-can
// button uses, so the coverage here focuses on behavior specific to tool
// mode: tool-switch, undo/redo, drag-cancel side effects.
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

t('eraser tool deletes the picked primitive (via deletePrimitive)', () => {
    resetState();
    // Add a second primitive so the scene isn't empty afterward.
    reg.create({ type: 'box', name: 'second',
                 params: { sx: 1, sy: 1, sz: 1 }, position: [3, 0, 0] });
    const target = reg.primitives[1];
    const id = target.id;
    E.setTool('erase');
    E.deletePrimitive(target);     // same code path the click handler runs
    eq(reg.getById(id), null, 'target erased');
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

console.log(`\n${tests - failed}/${tests} passed`);
if (failed > 0) throw new Error(`${failed} test(s) failed`);
