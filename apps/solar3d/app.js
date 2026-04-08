// Solar System 3D — WebGL2 + FastNoise terrain + broaudio spatial audio
// WASD fly-through with procedurally generated planets that sing

var canvas = document.getElementById('c');
var gl = canvas.getContext('webgl2');
var info = document.getElementById('info');
var W = 1024, H = 768;

// ============================================================================
// 3D Math (minimal Vec3 / Mat4)
// ============================================================================

function v3(x, y, z) { return [x, y, z]; }
function v3add(a, b) { return [a[0]+b[0], a[1]+b[1], a[2]+b[2]]; }
function v3sub(a, b) { return [a[0]-b[0], a[1]-b[1], a[2]-b[2]]; }
function v3scale(a, s) { return [a[0]*s, a[1]*s, a[2]*s]; }
function v3dot(a, b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
function v3cross(a, b) {
    return [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]];
}
function v3len(a) { return Math.sqrt(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]); }
function v3norm(a) { var l = v3len(a); return l > 0 ? v3scale(a, 1/l) : [0,0,0]; }

function mat4() { var m = new Float32Array(16); m[0]=m[5]=m[10]=m[15]=1; return m; }

function mat4perspective(fov, aspect, near, far) {
    var m = new Float32Array(16);
    var f = 1.0 / Math.tan(fov * 0.5);
    m[0] = f / aspect; m[5] = f;
    m[10] = (far + near) / (near - far);
    m[11] = -1;
    m[14] = (2 * far * near) / (near - far);
    return m;
}

function mat4lookAt(eye, center, up) {
    var f = v3norm(v3sub(center, eye));
    var s = v3norm(v3cross(f, up));
    var u = v3cross(s, f);
    var m = new Float32Array(16);
    m[0]=s[0]; m[4]=s[1]; m[8]=s[2];
    m[1]=u[0]; m[5]=u[1]; m[9]=u[2];
    m[2]=-f[0]; m[6]=-f[1]; m[10]=-f[2];
    m[12]=-(s[0]*eye[0]+s[1]*eye[1]+s[2]*eye[2]);
    m[13]=-(u[0]*eye[0]+u[1]*eye[1]+u[2]*eye[2]);
    m[14]=(f[0]*eye[0]+f[1]*eye[1]+f[2]*eye[2]);
    m[15]=1;
    return m;
}

function mat4translate(x, y, z) {
    var m = mat4(); m[12]=x; m[13]=y; m[14]=z; return m;
}

function mat4mul(a, b) {
    var r = new Float32Array(16);
    for (var i = 0; i < 4; i++)
        for (var j = 0; j < 4; j++) {
            var s = 0;
            for (var k = 0; k < 4; k++) s += a[j + k*4] * b[i*4 + k];
            r[i*4 + j] = s;
        }
    return r;
}

function mat3normalFromMat4(m) {
    // Upper-left 3x3, transposed inverse (works for uniform scale)
    var r = new Float32Array(9);
    r[0]=m[0]; r[1]=m[1]; r[2]=m[2];
    r[3]=m[4]; r[4]=m[5]; r[5]=m[6];
    r[6]=m[8]; r[7]=m[9]; r[8]=m[10];
    return r;
}

// ============================================================================
// WebGL Helpers
// ============================================================================

function compileShader(type, src) {
    var s = gl.createShader(type);
    gl.shaderSource(s, src);
    gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS))
        console.log('Shader error:', gl.getShaderInfoLog(s));
    return s;
}

function createProgram(vSrc, fSrc) {
    var p = gl.createProgram();
    gl.attachShader(p, compileShader(gl.VERTEX_SHADER, vSrc));
    gl.attachShader(p, compileShader(gl.FRAGMENT_SHADER, fSrc));
    gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS))
        console.log('Link error:', gl.getProgramInfoLog(p));
    return p;
}

// ============================================================================
// Shaders
// ============================================================================

var VERT = [
    '#version 300 es',
    'precision highp float;',
    'uniform mat4 uMVP;',
    'uniform mat4 uModel;',
    'uniform mat3 uNormal;',
    'in vec3 aPos;',
    'in vec3 aNorm;',
    'in vec3 aCol;',
    'out vec3 vN;',
    'out vec3 vWorld;',
    'out vec3 vCol;',
    'void main() {',
    '  vCol = aCol;',
    '  vN = normalize(uNormal * aNorm);',
    '  vWorld = (uModel * vec4(aPos, 1.0)).xyz;',
    '  gl_Position = uMVP * vec4(aPos, 1.0);',
    '}'
].join('\n');

