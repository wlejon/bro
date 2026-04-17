// Integration test: line drawing tool + closed-polygon detection.
//
// Run: bro-headless apps/scene-editor apps/scene-editor/test_line.js

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
function falsy(v, msg)  { if (v)  throw new Error(msg || 'expected falsy'); }

function resetState() {
    E.setupDefaultScene();
    h.clear();
    E.setTool('select');
    // Start with an empty scene for most line-tool tests — existing primitives
    // would interfere with the primitive-count assertions.
    reg.clear();
}

resetState();

// -------------------------------------------------------------------------
// LineTool pure-state tests
// -------------------------------------------------------------------------

t('LineTool starts inactive', () => {
    const s = LineTool.createState();
    eq(s.active, false);
    eq(s.points, []);
});

t('addPoint on inactive state is ignored', () => {
    const s = LineTool.createState();
    const r = LineTool.addPoint(s, [0, 0, 0]);
    eq(r.kind, 'ignored');
});

t('addPoint grows the polyline with unique points', () => {
    const s = LineTool.createState();
    LineTool.begin(s, { origin: [0,0,0], normal: [0,1,0], u: [1,0,0], v: [0,0,-1] },
                   [0, 0, 0]);
    eq(s.points.length, 1);
    eq(LineTool.addPoint(s, [1, 0, 0]).kind, 'segment');
    eq(s.points.length, 2);
    eq(LineTool.addPoint(s, [1, 0, 1]).kind, 'segment');
    eq(s.points.length, 3);
});

t('addPoint coinciding with last point is ignored (zero-length seg)', () => {
    const s = LineTool.createState();
    LineTool.begin(s, { origin: [0,0,0], normal: [0,1,0], u: [1,0,0], v: [0,0,-1] },
                   [0, 0, 0]);
    LineTool.addPoint(s, [1, 0, 0]);
    const r = LineTool.addPoint(s, [1, 0, 0]);
    eq(r.kind, 'ignored');
    eq(s.points.length, 2, 'no growth from duplicate');
});

t('closure requires ≥3 prior points', () => {
    const s = LineTool.createState();
    LineTool.begin(s, { origin: [0,0,0], normal: [0,1,0], u: [1,0,0], v: [0,0,-1] },
                   [0, 0, 0]);
    LineTool.addPoint(s, [1, 0, 0]);
    // Closing back with only 2 points is rejected (a "loop" of 2 vertices
    // is degenerate) — add another segment instead.
    const r = LineTool.addPoint(s, [0, 0, 0]);
    eq(r.kind, 'segment', 'treated as a regular segment, not a closure');
});

t('closure with 3 prior points creates polygon = [a, b, c]', () => {
    const s = LineTool.createState();
    LineTool.begin(s, { origin: [0,0,0], normal: [0,1,0], u: [1,0,0], v: [0,0,-1] },
                   [0, 0, 0]);
    LineTool.addPoint(s, [1, 0, 0]);
    LineTool.addPoint(s, [1, 0, 1]);
    const r = LineTool.addPoint(s, [0, 0, 0]);
    eq(r.kind, 'closed');
    eq(r.polygon.length, 3);
    eq(r.orphan.length, 0);
    eq(s.active, false, 'tool cleared after closure');
});

t('sub-loop closure puts prefix in orphan', () => {
    const s = LineTool.createState();
    LineTool.begin(s, { origin: [0,0,0], normal: [0,1,0], u: [1,0,0], v: [0,0,-1] },
                   [0, 0, 0]);   // A
    // A → B → C → D → E, then close to B.
    //   polygon = [B, C, D, E], orphan = [A]
    LineTool.addPoint(s, [1, 0, 0]);      // B
    LineTool.addPoint(s, [2, 0, 0]);      // C
    LineTool.addPoint(s, [2, 0, 1]);      // D
    LineTool.addPoint(s, [1, 0, 1]);      // E
    const r = LineTool.addPoint(s, [1, 0, 0]);   // close to B
    eq(r.kind, 'closed');
    eq(r.polygon.length, 4, 'sub-loop has 4 verts');
    eq(r.orphan.length, 1, '1 orphan vertex');
});

t('findClosureIndex reflects closure eligibility', () => {
    const s = LineTool.createState();
    LineTool.begin(s, { origin: [0,0,0], normal: [0,1,0], u: [1,0,0], v: [0,0,-1] },
                   [0, 0, 0]);
    LineTool.addPoint(s, [1, 0, 0]);
    LineTool.addPoint(s, [1, 0, 1]);
    eq(LineTool.findClosureIndex(s, [0, 0, 0]), 0, 'closes to first vertex');
    eq(LineTool.findClosureIndex(s, [5, 0, 5]), -1, 'no closure far from all verts');
    eq(LineTool.findClosureIndex(s, [1, 0, 1]), -1, 'last vertex is excluded');
});

// -------------------------------------------------------------------------
// App-level: begin → addPoint → closure creates a primitive
// -------------------------------------------------------------------------

t('triangle close commits a 3-vert primitive', () => {
    resetState();
    E.setTool('line');
    E.beginLine([0, 0, 0]);
    E.addLinePoint([2, 0, 0]);
    E.addLinePoint([0, 0, 2]);
    const r = E.addLinePoint([0, 0, 0]);   // closes
    eq(r.kind, 'closed');
    eq(reg.primitives.length, 1, 'one primitive committed');
    const p = reg.primitives[0];
    eq(p.name.startsWith('Polygon'), true);
    eq(p.positions.length / 3, 3, 'triangle has 3 verts');
    eq(p.indices.length / 3, 1, 'triangle has 1 tri');
    // All verts on ground.
    for (let i = 0; i < p.positions.length; i += 3) {
        near(p.positions[i + 1], 0, 1e-5, 'vert on ground plane');
    }
});

