// Headless tests for the Measurement Box (VCB).
//
// Exercises: parser, mid-drag distance override, post-commit re-apply,
// Backspace/Escape semantics, invalid-input guards. Uses __editor hooks
// (feedKey + programmatic pushpull) so nothing depends on real DOM focus
// or KeyboardEvent dispatch.
//
// Run: bro-headless apps/scene-editor apps/scene-editor/test_measurebox.js

advanceTime(100);
flush();

const E  = window.__editor;
const MB = window.MeasureBox;
assert(MB, 'MeasureBox module is loaded');

// --- Parser ----------------------------------------------------------------

assert(MB.parseValue('2')     === 2,      'parse "2"');
assert(MB.parseValue('2.5')   === 2.5,    'parse "2.5"');
assert(MB.parseValue('-1.5')  === -1.5,   'parse "-1.5"');
assert(MB.parseValue('0.25')  === 0.25,   'parse "0.25"');
assert(MB.parseValue('-0.5')  === -0.5,   'parse "-0.5"');
assert(MB.parseValue('')      === null,   'empty → null');
assert(MB.parseValue('-')     === null,   'orphan "-" → null');
assert(MB.parseValue('.')     === null,   'orphan "." → null');
assert(MB.parseValue('1.2.3') === null,   'bad number → null');
assert(MB.parseValue('abc')   === null,   'non-number → null');

// --- feedKey state transitions ---------------------------------------------

{
    const s = MB.createState();
    // Inactive state ignores all input.
    assert(MB.feedKey(s, '2') === 'ignored', 'feed ignored while inactive');

    MB.setActive(s, true);
    assert(MB.feedKey(s, '2') === 'append', 'append digit');
    assert(s.buffer === '2', `buffer = "2" (got "${s.buffer}")`);
    assert(MB.feedKey(s, '.') === 'append', 'append decimal');
    assert(MB.feedKey(s, '.') === 'ignored', 'second decimal ignored');
    assert(MB.feedKey(s, '5') === 'append', 'append digit after decimal');
    assert(s.buffer === '2.5', `buffer = "2.5" (got "${s.buffer}")`);
    assert(MB.feedKey(s, 'Backspace') === 'append', 'backspace = append');
    assert(s.buffer === '2.', `after backspace "2." (got "${s.buffer}")`);
    assert(MB.feedKey(s, 'Enter') === 'ignored',
        '"2." is not yet valid — Enter ignored');
    assert(MB.feedKey(s, '0') === 'append');
    assert(MB.feedKey(s, 'Enter') === 'commit', 'valid Enter commits');
    // Buffer NOT auto-cleared on commit (caller decides).
    assert(s.buffer === '2.0', 'buffer retained after commit');

    s.buffer = ''; s.tick++;
    assert(MB.feedKey(s, '-') === 'append', 'minus at start');
    assert(s.buffer === '-');
    assert(MB.feedKey(s, '-') === 'ignored', 'second minus ignored');
    assert(MB.feedKey(s, 'Enter') === 'ignored', '"-" alone invalid');
    assert(MB.feedKey(s, '1') === 'append');
    assert(s.buffer === '-1');
    assert(MB.feedKey(s, 'Enter') === 'commit', '"-1" commits');

    s.buffer = ''; s.tick++;
    assert(MB.feedKey(s, 'Escape') === 'cancel', 'Escape cancels');
    assert(MB.feedKey(s, 'x') === 'ignored', 'unknown key ignored');
    assert(MB.feedKey(s, 'Backspace') === 'ignored', 'backspace on empty ignored');
}

// --- VCB lifecycle during a push/pull drag ---------------------------------

// Start with a clean cube.
assert(E.boxPositions.length === 24 * 3, 'initial cube has 24 verts');

// Find top (+Y) face.
const topG = E.faceGroups.groups.find(g => g.normal[1] > 0.999);
assert(topG, '+Y face group present');
const topTri = topG.tris[0];

function centroidOf(triIdx) {
    const P = E.boxPositions, I = E.boxIndices;
    const i0 = I[triIdx*3], i1 = I[triIdx*3+1], i2 = I[triIdx*3+2];
    return [
        (P[i0*3]+P[i1*3]+P[i2*3])/3,
        (P[i0*3+1]+P[i1*3+1]+P[i2*3+1])/3,
        (P[i0*3+2]+P[i1*3+2]+P[i2*3+2])/3,
    ];
}

// Begin push/pull — this should activate the VCB.
E.beginPushPull({
    triangleIndex: topTri,
    position: centroidOf(topTri),
    normal: topG.normal.slice(),
    distance: 0,
});
assert(E.pushpull.active, 'drag active after begin');
assert(E.measureBoxState.active, 'VCB active after begin');
assert(E.measureBoxState.buffer === '', 'VCB starts empty');

// Type "2" + Enter — preview should be at 2, commit should bake y=3 top.
assert(E.handleMeasureBoxKey('2'), 'key "2" consumed');
assert(E.measureBoxState.buffer === '2', 'buffer = "2"');
// applyPushPull should have fired via the append action.
assert(Math.abs(E.pushpull.distance - 2) < 1e-6,
    `mid-drag preview at distance 2 (got ${E.pushpull.distance})`);

// Enter commits at exactly 2.
assert(E.handleMeasureBoxKey('Enter'), 'Enter consumed');
assert(!E.pushpull.active, 'drag ended after commit');

// SketchUp-style surgery dups 4 corner verts at the new top (y=3); the
// originals at y=1 stay (now interior to the extended side faces).
let topCount = 0;
for (let vi = 0; vi < E.boxPositions.length / 3; vi++) {
    if (Math.abs(E.boxPositions[vi * 3 + 1] - 3) < 1e-5) topCount++;
}
assert(topCount === 12, `12 top vert-indices at y=3 after exact-2 extrusion (got ${topCount})`);

