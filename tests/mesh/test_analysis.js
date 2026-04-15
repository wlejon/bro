// Surface analysis: bbox, volume, manifold, sampling, triangle areas,
// raycast, UV metrics.

const box = Mesh.box();
const bb = box.computeBBox();
assert(Math.abs(bb.extentX - 1) < 1e-4, 'box extentX≈1; got ' + bb.extentX);
assert(Math.abs(bb.extentY - 1) < 1e-4, 'box extentY≈1');
assert(Math.abs(bb.centerX) < 1e-4, 'box centered at origin X');

assert(box.isManifold(), 'box is manifold');
const vol = box.computeVolume();
assert(Math.abs(vol - 1) < 1e-3, 'unit box volume≈1; got ' + vol);

// surface area of a unit box = 6
const area = box.surfaceArea();
assert(Math.abs(area - 6) < 1e-3, 'unit box area≈6; got ' + area);

// triangleAreas: 12 triangles, each 0.5 for a unit box
const areas = box.triangleAreas();
assert(areas.length === 12, 'unit box has 12 triangles, got ' + areas.length);
let sum = 0;
for (let i = 0; i < areas.length; i++) sum += areas[i];
assert(Math.abs(sum - 6) < 1e-3, 'triangle areas sum to 6');

// sampleSurface: produces a point cloud
const pc = box.sampleSurface(1000, 42);
assert(pc.vertexCount === 1000, 'sampled 1000 points; got ' + pc.vertexCount);
assert(pc.triangleCount === 0, 'point cloud has no triangles');

// raycast from outside into a box
const hit = box.raycast([0, 0, 2], [0, 0, -1], 0);
assert(hit !== null, 'ray hits box from +Z');
assert(Math.abs(hit.distance - 1.5) < 1e-3, 'ray hits front face at dist≈1.5; got ' + hit.distance);

const miss = box.raycast([0, 0, 2], [1, 0, 0], 0);
assert(miss === null, 'ray parallel to x misses box');

// raycastTest / raycastAll
assert(box.raycastTest([0, 0, 2], [0, 0, -1], 0), 'raycastTest hits');
const all = box.raycastAll([0, 0, 2], [0, 0, -1], 0);
// Box has 12 triangles; a centered ray can hit both triangles of each face (front+back = 4)
assert(all.length >= 2, 'raycastAll returns >=2 hits; got ' + all.length);

// closestPoint
const cp = box.closestPoint([2, 0, 0]);
assert(cp !== null, 'closestPoint returns');
assert(Math.abs(cp.position[0] - 0.5) < 1e-3, 'closest point x≈0.5');

// UV metrics — a plane has trivial UVs
const plane = Mesh.plane(1, 1, 4, 4);
const dist = plane.computeUVDistortion();
assert(dist.length === plane.triangleCount, 'one distortion entry per triangle');

const uvq = plane.measureUVQuality();
assert(uvq.triangleCount === plane.triangleCount, 'uv quality tri count');
assert(typeof uvq.avgStretch === 'number', 'uv quality has avgStretch');
