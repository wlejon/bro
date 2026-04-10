// =============================================================================
// Terrain — Minecraft-esque blocky voxel world
// =============================================================================
//
// Pipeline (all main thread, no workers):
//   1. FastNoise FBm  → 2D heightmap (CHUNK_W × CHUNK_D)
//   2. Heights → Uint8 voxel grid (CHUNK_W × CHUNK_H × CHUNK_D), each cell
//      tagged with a material id (grass / dirt / stone / bedrock)
//   3. For each material, mask the voxel grid to that material only and run
//      Mesh.greedyMesh — produces a single quad-merged mesh per material
//   4. Hand each mesh to scene.createMesh with a uniform color
//   5. WASD/mouse fly camera, same pattern as solar3d
//
// MeshNode currently doesn't honor per-vertex colors, so a multi-material
// world needs one mesh node per material. With ~5 materials and a chunk this
// size that's still trivial — greedy meshing collapses each layer to a few
// hundred quads.
// =============================================================================

var canvas = document.getElementById('c');
var scene = canvas.getContext('scene');
var info = document.getElementById('info');

// ----------------------------------------------------------------------------
// Constants
// ----------------------------------------------------------------------------

var CHUNK_W = 64;   // X
var CHUNK_D = 64;   // Z
var CHUNK_H = 48;   // Y (vertical)

var BASE_HEIGHT = 18;   // average ground level (in voxels from y=0)
var HEIGHT_AMP  = 16;   // peak-to-trough amplitude
var NOISE_FREQ  = 0.035; // FBm sample frequency

// Material ids
var AIR     = 0;
var GRASS   = 1;
var DIRT    = 2;
var STONE   = 3;
var BEDROCK = 4;
var SAND    = 5;

var MATERIALS = [
    { id: GRASS,   name: 'grass',   color: [0.42, 0.70, 0.27] },
    { id: DIRT,    name: 'dirt',    color: [0.52, 0.34, 0.18] },
    { id: STONE,   name: 'stone',   color: [0.55, 0.55, 0.58] },
    { id: BEDROCK, name: 'bedrock', color: [0.18, 0.18, 0.20] },
    { id: SAND,    name: 'sand',    color: [0.86, 0.78, 0.49] },
];

var SEA_LEVEL = BASE_HEIGHT - 4; // sand below this elevation

// Simple 3-tuple math for the camera
function v3add(a, b)   { return [a[0]+b[0], a[1]+b[1], a[2]+b[2]]; }
function v3scale(a, s) { return [a[0]*s, a[1]*s, a[2]*s]; }

// ----------------------------------------------------------------------------
// Terrain generation
// ----------------------------------------------------------------------------

function buildHeightmap(seed) {
    var simplex = FastNoise.create('Simplex');
    var fbm = FastNoise.create('FractalFBm');
    fbm.set('Source', simplex);
    fbm.set('Octaves', 5);
    fbm.set('Gain', 0.5);
    fbm.set('Lacunarity', 2.0);

    // genUniformGrid2D returns Float32Array, layout [y * xSize + x]
    var raw = fbm.genUniformGrid2D(0, 0, CHUNK_W, CHUNK_D, NOISE_FREQ, seed);

    // Map roughly [-1, 1] → [0, 1] → integer height in voxel units.
    var heights = new Int32Array(CHUNK_W * CHUNK_D);
    for (var i = 0; i < raw.length; i++) {
        var t = (raw[i] + 1) * 0.5;          // 0..1
        if (t < 0) t = 0; else if (t > 1) t = 1;
        heights[i] = Math.floor(BASE_HEIGHT + (t - 0.5) * 2 * HEIGHT_AMP);
        if (heights[i] < 1) heights[i] = 1;
        if (heights[i] >= CHUNK_H) heights[i] = CHUNK_H - 1;
    }
    return heights;
}

