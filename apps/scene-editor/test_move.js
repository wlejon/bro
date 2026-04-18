// Headless test for the Move tool (TRS-based).
//
// Since Primitive is now a SceneObject with a local-space mesh + TRS, the
// Move tool mutates the target's `translation` — not its vertex buffer.
// This test verifies:
//   - begin/commit updates translation by the applied delta
//   - local-space positions are untouched across a move
//   - world-space raycast (raycastWorld) follows the moved primitive
//   - world-space inference geo follows the move (cache invalidation)
//   - cancel rolls translation back exactly
//   - drag is bound to its primitive, not registry.active
//   - click-release with no motion is a no-op
//   - registry exclusion: pickAt + collectInferenceGeos respect excludeId
//   - delete-during-drag cancels safely
//   - chained moves compose
//
// Run: bro-headless apps/scene-editor apps/scene-editor/test_move.js

advanceTime(100);
flush();

const E = window.__editor;
const reg = E.registry;

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

// Synthesize a world-space hit at the centroid of the +Y face.
function topHitFor(prim) {
    const top = prim.faceGroups.groups.find(g => g.normal[1] > 0.999);
    const tri = top.tris[0];
    const I = prim.indices, P = prim.positions;
    const i0 = I[tri*3], i1 = I[tri*3+1], i2 = I[tri*3+2];
    const cLocal = [
        (P[i0*3]   + P[i1*3]   + P[i2*3])   / 3,
        (P[i0*3+1] + P[i1*3+1] + P[i2*3+1]) / 3,
        (P[i0*3+2] + P[i1*3+2] + P[i2*3+2]) / 3,
    ];
    const cWorld = prim.localToWorldPoint(cLocal);
    const nWorld = prim.localToWorldNormal(top.normal);
    return { triangleIndex: tri, position: cWorld, normal: nWorld, distance: 0,
             _localPosition: cLocal, _localNormal: top.normal.slice() };
}

// --- Begin + apply (local positions stay clean; translation moves) ---------

const box2PositionsBefore = Array.from(box2.positions);
const box2TranslationBefore = box2.translation.slice();
const box1TranslationBefore = box1.translation.slice();

E.beginMove(box2, topHitFor(box2));
assert(E.moveToolState.active, 'move active after begin');
assert(E.moveToolState.object === box2, 'drag bound to box2');

E.applyMoveDelta(2, 0.5, -1);
// Local positions untouched — only translation changed.
for (let i = 0; i < box2PositionsBefore.length; i++) {
    assert(box2.positions[i] === box2PositionsBefore[i],
        `local positions unchanged during move (idx ${i})`);
}
assert(Math.abs(box2.translation[0] - (box2TranslationBefore[0] + 2)) < 1e-5,
    'mid-drag translation.x += 2');
assert(Math.abs(box2.translation[1] - (box2TranslationBefore[1] + 0.5)) < 1e-5,
    'mid-drag translation.y += 0.5');
assert(Math.abs(box2.translation[2] - (box2TranslationBefore[2] - 1)) < 1e-5,
    'mid-drag translation.z -= 1');

E.commitMove();
assert(!E.moveToolState.active, 'move inactive after commit');

// Local buffers remain identical after commit.
for (let i = 0; i < box2PositionsBefore.length; i++) {
    assert(box2.positions[i] === box2PositionsBefore[i],
        `post-commit local positions unchanged (idx ${i})`);
}
// Translation persists post-commit.
assert(Math.abs(box2.translation[0] - (box2TranslationBefore[0] + 2)) < 1e-5,
    'post-commit translation.x = start + 2');

// Box1 untouched.
for (let i = 0; i < 3; i++) {
    assert(box1.translation[i] === box1TranslationBefore[i],
        `box1 translation unchanged (axis ${i})`);
}
assert(box2.faceGroups.groups.length === 6, 'box2 still 6 face groups');

// World-space raycast follows the new position (box2 now at (5, 0.5, -1);
// box half-extent = 1, so the +Y face sits at world y = 1.5).
{
    const hit = box2.raycastWorld([5, 5, -1], [0, -1, 0], 0);
    assert(hit, 'world raycast at new box2 center hits');
    assert(Math.abs(hit.position[1] - 1.5) < 1e-4,
        `hit y ≈ 1.5 at +Y face top (got ${hit.position[1]})`);
}
// And no longer at the original location.
{
    const hit = box2.raycastWorld([3, 0, 5], [0, 0, -1], 0);
    assert(!hit, 'world raycast at original location misses');
}