// Post-commit window is still active; lastOp is stashed.
assert(E.measureBoxState.active, 'VCB stays active post-commit (re-apply window)');
assert(E.measureBoxState.lastOp, 'lastOp stashed for re-apply');
assert(E.measureBoxState.buffer === '', 'buffer cleared for fresh post-commit input');

// --- Post-commit re-apply --------------------------------------------------

// Type "1.5" + Enter without dragging — should extrude the +Y face again by 1.5.
assert(E.handleMeasureBoxKey('1'));
assert(E.handleMeasureBoxKey('.'));
assert(E.handleMeasureBoxKey('5'));
assert(E.measureBoxState.buffer === '1.5', 'buffer = "1.5"');
assert(E.handleMeasureBoxKey('Enter'), 'Enter re-applies');

// Top should now be at y = 3 + 1.5 = 4.5.
topCount = 0;
for (let vi = 0; vi < E.boxPositions.length / 3; vi++) {
    if (Math.abs(E.boxPositions[vi * 3 + 1] - 4.5) < 1e-5) topCount++;
}
assert(topCount === 12, `12 top vert-indices at y=4.5 after re-apply (got ${topCount})`);

// Post-re-apply: VCB still active with fresh lastOp for chaining.
assert(E.measureBoxState.active, 'VCB still active after re-apply');
assert(E.measureBoxState.lastOp, 'lastOp refreshed from re-apply commit');

// --- Invalid input doesn't commit ------------------------------------------

// Clear VCB state for a clean check. Start a new drag.
E.setTool('select');
E.setTool('pushpull');
assert(!E.measureBoxState.active, 'VCB off after tool switch');
assert(!E.measureBoxState.lastOp, 'lastOp cleared on tool switch');

const topG2 = E.faceGroups.groups.find(g => g.normal[1] > 0.999);
E.beginPushPull({
    triangleIndex: topG2.tris[0],
    position: centroidOf(topG2.tris[0]),
    normal: topG2.normal.slice(),
    distance: 0,
});
assert(E.measureBoxState.active, 'VCB back up for new drag');

// Type "-" alone → invalid. Enter should not commit.
assert(E.handleMeasureBoxKey('-'));
assert(E.measureBoxState.buffer === '-', 'buffer = "-"');
const before = E.handleMeasureBoxKey('Enter');  // consumed but ignored action
// action='ignored' means handleMeasureBoxKey returned false.
assert(!before, 'Enter on invalid buffer is ignored');
assert(E.pushpull.active, 'drag still active (invalid Enter didn\'t commit)');

// Now type a digit → "-1" → Enter commits.
assert(E.handleMeasureBoxKey('1'));
assert(E.handleMeasureBoxKey('Enter'));
assert(!E.pushpull.active, 'negative distance committed');

// Negative extrusion: dups 4 corner verts at y = 4.5 - 1 = 3.5.
topCount = 0;
for (let vi = 0; vi < E.boxPositions.length / 3; vi++) {
    if (Math.abs(E.boxPositions[vi * 3 + 1] - 3.5) < 1e-5) topCount++;
}
assert(topCount === 12, `12 top vert-indices at y=3.5 after -1 (got ${topCount})`);

// --- Escape during drag cancels ---------------------------------------------

const topG3 = E.faceGroups.groups.find(g => g.normal[1] > 0.999);
const snapshot = Array.from(E.boxPositions);
E.beginPushPull({
    triangleIndex: topG3.tris[0],
    position: centroidOf(topG3.tris[0]),
    normal: topG3.normal.slice(),
    distance: 0,
});
E.handleMeasureBoxKey('5');
assert(E.pushpull.distance === 5, 'previewed at 5');
assert(E.handleMeasureBoxKey('Escape'), 'Escape consumed');
assert(!E.pushpull.active, 'drag cancelled');
assert(!E.measureBoxState.active, 'VCB deactivated after Esc');
for (let i = 0; i < snapshot.length; i++) {
    assert(Math.abs(E.boxPositions[i] - snapshot[i]) < 1e-5,
        `Esc reverted geometry (idx ${i})`);
}

// --- Mousedown-on-canvas closes the post-commit window ---------------------
//
// Simulate: do a push/pull, commit (VCB enters re-apply window), then
// start a new drag. The post-commit lastOp should be dropped at the
// mousedown — beginPushPull clears it itself, so after the new begin the
// buffer should be empty and (most importantly) the lastOp should be the
// fresh one, not the previous one.

const topG4 = E.faceGroups.groups.find(g => g.normal[1] > 0.999);
E.beginPushPull({
    triangleIndex: topG4.tris[0],
    position: centroidOf(topG4.tris[0]),
    normal: topG4.normal.slice(),
    distance: 0,
});
E.handleMeasureBoxKey('0');
E.handleMeasureBoxKey('.');
E.handleMeasureBoxKey('5');
E.handleMeasureBoxKey('Enter');
assert(!E.pushpull.active, 'committed second extrusion');
const lastOpA = E.measureBoxState.lastOp;
assert(lastOpA, 'lastOp A stashed');

// Begin another drag — mid-drag the old lastOp should be superseded.
const topG5 = E.faceGroups.groups.find(g => g.normal[1] > 0.999);
E.beginPushPull({
    triangleIndex: topG5.tris[0],
    position: centroidOf(topG5.tris[0]),
    normal: topG5.normal.slice(),
    distance: 0,
});
// beginPushPull clears lastOp.
assert(E.measureBoxState.lastOp === null,
    'lastOp cleared by a new beginPushPull');
E.cancelPushPull();

console.log(`OK — VCB: parser + mid-drag override + post-commit re-apply + ` +
            `Esc cancels + invalid-input guards`);