// Build a Uint8Array voxel grid from a heightmap. Layout matches greedyMesh:
//   voxels[(z * CHUNK_H + y) * CHUNK_W + x]
function buildVoxelGrid(heights) {
    var voxels = new Uint8Array(CHUNK_W * CHUNK_H * CHUNK_D);
    for (var z = 0; z < CHUNK_D; z++) {
        for (var x = 0; x < CHUNK_W; x++) {
            var h = heights[z * CHUNK_W + x];
            for (var y = 0; y <= h && y < CHUNK_H; y++) {
                var mat;
                if (y === 0) {
                    mat = BEDROCK;
                } else if (y === h) {
                    // Surface: sand below sea level, grass above
                    mat = (h <= SEA_LEVEL) ? SAND : GRASS;
                } else if (y >= h - 3) {
                    mat = (h <= SEA_LEVEL) ? SAND : DIRT;
                } else {
                    mat = STONE;
                }
                voxels[(z * CHUNK_H + y) * CHUNK_W + x] = mat;
            }
        }
    }
    return voxels;
}

// ----------------------------------------------------------------------------
// Build the world from a seed
// ----------------------------------------------------------------------------

// Module-level state: the live voxel grid and the per-material mesh nodes
// that visualise it. Block edits mutate `voxels` in place and call
// rebuildMaterial(matId) to refresh just that material's mesh.
var voxels = null;
var terrainNodes = {};   // matId → SceneNode
var matStats = {};       // matId → { tris, verts }
var solidCount = 0;      // number of non-AIR voxels (updated incrementally)
var lastStats = { tris: 0, verts: 0, voxels: 0, materials: 0, seed: 0, ms: 0 };

// Deferred rebuild state — placement uses temp cubes for instant feedback,
// then batches the greedy mesh rebuild.
var pendingCubes = [];
var dirtyMaterials = {};
var pendingEditCount = 0;
var pendingTimer = null;
var PENDING_LIMIT = 100;
var PENDING_TIMEOUT = 2000;  // ms

// World-space offset of the chunk's local (0,0,0) corner. The chunk is
// centered on the origin so the camera frames it nicely.
var CHUNK_ORIGIN_X = -CHUNK_W * 0.5;
var CHUNK_ORIGIN_Y = 0;
var CHUNK_ORIGIN_Z = -CHUNK_D * 0.5;

function clearTerrain() {
    if (pendingTimer !== null) { clearTimeout(pendingTimer); pendingTimer = null; }
    for (var i = 0; i < pendingCubes.length; i++) pendingCubes[i].destroy();
    pendingCubes = [];
    dirtyMaterials = {};
    pendingEditCount = 0;
    for (var key in terrainNodes) {
        if (terrainNodes[key]) terrainNodes[key].destroy();
    }
    terrainNodes = {};
    matStats = {};
}

// Look up a material descriptor by id.
function findMaterial(matId) {
    for (var i = 0; i < MATERIALS.length; i++) {
        if (MATERIALS[i].id === matId) return MATERIALS[i];
    }
    return null;
}

// Refresh tri/vert totals from per-material aggregates (no grid scan).
function refreshTotals() {
    var t = 0, v = 0, m = 0;
    for (var id in matStats) {
        t += matStats[id].tris;
        v += matStats[id].verts;
        m++;
    }
    lastStats.tris = t;
    lastStats.verts = v;
    lastStats.materials = m;
    lastStats.voxels = solidCount;
}

// Count solid voxels (used once after world generation; edits update
// solidCount incrementally via ±1).
function countSolid() {
    var n = 0;
    for (var i = 0; i < voxels.length; i++) if (voxels[i] !== 0) n++;
    return n;
}

// Voxel grid index. (z * CHUNK_H + y) * CHUNK_W + x — must match
// buildVoxelGrid() and the layout greedyMesh expects.
function voxelIdx(x, y, z) {
    return (z * CHUNK_H + y) * CHUNK_W + x;
}

function inBounds(x, y, z) {
    return x >= 0 && x < CHUNK_W &&
           y >= 0 && y < CHUNK_H &&
           z >= 0 && z < CHUNK_D;
}

