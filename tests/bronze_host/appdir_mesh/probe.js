// The mesh probe: bro.mesh, Mesh and MeshBVH on bronze's native mechanism —
// native classes (a constructor whose handle owns the bromesh::MeshData),
// typed-array parameters, and typed-array RETURNS in both ownership modes —
// as a COMPILED app reads them.
//
// What each block pins: construction from MeshOptions and the readonly
// counts; attribute reads as COPIES (mutating what came back changes
// nothing) and the setters as the way back in; the primitives' vertex and
// triangle counts; in-place operations (computeNormals, transform, weld,
// flipFaces) through their effect on the mesh; TRANSFER-mode results
// (triangleAreas, buildMeshlets) surviving a collection the frames force
// while a WeakRef proves the collection ran; MeshBVH hit and miss; and the
// two TypeErrors the runtime raises at the call itself — a wrong-class or
// non-handle where a Mesh is required, a plain array where a Float32Array
// is required.

function say(label, value) { console.log('APP ' + label + '=' + value); }
function r2(x) { return Math.round(x * 100) / 100; }
function r4(x) { return Math.round(x * 10000) / 10000; }
function sum(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function bnd(b) { return b.min.map(r4).join(',') + '|' + b.max.map(r4).join(','); }
function err(fn) { try { fn(); return 'no throw'; } catch (e) { return e.name + ':' + e.message; } }

say('roots', typeof bro.mesh + ',' + typeof Mesh + ',' + typeof MeshBVH + ',' +
    (bro.mesh.Mesh === Mesh) + ',' + (bro.mesh.MeshBVH === MeshBVH) + ',' + (bro.mesh.box === Mesh.box));

// ---------------------------------------------------------------------------
// Construction from options; readonly counts; instanceof.
// ---------------------------------------------------------------------------
const quad = new Mesh({
    positions: new Float32Array([0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0]),
    indices: new Uint32Array([0, 1, 2, 0, 2, 3]),
});
say('ctor.counts', quad.vertexCount + ',' + quad.triangleCount + ',' + quad.hasNormals + ',' +
    quad.hasUVs + ',' + quad.hasColors + ',' + quad.empty);
say('ctor.instanceof', (quad instanceof Mesh) + ',' + (quad instanceof MeshBVH) + ',' +
    (Object.getPrototypeOf(quad) !== Mesh.prototype) + ',' + (typeof quad.computeNormals));
say('ctor.empty', new Mesh().empty + ',' + new Mesh({}).vertexCount);

// ---------------------------------------------------------------------------
// Attribute reads are copies; the setters write back.
// ---------------------------------------------------------------------------
const p = quad.positions;
say('positions.copy.type', (p instanceof Float32Array) + ',' + p.length);
p[0] = 99;
say('positions.copy.isolated', quad.positions[0] + ',' + (quad.positions !== p));
const idx = quad.indices;
say('indices.copy.type', (idx instanceof Uint32Array) + ',' + Array.from(idx).join(','));
quad.positions = new Float32Array([0, 0, 0, 2, 0, 0, 2, 2, 0, 0, 2, 0]);
say('positions.set', quad.positions[3] + ',' + r4(quad.surfaceArea()));
say('normals.absent', quad.normals.length);
say('indices.set.rangeError', err(() => { quad.indices = new Uint32Array([0, 1, 9]); }));

// ---------------------------------------------------------------------------
// Primitives: vertex and triangle counts.
// ---------------------------------------------------------------------------
function counts(name, m) { say('prim.' + name, m.vertexCount + ',' + m.triangleCount + ',' + (m instanceof Mesh)); }
counts('box', Mesh.box());
counts('sphere', Mesh.sphere());
counts('cylinder', Mesh.cylinder());
counts('capsule', Mesh.capsule());
counts('cone', Mesh.cone());
counts('plane', Mesh.plane(1, 1, 4, 4));
counts('torus', Mesh.torus());
counts('icosahedron', Mesh.icosahedron());
counts('dodecahedron', Mesh.dodecahedron());
counts('octahedron', Mesh.octahedron());
counts('tetrahedron', Mesh.tetrahedron());
counts('disk', Mesh.disk());
counts('geodesicSphere1', Mesh.geodesicSphere(1, 1));
counts('tube', Mesh.tube([[0, 0, 0], [0, 1, 0], [0, 2, 0]], 0.1, 8));
counts('merge', Mesh.merge([Mesh.box(), Mesh.box().translate(3, 0, 0)]));
say('prim.icosahedron.radius', bnd(Mesh.icosahedron(2).bounds()));

// ---------------------------------------------------------------------------
// Analysis on a unit cube: area 6, volume 1, bounds ±0.5.
// ---------------------------------------------------------------------------
const box = Mesh.box();
say('box.area', r4(box.surfaceArea()));
say('box.volume', r4(box.volume()));
say('box.bounds', bnd(box.bounds()));
say('box.flipFaces.volume', r4(box.clone().flipFaces().volume()));
say('box.weld', box.clone().weld().vertexCount);
say('ico.manifold', Mesh.icosahedron().isManifold());

// ---------------------------------------------------------------------------
// In-place operations: computeNormals, then a transform (a translation
// matrix, column-major, as bromesh takes it).
// ---------------------------------------------------------------------------
const sph = Mesh.sphere();
say('normals.before', sph.hasNormals + ',' + sph.normals.length);
sph.computeNormals();
say('normals.after', sph.hasNormals + ',' + sph.normals.length + ',' + (sph.normals.length === sph.vertexCount * 3));
const moved = Mesh.box();
moved.transform([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 2, 3, 4, 1]);
say('transform.bounds', bnd(moved.bounds()));
say('transform.badMatrix', err(() => Mesh.box().transform([1, 0, 0])));
const c = box.clone();
c.translate(1, 0, 0);
say('clone.independent', bnd(box.bounds()) + ' ' + bnd(c.bounds()));
say('scale.chain', bnd(Mesh.box().scale(2).bounds()));

// ---------------------------------------------------------------------------
// Transfer-mode results: the native's own block, viewed in place. Kept
// across the collection forced below by allocation pressure (the idle
// collector never runs while a script frame is live, and nothing exposes
// collectGarbage to the program); the WeakRef on a dropped mesh — made in
// its own function so no frame slot of this one pins the handle — is the
// proof that the collection happened.
// ---------------------------------------------------------------------------
const areas = box.triangleAreas();
say('areas.type', (areas instanceof Float32Array) + ',' + areas.length + ',' + r4(sum(areas)));
const ml = Mesh.sphere(1, 32, 24).buildMeshlets(64, 124);
say('meshlets.shape', (ml.meshletCount > 0) + ',' + (ml.meshlets instanceof ArrayBuffer) + ',' +
    (ml.meshlets.byteLength === ml.meshletCount * 64) + ',' + (ml.vertices instanceof Uint32Array) + ',' +
    (ml.triangles instanceof Uint8Array) + ',' + (ml.triangles.length % 3 === 0));
const rec = new Uint32Array(ml.meshlets, 0, 4);
say('meshlets.record0', rec[0] + ',' + (rec[1] > 0 && rec[1] <= 64) + ',' + rec[2] + ',' + (rec[3] > 0 && rec[3] <= 124));
function makeWeak() { return new WeakRef(Mesh.sphere(1, 8, 6)); }
const weak = makeWeak();
const uvPlane = Mesh.plane(1, 1, 2, 2).projectUVs('planarXZ');
say('uv.distortion', uvPlane.hasUVs + ',' + uvPlane.computeUVDistortion().length + ',' +
    r4(uvPlane.computeUVDistortion()[0].stretch));

// ---------------------------------------------------------------------------
// MeshBVH: hit and miss.
// ---------------------------------------------------------------------------
const target = Mesh.sphere(1, 32, 24);
const bvh = new MeshBVH(target, 8);
say('bvh.shape', (bvh instanceof MeshBVH) + ',' + bvh.empty + ',' + (bvh.triangleCount === target.triangleCount) +
    ',' + (bvh.nodeCount > 0) + ',' + bnd(bvh.bounds()));
const hit = bvh.raycast([0, 0, 3], [0, 0, -1]);
say('bvh.hit', (hit !== null) + ',' + hit.hit + ',' + r2(hit.distance) + ',' + hit.point.map(r2).join(',') + ',' +
    (hit.triangle >= 0) + ',' + r2(hit.barycentric[0] + hit.barycentric[1] + hit.barycentric[2]));
say('bvh.miss', bvh.raycast([5, 5, 5], [1, 0, 0], 10));
say('bvh.test', bvh.raycastTest([0, 0, 3], [0, 0, -1]) + ',' + bvh.raycastTest([5, 5, 5], [1, 0, 0], 10));
say('mesh.raycast', r2(target.raycast([0, 0, 3], [0, 0, -1]).distance));
say('bvh.fromMesh', target.buildBVH() instanceof MeshBVH);

// ---------------------------------------------------------------------------
// TypeErrors from the native call itself.
// ---------------------------------------------------------------------------
say('typeError.plainObject', err(() => new MeshBVH({})));
say('typeError.wrongClass', err(() => new MeshBVH(bvh)));
say('typeError.receiver', err(() => Mesh.prototype.computeNormals.call(bvh)));
say('typeError.plainArray', err(() => new Mesh({ positions: [0, 0, 0] })));
say('typeError.wrongKind', err(() => new Mesh({ positions: new Float64Array(3) })));
say('typeError.merge', err(() => Mesh.merge([Mesh.box(), 'no'])));

// The collection: 1.2 GiB of garbage against a 16 MiB collection threshold
// is dozens of flips. In a LATER job, because `new WeakRef(t)` keeps `t`
// for the rest of the job that made it (AddToKeptObjects); a microtask job
// always begins after ClearKeptObjects. Then the transferred blocks read
// back unchanged.
function churn() { for (let i = 0; i < 1200; i++) new Float32Array(262144); }
Promise.resolve().then(() => {
    churn();
    say('gc.ran', weak.deref() === undefined);
    say('areas.afterGC', areas.length + ',' + r4(sum(areas)) + ',' + r4(areas[0]));
    say('meshlets.afterGC', ml.vertices.length + ',' + (ml.meshlets.byteLength === ml.meshletCount * 64) + ',' +
        new Uint32Array(ml.meshlets, 0, 1)[0]);
    say('done', true);
});