// World-space inference geo reflects the new position (cache invalidation).
{
    const wg = box2.getWorldInferenceGeo();
    let foundShifted = false;
    for (let vi = 0; vi < wg.vertCount; vi++) {
        const x = wg.positions[vi * 3 + 0];
        // box2 is now at (5, 0.5, -1); its +X corners are at x=6.
        if (Math.abs(x - 6) < 1e-4) { foundShifted = true; break; }
    }
    assert(foundShifted, 'world inference geo has endpoint at new +X corner (x=6)');
}

// --- Cancel rolls translation back exactly ---------------------------------

const transBeforeCancel = box2.translation.slice();
E.beginMove(box2, topHitFor(box2));
E.applyMoveDelta(10, 10, 10);
E.cancelMove();
assert(!E.moveToolState.active, 'move inactive after cancel');
for (let i = 0; i < 3; i++) {
    assert(Math.abs(box2.translation[i] - transBeforeCancel[i]) < 1e-5,
        `cancel restores translation (axis ${i})`);
}

// --- Move targets the drag's primitive, not registry.active ----------------

reg.setActive(box1.id);
assert(reg.active === box1, 'box1 active going in');
const box1TransBefore2 = box1.translation.slice();

E.beginMove(box2, topHitFor(box2));
assert(E.moveToolState.object === box2, 'drag bound to box2 even though box1 active');
E.applyMoveDelta(0, 0, 1);
E.commitMove();

for (let i = 0; i < 3; i++) {
    assert(box1.translation[i] === box1TransBefore2[i],
        `box1 translation unchanged when move targeted box2 (axis ${i})`);
}

// --- Click-release with no motion is a safe no-op --------------------------

const transNoMotion = box2.translation.slice();
E.beginMove(box2, topHitFor(box2));
E.commitMove();
for (let i = 0; i < 3; i++) {
    assert(Math.abs(box2.translation[i] - transNoMotion[i]) < 1e-5,
        `no-motion commit is no-op (axis ${i})`);
}

// --- Registry exclusion: pickAt + collectInferenceGeos respect excludeId ---

{
    // box2 is currently at (5, 0.5, 0) [after chained earlier deltas].
    // Aim straight down at that spot.
    const cx = box2.translation[0], cy = box2.translation[1] + 5, cz = box2.translation[2];
    const pickAll = reg.pickAt([cx, cy, cz], [0, -1, 0]);
    assert(pickAll && pickAll.primitive === box2, 'control: pickAt hits box2');

    const pickEx = reg.pickAt([cx, cy, cz], [0, -1, 0], { excludeId: box2.id });
    assert(!pickEx, 'pickAt with excludeId=box2 misses box2');

    const geosAll = reg.collectInferenceGeos();
    const geosEx  = reg.collectInferenceGeos({ excludeId: box2.id });
    assert(geosAll.length === 2, 'all geos returns 2');
    assert(geosEx.length === 1,  'excludeId=box2 returns 1 geo');
    assert(geosEx[0]._owner === box1, 'remaining geo is box1');
}

// --- Delete-during-drag cancels and doesn't crash --------------------------

const ghost = reg.create({
    type: 'box',
    name: 'Ghost',
    color: '#9b59b6',
    position: [0, 5, 0],
    params: { sx: 1, sy: 1, sz: 1 },
});
E.beginMove(ghost, topHitFor(ghost));
assert(E.moveToolState.active && E.moveToolState.object === ghost,
    'move active on ghost');
E.cancelMove();
reg.remove(ghost.id);
assert(!E.moveToolState.active, 'move inactive after cancel+remove');
assert(!reg.getById(ghost.id), 'ghost removed');

// --- Chained moves compose on translation ----------------------------------

const transPre = box2.translation.slice();
E.beginMove(box2, topHitFor(box2));
E.applyMoveDelta(1, 0, 0);
E.commitMove();
E.beginMove(box2, topHitFor(box2));
E.applyMoveDelta(0, 1, 0);
E.commitMove();
assert(Math.abs(box2.translation[0] - (transPre[0] + 1)) < 1e-5,
    'chained move translation.x += 1');
assert(Math.abs(box2.translation[1] - (transPre[1] + 1)) < 1e-5,
    'chained move translation.y += 1');

reg.remove(box2.id);

console.log(`OK — move: translation-based (no vertex mutation), world ` +
            `raycast + inference follow the move, cancel restores, ` +
            `drag-targets-its-object, exclusion options work, ` +
            `cancel-then-remove safe, chained moves compose`);
