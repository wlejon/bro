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
// Camera (FPS-style fly)
// ============================================================================

function v3add(a, b)   { return [a[0]+b[0], a[1]+b[1], a[2]+b[2]]; }
function v3scale(a, s) { return [a[0]*s, a[1]*s, a[2]*s]; }

var cam = {
    pos: [50, 42, 50],
    yaw: -Math.PI / 4,
    pitch: -0.30,
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

// Material cycling for placement
var MATERIALS = ['grass', 'dirt', 'stone', 'bedrock', 'sand'];
var placeMatIndex = 2;   // stone
function activePlaceMat()     { return placeMatIndex + 1; }  // +1 because 0=air
function activePlaceMatName() { return MATERIALS[placeMatIndex]; }

document.addEventListener('keydown', function(e) {
    // Don't capture keys when interacting with panel inputs
    if (e.target && e.target.tagName === 'INPUT') return;

    keys[e.key.toLowerCase()] = true;

    if (e.key === 'Tab') {
        e.preventDefault();
        panel.classList.toggle('hidden');
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
    cam.yaw += e.movementX * camConfig.sensitivity;
    cam.pitch -= e.movementY * camConfig.sensitivity;
    if (cam.pitch >  1.4) cam.pitch =  1.4;
    if (cam.pitch < -1.4) cam.pitch = -1.4;
});

// ============================================================================
// Block picking
// ============================================================================

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
};

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
    },
    'flat': {
        frequency: 0.01, octaves: 2, gain: 0.3, lacunarity: 2.0,
        baseHeight: 20, heightAmplitude: 3, seaLevel: 8, cellSize: 1.0,
        chunkSizeX: 64, chunkSizeY: 48, chunkSizeZ: 64,
        loadRadius: 4, unloadRadius: 6, maxLoadsPerUpdate: 2,
    },
    'mountains': {
        frequency: 0.020, octaves: 8, gain: 0.55, lacunarity: 2.2,
        baseHeight: 48, heightAmplitude: 44, seaLevel: 16, cellSize: 1.0,
        chunkSizeX: 64, chunkSizeY: 96, chunkSizeZ: 64,
        loadRadius: 4, unloadRadius: 6, maxLoadsPerUpdate: 2,
    },
    'islands': {
        frequency: 0.045, octaves: 4, gain: 0.45, lacunarity: 2.0,
        baseHeight: 12, heightAmplitude: 14, seaLevel: 18, cellSize: 1.0,
        chunkSizeX: 64, chunkSizeY: 48, chunkSizeZ: 64,
        loadRadius: 5, unloadRadius: 7, maxLoadsPerUpdate: 2,
    },
    'alien': {
        frequency: 0.08, octaves: 6, gain: 0.70, lacunarity: 3.0,
        baseHeight: 24, heightAmplitude: 20, seaLevel: 6, cellSize: 1.0,
        chunkSizeX: 64, chunkSizeY: 48, chunkSizeZ: 64,
        loadRadius: 4, unloadRadius: 6, maxLoadsPerUpdate: 2,
    },
    'micro': {
        frequency: 0.035, octaves: 5, gain: 0.50, lacunarity: 2.0,
        baseHeight: 18, heightAmplitude: 16, seaLevel: 14, cellSize: 0.25,
        chunkSizeX: 64, chunkSizeY: 48, chunkSizeZ: 64,
        loadRadius: 4, unloadRadius: 6, maxLoadsPerUpdate: 3,
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
            cam.yaw = -Math.PI / 4;
            cam.pitch = -0.30;
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

    // Camera movement
    var moveSpeed = camConfig.speed * dt;
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
        fov: camConfig.fov,
        aspect: w / h,
        near: 0.5,
        far: camConfig.far,
        position: cam.pos,
        target: target,
    });

    info.textContent =
        'pos ' + cam.pos[0].toFixed(1) + ', ' + cam.pos[1].toFixed(1) +
        ', ' + cam.pos[2].toFixed(1) +
        ' | fps ' + fps +
        ' | chunks ' + terrain.chunkCount +
        ' | tris ' + terrain.triangleCount +
        ' | place: ' + activePlaceMatName() +
        ' | seed ' + config.seed;

    requestAnimationFrame(render);
}

requestAnimationFrame(render);
