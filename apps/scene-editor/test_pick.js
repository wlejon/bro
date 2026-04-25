// Headless smoke test for the picking spike.
//
// Run: bro-headless apps/scene-editor apps/scene-editor/test_pick.js

advanceTime(100);
flush();

const w = 1920, h = 1080;   // default headless viewport
screenshot('apps/scene-editor/_before.png');

// Click the box, dodging the translate gizmo. The gizmo anchors at the
// active primitive's centroid (here, the origin) and extends ~80px arrows
// along each world axis from screen center; clicking a pixel beyond the
// arrows but well inside the box's ~326px screen extent reliably reaches
// the box pick path.
const PICK_DX = 220, PICK_DY = 100;
click(w / 2 + PICK_DX, h / 2 + PICK_DY);
advanceTime(50);

const hit = window.__lastPick;
assert(hit, 'off-center click must hit the box');
assert(hit.triangleIndex >= 0 && hit.triangleIndex < 12,
    `triangleIndex in range (got ${hit && hit.triangleIndex})`);
assert(hit.distance > 0, 'hit distance must be positive');

// Box is Mesh.box(1,1,1) — half-extents of 1, so it spans [-1, 1] on each
// axis. Hit point must be on the box surface.
const p = hit.position;
const EPS = 0.01;
assert(p[0] >= -1 - EPS && p[0] <= 1 + EPS, `hit x in bbox (got ${p[0]})`);
assert(p[1] >= -1 - EPS && p[1] <= 1 + EPS, `hit y in bbox (got ${p[1]})`);
assert(p[2] >= -1 - EPS && p[2] <= 1 + EPS, `hit z in bbox (got ${p[2]})`);

// A miss: a far-right pixel — ray exits the view frustum beyond the box.
// Avoid the top-left corner: it overlaps the HUD overlay, and synthesized
// clicks on overlapping HTML don't fall through to the canvas listener.
const MISS_X = 1850, MISS_Y = 540;
const missRay = window.__editor.screenToRay(MISS_X, MISS_Y);
const miss = window.__editor.boxBVH.raycast(
    window.__editor.boxMesh, missRay.origin, missRay.dir, 0);
assert(!miss, `far-right pixel should miss (got ${miss && JSON.stringify(miss)})`);

// Clicking the box should install a highlight overlay node.
assert(window.__editor.highlightNode, 'highlight node should exist after pick');

// A box has exactly 6 face groups (2 coplanar triangles per side).
const fg = window.__editor.faceGroups;
assert(fg.groups.length === 6, `box should have 6 face groups (got ${fg.groups.length})`);
for (const g of fg.groups) {
    assert(g.tris.length === 2, `each box face group has 2 tris (got ${g.tris.length})`);
    const nL = Math.hypot(g.normal[0], g.normal[1], g.normal[2]);
    assert(Math.abs(nL - 1) < 1e-5, 'face-group normal is unit length');
}
// And the triangles of a face group share that group's normal.
const triNormals = fg.groups.map(g => g.normal);
// Six axis-aligned faces: normals should be {+X, -X, +Y, -Y, +Z, -Z}.
const axisHits = new Set();
for (const n of triNormals) {
    for (let a = 0; a < 3; a++) {
        if (Math.abs(n[a]) > 0.999) {
            axisHits.add((n[a] > 0 ? '+' : '-') + 'XYZ'[a]);
        }
    }
}
assert(axisHits.size === 6, `all 6 axis directions present (got ${[...axisHits].sort().join(',')})`);

// Second click on a miss (far-right pixel, off the box) should clear the
// highlight.
click(MISS_X, MISS_Y);
advanceTime(50);
assert(!window.__editor.highlightNode, 'highlight should clear on miss');

// Restore a highlight for the final screenshot — same off-center pixel.
click(w / 2 + PICK_DX, h / 2 + PICK_DY);
advanceTime(50);
assert(window.__editor.highlightNode, 'highlight restored on second box click');

screenshot('apps/scene-editor/_after.png');
console.log(`OK — picked tri ${hit.triangleIndex} at [${p[0].toFixed(3)}, ${p[1].toFixed(3)}, ${p[2].toFixed(3)}]`);