// Rebuild a single material's greedy mesh and update its scene node.
// The material filter is done in C++ (no JS masking loop) — the full
// voxel grid is passed directly and greedyMesh ignores non-matching IDs.
function rebuildMaterial(matId) {
    var mat = findMaterial(matId);
    if (!mat) return;

    var mesh = Mesh.greedyMesh(voxels, CHUNK_W, CHUNK_H, CHUNK_D, 1.0, matId);

    if (mesh.triangleCount === 0) {
        if (terrainNodes[matId]) {
            terrainNodes[matId].destroy();
            terrainNodes[matId] = null;
        }
        delete matStats[matId];
        return;
    }

    matStats[matId] = { tris: mesh.triangleCount, verts: mesh.vertexCount };

    if (terrainNodes[matId]) {
        // Fast path — reuse existing node, just swap the mesh data.
        terrainNodes[matId].updateMesh(mesh, { transfer: true });
    } else {
        terrainNodes[matId] = scene.createMesh({
            mesh: mesh,
            transfer: true,
            x: CHUNK_ORIGIN_X,
            y: CHUNK_ORIGIN_Y,
            z: CHUNK_ORIGIN_Z,
            color: mat.color,
            name: 'terrain-' + mat.name
        });
    }
}

// Rebuild every material from scratch — used by initial world generation
// and full regenerations (R key).
function rebuildMeshes() {
    var t0 = Date.now();
    for (var m = 0; m < MATERIALS.length; m++) {
        rebuildMaterial(MATERIALS[m].id);
    }
    solidCount = countSolid();
    refreshTotals();
    lastStats.ms = Date.now() - t0;
}

// --- Deferred rebuild (placement batching) ---

function scheduleDeferredRebuild() {
    if (pendingTimer !== null) clearTimeout(pendingTimer);
    pendingTimer = setTimeout(flushPendingEdits, PENDING_TIMEOUT);
}

function flushPendingEdits() {
    if (pendingTimer !== null) { clearTimeout(pendingTimer); pendingTimer = null; }
    if (pendingEditCount === 0) return;

    var t0 = Date.now();
    for (var matId in dirtyMaterials) {
        rebuildMaterial(parseInt(matId));
    }
    for (var i = 0; i < pendingCubes.length; i++) {
        pendingCubes[i].destroy();
    }
    pendingCubes = [];
    dirtyMaterials = {};
    pendingEditCount = 0;
    refreshTotals();
    lastStats.ms = Date.now() - t0;
}

function buildWorld(seed) {
    clearTerrain();
    var heights = buildHeightmap(seed);
    voxels = buildVoxelGrid(heights);
    lastStats.seed = seed;
    rebuildMeshes();
    console.log('terrain: seed=' + seed +
                ' voxels=' + lastStats.voxels +
                ' tris=' + lastStats.tris +
                ' verts=' + lastStats.verts +
                ' materials=' + lastStats.materials +
                ' build=' + lastStats.ms + 'ms');
}

// ----------------------------------------------------------------------------
// Block picking — mine + place via scene.raycast from the crosshair
// ----------------------------------------------------------------------------

// Convert a world-space point to its containing voxel coordinates. Hit
// points sit exactly on the surface, so the caller nudges by ±normal*eps
// before calling this to land on the right side of the boundary.
function worldToVoxel(wx, wy, wz) {
    return [
        Math.floor(wx - CHUNK_ORIGIN_X),
        Math.floor(wy - CHUNK_ORIGIN_Y),
        Math.floor(wz - CHUNK_ORIGIN_Z)
    ];
}

