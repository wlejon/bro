// Headless tests for multi-primitive support.
//
// Exercises PrimitiveRegistry + Primitive + multi-primitive inference via
// __editor hooks — no synthetic mouse events. Verifies:
//   - registry add/remove/setActive/setVisible
//   - pickAt returns the nearer primitive under a ray
//   - hidden primitives drop out of picking + inference
//   - push/pull on a non-active primitive commits on that primitive
//   - deleting a primitive removes its snap features from inference
//
// Run: bro-headless apps/scene-editor apps/scene-editor/test_primitives.js

advanceTime(100);
flush();

const E = window.__editor;
const reg = E.registry;

// --- Initial registry state ------------------------------------------------

assert(reg, 'registry exposed on __editor');
assert(reg.primitives.length === 1, 'one default primitive at startup');
const defaultBox = reg.primitives[0];
assert(defaultBox === reg.active, 'default primitive is active');
assert(defaultBox.name === 'Box', `default primitive name "Box" (got "${defaultBox.name}")`);

// --- Add a second box offset along +X --------------------------------------

const box2 = reg.create({
    type: 'box',
    name: 'Box 2',
    color: '#ffa502',
    position: [3, 0, 0],       // center at x=3, y=0, z=0 (same spec as outliner)
    params: { sx: 1, sy: 1, sz: 1 },
});
assert(reg.primitives.length === 2, 'two primitives after add');
assert(reg.active === defaultBox, 'adding does not steal active');

// Positions are LOCAL-space; the +3 offset lives on box2.translation and
// composes through box2.getWorldMatrix(). Verify world-space extents come
// out as x ∈ [2, 4].
assert(Math.abs(box2.translation[0] - 3) < 1e-5,
    `box2 translation.x ≈ 3 (got ${box2.translation[0]})`);
let minX = Infinity, maxX = -Infinity;
for (let i = 0; i < box2.positions.length; i += 3) {
    const wp = box2.localToWorldPoint([box2.positions[i], box2.positions[i+1], box2.positions[i+2]]);
    if (wp[0] < minX) minX = wp[0];
    if (wp[0] > maxX) maxX = wp[0];
}
assert(Math.abs(minX - 2) < 1e-5, `box2 world minX ≈ 2 (got ${minX})`);
assert(Math.abs(maxX - 4) < 1e-5, `box2 world maxX ≈ 4 (got ${maxX})`);

// The local BVH is identity-mesh only; world-space raycasts go through
// Primitive.raycastWorld which transforms ray into local before casting
// and returns the hit point in world.
const hit2 = box2.raycastWorld([3, 0, 5], [0, 0, -1], 0);
assert(hit2, 'box2 world raycast hits ray aimed at its center');
assert(Math.abs(hit2.position[0] - 3) < 1e-4,
    `box2 hit x ≈ 3 (got ${hit2 && hit2.position[0]})`);

// --- pickAt picks the nearer primitive -------------------------------------
//
// Ray from (0, 0, 5) along -Z hits both boxes, but the default box (at x=0)
// is much closer along this line of sight than box2 (at x=3). Actually,
// wait — the ray along -Z from (0,0,5) misses box2 entirely. Use a ray
// aimed specifically at the overlap-free region to clearly pick each.

{
    // Aim at default box center: origin (0, 0, 5), dir -Z. Hits default.
    const pick = reg.pickAt([0, 0, 5], [0, 0, -1]);
    assert(pick, 'ray at origin picks something');
    assert(pick.primitive === defaultBox, 'ray aimed at origin picks default box');
    assert(Math.abs(pick.hit.position[2] - 1) < 1e-4, `hit z ≈ 1 (got ${pick.hit.position[2]})`);
}
{
    // Aim at box2 center: origin (3, 0, 5), dir -Z. Hits box2.
    const pick = reg.pickAt([3, 0, 5], [0, 0, -1]);
    assert(pick, 'ray at x=3 picks something');
    assert(pick.primitive === box2, `ray at x=3 picks box2 (got "${pick.primitive.name}")`);
}

// Nearest-hit when a ray intersects both. Place a ray starting between the
// two boxes and aim it toward whichever is in front along the ray direction.
// From (2, 0, 5), -Z direction: only box2 is hit (its x-range [2,4]).
{
    const pick = reg.pickAt([2, 0, 5], [0, 0, -1]);
    assert(pick, 'edge pick succeeds');
    assert(pick.primitive === box2, 'shared-x ray hits box2 (default box x-range is [-1,1])');
}