t('quad close commits a 4-vert primitive with the correct bbox', () => {
    resetState();
    E.setTool('line');
    E.beginLine([0, 0, 0]);
    E.addLinePoint([3, 0, 0]);
    E.addLinePoint([3, 0, 2]);
    E.addLinePoint([0, 0, 2]);
    const r = E.addLinePoint([0, 0, 0]);
    eq(r.kind, 'closed');
    eq(reg.primitives.length, 1);
    const p = reg.primitives[0];
    eq(p.positions.length / 3, 4);
    eq(p.indices.length / 3, 2);
    let minX = Infinity, maxX = -Infinity, minZ = Infinity, maxZ = -Infinity;
    for (let i = 0; i < p.positions.length; i += 3) {
        minX = Math.min(minX, p.positions[i]);
        maxX = Math.max(maxX, p.positions[i]);
        minZ = Math.min(minZ, p.positions[i + 2]);
        maxZ = Math.max(maxZ, p.positions[i + 2]);
    }
    near(maxX - minX, 3, 1e-5, 'X extent = 3');
    near(maxZ - minZ, 2, 1e-5, 'Z extent = 2');
});

t('front face normal of a CW-drawn polygon faces +plane.normal', () => {
    // When the user draws CW-from-+Y, app should flip the polygon order so
    // the triangulation still faces +Y.
    resetState();
    E.setTool('line');
    // CW from +Y: (0,0,0) → (0,0,2) → (2,0,2) → (2,0,0) → close.
    E.beginLine([0, 0, 0]);
    E.addLinePoint([0, 0, 2]);
    E.addLinePoint([2, 0, 2]);
    E.addLinePoint([2, 0, 0]);
    E.addLinePoint([0, 0, 0]);
    const p = reg.primitives[0];
    // Every stored normal ≈ +Y.
    for (let i = 0; i < p.normals.length; i += 3) {
        near(p.normals[i + 1], 1, 1e-5,
             'stored vertex normal points +Y regardless of draw direction');
    }
});

t('sub-loop closure discards orphan prefix (MVP)', () => {
    resetState();
    E.setTool('line');
    E.beginLine([0, 0, 0]);       // orphan prefix root
    E.addLinePoint([1, 0, 0]);    // B — will close back here
    E.addLinePoint([2, 0, 0]);
    E.addLinePoint([2, 0, 2]);
    E.addLinePoint([1, 0, 2]);
    const r = E.addLinePoint([1, 0, 0]);   // closes to B
    eq(r.kind, 'closed');
    eq(r.orphan.length, 1, 'one orphan vertex (A)');
    eq(reg.primitives.length, 1, 'one primitive (the closed sub-loop)');
    const p = reg.primitives[0];
    eq(p.positions.length / 3, 4, 'sub-loop is a 4-vertex quad');
});

// -------------------------------------------------------------------------
// Chain / cancel / tool-switch behaviors
// -------------------------------------------------------------------------

t('Esc (cancelLine) ends the chain without creating a primitive', () => {
    resetState();
    E.setTool('line');
    E.beginLine([0, 0, 0]);
    E.addLinePoint([1, 0, 0]);
    E.addLinePoint([1, 0, 1]);
    eq(E.lineToolState.active, true);
    E.cancelLine();
    eq(E.lineToolState.active, false);
    eq(reg.primitives.length, 0, 'no primitive created');
});

t('commitLine (double-click) ends without creating', () => {
    resetState();
    E.setTool('line');
    E.beginLine([0, 0, 0]);
    E.addLinePoint([1, 0, 0]);
    E.addLinePoint([1, 0, 1]);
    E.commitLine();
    eq(E.lineToolState.active, false);
    eq(reg.primitives.length, 0);
});

t('tool-switch mid-draw cancels the line', () => {
    resetState();
    E.setTool('line');
    E.beginLine([0, 0, 0]);
    E.addLinePoint([1, 0, 0]);
    E.setTool('select');
    eq(E.lineToolState.active, false);
    eq(reg.primitives.length, 0);
});

t('tool stays armed after closure so user can draw another polygon', () => {
    resetState();
    E.setTool('line');
    E.beginLine([0, 0, 0]);
    E.addLinePoint([1, 0, 0]);
    E.addLinePoint([1, 0, 1]);
    E.addLinePoint([0, 0, 0]);
    eq(reg.primitives.length, 1);
    eq(E.currentTool, 'line', 'still in line tool');
    eq(E.lineToolState.active, false, 'chain cleared, ready for next beginLine');
    // Draw another polygon.
    E.beginLine([3, 0, 0]);
    E.addLinePoint([4, 0, 0]);
    E.addLinePoint([4, 0, 1]);
    E.addLinePoint([3, 0, 0]);
    eq(reg.primitives.length, 2, 'second polygon committed');
});

// -------------------------------------------------------------------------
// Undo / redo
// -------------------------------------------------------------------------

t('undo removes the polygon; redo restores it', () => {
    resetState();
    E.setTool('line');
    E.beginLine([0, 0, 0]);
    E.addLinePoint([1, 0, 0]);
    E.addLinePoint([1, 0, 1]);
    E.addLinePoint([0, 0, 0]);
    const id = reg.primitives[0].id;
    h.undo();
    eq(reg.getById(id), null, 'polygon removed by undo');
    h.redo();
    truthy(reg.getById(id), 'polygon back on redo');
});

// -------------------------------------------------------------------------
// Wrap-up.
// -------------------------------------------------------------------------

console.log(`\n${tests - failed}/${tests} passed`);
if (failed > 0) throw new Error(`${failed} test(s) failed`);