// Cast a ray from the camera through the crosshair (always screen center)
// and either remove the hit block or place a new block adjacent to it.
//   action: 'mine' → set the hit voxel to AIR
//           'place' → set the empty voxel adjacent to the hit face to `mat`
// Returns true if a block was modified.
//
// Mining rebuilds the affected material's greedy mesh immediately — the
// C++ greedyMesh with inline material filtering is fast enough (~5-15 ms
// Release) that the hole is visible the same frame.
//
// Placement drops a temp cube for instant feedback and batches the greedy
// mesh rebuild (deferred until PENDING_LIMIT edits or PENDING_TIMEOUT ms
// of inactivity).
function pickAndEdit(action, placeMat) {
    var fwd = camForward();
    var hit = scene.raycast(cam.pos, fwd, 200);
    if (!hit) return false;

    var p = hit.position || hit.point;
    var n = hit.normal || [0, 1, 0];

    var coord, idx, changedMat;
    if (action === 'mine') {
        coord = worldToVoxel(p[0] - n[0] * 0.5, p[1] - n[1] * 0.5, p[2] - n[2] * 0.5);
        if (!inBounds(coord[0], coord[1], coord[2])) return false;
        idx = voxelIdx(coord[0], coord[1], coord[2]);
        if (voxels[idx] === AIR) return false;
        changedMat = voxels[idx];
        voxels[idx] = AIR;
        solidCount--;

        // Immediate rebuild — no overlay needed, the hole is real.
        var t0 = Date.now();
        rebuildMaterial(changedMat);
        refreshTotals();
        lastStats.ms = Date.now() - t0;
    } else {
        coord = worldToVoxel(p[0] + n[0] * 0.5, p[1] + n[1] * 0.5, p[2] + n[2] * 0.5);
        if (!inBounds(coord[0], coord[1], coord[2])) return false;
        idx = voxelIdx(coord[0], coord[1], coord[2]);
        if (voxels[idx] !== AIR) return false;
        voxels[idx] = placeMat;
        changedMat = placeMat;
        solidCount++;

        // Instant visual feedback: drop a unit cube at the placed position.
        var mat = findMaterial(placeMat);
        pendingCubes.push(scene.createMesh({
            mesh: 'box',
            halfW: 0.5, halfH: 0.5, halfD: 0.5,
            x: CHUNK_ORIGIN_X + coord[0] + 0.5,
            y: CHUNK_ORIGIN_Y + coord[1] + 0.5,
            z: CHUNK_ORIGIN_Z + coord[2] + 0.5,
            color: mat.color,
            name: 'temp-cube'
        }));

        dirtyMaterials[changedMat] = true;
        pendingEditCount++;
        if (pendingEditCount >= PENDING_LIMIT) {
            flushPendingEdits();
        } else {
            scheduleDeferredRebuild();
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// Camera (FPS-style fly)
// ----------------------------------------------------------------------------

// Start outside the chunk in the +X+Z corner (same side as the directional
// light) at a moderate elevation, looking diagonally toward the chunk
// center. Shallow pitch keeps the side walls of each block visible so the
// terrain relief reads clearly.
var cam = {
    pos: [50, 42, 50],
    yaw: -Math.PI / 4,   // looking toward the -X-Z corner (chunk center)
    pitch: -0.30,        // ~17° down — keeps terrain relief readable
    speed: 18,
    sensitivity: 0.003
};

function camForward() {
    var cy = Math.cos(cam.yaw), sy = Math.sin(cam.yaw);
    var cp = Math.cos(cam.pitch), sp = Math.sin(cam.pitch);
    return [sy * cp, sp, -cy * cp];
}
function camRight() {
    return [Math.cos(cam.yaw), 0, Math.sin(cam.yaw)];
}

// ----------------------------------------------------------------------------
// Input
// ----------------------------------------------------------------------------

var keys = {};
var mouseCaptured = false;
var seed = 1337;

// Material the player will place on right-click. Cycles through MATERIALS
// (skipping AIR/BEDROCK) via [/] keys.
var placeMatIndex = 2;   // STONE
function activePlaceMat() {
    return MATERIALS[placeMatIndex].id;
}
function activePlaceMatName() {
    return MATERIALS[placeMatIndex].name;
}

document.addEventListener('keydown', function(e) {
    keys[e.key.toLowerCase()] = true;
    if (e.key === 'r' || e.key === 'R') {
        seed = (seed + 1) | 0;
        buildWorld(seed);
    }
    if (e.key === '[') {
        placeMatIndex = (placeMatIndex - 1 + MATERIALS.length) % MATERIALS.length;
    }
    if (e.key === ']') {
        placeMatIndex = (placeMatIndex + 1) % MATERIALS.length;
    }
    if (e.key === 'Escape') mouseCaptured = false;
});
document.addEventListener('keyup', function(e) {
    keys[e.key.toLowerCase()] = false;
});

// Mouse buttons:
//   - First click captures the pointer for look mode (mouseCaptured = true).
//   - Once captured, left = mine the targeted block, right = place a block.
canvas.addEventListener('mousedown', function(e) {
    if (!mouseCaptured) {
        mouseCaptured = true;
        return;   // first click just captures, doesn't edit
    }
    if (e.button === 0) {
        pickAndEdit('mine');
    } else if (e.button === 2) {
        pickAndEdit('place', activePlaceMat());
    }
});

// Suppress the browser context menu so right-click can be used for placement.
canvas.addEventListener('contextmenu', function(e) { e.preventDefault(); });

document.addEventListener('mousemove', function(e) {
    if (!mouseCaptured) return;
    cam.yaw += e.movementX * cam.sensitivity;
    cam.pitch -= e.movementY * cam.sensitivity;
    if (cam.pitch >  1.4) cam.pitch =  1.4;
    if (cam.pitch < -1.4) cam.pitch = -1.4;
});

// ----------------------------------------------------------------------------
// Initial build
// ----------------------------------------------------------------------------

buildWorld(seed);

// ----------------------------------------------------------------------------
// Render loop
// ----------------------------------------------------------------------------

var lastTime = Date.now();
var frameCount = 0;
var fpsAccum = 0;
var fps = 0;

function render() {
    var now = Date.now();
    var dt = Math.min((now - lastTime) / 1000, 0.05);
    lastTime = now;

    // FPS smoothing
    frameCount++;
    fpsAccum += dt;
    if (fpsAccum >= 0.5) {
        fps = Math.round(frameCount / fpsAccum);
        frameCount = 0;
        fpsAccum = 0;
    }

    // Camera movement
    var moveSpeed = cam.speed * dt;
    if (keys['shift']) moveSpeed *= 3;
    var fwd = camForward();
    var right = camRight();
    if (keys['w']) cam.pos = v3add(cam.pos, v3scale(fwd, moveSpeed));
    if (keys['s']) cam.pos = v3add(cam.pos, v3scale(fwd, -moveSpeed));
    if (keys['a']) cam.pos = v3add(cam.pos, v3scale(right, -moveSpeed));
    if (keys['d']) cam.pos = v3add(cam.pos, v3scale(right, moveSpeed));
    if (keys[' ']) cam.pos[1] += moveSpeed;
    if (keys['control']) cam.pos[1] -= moveSpeed;

    // Read canvas size every frame so the aspect ratio tracks window
    // resizes — caching at startup gave wrong proportions whenever the
    // window wasn't the size we were measured at.
    var w = canvas.clientWidth || 1;
    var h = canvas.clientHeight || 1;

    var target = v3add(cam.pos, fwd);
    scene.setCamera({
        fov: 65,
        aspect: w / h,
        near: 0.5,
        far: 500,
        position: cam.pos,
        target: target
    });

    info.textContent =
        'pos ' + cam.pos[0].toFixed(1) + ', ' + cam.pos[1].toFixed(1) +
        ', ' + cam.pos[2].toFixed(1) +
        ' | fps ' + fps +
        ' | tris ' + lastStats.tris +
        ' | edit ' + lastStats.ms + 'ms' +
        ' | place: ' + activePlaceMatName() +
        ' | seed ' + lastStats.seed;

    requestAnimationFrame(render);
}

requestAnimationFrame(render);
