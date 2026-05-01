// Flora - procedural plant viewer with full per-archetype customization.
// Mode "single" shows one plant; mode "forest" places many with optional
// canopy sharing (forest controls land in a follow-up commit).

const canvas = document.getElementById('stage');
const scene = canvas.getContext('scene');

scene.setAmbient([0.05, 0.06, 0.07]);
scene.setToneMap({ mode: 'aces', exposure: 1.0 });

// --- Orbit camera ----------------------------------------------------------
const cam = {
    target: [0, 3, 0],
    theta:  Math.PI * 0.25,
    phi:    Math.PI * 0.30,
    radius: 14,
    fov:    50,
    near:   0.1,
    far:    400,
};

function applyCamera() {
    const sp = Math.sin(cam.phi), cp = Math.cos(cam.phi);
    const st = Math.sin(cam.theta), ct = Math.cos(cam.theta);
    const eye = [
        cam.target[0] + cam.radius * sp * ct,
        cam.target[1] + cam.radius * cp,
        cam.target[2] + cam.radius * sp * st,
    ];
    scene.setCamera({
        position: eye,
        target:   cam.target,
        up:       [0, 1, 0],
        fov: cam.fov, near: cam.near, far: cam.far,
    });
}
applyCamera();

let dragMode = 0;
let lastX = 0, lastY = 0;
canvas.addEventListener('mousedown', (e) => {
    lastX = e.clientX; lastY = e.clientY;
    if (e.button === 2 || e.shiftKey) dragMode = 2;
    else if (e.button === 0)          dragMode = 1;
    e.preventDefault();
});
window.addEventListener('mouseup', () => { dragMode = 0; });
canvas.addEventListener('contextmenu', (e) => e.preventDefault());
canvas.addEventListener('mousemove', (e) => {
    if (!dragMode) return;
    const dx = e.clientX - lastX;
    const dy = e.clientY - lastY;
    lastX = e.clientX; lastY = e.clientY;
    if (dragMode === 1) {
        cam.theta += dx * 0.01;
        cam.phi   += dy * 0.01;
        const eps = 0.05;
        if (cam.phi < eps) cam.phi = eps;
        if (cam.phi > Math.PI - eps) cam.phi = Math.PI - eps;
    } else {
        const sp = Math.sin(cam.phi);
        const right = [-Math.sin(cam.theta), 0, Math.cos(cam.theta)];
        const fwd  = [sp * Math.cos(cam.theta), Math.cos(cam.phi), sp * Math.sin(cam.theta)];
        const up = [
            -fwd[1] * right[2],
            right[0] * fwd[2] - right[2] * fwd[0],
            -right[0] * fwd[1],
        ];
        const k = cam.radius * 0.0015;
        cam.target[0] += (-right[0] * dx + up[0] * dy) * k;
        cam.target[1] += (-right[1] * dx + up[1] * dy) * k;
        cam.target[2] += (-right[2] * dx + up[2] * dy) * k;
    }
    applyCamera();
});
canvas.addEventListener('wheel', (e) => {
    const f = Math.exp(e.deltaY * 0.001);
    cam.radius *= f;
    if (cam.radius < 0.5) cam.radius = 0.5;
    if (cam.radius > 500) cam.radius = 500;
    applyCamera();
    e.preventDefault();
}, { passive: false });

// --- Lights & ground -------------------------------------------------------
scene.createLight({
    type: 'directional',
    direction: [-0.4, -0.8, -0.3],
    color: [1.0, 0.97, 0.92],
    intensity: 2.0,
    castsShadow: true,
});

const groundPlane = scene.createMesh({
    mesh: 'plane',
    halfW: 10, halfD: 10,
    y: 0,
    color: '#9aa18f',
    metallic: 0.0,
    roughness: 0.95,
    receivesShadow: true,
});

function resizeGround(half) {
    if (groundPlane && groundPlane.destroy) groundPlane.destroy();
    return scene.createMesh({
        mesh: 'plane',
        halfW: half, halfD: half,
        y: 0,
        color: '#9aa18f',
        metallic: 0.0,
        roughness: 0.95,
        receivesShadow: true,
    });
}

