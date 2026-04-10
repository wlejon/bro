// =============================================================================
// World — Explorable terraced terrain
// =============================================================================

var canvas = document.getElementById('c');
var scene  = canvas.getContext('scene');
var info   = document.getElementById('info');
var status = document.getElementById('status');

// ============================================================================
// Planets
// ============================================================================

var EARTH_PALETTE = [
    0, 0, 0, 0,                    // 0: air
    0.42, 0.70, 0.27, 1.0,        // 1: grass
    0.52, 0.34, 0.18, 1.0,        // 2: dirt
    0.55, 0.55, 0.58, 1.0,        // 3: stone
    0.18, 0.18, 0.20, 1.0,        // 4: bedrock
    0.86, 0.78, 0.49, 1.0,        // 5: sand
];

var MARS_PALETTE = [
    0, 0, 0, 0,                    // 0: air
    0.76, 0.40, 0.22, 1.0,        // 1: rust soil
    0.60, 0.30, 0.15, 1.0,        // 2: dark regolith
    0.50, 0.35, 0.30, 1.0,        // 3: basalt
    0.30, 0.20, 0.15, 1.0,        // 4: bedrock
    0.82, 0.65, 0.40, 1.0,        // 5: dust
];

var ICE_PALETTE = [
    0, 0, 0, 0,                    // 0: air
    0.85, 0.92, 0.96, 1.0,        // 1: ice
    0.70, 0.82, 0.90, 1.0,        // 2: packed ice
    0.45, 0.55, 0.65, 1.0,        // 3: deep ice
    0.25, 0.30, 0.40, 1.0,        // 4: bedrock
    0.75, 0.85, 0.95, 1.0,        // 5: frost
];

// Planet spacing: ~40M units apart (several planet diameters)
var planets = [];

// Planet 1: Earth-like (at origin)
planets.push(scene.createTerrain({
    origin: [0, 0, 0],
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
    meshMode: 2,
    terraceStep: 16.0,
    continentFrequency: 0.0000125,
    continentMin: 0.2,
    continentMax: 1.8,
    mountainFrequency: 0.000005,
    mountainAmplitude: 500000,
    mountainOctaves: 4,
    lodLevels: 10,
    lodScaleFactor: 4,
    planetRadius: 6371000,
    palette: EARTH_PALETTE,
}));

// Planet 2: Mars-like (smaller, rougher, to the "east")
planets.push(scene.createTerrain({
    origin: [40000000, 0, 0],
    chunkSize: [64, 260, 64],
    cellSize: 1.0,
    loadRadius: 8,
    unloadRadius: 12,
    maxLoadsPerUpdate: 3,
    seed: 7742,
    noise: { frequency: 0.00018, octaves: 6, gain: 0.55, lacunarity: 2.2 },
    baseHeight: 5000,
    heightAmplitude: 60000,
    seaLevel: 0,
    meshMode: 2,
    terraceStep: 24.0,
    continentFrequency: 0.000015,
    continentMin: 0.3,
    continentMax: 2.2,
    mountainFrequency: 0.000003,
    mountainAmplitude: 800000,
    mountainOctaves: 5,
    lodLevels: 10,
    lodScaleFactor: 4,
    planetRadius: 3389500,
    palette: MARS_PALETTE,
}));

// Planet 3: Ice world (medium, smooth, to the "north")
planets.push(scene.createTerrain({
    origin: [0, 0, -40000000],
    chunkSize: [64, 260, 64],
    cellSize: 1.0,
    loadRadius: 8,
    unloadRadius: 12,
    maxLoadsPerUpdate: 3,
    seed: 31415,
    noise: { frequency: 0.0001, octaves: 10, gain: 0.45, lacunarity: 2.0 },
    baseHeight: 3000,
    heightAmplitude: 15000,
    seaLevel: 2000,
    meshMode: 2,
    terraceStep: 10.0,
    continentFrequency: 0.00001,
    continentMin: 0.5,
    continentMax: 1.2,
    mountainFrequency: 0.000008,
    mountainAmplitude: 200000,
    mountainOctaves: 3,
    lodLevels: 10,
    lodScaleFactor: 4,
    planetRadius: 5200000,
    palette: ICE_PALETTE,
}));