var FRAG = [
    '#version 300 es',
    'precision highp float;',
    'uniform vec3 uSunPos;',
    'uniform float uEmissive;',
    'uniform float uAmbient;',
    'in vec3 vN;',
    'in vec3 vWorld;',
    'in vec3 vCol;',
    'out vec4 fragColor;',
    'void main() {',
    '  vec3 N = normalize(vN);',
    '  vec3 L = normalize(uSunPos - vWorld);',
    '  float diff = max(dot(N, L), 0.0);',
    '  vec3 lit = vCol * (uAmbient + diff * (1.0 - uAmbient));',
    '  vec3 color = mix(lit, vCol, uEmissive);',
    '  fragColor = vec4(color, 1.0);',
    '}'
].join('\n');

// Star background shader
var STAR_VERT = [
    '#version 300 es',
    'precision highp float;',
    'uniform mat4 uVP;',
    'in vec3 aPos;',
    'in float aSize;',
    'in float aBright;',
    'out float vBright;',
    'void main() {',
    '  vBright = aBright;',
    '  gl_Position = uVP * vec4(aPos, 1.0);',
    '  gl_PointSize = aSize;',
    '}'
].join('\n');

var STAR_FRAG = [
    '#version 300 es',
    'precision highp float;',
    'in float vBright;',
    'out vec4 fragColor;',
    'void main() {',
    '  float d = length(gl_PointCoord - vec2(0.5));',
    '  float a = smoothstep(0.5, 0.1, d) * vBright;',
    '  fragColor = vec4(1.0, 0.95, 0.9, a);',
    '}'
].join('\n');

var prog = createProgram(VERT, FRAG);
var starProg = createProgram(STAR_VERT, STAR_FRAG);

// Uniform locations
var uMVP = gl.getUniformLocation(prog, 'uMVP');
var uModel = gl.getUniformLocation(prog, 'uModel');
var uNormal = gl.getUniformLocation(prog, 'uNormal');
var uSunPos = gl.getUniformLocation(prog, 'uSunPos');
var uEmissive = gl.getUniformLocation(prog, 'uEmissive');
var uAmbient = gl.getUniformLocation(prog, 'uAmbient');

var uStarVP = gl.getUniformLocation(starProg, 'uVP');

// Attribute locations
var aPos = gl.getAttribLocation(prog, 'aPos');
var aNorm = gl.getAttribLocation(prog, 'aNorm');
var aCol = gl.getAttribLocation(prog, 'aCol');

var aStarPos = gl.getAttribLocation(starProg, 'aPos');
var aStarSize = gl.getAttribLocation(starProg, 'aSize');
var aStarBright = gl.getAttribLocation(starProg, 'aBright');

// ============================================================================
// Sphere Mesh Generator with FastNoise terrain
// ============================================================================

function generateSphere(radius, segs, rings, baseColor, noiseAmp, noiseFreq, seed) {
    var positions = [];
    var normals = [];
    var colors = [];
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

            // Normalized direction
            var nx = sp * ct, ny = cp, nz = sp * st;

            // Sample noise at this direction for terrain displacement
            var displacement = 0;
            if (noise) {
                displacement = noise.genSingle3D(
                    nx * noiseFreq, ny * noiseFreq, nz * noiseFreq, seed
                ) * noiseAmp;
            }

            var r = radius + displacement;
            positions.push(nx * r, ny * r, nz * r);
            normals.push(nx, ny, nz);

            // Color varies with terrain height
            var t = noiseAmp > 0 ? (displacement / noiseAmp) * 0.5 + 0.5 : 0.5;
            t = Math.max(0, Math.min(1, t));
            var cr = baseColor[0] * (0.6 + 0.4 * t);
            var cg = baseColor[1] * (0.6 + 0.4 * t);
            var cb = baseColor[2] * (0.6 + 0.4 * t);
            colors.push(cr, cg, cb);
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
            var fn = v3norm(v3cross(v3sub(v1, v0), v3sub(v2, v0)));
            for (var j = 0; j < 3; j++) {
                var idx = indices[i + j] * 3;
                nn[idx] += fn[0]; nn[idx+1] += fn[1]; nn[idx+2] += fn[2];
            }
        }
        // Normalize accumulated normals
        for (var i = 0; i < nn.length; i += 3) {
            var l = Math.sqrt(nn[i]*nn[i] + nn[i+1]*nn[i+1] + nn[i+2]*nn[i+2]);
            if (l > 0) { nn[i] /= l; nn[i+1] /= l; nn[i+2] /= l; }
        }
        normals = nn;
    }

    // Create GL buffers
    var vao = gl.createVertexArray();
    gl.bindVertexArray(vao);

    var posBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, posBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(positions), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(aPos);
    gl.vertexAttribPointer(aPos, 3, gl.FLOAT, false, 0, 0);

    var normBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, normBuf);
    gl.bufferData(gl.ARRAY_BUFFER, normals instanceof Float32Array ? normals : new Float32Array(normals), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(aNorm);
    gl.vertexAttribPointer(aNorm, 3, gl.FLOAT, false, 0, 0);

    var colBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, colBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(colors), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(aCol);
    gl.vertexAttribPointer(aCol, 3, gl.FLOAT, false, 0, 0);

    var idxBuf = gl.createBuffer();
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, idxBuf);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint32Array(indices), gl.STATIC_DRAW);

    gl.bindVertexArray(null);

    return { vao: vao, count: indices.length };
}

