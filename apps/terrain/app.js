// =============================================================================
// Terrain — Infinite blocky voxel world
// =============================================================================
//
// All heavy lifting (noise, voxel grids, greedy meshing, chunk lifecycle)
// runs in C++ via the TerrainManager. JS just configures, drives the camera,
// and handles input.
// =============================================================================

var canvas = document.getElementById('c');
var scene = canvas.getContext('scene');
var info = document.getElementById('info');

// ----------------------------------------------------------------------------
// Terrain
// ----------------------------------------------------------------------------

var terrain = scene.createTerrain({
    chunkSize: [64, 48, 64],
    cellSize: 1.0,
    loadRadius: 4,
    unloadRadius: 6,
    maxLoadsPerUpdate: 2,
    seed: 1337,
    noise: { frequency: 0.035, octaves: 5, gain: 0.5, lacunarity: 2.0 },
    baseHeight: 18,
    heightAmplitude: 16,
    seaLevel: 14,
    palette: [
        0, 0, 0, 0,                    // 0: air
        0.42, 0.70, 0.27, 1.0,         // 1: grass
        0.52, 0.34, 0.18, 1.0,         // 2: dirt
        0.55, 0.55, 0.58, 1.0,         // 3: stone
        0.18, 0.18, 0.20, 1.0,         // 4: bedrock
        0.86, 0.78, 0.49, 1.0,         // 5: sand
    ]
});

// ----------------------------------------------------------------------------
// Camera (FPS-style fly)
// ----------------------------------------------------------------------------

function v3add(a, b)   { return [a[0]+b[0], a[1]+b[1], a[2]+b[2]]; }
function v3scale(a, s) { return [a[0]*s, a[1]*s, a[2]*s]; }

var cam = {
    pos: [50, 42, 50],
    yaw: -Math.PI / 4,
    pitch: -0.30,
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

// Material cycling for placement
var MATERIALS = ['grass', 'dirt', 'stone', 'bedrock', 'sand'];
var placeMatIndex = 2;   // stone
function activePlaceMat()     { return placeMatIndex + 1; }  // +1 because 0=air
function activePlaceMatName() { return MATERIALS[placeMatIndex]; }

document.addEventListener('keydown', function(e) {
    keys[e.key.toLowerCase()] = true;
    if (e.key === 'r' || e.key === 'R') {
        seed = (seed + 1) | 0;
        terrain.configure({
            chunkSize: [64, 48, 64],
            cellSize: 1.0,
            loadRadius: 4,
            unloadRadius: 6,
            maxLoadsPerUpdate: 2,
            seed: seed,
            noise: { frequency: 0.035, octaves: 5, gain: 0.5, lacunarity: 2.0 },
            baseHeight: 18,
            heightAmplitude: 16,
            seaLevel: 14,
            palette: [
                0,0,0,0,
                0.42,0.70,0.27,1.0,
                0.52,0.34,0.18,1.0,
                0.55,0.55,0.58,1.0,
                0.18,0.18,0.20,1.0,
                0.86,0.78,0.49,1.0,
            ]
        });
    }
    if (e.key === '[') placeMatIndex = (placeMatIndex - 1 + MATERIALS.length) % MATERIALS.length;
    if (e.key === ']') placeMatIndex = (placeMatIndex + 1) % MATERIALS.length;
    if (e.key === 'Escape') mouseCaptured = false;
});
document.addEventListener('keyup', function(e) {
    keys[e.key.toLowerCase()] = false;
});

canvas.addEventListener('mousedown', function(e) {
    if (!mouseCaptured) { mouseCaptured = true; return; }
    if (e.button === 0) pickAndEdit('mine');
    else if (e.button === 2) pickAndEdit('place', activePlaceMat());
});
canvas.addEventListener('contextmenu', function(e) { e.preventDefault(); });

document.addEventListener('mousemove', function(e) {
    if (!mouseCaptured) return;
    cam.yaw += e.movementX * cam.sensitivity;
    cam.pitch -= e.movementY * cam.sensitivity;
    if (cam.pitch >  1.4) cam.pitch =  1.4;
    if (cam.pitch < -1.4) cam.pitch = -1.4;
});

// ----------------------------------------------------------------------------
// Block picking
// ----------------------------------------------------------------------------

function pickAndEdit(action, placeMat) {
    var fwd = camForward();
    var hit = terrain.raycast(cam.pos, fwd, 200);
    if (!hit) return;

    var p = hit.position;
    var n = hit.normal;

    if (action === 'mine') {
        terrain.setVoxel(p[0] - n[0]*0.5, p[1] - n[1]*0.5, p[2] - n[2]*0.5, 0);
    } else {
        terrain.setVoxel(p[0] + n[0]*0.5, p[1] + n[1]*0.5, p[2] + n[2]*0.5, placeMat);
    }
    terrain.rebuild();
}

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

    // Drive chunk loading/unloading
    terrain.update(cam.pos[0], cam.pos[1], cam.pos[2]);

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
        ' | chunks ' + terrain.chunkCount +
        ' | tris ' + terrain.triangleCount +
        ' | place: ' + activePlaceMatName() +
        ' | seed ' + seed;

    requestAnimationFrame(render);
}

requestAnimationFrame(render);
