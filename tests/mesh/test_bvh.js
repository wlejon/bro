// MeshBVH — acceleration structure for ray queries.

const m = Mesh.sphere(1, 32, 24);
const bvh = new MeshBVH(m, 8);

assert(!bvh.empty, 'bvh non-empty for a sphere');
assert(bvh.triangleCount === m.triangleCount,
       'bvh triangleCount matches mesh; ' + bvh.triangleCount + ' vs ' + m.triangleCount);
assert(bvh.nodeCount > 0, 'bvh has nodes');

const b = bvh.bounds();
assert(Math.abs(b.extentX - 2) < 0.1, 'sphere bvh bounds extentX≈2; got ' + b.extentX);

// Ray through origin hits a unit sphere
const hit = bvh.raycast(m, [0, 0, 3], [0, 0, -1], 0);
assert(hit !== null, 'bvh raycast hits sphere');
assert(Math.abs(hit.distance - 2) < 0.05, 'front hit ~2 units out; got ' + hit.distance);

// Hit test
assert(bvh.raycastTest(m, [0, 0, 3], [0, 0, -1], 0), 'bvh raycastTest hits');
assert(!bvh.raycastTest(m, [5, 5, 5], [1, 0, 0], 10), 'bvh raycastTest misses');

// Sanity: BVH raycast agrees with Mesh.raycast
const direct = m.raycast([0, 0, 3], [0, 0, -1], 0);
assert(Math.abs(hit.distance - direct.distance) < 1e-3,
       'BVH and direct raycast agree on distance');