// --- Parameter schema ------------------------------------------------------
//
// Drives the dynamic per-archetype UI. Each entry is one of:
//   { key, label, type: 'range', min, max, step, default, fmt? }
//   { key, label, type: 'select', options: [...], default }
//   { key, label, type: 'color', default }   (hex string)
//   { key, label, type: 'int', min, max, default }
// `fmt` formats the value readout (default: 2 decimals).

const fmtInt = (v) => `${v | 0}`;
const fmt2 = (v) => v.toFixed(2);
const fmt3 = (v) => v.toFixed(3);

const archetypeSchema = {
    tree: [
        { key: 'height',         label: 'height',         type: 'range', min: 1.5, max: 14, step: 0.1,  default: 6,    fmt: fmt2 },
        { key: 'trunkRadius',    label: 'trunk radius',   type: 'range', min: 0.05, max: 0.6, step: 0.01, default: 0.18, fmt: fmt2 },
        { key: 'canopyRadius',   label: 'canopy radius',  type: 'range', min: 0.5, max: 6, step: 0.1,   default: 3,    fmt: fmt2 },
        { key: 'canopyShape',    label: 'canopy shape',   type: 'select', options: Recipes.CANOPY_SHAPES, default: 'round' },
        { key: 'blobCount',      label: 'blob count',     type: 'int',   min: 1, max: 7, default: 3 },
        { key: 'canopyColor',    label: 'canopy color',   type: 'color', default: '#4f8c39' },
    ],
    conifer: [
        { key: 'height',           label: 'height',          type: 'range', min: 2, max: 16, step: 0.1, default: 8, fmt: fmt2 },
        { key: 'trunkRadius',      label: 'trunk radius',    type: 'range', min: 0.04, max: 0.4, step: 0.01, default: 0.15, fmt: fmt2 },
        { key: 'layers',           label: 'cone layers',     type: 'int',   min: 3, max: 12, default: 7 },
        { key: 'baseCanopyRadius', label: 'base radius',     type: 'range', min: 0.5, max: 5, step: 0.1, default: 2.5, fmt: fmt2 },
        { key: 'canopyColor',      label: 'needle color',    type: 'color', default: '#2e6633' },
    ],
    shrub: [
        { key: 'height',     label: 'height',      type: 'range', min: 0.4, max: 3, step: 0.05, default: 1.5, fmt: fmt2 },
        { key: 'radius',     label: 'radius',      type: 'range', min: 0.3, max: 2.5, step: 0.05, default: 1.2, fmt: fmt2 },
        { key: 'blobCount',  label: 'blob count',  type: 'int',   min: 2, max: 9, default: 5 },
        { key: 'canopyColor',label: 'canopy color',type: 'color', default: '#52943d' },
    ],
    grassTuft: [
        { key: 'bladeCount', label: 'blades',      type: 'int', min: 3, max: 30, default: 12 },
        { key: 'height',     label: 'height',      type: 'range', min: 0.1, max: 1, step: 0.02, default: 0.4, fmt: fmt2 },
        { key: 'baseRadius', label: 'base radius', type: 'range', min: 0.02, max: 0.3, step: 0.01, default: 0.08, fmt: fmt2 },
        { key: 'bladeWidth', label: 'blade width', type: 'range', min: 0.005, max: 0.04, step: 0.001, default: 0.012, fmt: fmt3 },
        { key: 'bend',       label: 'bend',        type: 'range', min: 0, max: 1.5, step: 0.05, default: 0.6, fmt: fmt2 },
    ],
    vine: [
        { key: 'length',      label: 'length',       type: 'range', min: 1, max: 12, step: 0.2, default: 6, fmt: fmt2 },
        { key: 'radius',      label: 'stem radius',  type: 'range', min: 0.01, max: 0.15, step: 0.005, default: 0.04, fmt: fmt3 },
        { key: 'helixRadius', label: 'helix radius', type: 'range', min: 0.1, max: 1.5, step: 0.05, default: 0.5, fmt: fmt2 },
        { key: 'turns',       label: 'turns',        type: 'range', min: 0.5, max: 8, step: 0.25, default: 3, fmt: fmt2 },
    ],
    fern: [
        { key: 'leafletPairs',  label: 'leaflet pairs', type: 'int',   min: 4, max: 30, default: 14 },
        { key: 'length',        label: 'length',        type: 'range', min: 0.5, max: 3, step: 0.1, default: 1.5, fmt: fmt2 },
        { key: 'stemRadius',    label: 'stem radius',   type: 'range', min: 0.005, max: 0.04, step: 0.001, default: 0.012, fmt: fmt3 },
        { key: 'leafletLength', label: 'leaflet length',type: 'range', min: 0.1, max: 0.7, step: 0.02, default: 0.32, fmt: fmt2 },
        { key: 'curvature',     label: 'curvature',     type: 'range', min: 0.2, max: 3, step: 0.1, default: 1.4, fmt: fmt2 },
    ],
    succulent: [
        { key: 'leafCount',     label: 'leaf count',    type: 'int',   min: 5, max: 60, default: 24 },
        { key: 'leafLength',    label: 'leaf length',   type: 'range', min: 0.1, max: 0.7, step: 0.02, default: 0.35, fmt: fmt2 },
        { key: 'leafWidth',     label: 'leaf width',    type: 'range', min: 0.02, max: 0.18, step: 0.005, default: 0.06, fmt: fmt3 },
        { key: 'leafThickness', label: 'leaf thickness',type: 'range', min: 0.005, max: 0.06, step: 0.002, default: 0.02, fmt: fmt3 },
        { key: 'tilt',          label: 'tilt',          type: 'range', min: 0, max: 1.4, step: 0.05, default: 0.6, fmt: fmt2 },
    ],
};

