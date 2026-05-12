// Test bro scene.createTerrain — exercises src/scene/terrain_manager.cpp
// and src/js/terrain_bindings.cpp.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '128');
canvas.setAttribute('height', '128');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('no scene; skipping terrain test');
} else if (typeof scene.createTerrain !== 'function') {
    console.log('no createTerrain; skipping');
} else {
    const terrain = scene.createTerrain({
        chunkSize: [16, 16, 16],
        cellSize: 1.0,
        loadRadius: 1,
        unloadRadius: 2,
        maxLoadsPerUpdate: 2,
        seed: 42,
        noise: { frequency: 0.05, octaves: 3, gain: 0.5, lacunarity: 2.0 },
        baseHeight: 4,
        heightAmplitude: 4,
        seaLevel: 2,
        meshMode: 0,
        palette: new Float32Array([
            0, 0, 0, 0,
            0.4, 0.7, 0.3, 1,
            0.5, 0.3, 0.2, 1,
        ]),
    });
    assert(terrain !== null, 'createTerrain returns object');

    // Stream chunks at origin
    const loaded = terrain.update(0, 0, 0);
    assert(typeof loaded === 'number', 'update returns chunk count');

    // Repeated calls may load more
    terrain.update(0, 0, 0);
    terrain.update(8, 0, 0);
    terrain.update(-8, 0, -8);

    // Read-only counters
    assert(typeof terrain.chunkCount === 'number', 'chunkCount is number');
    assert(typeof terrain.triangleCount === 'number', 'triangleCount is number');
    assert(typeof terrain.vertexCount === 'number', 'vertexCount is number');

    // getVoxel
    const v = terrain.getVoxel(0, 0, 0);
    assert(typeof v === 'number', 'getVoxel returns number');

    // setVoxel + rebuild
    const set = terrain.setVoxel(0, 5, 0, 1);
    assert(typeof set === 'boolean', 'setVoxel returns bool');
    terrain.rebuild();

    // raycast
    const hit = terrain.raycast([0, 50, 0], [0, -1, 0], 100);
    // hit may be null if no chunk loaded yet — just verify type
    assert(hit === null || typeof hit === 'object', 'raycast returns null or object');
    if (hit) {
        assert(typeof hit.distance === 'number', 'hit.distance');
        assert(Array.isArray(hit.position), 'hit.position');
    }

    // configure (rebuild whole thing)
    terrain.configure({
        baseHeight: 6, heightAmplitude: 8,
    });

    // origin
    if ('origin' in terrain) {
        const o = terrain.origin;
        assert(Array.isArray(o), 'origin is array');
    }

    // Cleanup
    terrain.destroy();
}

document.body.removeChild(canvas);
