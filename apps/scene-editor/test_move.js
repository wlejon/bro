// Headless test for the Move tool.
//
// Drives MoveTool through __editor hooks (no synthetic mouse). Verifies:
//   - basic translate: positions shift by delta on commit, normals/indices
//     untouched, BVH + face groups + inference geo all rebuild
//   - canonical positions are NOT mutated mid-drag (preview only)
//   - cancel restores positions exactly
//   - non-target primitives are completely untouched
//   - move targets the drag's primitive, not registry.active
//   - registry exclusion: pickAt + collectInferenceGeos respect excludeId
//   - delete-during-drag cancels safely
//   - chained moves compose
//
// Run: bro-headless apps/scene-editor apps/scene-editor/test_move.js

advanceTime(100);
flush();

const E = window.__editor;
const reg = E.registry;

// --- Setup: default Box at origin + Box 2 at +X ----------------------------

const box1 = reg.primitives[0];
assert(box1.name === 'Box', 'default box present');

const box2 = reg.create({
    type: 'box',
    name: 'Box 2',
    color: '#ffa502',
    position: [3, 0, 0],
    params: { sx: 1, sy: 1, sz: 1 },
});
assert(reg.primitives.length === 2, 'two primitives');
assert(reg.active === box1, 'default box active');

// Helper: synthesize a hit at the centroid of box's +Y face (any face works
// for Move since the pivot is just a reference point). Mirrors test_pushpull.
function topHitFor(prim) {
    const top = prim.faceGroups.groups.find(g => g.normal[1] > 0.999);
    const tri = top.tris[0];
    const I = prim.indices, P = prim.positions;
    const i0 = I[tri*3], i1 = I[tri*3+1], i2 = I[tri*3+2];
    const c = [
        (P[i0*3]   + P[i1*3]   + P[i2*3])   / 3,
        (P[i0*3+1] + P[i1*3+1] + P[i2*3+1]) / 3,
        (P[i0*3+2] + P[i1*3+2] + P[i2*3+2]) / 3,
    ];
    return { triangleIndex: tri, position: c, normal: top.normal.slice(), distance: 0 };
}

// --- Begin + apply (canonical positions stay clean) ------------------------

const box2Before = Array.from(box2.positions);
const box1Before = Array.from(box1.positions);
const box2NormalsBefore = Array.from(box2.normals);
const box2IndicesBefore = Array.from(box2.indices);

E.beginMove(box2, topHitFor(box2));
assert(E.moveToolState.active, 'move active after begin');
assert(E.moveToolState.primitive === box2, 'drag bound to box2');

// Mid-drag: previewMesh updates render node, canonical buffers untouched.
E.applyMoveDelta(2, 0.5, -1);
for (let i = 0; i < box2Before.length; i++) {
    assert(Math.abs(box2.positions[i] - box2Before[i]) < 1e-5,
        `preview does not mutate canonical positions (idx ${i})`);
}

// --- Commit ----------------------------------------------------------------

E.commitMove();
assert(!E.moveToolState.active, 'move inactive after commit');

// Every vertex shifted by the delta.
for (let i = 0; i < box2Before.length; i += 3) {
    assert(Math.abs(box2.positions[i    ] - (box2Before[i    ] + 2 )) < 1e-5,
        `vertex ${i/3} x shifted by 2`);
    assert(Math.abs(box2.positions[i + 1] - (box2Before[i + 1] + 0.5)) < 1e-5,
        `vertex ${i/3} y shifted by 0.5`);
    assert(Math.abs(box2.positions[i + 2] - (box2Before[i + 2] - 1 )) < 1e-5,
        `vertex ${i/3} z shifted by -1`);
}

// Translation preserves indices and normals exactly.
assert(box2.indices.length === box2IndicesBefore.length, 'indices length unchanged');
for (let i = 0; i < box2IndicesBefore.length; i++) {
    assert(box2.indices[i] === box2IndicesBefore[i],
        `index ${i} unchanged`);
}
assert(box2.normals.length === box2NormalsBefore.length, 'normals length unchanged');
for (let i = 0; i < box2NormalsBefore.length; i++) {
    assert(Math.abs(box2.normals[i] - box2NormalsBefore[i]) < 1e-5,
        `normal ${i} unchanged after translate`);
}

// Box1 untouched.
for (let i = 0; i < box1Before.length; i++) {
    assert(Math.abs(box1.positions[i] - box1Before[i]) < 1e-5,
        `non-target box1 unchanged (idx ${i})`);
}

// Face groups still 6 (no topology change).
assert(box2.faceGroups.groups.length === 6,
    `box2 still 6 face groups after move (got ${box2.faceGroups.groups.length})`);

// BVH rebuilt — raycast at the new center hits.
{
    const hit = box2.bvh.raycast(box2.mesh, [5, 0.5, -1], [0, 0, -1], 0);
    assert(hit, 'BVH rebuilt: raycast at new box2 center hits');
    assert(Math.abs(hit.position[0] - 5) < 1e-4,
        `BVH hit x ≈ 5 at new box2 location (got ${hit.position[0]})`);
}
// And no longer hits the original location.
{
    const hit = box2.bvh.raycast(box2.mesh, [3, 0, 5], [0, 0, -1], 0);
    assert(!hit, 'BVH rebuilt: original location no longer hit');
}

