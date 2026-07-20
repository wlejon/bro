// =============================================================================
// bro Terrain API Reference
// =============================================================================
//
// Chunked terrain manager: noise-driven height fields with optional LOD
// rings. All heavy lifting (noise, heightmaps, meshing, chunk lifecycle)
// runs in C++ via TerrainManager. JS configures, drives the camera-following
// update, edits the surface, and reads picks.
//
// NOT a voxel engine. Storage is one float height per grid column
// (TerrainManager::ChunkEntry holds `heightmap` / `heightmapPadded`), not a
// 3D occupancy grid. Consequences:
//   - No overhangs, caves, arches, or floating geometry — the surface is a
//     height field, so every column has exactly one solid span from the
//     bottom up to its height.
//   - The "voxel" names below (setVoxel/getVoxel, chunkSize's y component)
//     are legacy; they address columns, not cells. See setVoxel().
//   - raycast() tests LOD-0 chunks only, so rays miss visible distant
//     terrain rendered from coarser LOD rings.
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
//   chunkSize:         [64, 48, 64],     // grid cells per chunk [x, y, z];
//                                        // y bounds the height range only
//   cellSize:          1.0,              // world units per cell
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
     * Cast a ray against the loaded terrain meshes. Hits return the surface
     * cell coords, the chunk it lives in, and the material ID. Returns
     * null on miss (or when the ray exits the loaded radius).
     *
     * LOD-0 chunks only — coarser LOD rings are skipped, so a ray aimed at
     * distant terrain that is plainly visible on screen returns null. Keep
     * picking within the LOD-0 radius.
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
     * Raise or lower the terrain column at world-space (wx, wz) by one
     * cell. Despite the name this is a heightmap edit, not a voxel write:
     *
     *   - `wy` is ignored entirely. You cannot dig at a chosen altitude,
     *     carve a cave, or punch a hole — only move the surface up/down.
     *   - `material` is read as a sign, not an ID: 0 lowers the column by
     *     one cell, anything non-zero raises it by one. Materials 1, 3 and
     *     7 all do the same thing. Materials are assigned by height at
     *     raycast time, not stored.
     *
     * Returns true if the column was modified (false if outside the loaded
     * radius). Edits flag the chunk dirty — call rebuild() to re-mesh.
     *
     * @returns {boolean}
     */
    setVoxel(wx, wy, wz, material) {}

    /**
     * Solidity test at a world-space point: returns 1 when wy is at or
     * below the height of the column at (wx, wz), else 0. Returns 0 if the
     * chunk is not loaded. Never returns a material ID.
     */
    getVoxel(wx, wy, wz) {}

    /** Re-mesh any chunks flagged dirty by setVoxel. Cheap if nothing changed. */
    rebuild() {}

    /**
     * Reconfigure the entire terrain (re-seeds noise, palette, mesh mode,
     * etc.). Frees every loaded chunk and regenerates nothing: the terrain
     * is empty until the next update() streams chunks back in, throttled by
     * maxLoadsPerUpdate. Debounce in UI sliders, and call update() right
     * after if you need geometry in the same frame.
     */
    configure(opts) {}

    /**
     * Supply chunk heights yourself, in place of the built-in FBm generator.
     * Pass null to go back to noise.
     *
     *   terrain.setHeightSource((cx, cz, lod, paddedW, paddedH,
     *                            cellSize, worldX0, worldZ0) => {
     *       const out = new Float32Array(paddedW * paddedH);
     *       for (let pz = 0; pz < paddedH; pz++)
     *           for (let px = 0; px < paddedW; px++)
     *               out[pz * paddedW + px] =
     *                   heightAt(worldX0 + px * cellSize,
     *                            worldZ0 + pz * cellSize);
     *       return out;
     *   });
     *
     * Return a Float32Array of exactly paddedW*paddedH absolute world-space Y
     * values (row-major, z-major: sample (px, pz) at `out[pz * paddedW + px]`),
     * or null/undefined to fall back to the built-in noise FOR THAT CHUNK —
     * which is how you serve only the region you have data for, or layer a
     * coarse learned source under noise detail.
     *
     * A short array or a thrown exception logs and falls back; it never renders
     * a partially filled chunk.
     *
     * USE worldX0/worldZ0 — DO NOT DERIVE THEM. The grid is padded one sample
     * beyond the chunk on every side (paddedW = chunkSizeX + 3), and that outer
     * ring is shared with the neighbours so edge normals can use true central
     * differences. worldX0/worldZ0 already carry that skirt offset. Re-deriving
     * it and dropping the -1 shifts every chunk one cell against its neighbours,
     * which for a coherent (non-noise) source produces terrain that looks
     * completely correct and simply does not line up — a silent seam at every
     * boundary. For the same reason the provider must be deterministic across
     * chunk boundaries: two chunks sampling the same shared world position must
     * return the same height.
     *
     * These positions include config.origin; the built-in noise path does not,
     * a pre-existing quirk that only shows up with a non-zero origin.
     *
     * RUNS INSIDE update(), ON THE JS THREAD, once per newly loaded chunk — so
     * it must be cheap. Sample an already-resident tile; do not generate on
     * demand here. See docs/worldgen-api.js for the intended tile-cache shape
     * with a multi-second generator behind it.
     *
     * Does not rebuild existing chunks — call configure() to regenerate.
     */
    setHeightSource(fn) {}

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
// Typical usage (from broworkshop's demos/terrain/app.js)
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

// Pick the surface under the crosshair and lower that column by one cell
// (material 0 = lower, non-zero = raise; the y argument is ignored):
const hit = terrain.raycast(cam.pos, cam.forward, 200);
if (hit) {
    terrain.setVoxel(hit.position[0], hit.position[1], hit.position[2], 0);
    terrain.rebuild();
}
