// Integration test: scene-editor save → new → load round-trips state.
//
// Run: bro-headless apps/scene-editor apps/scene-editor/test_project.js

'use strict';

const fs   = require('fs');
const path = require('path');
const os   = require('os');

const E    = window.__editor;
const reg  = E.registry;
const P    = E.proj;

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
function near(a, b, eps, msg) {
    if (Math.abs(a - b) > (eps || 1e-6)) {
        throw new Error((msg || 'near') + ': ' + a + ' vs ' + b);
    }
}

function rmrf(p) {
    if (!fs.existsSync(p)) return;
    if (fs.statSync(p).isDirectory()) {
        for (const entry of fs.readdirSync(p)) rmrf(path.join(p, entry));
        fs.rmdirSync(p);
    } else {
        fs.unlinkSync(p);
    }
}
let counter = 0;
function freshPath() {
    counter++;
    return path.join(os.tmpdir(),
        `bro-scene-editor-test-${Date.now()}-${counter}.bro`);
}

// Reset to a known baseline between tests: blank scene, zero history, clean.
function resetState() {
    E.setupDefaultScene();
    E.history.clear();
    P.markClean();
    // Drop any path inherited from a prior test's saveTo.
    P._path = null;
    P._createdAt = null;
}

resetState();

// -------------------------------------------------------------------------
// Baseline
// -------------------------------------------------------------------------

t('initial: clean, Untitled, one default box', () => {
    resetState();
    eq(P.path, null);
    eq(P.name, 'Untitled');
    eq(P.isDirty(), false);
    eq(reg.primitives.length, 1);
    eq(reg.primitives[0].name, 'Box');
});

// -------------------------------------------------------------------------
// Serialize / deserialize contract
// -------------------------------------------------------------------------

t('serializeScene captures all primitive state', () => {
    resetState();
    const cyl = E.outlinerAddPrimitive('cylinder');
    cyl.color = '#123456';
    reg.setActive(cyl.id);
    const state = E.serializeScene();
    eq(state.primitives.length, 2);
    const cylState = state.primitives.find(p => p.id === cyl.id);
    truthy(cylState, 'cylinder present');
    eq(cylState.color, '#123456');
    eq(Array.isArray(cylState.positions), true);
    eq(Array.isArray(cylState.indices), true);
    eq(cylState.positions.length, cyl.positions.length);
    eq(state.activeId, cyl.id);
});

t('deserializeScene restores primitives + preserves edits', () => {
    resetState();
    const cyl = E.outlinerAddPrimitive('cylinder');
    // Simulate an edit: translate every vertex +3 on X.
    const edited = new Float32Array(cyl.positions);
    for (let i = 0; i < edited.length; i += 3) edited[i] += 3.0;
    cyl.updateGeometry(edited, new Uint32Array(cyl.indices),
        cyl.normals ? new Float32Array(cyl.normals) : null);
    const editedX = cyl.positions[0];

    const snap = E.serializeScene();
    E.setupDefaultScene();
    eq(reg.primitives.length, 1);

    E.deserializeScene(snap);
    eq(reg.primitives.length, 2);
    const back = reg.getById(cyl.id);
    truthy(back, 'cylinder restored with same id');
    near(back.positions[0], editedX);
});

// -------------------------------------------------------------------------
// Full save → reload cycle
// -------------------------------------------------------------------------

t('saveTo writes bundle; openPath restores exact state', () => {
    const dir = freshPath();
    try {
        resetState();
        const cyl = E.outlinerAddPrimitive('cylinder');
        const cylId = cyl.id;
        reg.setName(cyl.id, 'Tower');
        const samplePos = Array.from(cyl.positions.slice(0, 9));

        truthy(P.saveTo(dir), 'saveTo returns true');
        truthy(fs.existsSync(path.join(dir, 'project.json')));
        eq(P.path, dir);
        eq(P.isDirty(), false);

        // Destroy state and reload.
        E.setupDefaultScene();
        E.history.clear();
        P.markClean();
        P._path = null;

        truthy(P.openPath(dir), 'openPath returns true');
        eq(P.path, dir);
        eq(reg.primitives.length, 2);
        const restored = reg.getById(cylId);
        truthy(restored, 'cylinder restored with original id');
        eq(restored.name, 'Tower');
        eq(Array.from(restored.positions.slice(0, 9)), samplePos);
    } finally { rmrf(dir); }
});

t('dirty flag tracks through save / edit / save', () => {
    const dir = freshPath();
    try {
        resetState();
        eq(P.isDirty(), false);

        E.outlinerAddPrimitive('sphere');   // history.record → dirty
        eq(P.isDirty(), true);

        P.saveTo(dir);
        eq(P.isDirty(), false, 'clean after save');

        E.outlinerAddPrimitive('plane');
        eq(P.isDirty(), true, 'dirty after another add');

        P.save();
        eq(P.isDirty(), false, 'clean again');
    } finally { rmrf(dir); }
});

t('new() resets to default scene, clears history + path', () => {
    const dir = freshPath();
    try {
        resetState();
        E.outlinerAddPrimitive('cylinder');
        E.outlinerAddPrimitive('sphere');
        P.saveTo(dir);
        eq(reg.primitives.length, 3);
        truthy(E.history.size() > 0);

        truthy(P.new());
        eq(reg.primitives.length, 1, 'default scene restored');
        eq(reg.primitives[0].name, 'Box');
        eq(P.path, null);
        eq(P.isDirty(), false);
        eq(E.history.size(), 0, 'history cleared');
    } finally { rmrf(dir); }
});

t('open clears history (no undo across file boundary)', () => {
    const dir = freshPath();
    try {
        resetState();
        E.outlinerAddPrimitive('cylinder');
        P.saveTo(dir);
        // Add more stuff post-save — creates history we should NOT carry
        // across a reload.
        E.outlinerAddPrimitive('sphere');
        E.outlinerAddPrimitive('plane');
        truthy(E.history.size() >= 2);

        P.openPath(dir);
        eq(E.history.size(), 0, 'history empty after load');
        eq(reg.primitives.length, 2, 'scene is saved state, not post-save state');
    } finally { rmrf(dir); }
});

// -------------------------------------------------------------------------
// Edge cases
// -------------------------------------------------------------------------

t('openPath of app-mismatched file throws', () => {
    const dir = freshPath();
    try {
        fs.mkdirSync(dir, { recursive: true });
        fs.writeFileSync(path.join(dir, 'project.json'), JSON.stringify({
            bro_project: 1, app: 'some-other-app', schema: 1, data: {},
        }), 'utf8');
        let threw = false;
        try { P.openPath(dir); } catch (e) { threw = true; }
        truthy(threw, 'openPath throws on mismatched app id');
    } finally { rmrf(dir); }
});

t('dirty indicator: history.record triggers P.change', () => {
    resetState();
    let changes = 0;
    const unsub = P.on('change', () => changes++);
    try {
        eq(changes, 0);
        E.outlinerAddPrimitive('sphere');
        truthy(changes >= 1, 'change emitted after dirty transition');
    } finally { unsub(); }
});

// -------------------------------------------------------------------------
// End
// -------------------------------------------------------------------------

console.log(`\n${tests - failed}/${tests} passed`);
if (failed > 0) throw new Error(`${failed} test(s) failed`);
