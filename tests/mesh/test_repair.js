// Repair and cleanup: weld, removeDegenerate, removeDuplicate, fillHoles,
// splitComponents, computeCreaseNormals.

// weld: flat normals duplicate vertices; weld merges them back
const box = Mesh.box();
const flat = box.computeFlatNormals();
assert(flat.vertexCount === 36, 'flat normals duplicates box verts to 36 (6*2*3)');
flat.weld(1e-4);
assert(flat.vertexCount < 36, 'weld reduces vertex count; got ' + flat.vertexCount);

// crease normals — should not crash, keeps mesh valid
const creased = Mesh.box();
creased.computeCreaseNormals(30.0);
assert(creased.hasNormals, 'crease normals present');
assert(creased.triangleCount > 0, 'crease mesh has triangles');

// removeDegenerate/removeDuplicate on a plain box (nothing to remove)
const clean = Mesh.box();
const origTri = clean.triangleCount;
clean.removeDegenerateTriangles(1e-8);
assert(clean.triangleCount === origTri, 'clean box has no degenerates');
clean.removeDuplicateTriangles();
assert(clean.triangleCount === origTri, 'clean box has no duplicates');

// splitComponents — a primitive box has flat-shaded duplicated verts per face,
// so each face is its own connected component until welded.
const single = Mesh.box();
single.weld(1e-4);
const comps1 = single.splitComponents();
assert(comps1.length === 1, 'welded box is one component; got ' + comps1.length);

// merged two translated welded boxes => 2 components
const a = Mesh.box(); a.weld(1e-4); a.translate(-2, 0, 0);
const b = Mesh.box(); b.weld(1e-4); b.translate( 2, 0, 0);
const merged = Mesh.merge([a, b]);
const comps2 = merged.splitComponents();
assert(comps2.length === 2, 'two disjoint components; got ' + comps2.length);

// fillHoles runs without crashing and preserves non-empty geometry.
// (Box primitive has duplicated per-face verts, so manifold checks need weld first.)
const holed = Mesh.box();
holed.weld(1e-4);
const origTris = holed.triangleCount;
holed.fillHoles(64);
assert(holed.triangleCount >= origTris, 'fillHoles preserves or adds triangles');
assert(holed.isManifold(), 'welded box is manifold');