// Common controls always shown above per-archetype controls.
const commonSchema = [
    { key: 'archetype', label: 'type',  type: 'select',
      options: Object.keys(archetypeSchema), default: 'tree' },
    { key: 'age',       label: 'age',   type: 'range', min: 0, max: 1, step: 0.01, default: 1, fmt: fmt2 },
    { key: 'seed',      label: 'seed',  type: 'int',   min: 0, max: 99999, default: 1 },
];

const forestSchema = [
    { key: 'archetype', label: 'type',  type: 'select',
      options: Object.keys(archetypeSchema), default: 'tree' },
    { key: 'count',     label: 'count', type: 'int', min: 1, max: 80, default: 18 },
    { key: 'patchSize', label: 'patch size', type: 'range', min: 6, max: 50, step: 0.5, default: 18, fmt: fmt2 },
    { key: 'jitter',    label: 'jitter', type: 'range', min: 0, max: 1, step: 0.02, default: 0.7, fmt: fmt2 },
    { key: 'sharing',   label: 'canopy sharing', type: 'range', min: 0, max: 1, step: 0.02, default: 0.6, fmt: fmt2 },
    { key: 'sizeJitter',label: 'size jitter', type: 'range', min: 0, max: 1, step: 0.02, default: 0.5, fmt: fmt2 },
    { key: 'shapeMix',  label: 'shape mix', type: 'select',
      options: ['round-only', 'broadleaf-mix', 'all-shapes'], default: 'broadleaf-mix' },
    { key: 'age',       label: 'age',   type: 'range', min: 0, max: 1, step: 0.01, default: 1, fmt: fmt2 },
    { key: 'seed',      label: 'seed',  type: 'int',   min: 0, max: 99999, default: 1 },
];

// --- Mode + parameter state -----------------------------------------------

let mode = 'single';
const state = {};      // current parameter values
const inputs = {};     // DOM input refs by key

function hexToRgb(hex) {
    const m = /^#?([a-f\d]{2})([a-f\d]{2})([a-f\d]{2})$/i.exec(hex);
    if (!m) return [0.4, 0.6, 0.3];
    return [parseInt(m[1], 16) / 255, parseInt(m[2], 16) / 255, parseInt(m[3], 16) / 255];
}