// Nearest-hit disambiguation: ray that passes through both boxes, box2
// first. From (5, 0, 0) along -X: hits box2 at x=4 (distance 1), then
// continues past to default box at x=1 (distance 4). Box2 wins.
{
    const o = [5, 0, 0];
    const d = [-1, 0, 0];
    const pick = reg.pickAt(o, d);
    assert(pick, 'collinear ray picks something');
    assert(pick.primitive === box2,
        `collinear ray hits box2 first (got "${pick.primitive.name}")`);
    const defaultHit = defaultBox.bvh.raycast(defaultBox.mesh, o, d, 0);
    assert(defaultHit, 'control: default box is also on the ray');
    assert(pick.hit.distance < defaultHit.distance,
        `picked distance < default-box distance (picked ${pick.hit.distance.toFixed(3)} ` +
        `vs default ${defaultHit.distance.toFixed(3)})`);
}

// --- setVisible removes from picking + inference ---------------------------

reg.setVisible(box2.id, false);
assert(!box2.visible, 'box2 hidden after setVisible(false)');
{
    const pick = reg.pickAt([3, 0, 5], [0, 0, -1]);
    assert(!pick, 'hidden box2 does not receive picks');
}
// Multi-geo inference must also drop the hidden primitive. collectInferenceGeos
// returns only visible ones.
{
    const geos = reg.collectInferenceGeos();
    assert(geos.length === 1, `hidden primitive excluded from inference (got ${geos.length} geos)`);
    // collectInferenceGeos now yields WORLD-space feature sets derived from
    // each visible primitive's local features. The returned object carries
    // its source primitive via `_owner` — identity-compare through that.
    assert(geos[0]._owner === defaultBox, 'remaining geo is default box');
}
reg.setVisible(box2.id, true);
assert(box2.visible, 'box2 visible again');

// --- Multi-primitive snap resolution ---------------------------------------
//
// A camera + cursor aimed at box2's (3,1,1) corner must produce an endpoint
// snap on box2, not on any default-box vertex. Uses the scene's real camera
// (orbit starts at dist=4 target=origin), so project manually via Inference.

{
    const I = window.Inference;
    const cnv = document.getElementById('canvas');
    const camOpts = Camera.orbitViewOpts(E.cam, cnv);
    // Use the canvas's own client dims for projection — that's the FBO size
    // the engine builds the projection matrix against. The window viewport
    // (1920x1080) and the canvas client area can differ when other DOM
    // elements share the page, so hardcoding either side breaks round-trip.
    const W = cnv.clientWidth, H = cnv.clientHeight;
    const corner = [3 + 1, 0 + 1, 0 + 1];  // box2's (+X, +Y, +Z) corner
    const sp = I.worldToScreen(corner, camOpts, W, H);
    assert(!sp.behind, 'box2 corner is in front of camera');
    // Build a ray matching a cursor at that screen position.
    const ray = E.screenToRay(sp.x, sp.y);
    const snap = E.resolveSnap(sp.x, sp.y, ray, false);
    assert(snap, 'resolveSnap returns a snap at box2 corner');
    assert(snap.type === 'endpoint',
        `snap is endpoint at box2 corner (got ${snap.type})`);
    assert(Math.abs(snap.position[0] - 4) < 1e-4, `snap x ≈ 4 (got ${snap.position[0]})`);
    assert(Math.abs(snap.position[1] - 1) < 1e-4, `snap y ≈ 1 (got ${snap.position[1]})`);
    assert(Math.abs(snap.position[2] - 1) < 1e-4, `snap z ≈ 1 (got ${snap.position[2]})`);
}

// --- Push/pull on the non-active primitive --------------------------------
//
// defaultBox is active. Begin a push/pull on box2 directly (as if the user
// clicked box2 with pushpull tool) and verify the commit lands on box2,
// not on the active primitive — the drag's captured primitive reference
// must win over activeness.

assert(reg.active === defaultBox, 'default box is active going in');
const defBefore = Array.from(defaultBox.positions);

