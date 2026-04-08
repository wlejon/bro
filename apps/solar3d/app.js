// Solar System 3D — Scene Graph + FastNoise terrain + broaudio spatial audio
// WASD fly-through with procedurally generated planets that sing

var canvas = document.getElementById('c');
var scene = canvas.getContext('scene');
var info = document.getElementById('info');
var W = 1024, H = 768;

// ============================================================================
// 3D Math helpers (for camera control only — rendering uses scene graph)
// ============================================================================

function v3add(a, b) { return [a[0]+b[0], a[1]+b[1], a[2]+b[2]]; }
function v3sub(a, b) { return [a[0]-b[0], a[1]-b[1], a[2]-b[2]]; }
function v3scale(a, s) { return [a[0]*s, a[1]*s, a[2]*s]; }
function v3len(a) { return Math.sqrt(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]); }
function v3norm(a) { var l = v3len(a); return l > 0 ? v3scale(a, 1/l) : [0,0,0]; }
function v3cross(a, b) {
    return [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]];
}

// ============================================================================
// Noise-displaced sphere generator → raw vertex arrays for scene.createMesh
// ============================================================================

function generateNoiseSphere(radius, segs, rings, noiseAmp, noiseFreq, seed) {
    var positions = [];
    var normals = [];
    var indices = [];

    // Set up noise generator
    var noise = null;
    if (noiseAmp > 0) {
        var simplex = FastNoise.Simplex();
        noise = FastNoise.FractalFBm();
        noise.set('Source', simplex);
        noise.set('Octaves', 4);
        noise.set('Gain', 0.5);
        noise.set('Lacunarity', 2.0);
    }

    for (var ring = 0; ring <= rings; ring++) {
        var phi = Math.PI * ring / rings;
        var sp = Math.sin(phi), cp = Math.cos(phi);

        for (var seg = 0; seg <= segs; seg++) {
            var theta = 2 * Math.PI * seg / segs;
            var st = Math.sin(theta), ct = Math.cos(theta);

            var nx = sp * ct, ny = cp, nz = sp * st;

            var displacement = 0;
            if (noise) {
                displacement = noise.genSingle3D(
                    nx * noiseFreq, ny * noiseFreq, nz * noiseFreq, seed
                ) * noiseAmp;
            }

            var r = radius + displacement;
            positions.push(nx * r, ny * r, nz * r);
            normals.push(nx, ny, nz); // placeholder — recomputed below
        }
    }

    for (var ring = 0; ring < rings; ring++) {
        for (var seg = 0; seg < segs; seg++) {
            var a = ring * (segs + 1) + seg;
            var b = a + segs + 1;
            indices.push(a, b, a + 1, b, b + 1, a + 1);
        }
    }

    // Recompute normals from faces for terrain
    if (noiseAmp > 0) {
        var nn = new Float32Array(normals.length);
        for (var i = 0; i < indices.length; i += 3) {
            var i0 = indices[i]*3, i1 = indices[i+1]*3, i2 = indices[i+2]*3;
            var v0 = [positions[i0], positions[i0+1], positions[i0+2]];
            var v1 = [positions[i1], positions[i1+1], positions[i1+2]];
            var v2 = [positions[i2], positions[i2+1], positions[i2+2]];
            var e1 = v3sub(v1, v0);
            var e2 = v3sub(v2, v0);
            var fn = v3norm(v3cross(e1, e2));
            for (var j = 0; j < 3; j++) {
                var idx = indices[i + j] * 3;
                nn[idx] += fn[0]; nn[idx+1] += fn[1]; nn[idx+2] += fn[2];
            }
        }
        for (var i = 0; i < nn.length; i += 3) {
            var l = Math.sqrt(nn[i]*nn[i] + nn[i+1]*nn[i+1] + nn[i+2]*nn[i+2]);
            if (l > 0) { nn[i] /= l; nn[i+1] /= l; nn[i+2] /= l; }
        }
        normals = nn;
    }

    return {
        positions: new Float32Array(positions),
        normals: normals instanceof Float32Array ? normals : new Float32Array(normals),
        indices: new Uint32Array(indices)
    };
}

// ============================================================================
// Celestial Bodies
// ============================================================================

var BODIES = [
    { name: 'Sun',      r: 8.0,  orbit: 0,   speed: 0,     col: [1.0, 0.85, 0.3],  noise: 0.3, nfreq: 3,  seed: 1,    emissive: true },
    { name: 'Mercury',  r: 1.0,  orbit: 20,  speed: 1.6,   col: [0.7, 0.65, 0.6],  noise: 0.15, nfreq: 5, seed: 10 },
    { name: 'Venus',    r: 1.8,  orbit: 30,  speed: 1.2,   col: [0.9, 0.75, 0.4],  noise: 0.1, nfreq: 4,  seed: 20 },
    { name: 'Earth',    r: 2.0,  orbit: 45,  speed: 1.0,   col: [0.2, 0.5, 0.8],   noise: 0.25, nfreq: 5, seed: 42 },
    { name: 'Mars',     r: 1.4,  orbit: 60,  speed: 0.8,   col: [0.85, 0.35, 0.2], noise: 0.3, nfreq: 4,  seed: 50 },
    { name: 'Jupiter',  r: 5.0,  orbit: 90,  speed: 0.4,   col: [0.85, 0.7, 0.5],  noise: 0.2, nfreq: 3,  seed: 60 },
    { name: 'Saturn',   r: 4.0,  orbit: 120, speed: 0.3,   col: [0.9, 0.8, 0.6],   noise: 0.15, nfreq: 3, seed: 70 },
    { name: 'Neptune',  r: 3.0,  orbit: 150, speed: 0.2,   col: [0.3, 0.4, 0.9],   noise: 0.2, nfreq: 4,  seed: 80 },
];

