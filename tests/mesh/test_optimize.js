// Optimization, encoding, analysis, spatial sort, meshlets, progressive mesh.

const sphere = Mesh.sphere(1, 32, 24);
const origTriCount = sphere.triangleCount;
const origVertCount = sphere.vertexCount;

// Cache / fetch / overdraw analyze
const vc = sphere.analyzeVertexCache(16);
assert(typeof vc.acmr === 'number', 'acmr is number');
assert(vc.acmr > 0, 'acmr > 0');

const vf = sphere.analyzeVertexFetch(32);
assert(vf.bytesFetched > 0, 'vertex fetch bytes > 0');

const od = sphere.analyzeOverdraw();
assert(typeof od.overdraw === 'number', 'overdraw is number');

// Optimize (mutates)
sphere.optimizeVertexCache();
sphere.optimizeVertexFetch();
sphere.optimizeOverdraw(1.05);
assert(sphere.triangleCount === origTriCount, 'optimize preserves triangle count');
assert(sphere.vertexCount === origVertCount, 'optimize preserves vertex count');

// spatial sort
const s2 = Mesh.sphere(1, 16, 12);
s2.spatialSortTriangles();
s2.spatialSortVertices();
assert(s2.triangleCount > 0, 'spatial sort keeps mesh valid');

// shadow index buffer
const sib = s2.generateShadowIndexBuffer();
assert(sib instanceof Uint32Array, 'shadow index buffer is Uint32Array');
assert(sib.length === s2.indices.length, 'shadow IB has same length as regular IB');

// stripify / unstripify
const strip = Mesh.stripify(s2.indices, s2.vertexCount);
assert(strip instanceof Uint32Array, 'stripify returns Uint32Array');
assert(strip.length > 0, 'strip non-empty');
const back = Mesh.unstripify(strip);
assert(back.length === s2.indices.length, 'unstripify restores triangle list length');

// encode / decode round-trip
const enc = s2.encode();
assert(enc.vertexData instanceof Uint8Array, 'encoded vertexData is Uint8Array');
assert(enc.indexData instanceof Uint8Array, 'encoded indexData is Uint8Array');
assert(enc.vertexCount === s2.vertexCount, 'encoded vertexCount matches');

const decoded = Mesh.decode(enc);
assert(decoded.vertexCount === s2.vertexCount,
       'decoded vertex count matches; ' + decoded.vertexCount + ' vs ' + s2.vertexCount);
assert(decoded.triangleCount === s2.triangleCount, 'decoded triangle count matches');

// meshlets
const ml = s2.buildMeshlets({ maxVertices: 64, maxTriangles: 124, coneWeight: 0.5 });
assert(Array.isArray(ml), 'meshlets is an array');
if (ml.length > 0) {
    const first = ml[0];
    assert(first.vertices instanceof Uint32Array, 'meshlet vertices is Uint32Array');
    assert(first.triangles instanceof Uint8Array, 'meshlet triangles is Uint8Array');
    assert(typeof first.bounds.radius === 'number', 'meshlet bounds has radius');
}

// Progressive mesh
const base = Mesh.sphere(1, 16, 12);
const pm = new ProgressiveMesh(base);
assert(pm.maxTriangles === base.triangleCount, 'pm max matches base');
assert(pm.minTriangles <= pm.maxTriangles, 'pm min <= max');

const half = pm.atRatio(0.5);
assert(!half.empty, 'pm.atRatio(0.5) non-empty');
assert(half.triangleCount <= pm.maxTriangles, 'half triangle count <= max');

const coarse = pm.atTriangleCount(pm.minTriangles);
assert(!coarse.empty, 'coarsest pm non-empty');

// Serialize / deserialize round-trip
const bytes = pm.serialize();
assert(bytes instanceof Uint8Array, 'serialized to Uint8Array');
assert(bytes.length > 0, 'serialized bytes non-empty');
const pm2 = ProgressiveMesh.deserialize(bytes);
assert(pm2.maxTriangles === pm.maxTriangles, 'deserialized pm preserves max');