function setDefaults() {
    const archSchema = archetypeSchema[state.archetype || 'tree'] || [];
    const schema = mode === 'single'
        ? commonSchema.concat(archSchema)
        : forestSchema;
    for (const f of schema) {
        if (state[f.key] === undefined) state[f.key] = f.default;
    }
}

function clearStateForArchetypeSwitch() {
    // When user switches archetype/mode, drop archetype-specific keys so
    // their defaults reload. Keep common keys (age, seed).
    const keep = new Set(['archetype', 'age', 'seed', 'count', 'patchSize',
        'jitter', 'sharing', 'sizeJitter', 'shapeMix']);
    for (const k of Object.keys(state)) {
        if (!keep.has(k)) delete state[k];
    }
}

function buildPanel() {
    const params = document.getElementById('params');
    params.innerHTML = '';
    inputs.clear && inputs.clear();
    for (const k of Object.keys(inputs)) delete inputs[k];

    const archSchema = archetypeSchema[state.archetype || 'tree'] || [];
    const schema = mode === 'single'
        ? commonSchema.concat(archSchema)
        : forestSchema;

    let currentSection = null;
    for (const f of schema) {
        if (f.key === 'archetype' && currentSection === null) {
            const h = document.createElement('h2');
            h.textContent = mode === 'single' ? 'plant' : 'forest';
            params.appendChild(h);
            currentSection = 'plant';
        }
        if (f.key === 'count' && currentSection !== 'forest') {
            currentSection = 'forest';
        }
        if (f.key === 'age' && currentSection !== 'common') {
            const h = document.createElement('h2');
            h.textContent = 'common';
            params.appendChild(h);
            currentSection = 'common';
        }
        if (mode === 'single' && f === archSchema[0]) {
            const h = document.createElement('h2');
            h.textContent = state.archetype;
            params.appendChild(h);
            currentSection = 'arch';
        }
        params.appendChild(buildRow(f));
    }
}

function buildRow(f) {
    const row = document.createElement('div');
    row.className = 'row';
    const lab = document.createElement('label');
    lab.textContent = f.label;
    row.appendChild(lab);

    const val = state[f.key];
    if (f.type === 'range') {
        const inp = document.createElement('input');
        inp.type = 'range';
        inp.min = f.min; inp.max = f.max; inp.step = f.step;
        inp.value = val;
        const out = document.createElement('span');
        out.className = 'v';
        const fmt = f.fmt || fmt2;
        out.textContent = fmt(parseFloat(val));
        inp.addEventListener('input', () => {
            const v = parseFloat(inp.value);
            state[f.key] = v;
            out.textContent = fmt(v);
            scheduleRegen();
        });
        row.appendChild(inp);
        row.appendChild(out);
        inputs[f.key] = inp;
    } else if (f.type === 'int') {
        const inp = document.createElement('input');
        inp.type = 'number';
        inp.min = f.min; inp.max = f.max; inp.step = 1;
        inp.value = val;
        inp.addEventListener('change', () => {
            const v = parseInt(inp.value, 10);
            state[f.key] = isNaN(v) ? f.default : v;
            scheduleRegen();
        });
        row.appendChild(inp);
        inputs[f.key] = inp;
    } else if (f.type === 'select') {
        const sel = document.createElement('select');
        for (const opt of f.options) {
            const o = document.createElement('option');
            o.value = opt; o.textContent = opt;
            if (opt === val) o.selected = true;
            sel.appendChild(o);
        }
        sel.addEventListener('change', () => {
            state[f.key] = sel.value;
            if (f.key === 'archetype') {
                clearStateForArchetypeSwitch();
                state.archetype = sel.value;
                setDefaults();
                buildPanel();
                regenerate(true);
            } else {
                regenerate(false);
            }
        });
        row.appendChild(sel);
        inputs[f.key] = sel;
    } else if (f.type === 'color') {
        const inp = document.createElement('input');
        inp.type = 'color';
        inp.value = val;
        inp.addEventListener('input', () => {
            state[f.key] = inp.value;
            scheduleRegen();
        });
        row.appendChild(inp);
        inputs[f.key] = inp;
    }
    return row;
}

