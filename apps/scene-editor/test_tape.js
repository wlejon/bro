// Integration test: tape measure tool (two-click distance readout).
//
// Run: bro-headless apps/scene-editor apps/scene-editor/test_tape.js

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
function near(a, b, eps, msg) {
    if (Math.abs(a - b) > (eps || 1e-5)) {
        throw new Error((msg || 'near') + ': ' + a + ' vs ' + b);
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

t('tool state starts inactive', () => {
    resetState();
    eq(E.tapeToolState.active, false);
});

t('begin sets from point and activates the tool', () => {
    resetState();
    E.setTool('tape');
    E.beginTape([0, 0, 0]);
    eq(E.tapeToolState.active, true);
    eq(E.tapeToolState.from, [0, 0, 0]);
    E.cancelTape();
});

t('update computes running distance', () => {
    resetState();
    E.setTool('tape');
    E.beginTape([0, 0, 0]);
    E.updateTapeAt([3, 0, 4]);
    near(TapeTool.distance(E.tapeToolState), 5, 1e-5, '3-4-5 = 5');
    E.cancelTape();
});

t('commit returns the final distance and clears state', () => {
    resetState();
    E.setTool('tape');
    E.beginTape([1, 2, 3]);
    E.updateTapeAt([4, 6, 3]);       // dx=3, dy=4, dz=0 → 5
    const d = E.commitTape();
    near(d, 5, 1e-5, 'commit distance = 5');
    eq(E.tapeToolState.active, false, 'tool idle after commit');
    eq(E.tapeToolState.from, null);
});

t('cancel mid-measurement resets without emitting', () => {
    resetState();
    E.setTool('tape');
    E.beginTape([0, 0, 0]);
    E.updateTapeAt([1, 1, 1]);
    E.cancelTape();
    eq(E.tapeToolState.active, false);
    eq(E.tapeToolState.from, null);
});

t('switching tools mid-measurement cancels the tape', () => {
    resetState();
    E.setTool('tape');
    E.beginTape([0, 0, 0]);
    eq(E.tapeToolState.active, true);
    E.setTool('select');
    eq(E.tapeToolState.active, false);
});

t('tape does not create geometry or record history', () => {
    resetState();
    const nPrims = reg.primitives.length;
    const histSize = h.size();
    E.setTool('tape');
    E.beginTape([0, 0, 0]);
    E.updateTapeAt([1, 1, 1]);
    E.commitTape();
    eq(reg.primitives.length, nPrims, 'no primitives added');
    eq(h.size(), histSize, 'no history entries');
});

console.log(`\n${tests - failed}/${tests} passed`);
if (failed > 0) throw new Error(`${failed} test(s) failed`);