const top2 = box2.faceGroups.groups.find(g => g.normal[1] > 0.999);
assert(top2, 'box2 has +Y face group');
const top2Tri = top2.tris[0];
function centroidOf(prim, triIdx) {
    const P = prim.positions, I = prim.indices;
    const i0 = I[triIdx*3], i1 = I[triIdx*3+1], i2 = I[triIdx*3+2];
    return [
        (P[i0*3]+P[i1*3]+P[i2*3])/3,
        (P[i0*3+1]+P[i1*3+1]+P[i2*3+1])/3,
        (P[i0*3+2]+P[i1*3+2]+P[i2*3+2])/3,
    ];
}
E.beginPushPullOn(box2, {
    triangleIndex: top2Tri,
    position: centroidOf(box2, top2Tri),
    normal: top2.normal.slice(),
    distance: 0,
});
assert(E.pushpull.active, 'push/pull active');
assert(E.pushpull.primitive === box2, 'drag bound to box2 (not default)');
E.applyPushPull(0.5);
E.commitPushPull();

// SketchUp-style surgery duplicates the 4 top corners at the new height
// rather than warping the originals. Original top verts at y=1 still exist
// (now part of the extended side faces).
let b2TopCount = 0;
for (let vi = 0; vi < box2.positions.length / 3; vi++) {
    if (Math.abs(box2.positions[vi*3 + 1] - 1.5) < 1e-5) b2TopCount++;
}
assert(b2TopCount === 12, `box2 has 12 top-vert indices at y=1.5 — top face + 4 retriangulated side walls each own their corners (got ${b2TopCount})`);

// defaultBox must be untouched.
for (let i = 0; i < defBefore.length; i++) {
    assert(Math.abs(defaultBox.positions[i] - defBefore[i]) < 1e-5,
        `default box unchanged by push/pull on box2 (idx ${i})`);
}

// --- Post-commit VCB re-apply targets the original primitive ---------------
//
// The VCB lastOp should stash box2 as its target. Re-applying at 0.25 must
// move box2's top from 1.5 → 1.75 and leave defaultBox alone.

assert(E.measureBoxState.lastOp, 'lastOp stashed after commit');
assert(E.measureBoxState.lastOp.primitiveId === box2.id,
    `lastOp primitiveId = box2.id (got ${E.measureBoxState.lastOp.primitiveId})`);
const ok = E.redoLastPushPull(0.25);
assert(ok, 'redoLastPushPull succeeded on non-active primitive');
b2TopCount = 0;
for (let vi = 0; vi < box2.positions.length / 3; vi++) {
    if (Math.abs(box2.positions[vi*3 + 1] - 1.75) < 1e-5) b2TopCount++;
}
assert(b2TopCount === 12, `box2 top at y=1.75 after redo (got ${b2TopCount}) — surgery adds 4 new corners, retri'd walls each own their own`);

// --- Remove a primitive ---------------------------------------------------

const box2Id = box2.id;
reg.remove(box2Id);
assert(reg.primitives.length === 1, 'one primitive after remove');
assert(reg.primitives[0] === defaultBox, 'only default box remains');
assert(reg.active === defaultBox, 'active falls back to remaining primitive');
assert(reg.getById(box2Id) === null, 'removed id no longer resolvable');
{
    const pick = reg.pickAt([3, 0, 5], [0, 0, -1]);
    assert(!pick, 'removed primitive no longer pickable');
}
// Inference geo list reflects removal.
assert(reg.collectInferenceGeos().length === 1,
    `inference geos = 1 after remove (got ${reg.collectInferenceGeos().length})`);

// --- Remove then active ---------------------------------------------------
//
// Remove the default box too — registry.active must become null (empty
// scene) and pickAt must return null.

reg.remove(defaultBox.id);
assert(reg.primitives.length === 0, 'empty registry');
assert(reg.active === null, 'no active primitive in empty registry');
assert(reg.pickAt([0, 0, 5], [0, 0, -1]) === null,
    'pickAt on empty registry → null');

// --- Restore a fresh box for the final screenshot / any follow-on tests ----

reg.create({
    type: 'box',
    name: 'Box',
    color: '#74b9ff',
    params: { sx: 1, sy: 1, sz: 1 },
});
assert(reg.primitives.length === 1, 'restored default box');

console.log(`OK — multi-primitive: pickAt nearest, hidden excluded, inference cross-primitive, ` +
            `push/pull targets drag's primitive (not active), VCB redo targets original, ` +
            `remove clears from pick + inference`);