// ============================================================================
// Star Field
// ============================================================================

function createStarField(count, radius) {
    var data = []; // x, y, z, size, brightness
    for (var i = 0; i < count; i++) {
        // Random point on sphere
        var u = Math.random() * 2 - 1;
        var theta = Math.random() * Math.PI * 2;
        var r = Math.sqrt(1 - u * u);
        data.push(r * Math.cos(theta) * radius, u * radius, r * Math.sin(theta) * radius);
        data.push(1.0 + Math.random() * 3.0); // size
        data.push(0.3 + Math.random() * 0.7); // brightness
    }
    var buf = new Float32Array(data);

    var vao = gl.createVertexArray();
    gl.bindVertexArray(vao);
    var vbo = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
    gl.bufferData(gl.ARRAY_BUFFER, buf, gl.STATIC_DRAW);
    gl.enableVertexAttribArray(aStarPos);
    gl.vertexAttribPointer(aStarPos, 3, gl.FLOAT, false, 20, 0);
    gl.enableVertexAttribArray(aStarSize);
    gl.vertexAttribPointer(aStarSize, 1, gl.FLOAT, false, 20, 12);
    gl.enableVertexAttribArray(aStarBright);
    gl.vertexAttribPointer(aStarBright, 1, gl.FLOAT, false, 20, 16);
    gl.bindVertexArray(null);

    return { vao: vao, count: count };
}

var stars = createStarField(2000, 500);

// ============================================================================
// Orbit Ring (simple line loop)
// ============================================================================

function createOrbitRing(radius, segments, color) {
    var positions = [];
    var normals = [];
    var colors = [];
    for (var i = 0; i <= segments; i++) {
        var a = (i / segments) * Math.PI * 2;
        positions.push(Math.cos(a) * radius, 0, Math.sin(a) * radius);
        normals.push(0, 1, 0);
        colors.push(color[0], color[1], color[2]);
    }
    var vao = gl.createVertexArray();
    gl.bindVertexArray(vao);

    var posBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, posBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(positions), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(aPos);
    gl.vertexAttribPointer(aPos, 3, gl.FLOAT, false, 0, 0);

    var normBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, normBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(normals), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(aNorm);
    gl.vertexAttribPointer(aNorm, 3, gl.FLOAT, false, 0, 0);

    var colBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, colBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(colors), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(aCol);
    gl.vertexAttribPointer(aCol, 3, gl.FLOAT, false, 0, 0);

    gl.bindVertexArray(null);
    return { vao: vao, count: segments + 1 };
}

// ============================================================================
// Celestial Bodies
// ============================================================================