// Inference geo rebuilt to the new positions — every endpoint must be at
// shifted coordinates. Sample one corner: (3+1)+2 = 6 in x.
{
    const geo = box2.inferenceGeo;
    let foundShifted = false;
    for (let vi = 0; vi < geo.vertCount; vi++) {
        const x = geo.positions[vi * 3 + 0];
        if (Math.abs(x - 6) < 1e-4) { foundShifted = true; break; }
    }
    assert(foundShifted, 'inference geo has an endpoint at the new box2 +X corner');
}

// --- Cancel rolls back exactly ---------------------------------------------

const beforeCancel = Array.from(box2.positions);
E.beginMove(box2, topHitFor(box2));
E.applyMoveDelta(10, 10, 10);          // anywhere
E.cancelMove();
assert(!E.moveToolState.active, 'move inactive after cancel');
for (let i = 0; i < beforeCancel.length; i++) {
    assert(Math.abs(box2.positions[i] - beforeCancel[i]) < 1e-5,
        `cancel restores positions (idx ${i})`);
}

// --- Move targets the drag's primitive, not registry.active ----------------

reg.setActive(box1.id);
assert(reg.active === box1, 'box1 active going in');
const box1BeforeMove2 = Array.from(box1.positions);

E.beginMove(box2, topHitFor(box2));
assert(E.moveToolState.primitive === box2, 'drag bound to box2 even though box1 active');
E.applyMoveDelta(0, 0, 1);
E.commitMove();

// box1 still untouched.
for (let i = 0; i < box1BeforeMove2.length; i++) {
    assert(Math.abs(box1.positions[i] - box1BeforeMove2[i]) < 1e-5,
        `box1 unchanged when move targeted box2 (idx ${i})`);
}

// --- Click-release with no motion is a safe no-op (no collapse) ------------

const beforeNoMotion = Array.from(box2.positions);
E.beginMove(box2, topHitFor(box2));
// Skip applyMoveDelta entirely — the seed at begin should have left the
// working buffer at start positions.
E.commitMove();
for (let i = 0; i < beforeNoMotion.length; i++) {
    assert(Math.abs(box2.positions[i] - beforeNoMotion[i]) < 1e-5,
        `click-release with no motion is a no-op (idx ${i})`);
}

// --- Registry exclusion: pickAt + collectInferenceGeos respect excludeId ---

{
    // Without exclusion: pickAt at box2's center hits box2.
    const pickAll = reg.pickAt([5, 0.5, 0], [0, 0, -1]);  // box2 currently at x≈5,z≈0
    assert(pickAll && pickAll.primitive === box2, 'control: pickAt hits box2');

    // With exclusion: pickAt should miss box2 and return null (no other geometry there).
    const pickEx = reg.pickAt([5, 0.5, 0], [0, 0, -1], { excludeId: box2.id });
    assert(!pickEx, 'pickAt with excludeId=box2 misses box2');

    // collectInferenceGeos drops the excluded primitive.
    const geosAll = reg.collectInferenceGeos();
    const geosEx  = reg.collectInferenceGeos({ excludeId: box2.id });
    assert(geosAll.length === 2, 'all geos returns 2 (box1 + box2)');
    assert(geosEx.length === 1,  'excludeId=box2 returns 1 geo');
    assert(geosEx[0] === box1.inferenceGeo, 'remaining geo is box1');
}

// --- Delete-during-drag cancels and doesn't crash --------------------------
//
// Outliner delete path calls cancelMove before remove when the drag targets
// the deleted primitive. Test the cancel + remove sequence directly.

const ghost = reg.create({
    type: 'box',
    name: 'Ghost',
    color: '#9b59b6',
    position: [0, 5, 0],
    params: { sx: 1, sy: 1, sz: 1 },
});
E.beginMove(ghost, topHitFor(ghost));
assert(E.moveToolState.active && E.moveToolState.primitive === ghost,
    'move active on ghost');
// Mirror the outliner path: cancel before destroying so commit on a freed
// scene node doesn't fire later.
E.cancelMove();
reg.remove(ghost.id);
assert(!E.moveToolState.active, 'move inactive after cancel+remove');
assert(!reg.getById(ghost.id), 'ghost removed');

// --- Chained moves compose -------------------------------------------------

const box2Pre = Array.from(box2.positions);
E.beginMove(box2, topHitFor(box2));
E.applyMoveDelta(1, 0, 0);
E.commitMove();
E.beginMove(box2, topHitFor(box2));
E.applyMoveDelta(0, 1, 0);
E.commitMove();
for (let i = 0; i < box2Pre.length; i += 3) {
    assert(Math.abs(box2.positions[i    ] - (box2Pre[i    ] + 1)) < 1e-5,
        `chained move x shift (vert ${i/3})`);
    assert(Math.abs(box2.positions[i + 1] - (box2Pre[i + 1] + 1)) < 1e-5,
        `chained move y shift (vert ${i/3})`);
}

// --- Cleanup so the GUI starts in a sensible state -------------------------

reg.remove(box2.id);

console.log(`OK — move: translates positions, preserves indices/normals, ` +
            `BVH+groups+inference rebuild, cancel rolls back, non-target ` +
            `untouched, drag-targets-its-primitive, exclusion options work, ` +
            `cancel-then-remove safe, chained moves compose`);
