// Headless smoke test for the picking spike.
//
// Run: bro-headless apps/scene-editor apps/scene-editor/test_pick.js

advanceTime(100);
flush();

const w = 1920, h = 1080;   // default headless viewport
screenshot('apps/scene-editor/_before.png');

// Click the center of the screen — camera starts looking at the origin from
// +Z-ish with a slight downward tilt, so a box at the origin fills the middle.
click(w / 2, h / 2);
advanceTime(50);

const hit = window.__lastPick;
assert(hit, 'center-of-screen click must hit the box');
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

// A miss: click far off-screen corner — ray should exit the view frustum
// beyond the box and not hit anything visible through that pixel.
// (The box only covers a small central region, so top-left pixel misses.)
const missRay = window.__editor.screenToRay(5, 5);
const miss = window.__editor.boxBVH.raycast(
    window.__editor.boxMesh, missRay.origin, missRay.dir, 0);
assert(!miss, `top-left corner should miss (got ${miss && JSON.stringify(miss)})`);

screenshot('apps/scene-editor/_after.png');
console.log(`OK — picked tri ${hit.triangleIndex} at [${p[0].toFixed(3)}, ${p[1].toFixed(3)}, ${p[2].toFixed(3)}]`);
