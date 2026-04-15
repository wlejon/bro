// Convex hull and convex decomposition.

// Convex hull of a box is a box (8 verts)
const box = Mesh.box();
const hull = box.convexHull();
assert(!hull.empty, 'convex hull non-empty');
assert(hull.vertexCount <= box.vertexCount,
       'hull has <= input vertices; ' + hull.vertexCount + ' vs ' + box.vertexCount);
assert(hull.triangleCount > 0, 'hull has triangles');

// Hull of a sphere is still bounded by same bbox
const sph = Mesh.sphere(1, 16, 12);
const sphHull = sph.convexHull();
const hb = sphHull.computeBBox();
const sb = sph.computeBBox();
assert(Math.abs(hb.extentX - sb.extentX) < 0.1, 'sphere hull preserves x extent');

// Convex decomposition: a single box should decompose to roughly one hull
const box2 = Mesh.box();
const hulls = box2.convexDecomposition({ maxHulls: 4, resolution: 50000 });
assert(Array.isArray(hulls), 'decomposition returns array');
assert(hulls.length >= 1, 'at least one hull; got ' + hulls.length);
for (let i = 0; i < hulls.length; i++) {
    assert(!hulls[i].empty, 'hull ' + i + ' non-empty');
    assert(hulls[i].triangleCount > 0, 'hull ' + i + ' has triangles');
}
