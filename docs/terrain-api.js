// =============================================================================
// bro Terrain API Reference
// =============================================================================
//
// Voxel terrain manager: noise-driven height fields, chunked, with greedy
// meshing and optional LOD rings. All heavy lifting (noise, voxel grids,
// meshing, chunk lifecycle) runs in C++ via TerrainManager. JS configures,
// drives the camera-following update, edits voxels, and reads picks.
//
// Created via the scene graph (chunks register as MeshNode children of the
// scene root, so they participate in the normal 3D pipeline — depth test,
// raycast, camera).
//
//   const scene = canvas.getContext('scene');
//   const terrain = scene.createTerrain(opts);
//
// The C++ side rate-limits chunk loads via maxLoadsPerUpdate so terrain
// rebuilds don't block the render loop. Call terrain.update(camX, camY, camZ)
// each frame to stream chunks around the camera.
// =============================================================================


// -----------------------------------------------------------------------------
// scene.createTerrain(opts) → Terrain
// -----------------------------------------------------------------------------
//
// All options are optional; defaults below match the C++ TerrainConfig struct.
//
// {
//   chunkSize:         [64, 48, 64],     // voxels per chunk [x, y, z]
//   cellSize:          1.0,              // world units per voxel
//
//   loadRadius:        4,                // Manhattan distance, in chunks
//   unloadRadius:      6,                // chunks farther than this are freed
//   maxLoadsPerUpdate: 2,                // throttle to avoid frame stalls
//
//   // Noise (built-in FBm generator)
//   seed:              1337,
//   noise: {
//     frequency:       0.035,
//     octaves:         5,
//     gain:            0.5,
//     lacunarity:      2.0,
//   },
//
//   // Terrain shape
//   baseHeight:        18,               // average column height (voxels)
//   heightAmplitude:   16,
//   seaLevel:          14,
//
//   // Mesh mode: 0 = smooth, 1 = flat, 2 = terraced, 3 = blocky
//   meshMode:          0,
//   terraceStep:       1.0,              // step height for meshMode=2
//
//   // Continental amplitude modulation (mountain regions). 0 = disabled.
//   continentFrequency: 0.0,
//   continentMin:       0.1,
//   continentMax:       1.5,
//
//   // Optional huge-scale mountain layer
//   mountainFrequency:  0.0,
//   mountainAmplitude:  0.0,
//   mountainOctaves:    3,
//
//   // LOD rings
//   lodLevels:         1,                // 1 = uniform (no LOD)
//   lodScaleFactor:    4,                // each ring N× coarser
//
//   // Planetary curvature (0 = flat). 6371000 = Earth radius.
//   planetRadius:      0.0,
//
//   // World-space origin of this terrain (allows multi-planet setups).
//   origin:            [0, 0, 0],
//
//   // Per-material RGBA palette: 4 floats (r,g,b,a in 0–1) per material ID.
//   // Index 0 must be air (alpha 0). Float32Array or plain Array.
//   palette: new Float32Array([
//     0,0,0,0,                     // 0: air
//     0.42,0.70,0.27,1,            // 1: grass
//     0.52,0.34,0.18,1,            // 2: dirt
//     0.55,0.55,0.58,1,            // 3: stone
//   ]),
// }


// -----------------------------------------------------------------------------
// Terrain instance methods
// -----------------------------------------------------------------------------

class Terrain {

    /**
     * Stream chunks around (x, y, z) in world space — typically the camera.
     * Loads up to maxLoadsPerUpdate new chunks and frees chunks beyond
     * unloadRadius. Returns the number of chunks newly loaded this call.
     * @returns {number}
     */
    update(x, y, z) {}

    /**
     * Cast a ray against the loaded voxel grid. Hits return the surface
     * voxel coords, the chunk it lives in, and the material ID. Returns
     * null on miss (or when the ray exits the loaded radius).
     *
     * @param {number[]} origin     - [x, y, z] world space
     * @param {number[]} direction  - [x, y, z] (need not be unit length)
     * @param {number}   [maxDist]  - 0 = unlimited within loaded radius
     * @returns {?{
     *   hit: true, distance: number,
     *   position: [number,number,number], normal: [number,number,number],
     *   chunk: [number,number],          // chunk x,z indices
     *   voxel: [number,number,number],   // local voxel x,y,z within chunk
     *   material: number,
     * }}
     */
    raycast(origin, direction, maxDist) {}

    /**
     * Set a voxel by world-space coordinate. Pass material 0 to clear
     * (dig). Returns true if the voxel was modified (false if outside the
     * loaded radius). Edits flag the chunk dirty — call rebuild() to
     * regenerate the mesh.
     *
     * @returns {boolean}
     */
    setVoxel(wx, wy, wz, material) {}

    /** Read a voxel by world-space coordinate. Returns 0 (air) if not loaded. */
    getVoxel(wx, wy, wz) {}

    /** Re-mesh any chunks flagged dirty by setVoxel. Cheap if nothing changed. */
    rebuild() {}

    /**
     * Reconfigure the entire terrain (re-seeds noise, palette, mesh mode,
     * etc.) and rebuilds all loaded chunks. Slow — debounce in UI sliders.
     */
    configure(opts) {}

    /** Free all chunks and detach the manager. The instance becomes inert. */
    destroy() {}

    // --- Read-only properties --------------------------------------------

    get chunkCount() {}
    get triangleCount() {}
    get vertexCount() {}
    get farDistance() {}      // unloadRadius converted to world units
    get planetRadius() {}
    get origin() {}           // [x, y, z]
}


// -----------------------------------------------------------------------------
// Typical usage (from apps/terrain/app.js)
// -----------------------------------------------------------------------------

const canvas  = document.getElementById('c');
const scene   = canvas.getContext('scene');

const terrain = scene.createTerrain({
    chunkSize: [64, 48, 64],
    cellSize:  1.0,
    loadRadius: 4, unloadRadius: 6, maxLoadsPerUpdate: 2,
    noise: { frequency: 0.035, octaves: 5, gain: 0.5, lacunarity: 2.0 },
    baseHeight: 18, heightAmplitude: 16, seaLevel: 14,
    meshMode:   0,
    palette: new Float32Array([
        0,0,0,0,
        0.42,0.70,0.27,1,    // grass
        0.52,0.34,0.18,1,    // dirt
        0.55,0.55,0.58,1,    // stone
    ]),
});

function frame() {
    terrain.update(cam.pos[0], cam.pos[1], cam.pos[2]);
    requestAnimationFrame(frame);
}
frame();

// Pick the voxel under the crosshair and dig:
const hit = terrain.raycast(cam.pos, cam.forward, 200);
if (hit) {
    terrain.setVoxel(hit.position[0], hit.position[1], hit.position[2], 0);
    terrain.rebuild();
}
