// =============================================================================
// Terrain Worker — generates one chunk per request
// =============================================================================
//
// Receives:
//   { type:'init', seed, chunkGrid, cellSize }
//   { type:'generate', id, cx, cy, cz, lod, edits }    // edits: Float32Array|null, 5 floats per edit
//
// Replies (with transferList):
//   { type:'ready' }
//   { type:'chunk', id, cx, cy, cz, lod, mesh, empty }
//     - mesh: transferred Mesh handle (zero-copy, C++-owned)
//     - empty: true if the chunk has no surface (no mesh is attached)
//
// Density convention: density < 0 = solid, density > 0 = air. Surface is at 0.
// Density is computed transiently inside the worker and never returned — the
// main thread's picking is done via scene.raycast() against the real triangles.
//
// =============================================================================

var SEED = 42;
var CHUNK_GRID = 32;          // voxels per chunk axis (same at every LOD)
var CELL_SIZE = 1.0;          // world units per voxel at LOD 0

// At LOD L the chunk physically spans CHUNK_GRID * CELL_SIZE * 2^L world units.
// So LOD 0 = 32m, LOD 1 = 64m, LOD 2 = 128m, ...  All LODs share the same noise
// field — a coarser LOD just samples that field at a coarser stride, so the
// hills/mountains line up regardless of which LOD a chunk lives at.
function chunkWorldSize(lod) { return CHUNK_GRID * CELL_SIZE * (1 << lod); }

// --- Noise graph (built lazily after init) ---------------------------------

var noiseReady = false;
var terrainFbm, ridged, biomeNoise, caveWarp;

// Reusable scratch buffers (allocated once per worker). Before this, each
// computeDensityField call allocated 4 fresh noise grids + a field buffer
// + an mcField copy, churning ~6× G³ floats on the worker JS heap per
// chunk. At CHUNK_GRID=128 that was ~50 MB of allocation per chunk —
// enough to OOM the worker's 256 MB QuickJS budget quickly. These buffers
// persist for the lifetime of the worker and are written into via
// FastNoise's genUniformGrid3DInto (for noise) or plain indexed writes.
var terrainBuf = null;
var ridgeBuf   = null;
var biomeBuf   = null;
var caveBuf    = null;
var fieldBuf   = null;
var mcFieldBuf = null;

function allocScratchBuffers() {
    var G = CHUNK_GRID + 1;
    var n = G * G * G;
    terrainBuf = new Float32Array(n);
    ridgeBuf   = new Float32Array(n);
    biomeBuf   = new Float32Array(n);
    caveBuf    = new Float32Array(n);
    fieldBuf   = new Float32Array(n);
    mcFieldBuf = new Float32Array(n);
}

function buildNoise() {
    // Base terrain: rolling FBm hills
    terrainFbm = FastNoise.create('FractalFBm');
    terrainFbm.set('Source', FastNoise.create('Simplex'));
    terrainFbm.set('Octaves', 6);
    terrainFbm.set('Gain', 0.5);
    terrainFbm.set('Lacunarity', 2.0);

    // Mountain ridges
    ridged = FastNoise.create('FractalRidged');
    ridged.set('Source', FastNoise.create('Perlin'));
    ridged.set('Octaves', 5);
    ridged.set('Gain', 0.5);
    ridged.set('Lacunarity', 2.1);

    // Biome blend (low frequency)
    biomeNoise = FastNoise.create('Simplex');

    // Caves: domain-warped ridged noise
    var caveBase = FastNoise.create('FractalRidged');
    caveBase.set('Source', FastNoise.create('Perlin'));
    caveBase.set('Octaves', 3);
    caveBase.set('Gain', 0.6);

    caveWarp = FastNoise.create('DomainWarpGradient');
    caveWarp.set('Source', caveBase);
    caveWarp.set('Warp Amplitude', 25.0);

    noiseReady = true;
}

// --- Density field generation ----------------------------------------------

// Constants tuned for visible features at default scale.
// Frequencies are in noise-input units per world unit; per-chunk noise span =
// CHUNK_WORLD_SIZE * FREQ. Aim for ~0.5–2 noise units per chunk so that the
// noise function varies visibly within a single chunk.
// Frequencies are tuned so that a LOD 0 chunk (32m) sees roughly one full
// noise feature. Smaller frequencies = smoother but huger features; larger
// frequencies = more detail per chunk but coarse LODs alias.
var TERRAIN_FREQ  = 0.035;    // rolling hills        (~1.1 features / LOD-0 chunk)
var RIDGE_FREQ    = 0.014;    // mountain ridges      (~0.45 / chunk)
var BIOME_FREQ    = 0.003;    // biome blend (slow)
var CAVE_FREQ     = 0.06;     // cave carving

var TERRAIN_AMP   = 35.0;     // hill amplitude (world units)
var RIDGE_AMP     = 130.0;    // mountain amplitude (world units)
var BASE_GROUND_Y = 0.0;      // average ground level

