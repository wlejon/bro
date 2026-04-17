// Headless smoke test for the push/pull tool (SketchUp-style surgery).
//
// Exercises the tool via __editor hooks (not synthetic mouse events) so the
// test is deterministic — no dependency on screen-space ray math, camera
// state, or pointer lock.
//
// Run: bro-headless apps/scene-editor apps/scene-editor/test_pushpull.js

advanceTime(100);
flush();

const E = window.__editor;

function vertCountAtY(y, eps) {
    eps = eps || 1e-3;
    let n = 0;
    for (let vi = 0; vi < E.boxPositions.length / 3; vi++) {
        if (Math.abs(E.boxPositions[vi * 3 + 1] - y) < eps) n++;
    }
    return n;
}

// --- Initial state ---------------------------------------------------------

assert(E.faceGroups.groups.length === 6, 'cube has 6 face groups');
assert(E.boxPositions.length === 24 * 3, 'cube has 24 vertices (4 per face × 6)');

const topGroupIdx = E.faceGroups.groups.findIndex(g =>
    Math.abs(g.normal[0]) < 1e-5 &&
    g.normal[1] > 0.999 &&
    Math.abs(g.normal[2]) < 1e-5);
assert(topGroupIdx >= 0, 'found top face group (+Y)');

const topGroup = E.faceGroups.groups[topGroupIdx];
const topTri   = topGroup.tris[0];

// --- Begin + apply (live preview) ------------------------------------------

function triCentroid(triIdx) {
    const i0 = E.boxIndices[triIdx * 3 + 0];
    const i1 = E.boxIndices[triIdx * 3 + 1];
    const i2 = E.boxIndices[triIdx * 3 + 2];
    const P = E.boxPositions;
    return [
        (P[i0 * 3 + 0] + P[i1 * 3 + 0] + P[i2 * 3 + 0]) / 3,
        (P[i0 * 3 + 1] + P[i1 * 3 + 1] + P[i2 * 3 + 1]) / 3,
        (P[i0 * 3 + 2] + P[i1 * 3 + 2] + P[i2 * 3 + 2]) / 3,
    ];
}
const centroid = triCentroid(topTri);
E.beginPushPull({
    triangleIndex: topTri,
    position: centroid,
    normal: topGroup.normal.slice(),
    distance: 0,
});
assert(E.pushpull.active, 'push/pull active after begin');
assert(E.pushpull.groupIdx === topGroupIdx, 'push/pull bound to correct group');

// Preview distance 0.5 without committing.
E.applyPushPull(0.5);
assert(Math.abs(E.pushpull.distance - 0.5) < 1e-6, 'distance recorded');

// Committed primitive buffers must NOT be mutated during preview — drag is
// live-updated via the meshNode's updateMesh only.
assert(E.boxPositions.length === 24 * 3,
    'preview does not grow committed positions buffer');

// --- Commit ----------------------------------------------------------------

E.commitPushPull();
assert(!E.pushpull.active, 'push/pull inactive after commit');

// Surgery is additive: top face gets its 4 moved corners at y=1.5, and each
// retriangulated side wall gets its own pair of seam-duplicate top corners
// (so per-face normals stay isolated) — 4 + 4*2 = 12 vert indices at y=1.5.
const newTopCount = vertCountAtY(1.5);
assert(newTopCount === 12,
    `top face + 4 side walls each own their y=1.5 corners (got ${newTopCount})`);

// Bottom face unchanged. On a hard-edged box each corner position has 3
// vertex indices (one per incident face), so 4 corners → 12 indices at y=-1.
const bottomCount = vertCountAtY(-1);
assert(bottomCount === 12,
    `bottom face still has 12 vertex-indices at y=-1 (got ${bottomCount})`);

// Bridges in the +Y top-cap pull are vertical (offset is +Y, edges are
// horizontal) so they merge into the side faces. Face-group count stays 6.
assert(E.faceGroups.groups.length === 6,
    `still 6 face groups after commit (got ${E.faceGroups.groups.length})`);

// BVH must pick up the new geometry — a ray from above at x=z=0 should hit
// the raised top face at y=1.5.
const rayHit = E.boxBVH.raycast(E.boxMesh, [0, 2.5, 0], [0, -1, 0], 0);
assert(rayHit, 'BVH picks up extruded geometry');
assert(Math.abs(rayHit.position[1] - 1.5) < 1e-4,
    `ray hits new top at y=1.5 (got ${rayHit && rayHit.position[1]})`);

// --- Second push/pull: extrude further -------------------------------------

const topGroup2  = E.faceGroups.groups.find(g => g.normal[1] > 0.999);
const topTri2    = topGroup2.tris[0];
const centroid2  = triCentroid(topTri2);
E.beginPushPull({
    triangleIndex: topTri2,
    position: centroid2,
    normal: topGroup2.normal.slice(),
    distance: 0,
});
E.applyPushPull(0.75);
E.commitPushPull();

const topAt225 = vertCountAtY(2.25);
assert(topAt225 === 12,
    `after second pull, top + 4 walls each own their y=2.25 corners (got ${topAt225})`);
assert(E.faceGroups.groups.length === 6,
    `still 6 face groups (got ${E.faceGroups.groups.length})`);

// --- Cancel path -----------------------------------------------------------

const before = Array.from(E.boxPositions);
const beforeCount = before.length / 3;
const topGroup3 = E.faceGroups.groups.find(g => g.normal[1] > 0.999);
E.beginPushPull({
    triangleIndex: topGroup3.tris[0],
    position: triCentroid(topGroup3.tris[0]),
    normal: topGroup3.normal.slice(),
    distance: 0,
});
E.applyPushPull(0.6);     // any non-zero preview, then cancel
E.cancelPushPull();
assert(!E.pushpull.active, 'push/pull inactive after cancel');
assert(E.boxPositions.length / 3 === beforeCount,
    `cancel restores vertex count (got ${E.boxPositions.length/3} vs ${beforeCount})`);
for (let i = 0; i < before.length; i++) {
    assert(Math.abs(E.boxPositions[i] - before[i]) < 1e-5,
        `cancel restored positions (idx ${i})`);
}

screenshot('apps/scene-editor/_pushpull_after.png');

console.log(`OK — push/pull surgery extrudes box top, second pull stacks, cancel reverts cleanly`);