// --- Plant generation ------------------------------------------------------

let plantNodes = [];

function destroyNodes() {
    for (const n of plantNodes) { n.destroy && n.destroy(); }
    plantNodes = [];
}

function spawnPart(part, tx, ty, tz) {
    if (!part.mesh) return null;
    return scene.createMesh({
        data: part.mesh,
        x: tx || 0, y: ty || 0, z: tz || 0,
        color: part.color || [0.6, 0.6, 0.6],
        metallic: part.metallic ?? 0.0,
        roughness: part.roughness ?? 0.9,
        castsShadow: true,
        receivesShadow: true,
    });
}

function buildSinglePlantOpts() {
    const archSchema = archetypeSchema[state.archetype] || [];
    const opts = { seed: state.seed | 0, age01: state.age };
    for (const f of archSchema) {
        let v = state[f.key];
        if (f.type === 'color') v = hexToRgb(v);
        opts[f.key] = v;
    }
    return opts;
}

function regenerateSingle() {
    destroyNodes();
    const t0 = performance.now();
    const opts = buildSinglePlantOpts();
    const result = Recipes[state.archetype](opts);
    const ms = performance.now() - t0;
    let partCount = 0;
    if (result.parts) {
        for (const p of result.parts) {
            const node = spawnPart(p, 0, 0, 0);
            if (node) { plantNodes.push(node); partCount++; }
        }
    }
    document.getElementById('stats').textContent =
        `${state.archetype} · ${ms.toFixed(1)} ms · ${partCount} parts`;
    return result;
}

function fitCameraToBounds(min, max) {
    const cx = (max[0] + min[0]) * 0.5;
    const cy = (max[1] + min[1]) * 0.5;
    const cz = (max[2] + min[2]) * 0.5;
    const sx = max[0] - min[0], sy = max[1] - min[1], sz = max[2] - min[2];
    const ext = Math.max(sx, sy, sz);
    cam.target = [cx, cy, cz];
    cam.radius = Math.max(1.0, ext * 2.0);
    applyCamera();
}

// Forest mode is implemented in the next commit; this stub regenerates a
// single plant so the tab stays functional.
function regenerateForest() {
    return regenerateSingle();
}

let regenTimer = null;
function scheduleRegen() {
    if (regenTimer) return;
    regenTimer = setTimeout(() => { regenTimer = null; regenerate(false); }, 16);
}

let needFitCamera = true;
function regenerate(refit) {
    const result = mode === 'single' ? regenerateSingle() : regenerateForest();
    if ((refit || needFitCamera) && result && result.aabbMin && result.aabbMax) {
        fitCameraToBounds(result.aabbMin, result.aabbMax);
        needFitCamera = false;
    }
}

// --- Tabs + actions --------------------------------------------------------

document.querySelectorAll('.tab').forEach((t) => {
    t.addEventListener('click', () => {
        const next = t.getAttribute('data-mode');
        if (next === mode) return;
        document.querySelectorAll('.tab').forEach((x) => x.classList.remove('active'));
        t.classList.add('active');
        mode = next;
        // Reset state so each mode picks up its own defaults.
        for (const k of Object.keys(state)) delete state[k];
        setDefaults();
        buildPanel();
        needFitCamera = true;
        regenerate(true);
    });
});

document.getElementById('regen').addEventListener('click', () => regenerate(false));
document.getElementById('reseed').addEventListener('click', () => {
    state.seed = (Math.random() * 99999) | 0;
    if (inputs.seed) inputs.seed.value = state.seed;
    regenerate(false);
});
document.getElementById('reset').addEventListener('click', () => {
    for (const k of Object.keys(state)) delete state[k];
    setDefaults();
    buildPanel();
    needFitCamera = true;
    regenerate(true);
});

// --- Boot ------------------------------------------------------------------

setDefaults();
buildPanel();
regenerate(true);