var MOONS = [
    { parent: 3, name: 'Moon',      r: 0.6, orbit: 5,  speed: 3.0, col: [0.75, 0.73, 0.7], noise: 0.1, nfreq: 6, seed: 100 },
    { parent: 5, name: 'Io',        r: 0.7, orbit: 8,  speed: 2.5, col: [0.9, 0.85, 0.3],  noise: 0.15, nfreq: 5, seed: 110 },
    { parent: 5, name: 'Europa',    r: 0.6, orbit: 10, speed: 2.0, col: [0.8, 0.82, 0.85], noise: 0.05, nfreq: 4, seed: 120 },
    { parent: 5, name: 'Ganymede',  r: 0.9, orbit: 13, speed: 1.5, col: [0.6, 0.55, 0.5],  noise: 0.12, nfreq: 5, seed: 130 },
    { parent: 5, name: 'Callisto',  r: 0.8, orbit: 16, speed: 1.2, col: [0.5, 0.48, 0.45], noise: 0.1, nfreq: 4, seed: 140 },
    { parent: 6, name: 'Titan',     r: 1.0, orbit: 9,  speed: 1.8, col: [0.8, 0.6, 0.3],   noise: 0.15, nfreq: 4, seed: 150 },
    { parent: 7, name: 'Triton',    r: 0.7, orbit: 7,  speed: 2.2, col: [0.6, 0.7, 0.85],  noise: 0.1, nfreq: 5, seed: 160 },
];

// ============================================================================
// Create scene graph nodes for each body
// ============================================================================

console.log('Generating planet meshes...');

for (var i = 0; i < BODIES.length; i++) {
    var b = BODIES[i];
    var segs = b.r > 3 ? 32 : 24;
    var rings = b.r > 3 ? 24 : 16;
    var meshData = generateNoiseSphere(b.r, segs, rings, b.noise, b.nfreq, b.seed);

    b.node = scene.createMesh({
        name: b.name,
        positions: meshData.positions,
        normals: meshData.normals,
        indices: meshData.indices,
        color: b.col,
        emissive: b.emissive ? 1.0 : 0.0
    });
    b.angle = Math.random() * Math.PI * 2;
    b.pos = [0, 0, 0];
}

for (var i = 0; i < MOONS.length; i++) {
    var m = MOONS[i];
    var meshData = generateNoiseSphere(m.r, 16, 12, m.noise, m.nfreq, m.seed);

    m.node = scene.createMesh({
        name: m.name,
        positions: meshData.positions,
        normals: meshData.normals,
        indices: meshData.indices,
        color: m.col
    });
    m.angle = Math.random() * Math.PI * 2;
    m.pos = [0, 0, 0];
}

console.log('Meshes generated');

// ============================================================================
// Camera (FPS-style)
// ============================================================================

var cam = {
    pos: [0, 15, 80],
    yaw: Math.PI,
    pitch: -0.15,
    speed: 30,
    sensitivity: 0.003
};

function camForward() {
    return [Math.sin(cam.yaw) * Math.cos(cam.pitch), Math.sin(cam.pitch), -Math.cos(cam.yaw) * Math.cos(cam.pitch)];
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
    if (e.key === 'm' || e.key === 'M') toggleAudio();
    if (e.key === '+' || e.key === '=') masterVolume = Math.min(1, masterVolume + 0.1);
    if (e.key === '-') masterVolume = Math.max(0, masterVolume - 0.1);
});
document.addEventListener('keyup', function(e) { keys[e.key.toLowerCase()] = false; });

canvas.addEventListener('click', function() {
    mouseCaptured = true;
});

document.addEventListener('mousemove', function(e) {
    if (!mouseCaptured) return;
    cam.yaw += e.movementX * cam.sensitivity;
    cam.pitch -= e.movementY * cam.sensitivity;
    cam.pitch = Math.max(-1.4, Math.min(1.4, cam.pitch));
});

document.addEventListener('keydown', function(e) {
    if (e.key === 'Escape') mouseCaptured = false;
});

// ============================================================================
// Audio — each body gets an oscillator at a frequency based on its size
// ============================================================================

var audioCtx = typeof AudioContext !== 'undefined' ? new AudioContext() : null;
var audioEnabled = true;
var masterVolume = 0.3;
var oscillators = [];

