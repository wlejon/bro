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
    loadRadius: 8,
    unloadRadius: 12,
    maxLoadsPerUpdate: 4,
    seed: 220,
    noise: { frequency: 0.000125, octaves: 8, gain: 0.6, lacunarity: 2.5 },
    baseHeight: 8000,
    heightAmplitude: 40000,
    seaLevel: 1600,
    meshMode: 2,        // terraced
    terraceStep: 16.0,
    // Continental noise: large-scale mountain range / plains variation
    continentFrequency: 0.0000125,
    continentMin: 0.2,      // plains: 20% of heightAmplitude (8000 units)
    continentMax: 1.8,      // mountain ranges: 180% of heightAmplitude (72000 units)
    // Enormous mountain pass — ridged noise for massive peaks
    mountainFrequency: 0.000005,
    mountainAmplitude: 500000,
    mountainOctaves: 4,
    // LOD rings: 8 levels, each 4x coarser, with Earth curvature
    // LOD0: 64m chunks, LOD7: ~1M unit chunks → ~3M unit view range
    lodLevels: 10,
    lodScaleFactor: 4,
    planetRadius: 6371000,
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
// Camera (6DOF quaternion-based)
// ============================================================================

function v3add(a, b)   { return [a[0]+b[0], a[1]+b[1], a[2]+b[2]]; }
function v3scale(a, s) { return [a[0]*s, a[1]*s, a[2]*s]; }
function v3len(a)      { return Math.sqrt(a[0]*a[0] + a[1]*a[1] + a[2]*a[2]); }

// Quaternion helpers: [x, y, z, w]
function quat()          { return [0, 0, 0, 1]; }
function quatFromAxis(ax, ay, az, angle) {
    var s = Math.sin(angle * 0.5), c = Math.cos(angle * 0.5);
    return [ax * s, ay * s, az * s, c];
}
function quatMul(a, b) {
    return [
        a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1],
        a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0],
        a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3],
        a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2]
    ];
}
function quatNorm(q) {
    var len = Math.sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    if (len < 1e-12) return [0, 0, 0, 1];
    return [q[0]/len, q[1]/len, q[2]/len, q[3]/len];
}
function quatRotVec(q, v) {
    // q * [v,0] * q^-1
    var x = q[0], y = q[1], z = q[2], w = q[3];
    var vx = v[0], vy = v[1], vz = v[2];
    // t = 2 * cross(q.xyz, v)
    var tx = 2 * (y*vz - z*vy);
    var ty = 2 * (z*vx - x*vz);
    var tz = 2 * (x*vy - y*vx);
    return [
        vx + w*tx + (y*tz - z*ty),
        vy + w*ty + (z*tx - x*tz),
        vz + w*tz + (x*ty - y*tx)
    ];
}

var cam = {
    pos: [50, 12000, 50],
    rot: quatFromAxis(0, 1, 0, -Math.PI / 4), // initial yaw
    baseSpeed: 24,
    sensitivity: 0.003,
    fov: 65,
};
// Apply initial pitch
cam.rot = quatNorm(quatMul(cam.rot, quatFromAxis(1, 0, 0, -0.30)));

function camForward() { return quatRotVec(cam.rot, [0, 0, -1]); }
function camRight()   { return quatRotVec(cam.rot, [1, 0, 0]); }
function camUp()      { return quatRotVec(cam.rot, [0, 1, 0]); }

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

var rightMouseDown = false;

canvas.addEventListener('mousedown', function(e) {
    if (e.button === 2) {
        rightMouseDown = true;
        canvas.requestPointerLock();
    }
});
document.addEventListener('mouseup', function(e) {
    if (e.button === 2) {
        rightMouseDown = false;
        document.exitPointerLock();
    }
});
canvas.addEventListener('contextmenu', function(e) { e.preventDefault(); });

document.addEventListener('mousemove', function(e) {
    if (!rightMouseDown) return;
    var dx = e.movementX * cam.sensitivity;
    var dy = e.movementY * cam.sensitivity;
    var yawQ  = quatFromAxis(0, 1, 0, -dx);
    var right = camRight();
    var pitchQ = quatFromAxis(right[0], right[1], right[2], -dy);
    cam.rot = quatNorm(quatMul(pitchQ, quatMul(yawQ, cam.rot)));
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

    // Camera movement — 6DOF, speed scales with altitude
    var altitude = Math.max(cam.pos[1], 1);
    var moveSpeed = (cam.baseSpeed + altitude * 0.5) * dt;
    if (keys['shift']) moveSpeed *= 3;
    var fwd   = camForward();
    var right = camRight();
    var up    = camUp();
    if (keys['w']) cam.pos = v3add(cam.pos, v3scale(fwd, moveSpeed));
    if (keys['s']) cam.pos = v3add(cam.pos, v3scale(fwd, -moveSpeed));
    if (keys['a']) cam.pos = v3add(cam.pos, v3scale(right, -moveSpeed));
    if (keys['d']) cam.pos = v3add(cam.pos, v3scale(right, moveSpeed));
    if (keys[' ']) cam.pos = v3add(cam.pos, v3scale(up, moveSpeed));
    if (keys['control']) cam.pos = v3add(cam.pos, v3scale(up, -moveSpeed));
    // Roll
    if (keys['q']) cam.rot = quatNorm(quatMul(quatFromAxis(fwd[0], fwd[1], fwd[2], 2.0 * dt), cam.rot));
    if (keys['e']) cam.rot = quatNorm(quatMul(quatFromAxis(fwd[0], fwd[1], fwd[2], -2.0 * dt), cam.rot));

    terrain.update(cam.pos[0], cam.pos[1], cam.pos[2]);

    // Dynamic near/far planes based on altitude
    var w = canvas.clientWidth || 1;
    var h = canvas.clientHeight || 1;
    var target = v3add(cam.pos, fwd);
    var farDist = terrain.farDistance || 100000;
    var nearPlane = Math.max(0.5, Math.min(altitude * 0.01, 1000));
    scene.setCamera({
        fov: cam.fov,
        aspect: w / h,
        near: nearPlane,
        far: farDist * 1.1,
        position: cam.pos,
        target: target,
        up: up,
    });

    // Format altitude for readability
    var alt = cam.pos[1];
    var altStr = alt < 1000 ? alt.toFixed(0) + 'm'
               : alt < 1000000 ? (alt / 1000).toFixed(1) + 'km'
               : (alt / 1000000).toFixed(2) + 'Mm';

    info.textContent =
        'alt ' + altStr +
        ' | fps ' + fps +
        ' | chunks ' + terrain.chunkCount +
        ' | tris ' + terrain.triangleCount;

    requestAnimationFrame(render);
}

requestAnimationFrame(render);
