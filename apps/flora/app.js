// Flora - procedural plant viewer.
// Archetype selector, age slider, seed, mouse-driven orbit camera.

const canvas = document.getElementById('stage');
const scene = canvas.getContext('scene');

scene.setAmbient([0.05, 0.06, 0.07]);
scene.setToneMap({ mode: 'aces', exposure: 1.0 });

// --- Orbit camera ----------------------------------------------------------
const cam = {
    target: [0, 3, 0],
    theta:  Math.PI * 0.25,   // azimuth around Y
    phi:    Math.PI * 0.30,   // polar from +Y axis (0..PI)
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

let dragMode = 0;  // 0 none, 1 orbit, 2 pan
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
        // Pan: move target in screen-aligned plane.
        const sp = Math.sin(cam.phi);
        const right = [-Math.sin(cam.theta), 0, Math.cos(cam.theta)];
        // up_world projected onto camera-up-ish axis.
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

scene.createMesh({
    mesh: 'plane',
    halfW: 10, halfD: 10,
    y: 0,
    color: '#9aa18f',
    metallic: 0.0,
    roughness: 0.95,
    receivesShadow: true,
});

// --- Leaf atlas ------------------------------------------------------------
function buildLeafAtlas(size = 256) {
    const c = document.createElement('canvas');
    c.width = c.height = size;
    const ctx = c.getContext('2d');
    ctx.clearRect(0, 0, size, size);

    const cell = size / 2;
    const palette = [
        ['#3a8c3a', '#1f5c20'],
        ['#6db84a', '#2e6b1f'],
        ['#d6b34a', '#7a5c1c'],
        ['#c2502a', '#5a1d10'],
    ];

    // Important: the radial gradient is constructed in *current* canvas
    // space, so it must be created *after* any translate/rotate — otherwise
    // the gradient sits at canvas origin but the ellipse is drawn in the
    // transformed frame, and the leaf samples a near-transparent edge of
    // the gradient instead of its solid centre.
    for (let i = 0; i < 4; i++) {
        const cx = (i % 2) * cell + cell * 0.5;
        const cy = Math.floor(i / 2) * cell + cell * 0.5;
        const rx = cell * 0.42;
        const ry = cell * 0.30;

        const [hi, lo] = palette[i];

        ctx.save();
        ctx.translate(cx, cy);
        ctx.rotate((i - 1.5) * 0.15);

        const grad = ctx.createRadialGradient(0, -ry * 0.2, 0, 0, 0, rx);
        grad.addColorStop(0.0, hi);
        grad.addColorStop(0.7, lo);
        grad.addColorStop(1.0, 'rgba(0,0,0,0)');

        ctx.beginPath();
        ctx.ellipse(0, 0, rx, ry, 0, 0, Math.PI * 2);
        ctx.fillStyle = grad;
        ctx.fill();

        ctx.strokeStyle = 'rgba(0,0,0,0.25)';
        ctx.lineWidth = Math.max(1, cell * 0.012);
        ctx.beginPath();
        ctx.moveTo(-rx * 0.85, 0);
        ctx.lineTo(rx * 0.85, 0);
        ctx.stroke();
        ctx.restore();
    }

    return ctx.getImageData(0, 0, size, size);
}

const atlas = buildLeafAtlas(256);

// --- Archetype generators --------------------------------------------------
const archetypeDefaults = {
    tree:      { height: 6,   attractorCount: 600 },
    conifer:   { height: 8,   whorlCount: 8, branchesPerWhorl: 6 },
    shrub:     { height: 1.5, radius: 1.2, stemCount: 4, attractorCount: 200 },
    grassTuft: { bladeCount: 12, height: 0.4 },
    vine:      { length: 6, radius: 0.04, helixRadius: 0.5, turns: 3 },
    fern:      { leafletPairs: 14, length: 1.5 },
    succulent: { leafCount: 24, leafLength: 0.35 },
};

const archetypeFns = {
    tree:      (o) => Mesh.tree(o),
    conifer:   (o) => Mesh.conifer(o),
    shrub:     (o) => Mesh.shrub(o),
    grassTuft: (o) => Mesh.grassTuft(o),
    vine:      (o) => Mesh.vine(o),
    fern:      (o) => Mesh.fern(o),
    succulent: (o) => Mesh.succulent(o),
};

function buildPlant(archetype, seed, age01) {
    const opts = Object.assign({ seed, age01 }, archetypeDefaults[archetype]);
    return archetypeFns[archetype](opts);
}

// --- Scene state -----------------------------------------------------------
const archIn  = document.getElementById('archetype');
const ageIn   = document.getElementById('age');
const seedIn  = document.getElementById('seed');
const ageVal  = document.getElementById('ageVal');
const regenBtn = document.getElementById('regen');

// Half-extents — actual quad is 0.6x0.6 in mesh space, then scaled per
// instance by the procedural plant builder. Bigger than a real leaf so
// each instance reads as a small "cluster card" of foliage rather than a
// single needle, which keeps the canopy visible from a comfortable
// orbit distance.
const leafQuad = Mesh.plane(0.3, 0.3);

let trunk = null;
let leaves = null;

function recreateNodes(result) {
    if (trunk)  { trunk.destroy && trunk.destroy(); trunk = null; }
    if (leaves) { leaves.destroy && leaves.destroy(); leaves = null; }

    trunk = scene.createMesh({
        mesh: result.mesh,
        color: [0.45, 0.30, 0.18],
        metallic: 0.0,
        roughness: 0.95,
        castsShadow: true,
        receivesShadow: true,
    });

    leaves = scene.createInstancedMesh({
        mesh: leafQuad,
        instancesFromTransforms: result.leaves,
        texture: atlas,
        atlasCols: 2,
        atlasRows: 2,
        color: [1, 1, 1, 1],
        metallic: 0.0,
        roughness: 0.85,
        alphaCutoff: 0.5,
        doubleSided: true,
        castsShadow: true,
        receivesShadow: true,
    });
    console.log('leaf alphaCutoff:', leaves.alphaCutoff,
                'doubleSided:', leaves.doubleSided,
                'count:', leaves.instanceCount);
}

function fitCameraTo(result) {
    const ax = result.aabbMax || [1, 1, 1];
    const an = result.aabbMin || [-1, 0, -1];
    const cx = (ax[0] + an[0]) * 0.5;
    const cy = (ax[1] + an[1]) * 0.5;
    const cz = (ax[2] + an[2]) * 0.5;
    const sx = ax[0] - an[0], sy = ax[1] - an[1], sz = ax[2] - an[2];
    const ext = Math.max(sx, sy, sz);
    cam.target = [cx, cy, cz];
    cam.radius = Math.max(1.0, ext * 2.2);
    applyCamera();
}

function regenerate() {
    const archetype = archIn.value || 'tree';
    const seed = parseInt(seedIn.value, 10) || 1;
    const age  = parseFloat(ageIn.value);
    ageVal.textContent = age.toFixed(2);

    const t0 = performance.now();
    const result = buildPlant(archetype, seed, age);
    const ms = performance.now() - t0;
    console.log('regen', archetype, ms.toFixed(2), 'ms,', result.leafCount, 'leaves');
    document.getElementById('stats').textContent =
        `${archetype} - ${ms.toFixed(2)} ms - ${result.leafCount} leaves`;

    recreateNodes(result);
    return result;
}

const initial = regenerate();
fitCameraTo(initial);

// One-shot smoke check on first load: same-seed monotonicity + archetype
// switching survives without exception. Cheap to run; helpful diagnostic.
(function smoke() {
    const a = Mesh.tree({ seed: 1, attractorCount: 600, age01: 0.3 }).mesh.vertexCount;
    const b = Mesh.tree({ seed: 1, attractorCount: 600, age01: 0.6 }).mesh.vertexCount;
    const c = Mesh.tree({ seed: 1, attractorCount: 600, age01: 1.0 }).mesh.vertexCount;
    console.log('topology:', a, '<=', b, '<=', c, (a <= b && b <= c) ? 'OK' : 'FAIL');
    for (const k of Object.keys(archetypeFns)) {
        const r = archetypeFns[k]({ seed: 3 });
        console.log('arch', k, 'verts=', r.mesh.vertexCount, 'leaves=', r.leafCount || 0);
    }
})();

archIn.addEventListener('change', () => {
    const r = regenerate();
    fitCameraTo(r);
});
ageIn.addEventListener('input', regenerate);
seedIn.addEventListener('change', regenerate);
regenBtn.addEventListener('click', regenerate);