var REF_RADIUS = 2.0;
var REF_FREQ = 220;

function setupAudio() {
    if (!audioCtx) return;

    var allBodies = BODIES.concat(MOONS);
    for (var i = 0; i < allBodies.length; i++) {
        var b = allBodies[i];
        var freq = REF_FREQ * (REF_RADIUS / b.r);
        var semitones = Math.round(12 * Math.log2(freq / REF_FREQ));
        freq = REF_FREQ * Math.pow(2, semitones / 12);

        var osc = audioCtx.createOscillator();
        osc.type = 'sine';
        osc.frequency.value = freq;
        osc.attack.value = 0.5;
        osc.sustain.value = 1.0;
        osc.release.value = 0.5;

        var gain = audioCtx.createGain();
        gain.gain.value = masterVolume * 0.15;
        osc.connect(gain);
        osc.start();

        var vid = osc.voiceId;
        audioCtx.setVoiceSpatialEnabled(vid, true);
        audioCtx.setVoiceSpatialRefDistance(vid, 5.0);
        audioCtx.setVoiceSpatialMaxDistance(vid, 200.0);
        audioCtx.setVoiceSpatialRolloff(vid, 1.0);
        audioCtx.setVoiceSpatialDistanceModel(vid, 'inverse');

        oscillators.push({ body: b, osc: osc, gain: gain, vid: vid, freq: freq });
    }

    audioCtx.setHeadModelEnabled(true);
}

function toggleAudio() {
    audioEnabled = !audioEnabled;
    for (var i = 0; i < oscillators.length; i++) {
        oscillators[i].gain.gain.value = audioEnabled ? masterVolume * 0.15 : 0;
    }
}

function updateAudio() {
    if (!audioCtx) return;

    var fwd = camForward();
    audioCtx.setListenerPosition(cam.pos[0], cam.pos[1], cam.pos[2]);
    audioCtx.setListenerOrientation(fwd[0], fwd[1], fwd[2], 0, 1, 0);

    for (var i = 0; i < oscillators.length; i++) {
        var o = oscillators[i];
        var p = o.body.pos;
        audioCtx.setVoiceSpatialPosition(o.vid, p[0], p[1], p[2]);
        if (audioEnabled) {
            o.gain.gain.value = masterVolume * 0.15;
        }
    }
}

setupAudio();

// ============================================================================
// Render Loop
// ============================================================================

var time = 0;
var lastTime = Date.now();

function render() {
    var now = Date.now();
    var dt = Math.min((now - lastTime) / 1000, 0.05);
    lastTime = now;
    time += dt;

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

    // Update camera
    var target = v3add(cam.pos, fwd);
    scene.setCamera({
        fov: 60,
        aspect: W / H,
        near: 0.5,
        far: 1000,
        position: cam.pos,
        target: target
    });

    // Update body positions via scene graph transforms
    for (var i = 0; i < BODIES.length; i++) {
        var b = BODIES[i];
        if (b.orbit > 0) {
            b.angle += b.speed * dt * 0.3;
            b.pos = [Math.cos(b.angle) * b.orbit, 0, Math.sin(b.angle) * b.orbit];
        } else {
            b.pos = [0, 0, 0];
        }
        b.node.x = b.pos[0];
        b.node.y = b.pos[1];
        b.node.z = b.pos[2];
    }

    for (var i = 0; i < MOONS.length; i++) {
        var m = MOONS[i];
        var parent = BODIES[m.parent];
        m.angle += m.speed * dt * 0.5;
        m.pos = v3add(parent.pos, [
            Math.cos(m.angle) * m.orbit,
            Math.sin(m.angle * 0.3) * m.orbit * 0.1,
            Math.sin(m.angle) * m.orbit
        ]);
        m.node.x = m.pos[0];
        m.node.y = m.pos[1];
        m.node.z = m.pos[2];
    }

    // Render the scene graph (handles all GL rendering + compositing)
    scene.render();

    // Update audio positions
    updateAudio();

    // HUD
    var nearest = null, nearDist = Infinity;
    var allBodies = BODIES.concat(MOONS);
    for (var i = 0; i < allBodies.length; i++) {
        var d = v3len(v3sub(allBodies[i].pos, cam.pos));
        if (d < nearDist) { nearDist = d; nearest = allBodies[i]; }
    }
    var freqStr = '';
    for (var i = 0; i < oscillators.length; i++) {
        if (oscillators[i].body === nearest) {
            freqStr = ' (' + oscillators[i].freq.toFixed(0) + ' Hz)';
            break;
        }
    }
    info.textContent = 'Nearest: ' + (nearest ? nearest.name : '?') +
        ' [' + nearDist.toFixed(1) + 'u]' + freqStr +
        ' | Audio: ' + (audioEnabled ? 'ON' : 'OFF') +
        ' | Vol: ' + (masterVolume * 100).toFixed(0) + '%';

    requestAnimationFrame(render);
}

console.log('Starting render loop...');
requestAnimationFrame(render);