// Caves: voxels with cave noise > threshold are carved out
var CAVE_THRESHOLD = 0.55;
var CAVE_STRENGTH  = 22.0;

function computeDensityField(cx, cy, cz, lod) {
    var step = CELL_SIZE * (1 << lod);
    var G = CHUNK_GRID + 1;       // sample count per axis (extra sample for boundary)

    // World-space origin of this chunk. Each LOD has its own chunk grid so
    // multiplying chunk coords by the LOD-specific chunk size gives the right
    // world position regardless of which level we're meshing.
    var sz = chunkWorldSize(lod);
    var wx = cx * sz;
    var wy = cy * sz;
    var wz = cz * sz;

    // Generate 3D grids via FastNoise2 (SIMD-accelerated), writing into the
    // reusable module-level scratch buffers instead of allocating new ones.
    //
    // FastNoise2's GenUniformGrid3D samples noise at:
    //   (xOffset + i*xStepSize, yOffset + j*yStepSize, zOffset + k*zStepSize)
    // in *noise-input space*. To get a coherent noise field across chunks, both
    // the offsets AND the step must be in noise-input space — i.e. world coords
    // pre-multiplied by the frequency. Otherwise neighboring chunks each sample
    // a far-apart, essentially-constant region of the noise function.
    terrainFbm.genUniformGrid3DInto(terrainBuf,
        wx * TERRAIN_FREQ, wy * TERRAIN_FREQ, wz * TERRAIN_FREQ,
        G, G, G, TERRAIN_FREQ * step, SEED);
    ridged.genUniformGrid3DInto(ridgeBuf,
        wx * RIDGE_FREQ, wy * RIDGE_FREQ, wz * RIDGE_FREQ,
        G, G, G, RIDGE_FREQ * step, SEED + 1);
    biomeNoise.genUniformGrid3DInto(biomeBuf,
        wx * BIOME_FREQ, wy * BIOME_FREQ, wz * BIOME_FREQ,
        G, G, G, BIOME_FREQ * step, SEED + 2);
    caveWarp.genUniformGrid3DInto(caveBuf,
        wx * CAVE_FREQ, wy * CAVE_FREQ, wz * CAVE_FREQ,
        G, G, G, CAVE_FREQ * step, SEED + 3);

    var terrainGrid = terrainBuf;
    var ridgeGrid   = ridgeBuf;
    var biomeGrid   = biomeBuf;
    var caveGrid    = caveBuf;

    // `field` is now worker-local — no longer sent back to the main thread
    // since picking moved to scene.raycast(). We can safely reuse the same
    // buffer across chunks. generateChunk consumes it synchronously between
    // calls.
    var field = fieldBuf;

    for (var z = 0; z < G; z++) {
        for (var y = 0; y < G; y++) {
            for (var x = 0; x < G; x++) {
                var idx = (z * G + y) * G + x;
                var worldY = wy + y * step;

                // Surface height contribution (positive = higher ground)
                // FBm output is roughly in [-1, 1] but biased
                var hillH  = terrainGrid[idx] * TERRAIN_AMP;
                var ridgeH = (ridgeGrid[idx] * 0.5 + 0.5) * RIDGE_AMP;  // remap to [0, RIDGE_AMP]

                // Biome blend: 0 = plains, 1 = mountains. The previous square()
                // shoved everything toward plains and made mountains rare/subtle;
                // a milder pow keeps a real mountain biome visible.
                var biome = biomeGrid[idx] * 0.5 + 0.5;
                biome = Math.pow(biome, 1.4);
                var surfaceH = BASE_GROUND_Y + hillH * (1 - biome) + ridgeH * biome;

                // Base density: positive above surface (air), negative below (solid)
                var d = worldY - surfaceH;

                // Cliff/overhang: ridge noise also adds 3D variation directly to density.
                // This is what gives natural overhangs since the surface height varies with Y.
                d -= ridgeGrid[idx] * 14.0 * biome;

                // Cave carving: above ground unaffected; below ground, carve where cave noise spikes
                if (d < 0) {
                    var cave = caveGrid[idx];
                    if (cave > CAVE_THRESHOLD) {
                        var carve = (cave - CAVE_THRESHOLD) * CAVE_STRENGTH;
                        d += carve;  // push toward air
                    }
                }

                field[idx] = d;
            }
        }
    }

    return field;
}

// --- Edit application ------------------------------------------------------