var BODIES = [
    // name, radius, orbitRadius, orbitSpeed, color [r,g,b], noiseAmp, noiseFreq, seed, parent
    { name: 'Sun',      r: 8.0,  orbit: 0,   speed: 0,     col: [1.0, 0.85, 0.3],  noise: 0.3, nfreq: 3,  seed: 1,    emissive: true },
    { name: 'Mercury',  r: 1.0,  orbit: 20,  speed: 1.6,   col: [0.7, 0.65, 0.6],  noise: 0.15, nfreq: 5, seed: 10 },
    { name: 'Venus',    r: 1.8,  orbit: 30,  speed: 1.2,   col: [0.9, 0.75, 0.4],  noise: 0.1, nfreq: 4,  seed: 20 },
    { name: 'Earth',    r: 2.0,  orbit: 45,  speed: 1.0,   col: [0.2, 0.5, 0.8],   noise: 0.25, nfreq: 5, seed: 42 },
    { name: 'Mars',     r: 1.4,  orbit: 60,  speed: 0.8,   col: [0.85, 0.35, 0.2], noise: 0.3, nfreq: 4,  seed: 50 },
    { name: 'Jupiter',  r: 5.0,  orbit: 90,  speed: 0.4,   col: [0.85, 0.7, 0.5],  noise: 0.2, nfreq: 3,  seed: 60 },
    { name: 'Saturn',   r: 4.0,  orbit: 120, speed: 0.3,   col: [0.9, 0.8, 0.6],   noise: 0.15, nfreq: 3, seed: 70 },
    { name: 'Neptune',  r: 3.0,  orbit: 150, speed: 0.2,   col: [0.3, 0.4, 0.9],   noise: 0.2, nfreq: 4,  seed: 80 },
];

// Moons: parentIndex, name, radius, orbitRadius, orbitSpeed, color, noiseAmp, noiseFreq, seed
var MOONS = [
    { parent: 3, name: 'Moon',      r: 0.6, orbit: 5,  speed: 3.0, col: [0.75, 0.73, 0.7], noise: 0.1, nfreq: 6, seed: 100 },
    { parent: 5, name: 'Io',        r: 0.7, orbit: 8,  speed: 2.5, col: [0.9, 0.85, 0.3],  noise: 0.15, nfreq: 5, seed: 110 },
    { parent: 5, name: 'Europa',    r: 0.6, orbit: 10, speed: 2.0, col: [0.8, 0.82, 0.85], noise: 0.05, nfreq: 4, seed: 120 },
    { parent: 5, name: 'Ganymede',  r: 0.9, orbit: 13, speed: 1.5, col: [0.6, 0.55, 0.5],  noise: 0.12, nfreq: 5, seed: 130 },
    { parent: 5, name: 'Callisto',  r: 0.8, orbit: 16, speed: 1.2, col: [0.5, 0.48, 0.45], noise: 0.1, nfreq: 4, seed: 140 },
    { parent: 6, name: 'Titan',     r: 1.0, orbit: 9,  speed: 1.8, col: [0.8, 0.6, 0.3],   noise: 0.15, nfreq: 4, seed: 150 },
    { parent: 7, name: 'Triton',    r: 0.7, orbit: 7,  speed: 2.2, col: [0.6, 0.7, 0.85],  noise: 0.1, nfreq: 5, seed: 160 },
];

// Generate meshes
console.log('Generating planet meshes...');
for (var i = 0; i < BODIES.length; i++) {
    var b = BODIES[i];
    var segs = b.r > 3 ? 32 : 24;
    var rings = b.r > 3 ? 24 : 16;
    b.mesh = generateSphere(b.r, segs, rings, b.col, b.noise, b.nfreq, b.seed);
    b.angle = Math.random() * Math.PI * 2; // random starting orbit position
    b.pos = [0, 0, 0];
    if (b.orbit > 0) b.orbitRing = createOrbitRing(b.orbit, 64, [0.15, 0.15, 0.25]);
}

for (var i = 0; i < MOONS.length; i++) {
    var m = MOONS[i];
    m.mesh = generateSphere(m.r, 16, 12, m.col, m.noise, m.nfreq, m.seed);
    m.angle = Math.random() * Math.PI * 2;
    m.pos = [0, 0, 0];
}
console.log('Meshes generated');

// ============================================================================
// Camera (FPS-style)
// ============================================================================

