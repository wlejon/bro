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
var W = canvas.clientWidth, H = canvas.clientHeight;

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

// Mask the voxel grid down to a single material (others → 0) so greedyMesh
// produces only that material's faces. Returns a fresh Uint8Array.
function maskMaterial(voxels, matId) {
    var out = new Uint8Array(voxels.length);
    for (var i = 0; i < voxels.length; i++) {
        if (voxels[i] === matId) out[i] = matId;
    }
    return out;
}

// ----------------------------------------------------------------------------
// Build the world from a seed
// ----------------------------------------------------------------------------

var terrainNodes = [];
var lastStats = { tris: 0, verts: 0, voxels: 0, materials: 0 };

function clearTerrain() {
    for (var i = 0; i < terrainNodes.length; i++) terrainNodes[i].destroy();
    terrainNodes = [];
}

function buildWorld(seed) {
    var t0 = Date.now();
    clearTerrain();

    var heights = buildHeightmap(seed);
    var voxels = buildVoxelGrid(heights);

    var totalTris = 0, totalVerts = 0, totalVoxels = 0;
    for (var i = 0; i < voxels.length; i++) if (voxels[i] !== 0) totalVoxels++;

    var matCount = 0;
    for (var m = 0; m < MATERIALS.length; m++) {
        var mat = MATERIALS[m];
        var masked = maskMaterial(voxels, mat.id);
        var mesh = Mesh.greedyMesh(masked, CHUNK_W, CHUNK_H, CHUNK_D, 1.0);
        if (mesh.triangleCount === 0) continue;

        totalTris  += mesh.triangleCount;
        totalVerts += mesh.vertexCount;
        matCount++;

        var node = scene.createMesh({
            mesh: mesh,
            transfer: true,
            // Center the chunk horizontally on the origin so the camera
            // start position frames it nicely.
            x: -CHUNK_W * 0.5,
            y: 0,
            z: -CHUNK_D * 0.5,
            color: mat.color,
            name: 'terrain-' + mat.name
        });
        terrainNodes.push(node);
    }

    lastStats = {
        tris: totalTris,
        verts: totalVerts,
        voxels: totalVoxels,
        materials: matCount,
        seed: seed,
        ms: Date.now() - t0
    };

    console.log('terrain: seed=' + seed +
                ' voxels=' + totalVoxels +
                ' tris=' + totalTris +
                ' verts=' + totalVerts +
                ' materials=' + matCount +
                ' build=' + lastStats.ms + 'ms');
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

document.addEventListener('keydown', function(e) {
    keys[e.key.toLowerCase()] = true;
    if (e.key === 'r' || e.key === 'R') {
        seed = (seed + 1) | 0;
        buildWorld(seed);
    }
    if (e.key === 'Escape') mouseCaptured = false;
});
document.addEventListener('keyup', function(e) {
    keys[e.key.toLowerCase()] = false;
});

canvas.addEventListener('click', function() { mouseCaptured = true; });

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

    var target = v3add(cam.pos, fwd);
    scene.setCamera({
        fov: 65,
        aspect: W / H,
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
        ' | verts ' + lastStats.verts +
        ' | seed ' + lastStats.seed;

    requestAnimationFrame(render);
}

requestAnimationFrame(render);
