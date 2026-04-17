// Integration test: circle drawing tool + VCB + history.
//
// Run: bro-headless apps/scene-editor apps/scene-editor/test_circle.js

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

function resetState() {
    E.setupDefaultScene();
    h.clear();
    E.setTool('select');
}

resetState();

// -------------------------------------------------------------------------
// Tool lifecycle
// -------------------------------------------------------------------------

t('tool state starts inactive', () => {
    resetState();
    eq(E.circleToolState.active, false);
});

t('setTool(circle) selects the tool, drawing is idle', () => {
    resetState();
    E.setTool('circle');
    eq(E.currentTool, 'circle');
    eq(E.circleToolState.active, false);
});

t('begin → update → commit produces a 32-gon primitive', () => {
    resetState();
    E.setTool('circle');
    const before = reg.primitives.length;
    E.beginCircle([0, 0, 0]);
    eq(E.circleToolState.active, true);
    E.updateCircleAt([2, 0, 0]);   // radius 2
    E.commitCircle();
    eq(E.circleToolState.active, false, 'state resets after commit');
    eq(reg.primitives.length, before + 1, 'one primitive added');
    const last = reg.primitives[reg.primitives.length - 1];
    eq(last.name.startsWith('Circle'), true);
    eq(last.positions.length / 3, 32, '32 verts');
    // Triangulating a regular 32-gon yields 30 tris (fan-ish).
    eq(last.indices.length / 3, 30, '30 tris');
    // All verts at distance ≈ 2 from center.
    for (let i = 0; i < last.positions.length; i += 3) {
        const r = Math.hypot(last.positions[i], last.positions[i + 2]);
        near(r, 2, 1e-4, 'vert radius ≈ 2');
        near(last.positions[i + 1], 0, 1e-5, 'vert on ground plane');
    }
});

t('zero-radius commit adds nothing', () => {
    resetState();
    const before = reg.primitives.length;
    E.setTool('circle');
    E.beginCircle([0, 0, 0]);
    E.updateCircleAt([0, 0, 0]);
    E.commitCircle();
    eq(reg.primitives.length, before, 'no primitive added');
});

t('cancel aborts without creating a primitive', () => {
    resetState();
    const before = reg.primitives.length;
    E.setTool('circle');
    E.beginCircle([0, 0, 0]);
    E.updateCircleAt([1, 0, 0]);
    E.cancelCircle();
    eq(reg.primitives.length, before);
    eq(E.circleToolState.active, false);
});

t('switching tools mid-draw cancels the circle preview', () => {
    resetState();
    E.setTool('circle');
    E.beginCircle([0, 0, 0]);
    E.updateCircleAt([1, 0, 0]);
    eq(E.circleToolState.active, true);
    E.setTool('select');
    eq(E.circleToolState.active, false);
});

// -------------------------------------------------------------------------
// VCB (single-value radius input)
// -------------------------------------------------------------------------

t('beginCircle arms the VCB in single-value mode', () => {
    resetState();
    E.setTool('circle');
    E.beginCircle([0, 0, 0]);
    eq(E.measureBoxState.active, true);
    eq(E.measureBoxState.pairMode, false, 'single-value mode for radius');
});

t('typing "2.5 + Enter" commits a circle of exact radius', () => {
    resetState();
    const before = reg.primitives.length;
    E.setTool('circle');
    E.beginCircle([0, 0, 0]);
    E.updateCircleAt([1, 0, 0]);     // arbitrary direction
    '2.5'.split('').forEach(ch => E.handleMeasureBoxKey(ch));
    E.handleMeasureBoxKey('Enter');
    eq(reg.primitives.length, before + 1, 'circle committed');
    eq(E.circleToolState.active, false);
    eq(E.measureBoxState.active, false);
    const last = reg.primitives[reg.primitives.length - 1];
    // Every vert at radius ≈ 2.5
    for (let i = 0; i < last.positions.length; i += 3) {
        const r = Math.hypot(last.positions[i], last.positions[i + 2]);
        near(r, 2.5, 1e-4, 'VCB-set radius ≈ 2.5');
    }
});

t('Esc cancels mid-VCB typing', () => {
    resetState();
    const before = reg.primitives.length;
    E.setTool('circle');
    E.beginCircle([0, 0, 0]);
    E.updateCircleAt([1, 0, 0]);
    E.handleMeasureBoxKey('3');
    E.handleMeasureBoxKey('Escape');
    eq(E.circleToolState.active, false);
    eq(reg.primitives.length, before);
});

// -------------------------------------------------------------------------
// History + face-pick
// -------------------------------------------------------------------------

t('circle commit records one undoable history entry', () => {
    resetState();
    E.setTool('circle');
    E.beginCircle([0, 0, 0]);
    E.updateCircleAt([1, 0, 0]);
    E.commitCircle();
    eq(h.size(), 1);
    truthy(h.canUndo());
    h.undo();
    eq(reg.primitives.findIndex(p => p.name.startsWith('Circle')), -1,
       'circle removed by undo');
    h.redo();
    truthy(reg.primitives.find(p => p.name.startsWith('Circle')),
           'circle back after redo');
});

t('circle drawn on a face plane places all verts on that plane', () => {
    resetState();
    // Default scene is a unit box; top face at y=1.
    const topPlane = {
        origin: [0, 1, 0],
        normal: [0, 1, 0],
        u: [1, 0, 0],
        v: [0, 0, 1],
    };
    E.setTool('circle');
    E.beginCircle([0, 1, 0], topPlane);
    E.updateCircleAt([0.4, 1, 0]);
    E.commitCircle();
    const last = reg.primitives[reg.primitives.length - 1];
    for (let i = 0; i < last.positions.length; i += 3) {
        near(last.positions[i + 1], 1, 1e-5, 'y = 1 for every vert');
    }
});

// -------------------------------------------------------------------------
// Wrap-up.
// -------------------------------------------------------------------------

console.log(`\n${tests - failed}/${tests} passed`);
if (failed > 0) throw new Error(`${failed} test(s) failed`);
