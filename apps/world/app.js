// =============================================================================
// World — Explorable terraced terrain
// =============================================================================

var canvas = document.getElementById('c');
var scene  = canvas.getContext('scene');
var info   = document.getElementById('info');
var status = document.getElementById('status');

// ============================================================================
// Terrain
// ============================================================================

var terrain = scene.createTerrain({
    chunkSize: [64, 260, 64],
    cellSize: 1.0,
    loadRadius: 10,
    unloadRadius: 14,
    maxLoadsPerUpdate: 2,
    seed: 220,
    noise: { frequency: 0.150, octaves: 10, gain: 0.71, lacunarity: 1.50 },
    baseHeight: 1,
    heightAmplitude: 128,
    seaLevel: 6,
    meshMode: 2,        // terraced
    terraceStep: 0.25,
    palette: [
        0, 0, 0, 0,                    // 0: air
        0.42, 0.70, 0.27, 1.0,         // 1: grass
        0.52, 0.34, 0.18, 1.0,         // 2: dirt
        0.55, 0.55, 0.58, 1.0,         // 3: stone
        0.18, 0.18, 0.20, 1.0,         // 4: bedrock
        0.86, 0.78, 0.49, 1.0,         // 5: sand
    ]
});

// ============================================================================
// Camera (FPS-style fly)
// ============================================================================

function v3add(a, b)   { return [a[0]+b[0], a[1]+b[1], a[2]+b[2]]; }
function v3scale(a, s) { return [a[0]*s, a[1]*s, a[2]*s]; }

var cam = {
    pos: [50, 80, 50],
    yaw: -Math.PI / 4,
    pitch: -0.30,
    speed: 24,
    sensitivity: 0.003,
    fov: 65,
    far: 1200,
};

function camForward() {
    var cy = Math.cos(cam.yaw), sy = Math.sin(cam.yaw);
    var cp = Math.cos(cam.pitch), sp = Math.sin(cam.pitch);
    return [sy * cp, sp, -cy * cp];
}
function camRight() {
    return [Math.cos(cam.yaw), 0, Math.sin(cam.yaw)];
}

// ============================================================================
// Input
// ============================================================================

var keys = {};
var mouseCaptured = false;

document.addEventListener('keydown', function(e) {
    keys[e.key.toLowerCase()] = true;
    if (e.key === 'Escape') mouseCaptured = false;
});
document.addEventListener('keyup', function(e) {
    keys[e.key.toLowerCase()] = false;
});

canvas.addEventListener('mousedown', function(e) {
    if (!mouseCaptured) { mouseCaptured = true; return; }
});
canvas.addEventListener('contextmenu', function(e) { e.preventDefault(); });

document.addEventListener('mousemove', function(e) {
    if (!mouseCaptured) return;
    cam.yaw += e.movementX * cam.sensitivity;
    cam.pitch -= e.movementY * cam.sensitivity;
    if (cam.pitch >  1.4) cam.pitch =  1.4;
    if (cam.pitch < -1.4) cam.pitch = -1.4;
});

// ============================================================================
// Render loop
// ============================================================================

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

    terrain.update(cam.pos[0], cam.pos[1], cam.pos[2]);

    var w = canvas.clientWidth || 1;
    var h = canvas.clientHeight || 1;
    var target = v3add(cam.pos, fwd);
    scene.setCamera({
        fov: cam.fov,
        aspect: w / h,
        near: 0.5,
        far: cam.far,
        position: cam.pos,
        target: target,
    });

    info.textContent =
        'pos ' + cam.pos[0].toFixed(1) + ', ' + cam.pos[1].toFixed(1) +
        ', ' + cam.pos[2].toFixed(1) +
        ' | fps ' + fps +
        ' | chunks ' + terrain.chunkCount +
        ' | tris ' + terrain.triangleCount;

    requestAnimationFrame(render);
}

requestAnimationFrame(render);
