// Headless smoke test for the push/pull tool.
//
// Exercises the tool via __editor hooks (not synthetic mouse events) so the
// test is deterministic — no dependency on screen-space ray math, camera
// state, or pointer lock.
//
// Run: bro-headless apps/scene-editor apps/scene-editor/test_pushpull.js

advanceTime(100);
flush();

const E = window.__editor;

// --- Initial state ---------------------------------------------------------

assert(E.faceGroups.groups.length === 6, 'cube has 6 face groups');
assert(E.boxPositions.length === 24 * 3, 'cube has 24 vertices (4 per face × 6)');

// Find the +Y (top) face group and one of its triangles + its click point.
const topGroupIdx = E.faceGroups.groups.findIndex(g =>
    Math.abs(g.normal[0]) < 1e-5 &&
    g.normal[1] > 0.999 &&
    Math.abs(g.normal[2]) < 1e-5);
assert(topGroupIdx >= 0, 'found top face group (+Y)');

const topGroup = E.faceGroups.groups[topGroupIdx];
const topTri   = topGroup.tris[0];

// --- Affected-vertex math --------------------------------------------------

// The top face has 4 corner positions; each corner is shared by 3 incident
// faces in the hard-edged box (each face has its own 4 vertices). So 12
// vertex indices should translate together to keep the side faces stretched
// flush with the top.
const affected = E.collectAffectedVertexIndices(topGroupIdx);
assert(affected.length === 12,
    `top face affects 12 vertex indices (got ${affected.length})`);

// Every affected vertex must currently sit at y = 1 (cube half-extent).
for (const vi of affected) {
    const y = E.boxPositions[vi * 3 + 1];
    assert(Math.abs(y - 1) < 1e-5,
        `affected vertex ${vi} sits on top plane y=1 (got ${y})`);
}

// --- Begin + apply ---------------------------------------------------------

// Synthesize a hit at the triangle centroid — good enough; beginPushPull
// only reads hit.triangleIndex and hit.position.
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

// boxPositions must NOT be mutated during preview — drag is live-updated via
// updateMesh only, commit is what bakes.
for (const vi of affected) {
    assert(Math.abs(E.boxPositions[vi * 3 + 1] - 1) < 1e-5,
        `preview does not mutate boxPositions (vi ${vi})`);
}

// --- Commit ----------------------------------------------------------------

E.commitPushPull();
assert(!E.pushpull.active, 'push/pull inactive after commit');

// Now boxPositions must reflect the extrusion.
for (const vi of affected) {
    const y = E.boxPositions[vi * 3 + 1];
    assert(Math.abs(y - 1.5) < 1e-5,
        `commit raised affected vertices to y=1.5 (vi ${vi} → ${y})`);
}
// Bottom face's vertices should be unchanged. On a hard-edged box each
// corner position has 3 vertex indices (one per incident face), so the 4
// bottom corners → 12 indices at y=-1.
let bottomCount = 0;
for (let vi = 0; vi < E.boxPositions.length / 3; vi++) {
    if (E.boxPositions[vi * 3 + 1] < -0.999) bottomCount++;
}
assert(bottomCount === 12,
    `bottom face still has 12 vertex-indices at y=-1 (got ${bottomCount})`);

// Face groups must still resolve to 6 (still a closed box, just taller).
assert(E.faceGroups.groups.length === 6,
    `still 6 face groups after commit (got ${E.faceGroups.groups.length})`);

// BVH must pick up the new geometry — a ray from above at x=z=0 should hit
// the raised top face at distance 1 (origin y=2.5, top at y=1.5).
const rayHit = E.boxBVH.raycast(E.boxMesh, [0, 2.5, 0], [0, -1, 0], 0);
assert(rayHit, 'BVH picks up extruded geometry');
assert(Math.abs(rayHit.position[1] - 1.5) < 1e-4,
    `ray hits new top at y=1.5 (got ${rayHit && rayHit.position[1]})`);

// --- Second push/pull: extrude further, verify state stays consistent -----

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

let topCount = 0;
for (let vi = 0; vi < E.boxPositions.length / 3; vi++) {
    if (E.boxPositions[vi * 3 + 1] > 1.5 + 0.75 - 1e-4) topCount++;
}
assert(topCount === 12, `after second push/pull top cluster is 12 verts at y=2.25 (got ${topCount})`);

// --- Cancel path ----------------------------------------------------------

// Start a third push/pull and cancel — geometry must revert.
const before = Array.from(E.boxPositions);
const topGroup3 = E.faceGroups.groups.find(g => g.normal[1] > 0.999);
E.beginPushPull({
    triangleIndex: topGroup3.tris[0],
    position: triCentroid(topGroup3.tris[0]),
    normal: topGroup3.normal.slice(),
    distance: 0,
});
E.applyPushPull(-1.5);       // negative = push inward
E.cancelPushPull();
assert(!E.pushpull.active, 'push/pull inactive after cancel');
for (let i = 0; i < before.length; i++) {
    assert(Math.abs(E.boxPositions[i] - before[i]) < 1e-5,
        `cancel restored positions (idx ${i})`);
}

screenshot('apps/scene-editor/_pushpull_after.png');
console.log(`OK — push/pull extrudes (top=+0.5 then +0.75), cancel reverts, BVH + face groups rebuild`);