// ============================================================================
// Camera (6DOF quaternion-based with smooth velocity controls)
// ============================================================================

function v3add(a, b)   { return [a[0]+b[0], a[1]+b[1], a[2]+b[2]]; }
function v3scale(a, s) { return [a[0]*s, a[1]*s, a[2]*s]; }
function v3len(a)      { return Math.sqrt(a[0]*a[0] + a[1]*a[1] + a[2]*a[2]); }

// Quaternion helpers: [x, y, z, w]
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
    var x = q[0], y = q[1], z = q[2], w = q[3];
    var vx = v[0], vy = v[1], vz = v[2];
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
    rot: quatFromAxis(0, 1, 0, -Math.PI / 4),
    vel: [0, 0, 0],          // velocity in world space
    angVel: [0, 0, 0],       // angular velocity: [pitch, yaw, roll] rad/s
    baseSpeed: 24,
    sensitivity: 0.002,
    fov: 65,
    accel: 12.0,              // acceleration multiplier
    damping: 6.0,             // velocity damping factor (higher = snappier stop)
    angDamping: 12.0,         // angular velocity damping
    rollSpeed: 2.5,           // roll rate rad/s
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
var mouseAccumX = 0, mouseAccumY = 0;

document.addEventListener('keydown', function(e) {
    keys[e.key.toLowerCase()] = true;
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

// Accumulate mouse deltas — applied smoothly during update
document.addEventListener('mousemove', function(e) {
    if (!rightMouseDown) return;
    mouseAccumX += e.movementX;
    mouseAccumY += e.movementY;
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

    // --- Mouse look: apply accumulated deltas as angular velocity ---
    var dx = mouseAccumX * cam.sensitivity;
    var dy = mouseAccumY * cam.sensitivity;
    mouseAccumX = 0;
    mouseAccumY = 0;

    if (dx !== 0 || dy !== 0) {
        // Yaw around world Y, pitch around camera's local right axis
        var yawQ   = quatFromAxis(0, 1, 0, -dx);
        var right  = camRight();
        var pitchQ = quatFromAxis(right[0], right[1], right[2], -dy);
        cam.rot = quatNorm(quatMul(pitchQ, quatMul(yawQ, cam.rot)));
    }

    // --- Roll (smooth via angular velocity) ---
    var rollInput = 0;
    if (keys['q']) rollInput += 1;
    if (keys['e']) rollInput -= 1;
    if (rollInput !== 0) {
        var fwd = camForward();
        var rollQ = quatFromAxis(fwd[0], fwd[1], fwd[2], rollInput * cam.rollSpeed * dt);
        cam.rot = quatNorm(quatMul(rollQ, cam.rot));
    }

    // --- Velocity-based movement (smooth acceleration + damping) ---
    // Find nearest planet and compute altitude from its surface
    // Planet sphere center is at (origin.x, origin.y - planetRadius, origin.z)
    var altitude = 1e15;
    var nearestPlanet = 0;
    for (var pi = 0; pi < planets.length; pi++) {
        var o = planets[pi].origin;
        var R = planets[pi].planetRadius || 1000000;
        var dx2 = cam.pos[0] - o[0];
        var dy2 = cam.pos[1] - (o[1] - R);
        var dz2 = cam.pos[2] - o[2];
        var dist = Math.sqrt(dx2*dx2 + dy2*dy2 + dz2*dz2);
        var alt = dist - R;
        if (alt < altitude) {
            altitude = alt;
            nearestPlanet = pi;
        }
    }
    altitude = Math.max(altitude, 1);
    var maxSpeed = (cam.baseSpeed + altitude * 0.5);
    if (keys['shift']) maxSpeed *= 3;

    var fwd   = camForward();
    var right = camRight();
    var up    = camUp();

    // Build desired thrust direction in world space
    var thrustX = 0, thrustY = 0, thrustZ = 0;
    if (keys['w']) { thrustX += fwd[0];   thrustY += fwd[1];   thrustZ += fwd[2]; }
    if (keys['s']) { thrustX -= fwd[0];   thrustY -= fwd[1];   thrustZ -= fwd[2]; }
    if (keys['a']) { thrustX -= right[0]; thrustY -= right[1]; thrustZ -= right[2]; }
    if (keys['d']) { thrustX += right[0]; thrustY += right[1]; thrustZ += right[2]; }
    if (keys[' '])       { thrustX += up[0]; thrustY += up[1]; thrustZ += up[2]; }
    if (keys['control']) { thrustX -= up[0]; thrustY -= up[1]; thrustZ -= up[2]; }

    // Normalize thrust direction
    var thrustLen = Math.sqrt(thrustX*thrustX + thrustY*thrustY + thrustZ*thrustZ);
    if (thrustLen > 1e-6) {
        var inv = 1.0 / thrustLen;
        thrustX *= inv; thrustY *= inv; thrustZ *= inv;
    }

    // Accelerate towards target velocity, damp when no input
    var targetVelX = thrustX * maxSpeed;
    var targetVelY = thrustY * maxSpeed;
    var targetVelZ = thrustZ * maxSpeed;

    var blend = 1.0 - Math.exp(-cam.accel * dt);
    var dampBlend = 1.0 - Math.exp(-cam.damping * dt);

    if (thrustLen > 1e-6) {
        cam.vel[0] += (targetVelX - cam.vel[0]) * blend;
        cam.vel[1] += (targetVelY - cam.vel[1]) * blend;
        cam.vel[2] += (targetVelZ - cam.vel[2]) * blend;
    } else {
        cam.vel[0] *= (1.0 - dampBlend);
        cam.vel[1] *= (1.0 - dampBlend);
        cam.vel[2] *= (1.0 - dampBlend);
    }

    // Integrate position
    cam.pos[0] += cam.vel[0] * dt;
    cam.pos[1] += cam.vel[1] * dt;
    cam.pos[2] += cam.vel[2] * dt;

    // Update all planet terrains
    var totalChunks = 0, totalTris = 0;
    var farDist = 100000;
    for (var pi = 0; pi < planets.length; pi++) {
        planets[pi].update(cam.pos[0], cam.pos[1], cam.pos[2]);
        totalChunks += planets[pi].chunkCount;
        totalTris += planets[pi].triangleCount;
        var fd = planets[pi].farDistance || 100000;
        if (fd > farDist) farDist = fd;
    }

    // Dynamic near/far planes based on altitude
    var w = canvas.clientWidth || 1;
    var h = canvas.clientHeight || 1;
    // In deep space (far from any surface), extend far plane to reach distant planets
    var nearPlane = Math.max(0.5, Math.min(altitude * 0.01, 1000));
    if (altitude > 1000000) farDist = Math.max(farDist, 100000000);

    // Pass quaternion directly — avoids lookAt precision loss at large coordinates
    scene.setCamera({
        fov: cam.fov,
        aspect: w / h,
        near: nearPlane,
        far: farDist * 1.1,
        position: cam.pos,
        quaternion: cam.rot,
    });

    // Format altitude for readability
    var alt = altitude;
    var altStr = alt < 1000 ? alt.toFixed(0) + 'm'
               : alt < 1000000 ? (alt / 1000).toFixed(1) + 'km'
               : (alt / 1000000).toFixed(2) + 'Mm';
    var planetNames = ['earth', 'mars', 'ice'];

    info.textContent =
        planetNames[nearestPlanet] + ' alt ' + altStr +
        ' | fps ' + fps +
        ' | chunks ' + totalChunks +
        ' | tris ' + totalTris;

    requestAnimationFrame(render);
}

requestAnimationFrame(render);
