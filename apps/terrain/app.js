// =============================================================================
// Terrain — Infinite blocky voxel world with comprehensive UI
// =============================================================================
//
// All heavy lifting (noise, voxel grids, greedy meshing, chunk lifecycle)
// runs in C++ via the TerrainManager. JS configures, drives the camera,
// handles input, and provides the settings UI.
//
// The C++ side rate-limits chunk loads via maxLoadsPerUpdate so terrain
// rebuilds don't block the render loop. UI changes are debounced to avoid
// hammering configure() on every slider tick.
// =============================================================================

var canvas = document.getElementById('c');
var scene  = canvas.getContext('scene');
var info   = document.getElementById('info');
var status = document.getElementById('status');
var panel  = document.getElementById('panel');

// Engine crosshair — replaces the old CSS circle
bro.crosshair.configure({
    style: 'circle', size: 6, thickness: 1,
    color: '#ffffff', opacity: 0.7, outline: false
});
bro.crosshair.show();

// ============================================================================
// Config — single source of truth for all terrain parameters
// ============================================================================

var DEFAULT_PALETTE = [
    0, 0, 0, 0,                    // 0: air
    0.42, 0.70, 0.27, 1.0,         // 1: grass
    0.52, 0.34, 0.18, 1.0,         // 2: dirt
    0.55, 0.55, 0.58, 1.0,         // 3: stone
    0.18, 0.18, 0.20, 1.0,         // 4: bedrock
    0.86, 0.78, 0.49, 1.0,         // 5: sand
];

var config = {
    // Noise
    frequency: 0.035,
    octaves: 5,
    gain: 0.50,
    lacunarity: 2.0,
    seed: 1337,
    // Shape
    baseHeight: 18,
    heightAmplitude: 16,
    seaLevel: 14,
    cellSize: 1.0,
    // Chunks
    chunkSizeX: 64,
    chunkSizeY: 48,
    chunkSizeZ: 64,
    loadRadius: 4,
    unloadRadius: 6,
    maxLoadsPerUpdate: 2,
    // Mesh mode: 0=smooth, 1=flat, 2=terraced, 3=blocky
    meshMode: 0,
    terraceStep: 1.0,
    // Palette (RGBA floats, 6 materials)
    palette: DEFAULT_PALETTE.slice(),
};

// Camera config (not part of terrain.configure)
var camConfig = {
    speed: 18,
    sensitivity: 0.003,
    fov: 65,
    far: 500,
};

function buildTerrainOpts() {
    // Auto-size chunkSizeY so terrain never clips at the top.
    // Height ranges from (baseHeight - heightAmplitude) to (baseHeight + heightAmplitude),
    // so we need at least baseHeight + heightAmplitude + 2 voxels of vertical space.
    var minY = config.baseHeight + config.heightAmplitude + 2;
    var chunkY = Math.max(config.chunkSizeY, minY);

    return {
        chunkSize: [config.chunkSizeX, chunkY, config.chunkSizeZ],
        cellSize: config.cellSize,
        loadRadius: config.loadRadius,
        unloadRadius: config.unloadRadius,
        maxLoadsPerUpdate: config.maxLoadsPerUpdate,
        seed: config.seed,
        noise: {
            frequency: config.frequency,
            octaves: config.octaves,
            gain: config.gain,
            lacunarity: config.lacunarity,
        },
        baseHeight: config.baseHeight,
        heightAmplitude: config.heightAmplitude,
        seaLevel: config.seaLevel,
        meshMode: config.meshMode,
        terraceStep: config.terraceStep,
        palette: config.palette.slice(),
    };
}

// ============================================================================
// Create terrain
// ============================================================================

var terrain = scene.createTerrain(buildTerrainOpts());

// ============================================================================
// Debounced reconfigure — avoids calling configure() on every slider tick
// ============================================================================

var configDirty = false;
var configTimer = null;
var CONFIG_DEBOUNCE_MS = 200;

function scheduleReconfigure() {
    configDirty = true;
    if (configTimer !== null) clearTimeout(configTimer);
    configTimer = setTimeout(applyConfig, CONFIG_DEBOUNCE_MS);
}

function applyConfig() {
    configTimer = null;
    configDirty = false;
    showStatus('Regenerating...');
    terrain.configure(buildTerrainOpts());
    // Status clears after a couple frames once chunks start loading
    setTimeout(function() { hideStatus(); }, 400);
}