function applyEdits(field, gridSize, cx, cy, cz, lod, edits) {
    var G = gridSize;
    var step = CELL_SIZE * (1 << lod);
    var sz = chunkWorldSize(lod);
    var wx = cx * sz;
    var wy = cy * sz;
    var wz = cz * sz;

    var n = (edits.length / 5) | 0;
    for (var e = 0; e < n; e++) {
        // Edits store world-space coordinates
        var ex = edits[e * 5 + 0];
        var ey = edits[e * 5 + 1];
        var ez = edits[e * 5 + 2];
        var er = edits[e * 5 + 3];
        var es = edits[e * 5 + 4];
        var r2 = er * er;

        // Convert world to local voxel space
        var lx = (ex - wx) / step;
        var ly = (ey - wy) / step;
        var lz = (ez - wz) / step;
        var lr = er / step;
        var lr2 = lr * lr;

        var minX = Math.max(0, Math.floor(lx - lr));
        var maxX = Math.min(G - 1, Math.ceil(lx + lr));
        var minY = Math.max(0, Math.floor(ly - lr));
        var maxY = Math.min(G - 1, Math.ceil(ly + lr));
        var minZ = Math.max(0, Math.floor(lz - lr));
        var maxZ = Math.min(G - 1, Math.ceil(lz + lr));

        for (var z = minZ; z <= maxZ; z++) {
            for (var y = minY; y <= maxY; y++) {
                for (var x = minX; x <= maxX; x++) {
                    var dx = x - lx, dy = y - ly, dz = z - lz;
                    var d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 < lr2) {
                        var t = 1.0 - Math.sqrt(d2) / lr;
                        var idx = (z * G + y) * G + x;
                        field[idx] += es * t * t;
                    }
                }
            }
        }
    }
}

// --- Meshing ---------------------------------------------------------------

function generateChunk(id, cx, cy, cz, lod, edits) {
    if (!noiseReady) buildNoise();

    var G = CHUNK_GRID + 1;
    var field = computeDensityField(cx, cy, cz, lod);

    if (edits && edits.length > 0) {
        applyEdits(field, G, cx, cy, cz, lod, edits);
    }

    // Quick early-out: if all samples have the same sign, the chunk is uniform
    var hasPos = false, hasNeg = false;
    for (var i = 0; i < field.length; i++) {
        if (field[i] > 0) hasPos = true;
        else if (field[i] < 0) hasNeg = true;
        if (hasPos && hasNeg) break;
    }
    if (!hasPos || !hasNeg) {
        return {
            msg: {
                type: 'chunk', id: id, cx: cx, cy: cy, cz: cz, lod: lod,
                empty: true
            },
            transfer: []
        };
    }

    // Marching cubes (cellSize accounts for LOD step).
    //
    // Convention mismatch: bromesh::marchingCubes treats POSITIVE field values
    // as "inside the surface" (see bromesh tests/test_main.cpp marching_cubes_sphere
    // which builds `field[idx] = radius - dist` — positive inside the sphere).
    // Our terrain density uses the opposite sign (negative = solid). Without
    // negating, the resulting triangles wind backwards, normals point INTO the
    // ground, and back-face culling drops every triangle (=> nothing rendered).
    //
    // We sign-flip into the reusable mcFieldBuf instead of allocating a fresh
    // Float32Array; marchingCubes copies the field into its own C++ vector so
    // the JS buffer can be reused on the next call safely.
    var step = CELL_SIZE * (1 << lod);
    for (var k = 0; k < field.length; k++) mcFieldBuf[k] = -field[k];
    var mesh = Mesh.marchingCubes(mcFieldBuf, G, G, G, 0, step);

    if (!mesh || mesh.empty) {
        return {
            msg: {
                type: 'chunk', id: id, cx: cx, cy: cy, cz: cz, lod: lod,
                empty: true
            },
            transfer: []
        };
    }

    // The mesh is transferred by handle: the underlying C++ MeshData moves
    // across thread ownership without ever materializing as a JS typed array
    // on either side. Density is never sent back — the main thread uses
    // scene.raycast() against the real scene-graph triangles for picking.
    return {
        msg: {
            type: 'chunk', id: id, cx: cx, cy: cy, cz: cz, lod: lod,
            mesh: mesh,
            empty: false
        },
        transfer: [mesh]
    };
}

// --- Message handler -------------------------------------------------------

self.onmessage = function(e) {
    var msg = e.data;
    if (!msg || !msg.type) return;

    if (msg.type === 'init') {
        if (typeof msg.seed === 'number') SEED = msg.seed | 0;
        if (typeof msg.chunkGrid === 'number') CHUNK_GRID = msg.chunkGrid | 0;
        if (typeof msg.cellSize === 'number') CELL_SIZE = msg.cellSize;
        buildNoise();
        allocScratchBuffers();
        self.postMessage({ type: 'ready' });
        return;
    }

    if (msg.type === 'generate') {
        var result = generateChunk(msg.id, msg.cx, msg.cy, msg.cz, msg.lod || 0, msg.edits || null);
        // result is {msg, transfer}: the Mesh (if any) is in the transfer
        // list so it moves by pointer across threads, not as bytes.
        self.postMessage(result.msg, result.transfer);
        return;
    }
};

// Signal ready immediately so main thread can start dispatching even before init
self.postMessage({ type: 'ready' });
