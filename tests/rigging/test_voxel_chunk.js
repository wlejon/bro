// VoxelChunk — set/get, fill, data round-trip, buildMesh.

const c = new VoxelChunk(4, 4, 4, 1.0);
assert(c.sizeX === 4, 'sizeX');
assert(c.sizeY === 4, 'sizeY');
assert(c.sizeZ === 4, 'sizeZ');
assert(Math.abs(c.cellSize - 1.0) < 1e-6, 'cellSize');

// Empty chunk -> empty mesh.
{
    const m = c.buildMesh();
    assert(m.empty, 'empty chunk -> empty mesh');
}

// set/get round-trip.
c.setVoxel(0, 0, 0, 1);
c.setVoxel(1, 0, 0, 2);
assert(c.getVoxel(0, 0, 0) === 1, 'get(0,0,0)');
assert(c.getVoxel(1, 0, 0) === 2, 'get(1,0,0)');
assert(c.getVoxel(2, 0, 0) === 0, 'get unset is 0');
assert(c.getVoxel(99, 0, 0) === 0, 'OOB returns 0');

// dirty flag.
assert(c.isDirty === true, 'dirty after setVoxel');
c.clearDirty();
assert(c.isDirty === false, 'cleared');
c.markDirty();
assert(c.isDirty === true, 'reset');
c.isDirty = false;
assert(c.isDirty === false, 'setter clears');

// fill.
c.fill(0);
assert(c.getVoxel(0, 0, 0) === 0, 'cleared by fill');
c.fill(7);
assert(c.getVoxel(0, 0, 0) === 7, 'filled');
assert(c.getVoxel(3, 3, 3) === 7, 'filled corner');

// data round-trip.
const buf = c.data();
assert(buf.length === 64, 'data length = sizeX*sizeY*sizeZ');
assert(buf[0] === 7, 'data has fill value');
const snap = new Uint8Array(64);
for (let i = 0; i < 64; i++) snap[i] = i & 0xff;
c.setData(snap);
assert(c.getVoxel(0, 0, 0) === 0,    'setData[0]');
assert(c.getVoxel(1, 0, 0) === 1,    'setData[1]');

// buildMesh produces something.
c.fill(0);
c.setVoxel(0, 0, 0, 1);
c.setVoxel(1, 0, 0, 1);
c.setVoxel(0, 1, 0, 1);
const m = c.buildMesh();
assert(!m.empty, 'mesh not empty after solid voxels');
assert(m.vertexCount > 0, 'has vertices');
assert(m.triangleCount > 0, 'has triangles');

// Palette colors (4 floats per material id, slot 0 = air = unused).
const palette = new Float32Array([
    0, 0, 0, 0,           // 0 air
    1, 0.5, 0.25, 1,      // 1 brown
]);
const m2 = c.buildMesh(palette, 2);
assert(!m2.empty,         'palette mesh not empty');
assert(m2.hasColors,      'palette mesh has colors');

console.log('PASS test_voxel_chunk');