function showStatus(msg) {
    status.textContent = msg;
    status.className = 'show';
}
function hideStatus() {
    status.className = '';
}

// ============================================================================
// Camera (6DOF quaternion-based with smooth velocity controls)
// ============================================================================

function v3add(a, b)   { return [a[0]+b[0], a[1]+b[1], a[2]+b[2]]; }
function v3scale(a, s) { return [a[0]*s, a[1]*s, a[2]*s]; }

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
    pos: [50, 42, 50],
    rot: quatFromAxis(0, 1, 0, -Math.PI / 4),
    vel: [0, 0, 0],
    accel: 12.0,
    damping: 6.0,
    rollSpeed: 2.5,
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
var rightMouseDown = false;

// Sculpting mode
var sculptMode = 'raise';  // 'raise' or 'lower'

document.addEventListener('keydown', function(e) {
    // Don't capture keys when interacting with panel inputs
    if (e.target && e.target.tagName === 'INPUT') return;

    keys[e.key.toLowerCase()] = true;

    if (e.key === 'Tab') {
        e.preventDefault();
        panel.classList.toggle('hidden');
    }
    if (e.key === '[' || e.key === ']') sculptMode = (sculptMode === 'raise') ? 'lower' : 'raise';
});
document.addEventListener('keyup', function(e) {
    keys[e.key.toLowerCase()] = false;
});

