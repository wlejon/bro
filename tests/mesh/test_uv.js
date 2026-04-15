// UV projection, unwrapping, and distortion metrics.

// Projection: each mode writes valid UVs
for (const type of ['box', 'planarXY', 'planarXZ', 'planarYZ', 'cylindrical', 'spherical']) {
    const m = Mesh.sphere(1, 16, 12);
    m.projectUVs(type, 1.0);
    assert(m.hasUVs, 'projection ' + type + ' produces UVs');
    assert(m.uvs.length === m.vertexCount * 2, 'UVs stride 2 for ' + type);
}

// Unwrap
const m = Mesh.sphere(1, 16, 12);
const res = m.unwrapUVs();
assert(typeof res.atlasWidth === 'number', 'atlasWidth is number');
assert(typeof res.chartCount === 'number', 'chartCount is number');
assert(typeof res.success === 'boolean', 'success is bool');

// UV metrics require UVs
const plane = Mesh.plane(1, 1, 8, 8);
const dist = plane.computeUVDistortion();
assert(dist.length === plane.triangleCount, 'one distortion per triangle');
for (let i = 0; i < Math.min(dist.length, 4); i++) {
    assert(typeof dist[i].stretch === 'number', 'stretch is number');
    assert(typeof dist[i].areaDistortion === 'number', 'areaDistortion is number');
    assert(typeof dist[i].angleDistortion === 'number', 'angleDistortion is number');
}

const q = plane.measureUVQuality();
assert(q.triangleCount === plane.triangleCount, 'quality tri count matches');
assert(q.maxStretch >= q.avgStretch, 'maxStretch >= avgStretch');
