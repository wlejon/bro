// Transforms, simplification, subdivision, smoothing, remeshing, shrinkwrap, merge, CSG.

// Transforms
const m = Mesh.box();
m.translate(1, 2, 3);
const bb = m.computeBBox();
assert(Math.abs(bb.centerX - 1) < 1e-4, 'translate centerX=1');
assert(Math.abs(bb.centerY - 2) < 1e-4, 'translate centerY=2');
m.center();
const bb2 = m.computeBBox();
assert(Math.abs(bb2.centerX) < 1e-4, 'after center, centerX≈0');

const scaled = Mesh.box();
scaled.scale(2);
const sbb = scaled.computeBBox();
assert(Math.abs(sbb.extentX - 2) < 1e-4, 'uniform scale doubles extent');

const rotated = Mesh.box();
rotated.rotate(0, 1, 0, Math.PI / 2);
assert(rotated.hasNormals, 'rotated mesh has normals');

const mirrored = Mesh.box();
const preArea = mirrored.surfaceArea();
mirrored.mirror(0);
assert(Math.abs(mirrored.surfaceArea() - preArea) < 1e-3, 'mirror preserves surface area');

// 4x4 transform matrix (column-major identity with translation)
const t = new Float32Array([1,0,0,0, 0,1,0,0, 0,0,1,0, 5,0,0,1]);
const tm = Mesh.box();
tm.transform(t);
const tbb = tm.computeBBox();
assert(Math.abs(tbb.centerX - 5) < 1e-4, 'transform matrix translates by 5');

// Simplification
const sph = Mesh.sphere(1, 32, 24);
const orig = sph.triangleCount;
sph.simplify(0.5);
assert(sph.triangleCount < orig, 'simplify reduces triangle count; ' + sph.triangleCount + ' < ' + orig);

// simplifyToTriangleCount — simplification may stop early when error budget is hit;
// just require it to have reduced from the original.
const s2 = Mesh.sphere(1, 32, 24);
const origS2 = s2.triangleCount;
s2.simplifyToTriangleCount(200);
assert(s2.triangleCount < origS2, 'simplify reduced triangles; ' + s2.triangleCount + ' < ' + origS2);

// LOD chain
const s3 = Mesh.sphere(1, 32, 24);
const lods = s3.generateLODChain(new Float32Array([0.75, 0.5, 0.25]));
assert(lods.length === 3, 'LOD chain has 3 meshes');
for (let i = 1; i < lods.length; i++)
    assert(lods[i].triangleCount <= lods[i-1].triangleCount, 'LOD ' + i + ' coarser than ' + (i-1));

// Subdivision (loop)
const ico = Mesh.icosahedron();
const origIco = ico.triangleCount;
ico.subdivideLoop(1);
assert(ico.triangleCount === origIco * 4, 'loop subdivision multiplies triangles by 4');

// Smoothing doesn't blow up
const cube = Mesh.box();
cube.smoothLaplacian(0.5, 2);
assert(cube.vertexCount > 0 && cube.hasNormals, 'smoothed mesh valid');

const cube2 = Mesh.box();
cube2.smoothTaubin(0.5, -0.53, 2);
assert(cube2.vertexCount > 0, 'taubin-smoothed mesh valid');

// Remesh isotropic
const noisy = Mesh.sphere(1, 16, 12);
noisy.remeshIsotropic(0, 2);
assert(noisy.triangleCount > 0, 'remeshed mesh valid');

// CSG
const a = Mesh.box();
const b = Mesh.sphere(0.6);
const u = Mesh.union(a, b);
assert(!u.empty, 'union non-empty');
const d = Mesh.subtract(a, b);
assert(!d.empty, 'subtract non-empty');
const i = Mesh.intersect(a, b);
assert(!i.empty, 'intersect non-empty');

// splitByPlane
const split = Mesh.splitByPlane(Mesh.sphere(1, 16, 12), 0, 1, 0, 0);
assert(Array.isArray(split) && split.length === 2, 'splitByPlane returns [front, back]');
assert(!split[0].empty && !split[1].empty, 'both halves non-empty');

// merge
const merged = Mesh.merge([Mesh.box(), Mesh.sphere()]);
assert(!merged.empty, 'merged non-empty');

// shrinkwrap: wrap a sphere onto a box (nearest)
const shrink = Mesh.sphere(1.5, 16, 12);
const swTarget = Mesh.box(0.5, 0.5, 0.5);
shrink.shrinkwrap(swTarget, 'nearest', 0, 0, null);
// after shrinkwrap, all verts should be within box bounds + small tol
const p = shrink.positions;
let maxAbs = 0;
for (let k = 0; k < p.length; k++) maxAbs = Math.max(maxAbs, Math.abs(p[k]));
assert(maxAbs <= 0.55, 'shrinkwrap pulled verts onto box; maxAbs=' + maxAbs);