canvas.addEventListener('mousedown', function(e) {
    if (e.button === 0) pickAndEdit('lower');
    else if (e.button === 2) {
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
// Block picking
// ============================================================================

function pickAndEdit(action) {
    var fwd = camForward();
    var hit = terrain.raycast(cam.pos, fwd, 200);
    if (!hit) return;

    var p = hit.position;
    // material 0 = lower, material 1 = raise
    terrain.setVoxel(p[0], p[1], p[2], action === 'lower' ? 0 : 1);
    terrain.rebuild();
}

// ============================================================================
// UI — Slider bindings
// ============================================================================

// Map slider IDs to config keys and formatting
var SLIDERS = {
    'sl-frequency':        { key: 'frequency',        fmt: 3 },
    'sl-octaves':          { key: 'octaves',          fmt: 0 },
    'sl-gain':             { key: 'gain',             fmt: 2 },
    'sl-lacunarity':       { key: 'lacunarity',       fmt: 2 },
    'sl-seed':             { key: 'seed',             fmt: 0 },
    'sl-baseHeight':       { key: 'baseHeight',       fmt: 0 },
    'sl-heightAmplitude':  { key: 'heightAmplitude',  fmt: 0 },
    'sl-seaLevel':         { key: 'seaLevel',         fmt: 0 },
    'sl-cellSize':         { key: 'cellSize',         fmt: 2 },
    'sl-chunkSizeX':       { key: 'chunkSizeX',       fmt: 0 },
    'sl-chunkSizeY':       { key: 'chunkSizeY',       fmt: 0 },
    'sl-chunkSizeZ':       { key: 'chunkSizeZ',       fmt: 0 },
    'sl-loadRadius':       { key: 'loadRadius',       fmt: 0 },
    'sl-unloadRadius':     { key: 'unloadRadius',     fmt: 0 },
    'sl-maxLoadsPerUpdate':{ key: 'maxLoadsPerUpdate', fmt: 0 },
    'sl-meshMode':         { key: 'meshMode',         fmt: 0 },
    'sl-terraceStep':      { key: 'terraceStep',      fmt: 2 },
};

// Mode label names
var MODE_NAMES = ['smooth', 'flat-shaded', 'terraced', 'blocky'];

function updateModeLabel() {
    var el = document.getElementById('mode-label');
    if (el) el.textContent = MODE_NAMES[config.meshMode] || 'unknown';
}

// Camera sliders (don't trigger terrain reconfigure)
var CAM_SLIDERS = {
    'sl-speed':       { key: 'speed',       fmt: 0 },
    'sl-sensitivity': { key: 'sensitivity', fmt: 4 },
    'sl-fov':         { key: 'fov',         fmt: 0 },
    'sl-far':         { key: 'far',         fmt: 0 },
};

function initSliders() {
    var id, def, el, valEl;

    for (id in SLIDERS) {
        def = SLIDERS[id];
        el = document.getElementById(id);
        valEl = document.getElementById('v-' + def.key);
        if (!el) continue;
        el.value = config[def.key];
        if (valEl) valEl.textContent = Number(config[def.key]).toFixed(def.fmt);
        (function(slider, valSpan, d) {
            slider.addEventListener('input', function() {
                var v = d.fmt === 0 ? parseInt(slider.value) : parseFloat(slider.value);
                config[d.key] = v;
                if (valSpan) valSpan.textContent = v.toFixed(d.fmt);
                if (d.key === 'meshMode') updateModeLabel();
                scheduleReconfigure();
            });
        })(el, valEl, def);
    }

    for (id in CAM_SLIDERS) {
        def = CAM_SLIDERS[id];
        el = document.getElementById(id);
        valEl = document.getElementById('v-' + def.key);
        if (!el) continue;
        el.value = camConfig[def.key];
        if (valEl) valEl.textContent = Number(camConfig[def.key]).toFixed(def.fmt);
        (function(slider, valSpan, d) {
            slider.addEventListener('input', function() {
                var v = d.fmt === 0 ? parseInt(slider.value) : parseFloat(slider.value);
                camConfig[d.key] = v;
                if (valSpan) valSpan.textContent = v.toFixed(d.fmt);
            });
        })(el, valEl, def);
    }
}

// ============================================================================
// UI — Color pickers
// ============================================================================

// Convert 0.0-1.0 RGB to hex
function rgbToHex(r, g, b) {
    function c(v) { var h = Math.round(v * 255).toString(16); return h.length < 2 ? '0' + h : h; }
    return '#' + c(r) + c(g) + c(b);
}

// Convert hex to 0.0-1.0 RGB array
function hexToRgb(hex) {
    var v = parseInt(hex.substring(1), 16);
    return [(v >> 16 & 255) / 255, (v >> 8 & 255) / 255, (v & 255) / 255];
}

function initColorPickers() {
    for (var i = 1; i <= 5; i++) {
        var colEl = document.getElementById('col-' + i);
        var hexEl = document.getElementById('hex-' + i);
        if (!colEl) continue;

        var off = i * 4;  // palette offset
        var hex = rgbToHex(config.palette[off], config.palette[off+1], config.palette[off+2]);
        colEl.value = hex;
        if (hexEl) hexEl.textContent = hex;

        (function(picker, hexSpan, matIndex) {
            picker.addEventListener('input', function() {
                var rgb = hexToRgb(picker.value);
                var base = matIndex * 4;
                config.palette[base]   = rgb[0];
                config.palette[base+1] = rgb[1];
                config.palette[base+2] = rgb[2];
                if (hexSpan) hexSpan.textContent = picker.value;
                scheduleReconfigure();
            });
        })(colEl, hexEl, i);
    }
}

function resetColors() {
    config.palette = DEFAULT_PALETTE.slice();
    initColorPickers();
    scheduleReconfigure();
}

// ============================================================================
// UI — Section toggles
// ============================================================================

function initSectionToggles() {
    var titles = document.querySelectorAll('.section-title[data-toggle]');
    for (var i = 0; i < titles.length; i++) {
        (function(title) {
            title.addEventListener('click', function() {
                var body = document.getElementById('body-' + title.getAttribute('data-toggle'));
                if (body) body.classList.toggle('collapsed');
            });
        })(titles[i]);
    }
}

// ============================================================================
// UI — Presets
// ============================================================================

var PRESETS = {
    'default': {
        frequency: 0.035, octaves: 5, gain: 0.50, lacunarity: 2.0,
        baseHeight: 18, heightAmplitude: 16, seaLevel: 14, cellSize: 1.0,
        chunkSizeX: 64, chunkSizeY: 48, chunkSizeZ: 64,
        loadRadius: 4, unloadRadius: 6, maxLoadsPerUpdate: 2,
        meshMode: 0, terraceStep: 1.0,
    },
    'flat': {
        frequency: 0.01, octaves: 2, gain: 0.3, lacunarity: 2.0,
        baseHeight: 20, heightAmplitude: 3, seaLevel: 8, cellSize: 1.0,
        chunkSizeX: 64, chunkSizeY: 48, chunkSizeZ: 64,
        loadRadius: 4, unloadRadius: 6, maxLoadsPerUpdate: 2,
        meshMode: 0, terraceStep: 1.0,
    },
    'lowpoly': {
        frequency: 0.025, octaves: 4, gain: 0.50, lacunarity: 2.0,
        baseHeight: 24, heightAmplitude: 20, seaLevel: 14, cellSize: 2.0,
        chunkSizeX: 32, chunkSizeY: 48, chunkSizeZ: 32,
        loadRadius: 6, unloadRadius: 8, maxLoadsPerUpdate: 2,
        meshMode: 1, terraceStep: 1.0,
    },
    'mountains': {
        frequency: 0.020, octaves: 8, gain: 0.55, lacunarity: 2.2,
        baseHeight: 48, heightAmplitude: 44, seaLevel: 16, cellSize: 1.0,
        chunkSizeX: 64, chunkSizeY: 96, chunkSizeZ: 64,
        loadRadius: 4, unloadRadius: 6, maxLoadsPerUpdate: 2,
        meshMode: 0, terraceStep: 1.0,
    },
    'terraced': {
        frequency: 0.030, octaves: 5, gain: 0.50, lacunarity: 2.0,
        baseHeight: 24, heightAmplitude: 20, seaLevel: 10, cellSize: 1.0,
        chunkSizeX: 64, chunkSizeY: 48, chunkSizeZ: 64,
        loadRadius: 4, unloadRadius: 6, maxLoadsPerUpdate: 2,
        meshMode: 2, terraceStep: 2.0,
    },
    'blocky': {
        frequency: 0.035, octaves: 5, gain: 0.50, lacunarity: 2.0,
        baseHeight: 18, heightAmplitude: 16, seaLevel: 14, cellSize: 1.0,
        chunkSizeX: 64, chunkSizeY: 48, chunkSizeZ: 64,
        loadRadius: 4, unloadRadius: 6, maxLoadsPerUpdate: 2,
        meshMode: 3, terraceStep: 1.0,
    },
    'islands': {
        frequency: 0.045, octaves: 4, gain: 0.45, lacunarity: 2.0,
        baseHeight: 12, heightAmplitude: 14, seaLevel: 18, cellSize: 1.0,
        chunkSizeX: 64, chunkSizeY: 48, chunkSizeZ: 64,
        loadRadius: 5, unloadRadius: 7, maxLoadsPerUpdate: 2,
        meshMode: 0, terraceStep: 1.0,
    },
    'alien': {
        frequency: 0.08, octaves: 6, gain: 0.70, lacunarity: 3.0,
        baseHeight: 24, heightAmplitude: 20, seaLevel: 6, cellSize: 1.0,
        chunkSizeX: 64, chunkSizeY: 48, chunkSizeZ: 64,
        loadRadius: 4, unloadRadius: 6, maxLoadsPerUpdate: 2,
        meshMode: 0, terraceStep: 1.0,
    },
};

function applyPreset(name) {
    var p = PRESETS[name];
    if (!p) return;

    for (var k in p) {
        config[k] = p[k];
    }
    // Refresh all sliders to match
    syncSlidersFromConfig();
    updateModeLabel();
    scheduleReconfigure();
}

function syncSlidersFromConfig() {
    var id, def, el, valEl;
    for (id in SLIDERS) {
        def = SLIDERS[id];
        el = document.getElementById(id);
        valEl = document.getElementById('v-' + def.key);
        if (!el) continue;
        el.value = config[def.key];
        if (valEl) valEl.textContent = Number(config[def.key]).toFixed(def.fmt);
    }
}

function initPresets() {
    var buttons = document.querySelectorAll('[data-preset]');
    for (var i = 0; i < buttons.length; i++) {
        (function(btn) {
            btn.addEventListener('click', function() {
                applyPreset(btn.getAttribute('data-preset'));
            });
        })(buttons[i]);
    }
}

// ============================================================================
// UI — Buttons
// ============================================================================

function initButtons() {
    var randBtn = document.getElementById('btn-random-seed');
    if (randBtn) {
        randBtn.addEventListener('click', function() {
            config.seed = Math.floor(Math.random() * 10000);
            var el = document.getElementById('sl-seed');
            var valEl = document.getElementById('v-seed');
            if (el) el.value = config.seed;
            if (valEl) valEl.textContent = config.seed;
            scheduleReconfigure();
        });
    }

    var resetColBtn = document.getElementById('btn-reset-colors');
    if (resetColBtn) {
        resetColBtn.addEventListener('click', resetColors);
    }

    var resetPosBtn = document.getElementById('btn-reset-pos');
    if (resetPosBtn) {
        resetPosBtn.addEventListener('click', function() {
            cam.pos = [50, 42, 50];
            cam.vel = [0, 0, 0];
            cam.rot = quatFromAxis(0, 1, 0, -Math.PI / 4);
            cam.rot = quatNorm(quatMul(cam.rot, quatFromAxis(1, 0, 0, -0.30)));
        });
    }
}

// ============================================================================
// Initialize UI
// ============================================================================

initSliders();
initColorPickers();
initSectionToggles();
initPresets();
initButtons();

// Prevent panel interactions from capturing the mouse
panel.addEventListener('mousedown', function(e) {
    e.stopPropagation();
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

    // --- Mouse look: apply accumulated deltas as quaternion rotation ---
    var dx = mouseAccumX * camConfig.sensitivity;
    var dy = mouseAccumY * camConfig.sensitivity;
    mouseAccumX = 0;
    mouseAccumY = 0;

    if (dx !== 0 || dy !== 0) {
        var yawQ   = quatFromAxis(0, 1, 0, -dx);
        var right  = camRight();
        var pitchQ = quatFromAxis(right[0], right[1], right[2], -dy);
        cam.rot = quatNorm(quatMul(pitchQ, quatMul(yawQ, cam.rot)));
    }

    // --- Roll (Q/E) ---
    var rollInput = 0;
    if (keys['q']) rollInput += 1;
    if (keys['e']) rollInput -= 1;
    if (rollInput !== 0) {
        var fwd = camForward();
        var rollQ = quatFromAxis(fwd[0], fwd[1], fwd[2], rollInput * cam.rollSpeed * dt);
        cam.rot = quatNorm(quatMul(rollQ, cam.rot));
    }

    // --- Velocity-based movement (smooth acceleration + damping) ---
    var maxSpeed = camConfig.speed;
    if (keys['shift']) maxSpeed *= 3;

    var fwd   = camForward();
    var right = camRight();
    var up    = camUp();

    var thrustX = 0, thrustY = 0, thrustZ = 0;
    if (keys['w']) { thrustX += fwd[0];   thrustY += fwd[1];   thrustZ += fwd[2]; }
    if (keys['s']) { thrustX -= fwd[0];   thrustY -= fwd[1];   thrustZ -= fwd[2]; }
    if (keys['a']) { thrustX -= right[0]; thrustY -= right[1]; thrustZ -= right[2]; }
    if (keys['d']) { thrustX += right[0]; thrustY += right[1]; thrustZ += right[2]; }
    if (keys[' '])       { thrustX += up[0]; thrustY += up[1]; thrustZ += up[2]; }
    if (keys['control']) { thrustX -= up[0]; thrustY -= up[1]; thrustZ -= up[2]; }

    var thrustLen = Math.sqrt(thrustX*thrustX + thrustY*thrustY + thrustZ*thrustZ);
    if (thrustLen > 1e-6) {
        var inv = 1.0 / thrustLen;
        thrustX *= inv; thrustY *= inv; thrustZ *= inv;
    }

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

    cam.pos[0] += cam.vel[0] * dt;
    cam.pos[1] += cam.vel[1] * dt;
    cam.pos[2] += cam.vel[2] * dt;

    // Drive chunk loading/unloading
    terrain.update(cam.pos[0], cam.pos[1], cam.pos[2]);

    var w = canvas.clientWidth || 1;
    var h = canvas.clientHeight || 1;
    scene.setCamera({
        fov: camConfig.fov,
        aspect: w / h,
        near: 0.5,
        far: camConfig.far,
        position: cam.pos,
        quaternion: cam.rot,
    });

    info.textContent =
        'pos ' + cam.pos[0].toFixed(1) + ', ' + cam.pos[1].toFixed(1) +
        ', ' + cam.pos[2].toFixed(1) +
        ' | fps ' + fps +
        ' | chunks ' + terrain.chunkCount +
        ' | tris ' + terrain.triangleCount +
        ' | seed ' + config.seed;

    requestAnimationFrame(render);
}

requestAnimationFrame(render);