var cam = {
    pos: [0, 15, -80],
    yaw: 0,
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

// Frequency mapping: larger bodies = lower pitch (harmonic series feel)
// Reference: Earth (r=2.0) = 220 Hz (A3)
var REF_RADIUS = 2.0;
var REF_FREQ = 220;

function setupAudio() {
    if (!audioCtx) return;

    var allBodies = BODIES.concat(MOONS);
    for (var i = 0; i < allBodies.length; i++) {
        var b = allBodies[i];
        var freq = REF_FREQ * (REF_RADIUS / b.r);
        // Quantize to nearest musical note for harmony
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

    // Enable head model for immersive 3D audio
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

    // Update listener position and orientation
    var fwd = camForward();
    audioCtx.setListenerPosition(cam.pos[0], cam.pos[1], cam.pos[2]);
    audioCtx.setListenerOrientation(fwd[0], fwd[1], fwd[2], 0, 1, 0);

    // Update each source position
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

gl.enable(gl.DEPTH_TEST);
gl.depthFunc(gl.LEQUAL);
gl.enable(gl.CULL_FACE);
gl.cullFace(gl.BACK);

var time = 0;
var lastTime = Date.now();
var projMatrix = mat4perspective(Math.PI / 3, W / H, 0.5, 1000);

function drawMesh(mesh, modelMatrix, emissive) {
    var mvp = mat4mul(projMatrix, mat4mul(viewMatrix, modelMatrix));
    var nmat = mat3normalFromMat4(modelMatrix);

    gl.useProgram(prog);
    gl.uniformMatrix4fv(uMVP, false, mvp);
    gl.uniformMatrix4fv(uModel, false, modelMatrix);
    gl.uniformMatrix3fv(uNormal, false, nmat);
    gl.uniform3f(uSunPos, 0, 0, 0);
    gl.uniform1f(uEmissive, emissive ? 1.0 : 0.0);
    gl.uniform1f(uAmbient, 0.15);

    gl.bindVertexArray(mesh.vao);
    gl.drawElements(gl.TRIANGLES, mesh.count, gl.UNSIGNED_INT, 0);
    gl.bindVertexArray(null);
}

function drawOrbitRing(ring) {
    var model = mat4();
    var mvp = mat4mul(projMatrix, mat4mul(viewMatrix, model));
    gl.useProgram(prog);
    gl.uniformMatrix4fv(uMVP, false, mvp);
    gl.uniformMatrix4fv(uModel, false, model);
    gl.uniformMatrix3fv(uNormal, false, mat3normalFromMat4(model));
    gl.uniform3f(uSunPos, 0, 0, 0);
    gl.uniform1f(uEmissive, 1.0);
    gl.uniform1f(uAmbient, 1.0);

    gl.bindVertexArray(ring.vao);
    gl.drawArrays(gl.LINE_STRIP, 0, ring.count);
    gl.bindVertexArray(null);
}

var viewMatrix;

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
    // Note: shift is handled above for sprint, ctrl for down
    if (keys['control']) cam.pos[1] -= moveSpeed;

    // View matrix
    var target = v3add(cam.pos, fwd);
    viewMatrix = mat4lookAt(cam.pos, target, [0, 1, 0]);

    // Update body positions
    for (var i = 0; i < BODIES.length; i++) {
        var b = BODIES[i];
        if (b.orbit > 0) {
            b.angle += b.speed * dt * 0.3;
            b.pos = [Math.cos(b.angle) * b.orbit, 0, Math.sin(b.angle) * b.orbit];
        } else {
            b.pos = [0, 0, 0];
        }
    }
    for (var i = 0; i < MOONS.length; i++) {
        var m = MOONS[i];
        var parent = BODIES[m.parent];
        m.angle += m.speed * dt * 0.5;
        m.pos = v3add(parent.pos, [
            Math.cos(m.angle) * m.orbit,
            Math.sin(m.angle * 0.3) * m.orbit * 0.1, // slight orbital tilt
            Math.sin(m.angle) * m.orbit
        ]);
    }

    // Clear
    gl.viewport(0, 0, W, H);
    gl.clearColor(0.02, 0.02, 0.06, 1);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

    // Draw stars
    gl.useProgram(starProg);
    gl.uniformMatrix4fv(uStarVP, false, mat4mul(projMatrix, viewMatrix));
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE);
    gl.disable(gl.DEPTH_TEST);
    gl.bindVertexArray(stars.vao);
    gl.drawArrays(gl.POINTS, 0, stars.count);
    gl.bindVertexArray(null);
    gl.disable(gl.BLEND);
    gl.enable(gl.DEPTH_TEST);

    // Draw orbit rings
    for (var i = 0; i < BODIES.length; i++) {
        if (BODIES[i].orbitRing) {
            drawOrbitRing(BODIES[i].orbitRing);
        }
    }

    // Draw bodies
    for (var i = 0; i < BODIES.length; i++) {
        var b = BODIES[i];
        var model = mat4translate(b.pos[0], b.pos[1], b.pos[2]);
        drawMesh(b.mesh, model, b.emissive || false);
    }

    // Draw moons
    for (var i = 0; i < MOONS.length; i++) {
        var m = MOONS[i];
        var model = mat4translate(m.pos[0], m.pos[1], m.pos[2]);
        drawMesh(m.mesh, model, false);
    }

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
