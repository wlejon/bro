const canvas = document.getElementById('stage');
const scene = canvas.getContext('scene');

// Orbit camera around the sphere row at world-Y=1. Right-drag orbits,
// middle-drag pans, wheel zooms. Left-click stays free for the
// light-icon picker below.
const cam = Camera.createOrbit({
    target: [0, 1, 0],
    dist:   10,
    fov:    50,
    near:   0.1,
    far:    200,
});

scene.setAmbient([0.03, 0.03, 0.035]);
scene.setToneMap({ mode: 'aces', exposure: 1.0 });
scene.showLightIcons = true;

// Ground plane — rough dielectric, neutral gray.
scene.createMesh({
    mesh: 'plane',
    halfW: 12, halfD: 12,
    y: 0,
    color: '#353a40',
    metallic: 0.0,
    roughness: 0.9,
});

// Two rows of spheres. Row 1 = dielectric (metallic=0), Row 2 = metal.
// Roughness sweeps 0.05 -> 0.95 across the row.
const N = 7;
const spacing = 1.3;
const xStart = -((N - 1) * spacing) / 2;

for (let i = 0; i < N; ++i) {
    const t = i / (N - 1);
    const roughness = 0.05 + t * 0.9;
    // Dielectric row (plastic-like) — base color is the test swatch.
    scene.createMesh({
        mesh: 'sphere',
        radius: 0.5,
        segments: 32,
        rings: 24,
        x: xStart + i * spacing,
        y: 0.6,
        z: 1.2,
        color: '#d06050',
        metallic: 0.0,
        roughness,
    });
    // Metal row — baseColor tints the reflection (F0 = baseColor when metallic=1).
    scene.createMesh({
        mesh: 'sphere',
        radius: 0.5,
        segments: 32,
        rings: 24,
        x: xStart + i * spacing,
        y: 0.6,
        z: -1.2,
        color: '#e0c060',   // gold-ish
        metallic: 1.0,
        roughness,
    });
}

// Glowing emissive accent bar behind the spheres.
scene.createMesh({
    mesh: 'box',
    halfW: 5, halfH: 0.1, halfD: 0.1,
    x: 0, y: 2.2, z: -3,
    color: '#ffffff',
    emissive: 3.0,
    emissiveColor: [0.5, 0.8, 1.0],
    metallic: 0.0,
    roughness: 1.0,
});

// --- Lights ----------------------------------------------------------------
// Directional "sun" — the primary key light.
const sun = scene.createLight({
    type: 'directional',
    position: [-4, 4.5, 2],          // purely cosmetic; directional lights
    direction: [-0.4, -1.0, -0.3],   // are infinite-distance, position only
    color: [1.0, 0.98, 0.92],        // affects icon placement
    intensity: 3.0,
    name: 'sun',
});

// Three orbiting point lights (RGB).
const p1 = scene.createLight({
    type: 'point',
    position: [2, 1.5, 0],
    color: [1.0, 0.2, 0.2],
    intensity: 12,
    range: 5,
});
const p2 = scene.createLight({
    type: 'point',
    position: [-2, 1.5, 0],
    color: [0.2, 1.0, 0.3],
    intensity: 12,
    range: 5,
});
const p3 = scene.createLight({
    type: 'point',
    position: [0, 1.5, 2],
    color: [0.3, 0.5, 1.0],
    intensity: 12,
    range: 5,
});

// A sweeping yellow spot light from above to show the cone falloff.
const spot = scene.createLight({
    type: 'spot',
    position: [0, 5, 0],
    direction: [0, -1, 0],
    color: [1.0, 0.9, 0.6],
    intensity: 40,
    range: 10,
    innerAngle: 0.25,
    outerAngle: 0.45,
});

// --- Selection + gizmo -----------------------------------------------------
// Click a light icon → attach bro.gizmo to that node. Directional lights
// use rotate mode (direction vector); point/spot use translate (position).
let selected = null;     // SceneNode or null
let selKind  = null;     // "directional" | "point" | "spot" | null
const selInfo = document.getElementById('selInfo');

function describe(node, kind) {
    if (!node) return 'Click a light icon to select.';
    const n = node.name || `light #${node.id}`;
    return `Selected: ${n} (${kind}) — drag gizmo handles to move.`;
}

function attachGizmoFor(node, kind) {
    selected = node;
    selKind  = kind;
    selInfo.textContent = describe(node, kind);

    if (!node) { bro.gizmo.detach(); return; }

    if (kind === 'directional') {
        // Represent direction as a rotation: treat the node's scene transform
        // as the source of truth. We update `direction` from the node's local
        // rotation each frame (applied to -Y = default "down" direction).
        bro.gizmo.setMode('rotate');
        bro.gizmo.attach({
            position:    () => [node.x, node.y, node.z],
            orientation: () => [0, 0, 0, 1],   // identity; rotate in world space
            rotate: (qx, qy, qz, qw) => {
                // Rotate the current direction vector by the quaternion delta.
                const d = node.direction;
                // q * (0,d) * q^-1 — quaternion rotation of vec3 d
                const ix =  qw*d[0] + qy*d[2] - qz*d[1];
                const iy =  qw*d[1] + qz*d[0] - qx*d[2];
                const iz =  qw*d[2] + qx*d[1] - qy*d[0];
                const iw = -qx*d[0] - qy*d[1] - qz*d[2];
                node.direction = [
                    ix*qw + iw*(-qx) + iy*(-qz) - iz*(-qy),
                    iy*qw + iw*(-qy) + iz*(-qx) - ix*(-qz),
                    iz*qw + iw*(-qz) + ix*(-qy) - iy*(-qx),
                ];
            },
        });
    } else {
        bro.gizmo.setMode('translate');
        bro.gizmo.attach({
            position: () => [node.x, node.y, node.z],
            translate: (dx, dy, dz) => {
                node.x += dx; node.y += dy; node.z += dz;
            },
        });
    }
}

function pickLightAt(clientX, clientY) {
    const rect = canvas.getBoundingClientRect();
    const lx = clientX - rect.left;
    const ly = clientY - rect.top;
    const ray = scene.unprojectLocal(lx, ly);
    if (!ray) return null;
    const hit = scene.raycast(ray.origin, ray.dir, 100);
    return hit ? hit.node : null;
}

canvas.addEventListener('mousedown', (ev) => {
    if (ev.button !== 0) return;  // right/middle reserved for camera below
    // Let the gizmo consume clicks on its handles first (it hit-tests
    // before any other listener via the engine routing layer). If the
    // click wasn't on a handle, try selecting a light.
    if (bro.gizmo.dragging) return;
    const node = pickLightAt(ev.clientX, ev.clientY);
    if (!node) {
        if (selected) attachGizmoFor(null);
        return;
    }
    if (node.type !== 'light') return;  // only lights are selectable in this demo
    attachGizmoFor(node, node.kind);
});

// --- Camera input (right=orbit, middle=pan, wheel=zoom) ---------------------
let rightDown = false, middleDown = false;
function updatePointerLock() {
    const want = rightDown || middleDown;
    const locked = document.pointerLockElement === canvas;
    if (want && !locked) canvas.requestPointerLock();
    else if (!want && locked) document.exitPointerLock();
}
canvas.addEventListener('mousedown', (e) => {
    if (e.button === 2)      { rightDown  = true; e.preventDefault(); updatePointerLock(); }
    else if (e.button === 1) { middleDown = true; e.preventDefault(); updatePointerLock(); }
});
document.addEventListener('mouseup', (e) => {
    if (e.button === 2) rightDown  = false;
    if (e.button === 1) middleDown = false;
    updatePointerLock();
});
document.addEventListener('mousemove', (e) => {
    if (rightDown)  Camera.orbitLook(cam, e.movementX, e.movementY);
    if (middleDown) Camera.orbitPan (cam, e.movementX, e.movementY);
});
canvas.addEventListener('contextmenu', (e) => e.preventDefault());
canvas.addEventListener('auxclick', (e) => { if (e.button === 1) e.preventDefault(); });
canvas.addEventListener('wheel', (e) => {
    cam.dist = Math.max(0.5, cam.dist * Math.exp(e.deltaY * 0.001));
    e.preventDefault();
});

document.addEventListener('keydown', (ev) => {
    if (ev.key === 'Escape') attachGizmoFor(null);
});

// --- HUD wiring ------------------------------------------------------------
const modeSel = document.getElementById('mode');
const exposureIn = document.getElementById('exposure');
const exposureVal = document.getElementById('exposureVal');
const sunIn = document.getElementById('sun');
const sunVal = document.getElementById('sunVal');
const ambientIn = document.getElementById('ambient');
const ambientVal = document.getElementById('ambientVal');

function applyToneMap() {
    scene.setToneMap({
        mode: modeSel.value,
        exposure: parseFloat(exposureIn.value),
        gamma: 2.2,
    });
    exposureVal.textContent = parseFloat(exposureIn.value).toFixed(2);
}
function applySun() {
    sun.intensity = parseFloat(sunIn.value);
    sunVal.textContent = sun.intensity.toFixed(1);
}
function applyAmbient() {
    const a = parseFloat(ambientIn.value);
    scene.setAmbient([a, a, a]);
    ambientVal.textContent = a.toFixed(3);
}
const showIconsIn = document.getElementById('showIcons');
const animateIn = document.getElementById('animate');
const shadowsIn = document.getElementById('shadows');
const hdriSel = document.getElementById('hdri');
const envStatus = document.getElementById('envStatus');
const iblIntensityIn = document.getElementById('iblIntensity');
const iblIntensityVal = document.getElementById('iblIntensityVal');
const iblRotationIn = document.getElementById('iblRotation');
const iblRotationVal = document.getElementById('iblRotationVal');

// Try the script's default first, then fall back through the other tiers
// the user might have on disk. If nothing loads, prompt them to run the
// download script — the .hdr files are .gitignored so a fresh checkout
// won't have them yet.
const HDRI_RES_ORDER = ['2k', '4k', '1k', '8k'];
function applyEnvironment() {
    const slug = hdriSel.value;
    if (!slug) {
        scene.setEnvironment(null);
        envStatus.textContent = '';
        return;
    }
    const opts = {
        intensity: parseFloat(iblIntensityIn.value),
        rotation:  parseFloat(iblRotationIn.value),
    };
    for (const res of HDRI_RES_ORDER) {
        const path = `apps/lighting-demo/hdri/${slug}_${res}.hdr`;
        if (scene.setEnvironment({ ...opts, hdr: path })) {
            envStatus.textContent = `loaded ${res}`;
            envStatus.style.color = '#7bed9f';
            return;
        }
    }
    envStatus.textContent = 'HDRI missing — run apps/lighting-demo/hdri/download.sh';
    envStatus.style.color = '#fd9';
}
function applyIBLIntensity() {
    const v = parseFloat(iblIntensityIn.value);
    iblIntensityVal.textContent = v.toFixed(2);
    if (hdriSel.value) scene.setEnvironment({ intensity: v });
}
function applyIBLRotation() {
    const v = parseFloat(iblRotationIn.value);
    iblRotationVal.textContent = v.toFixed(2);
    if (hdriSel.value) scene.setEnvironment({ rotation: v });
}

modeSel.addEventListener('change', applyToneMap);
exposureIn.addEventListener('input', applyToneMap);
sunIn.addEventListener('input', applySun);
ambientIn.addEventListener('input', applyAmbient);
hdriSel.addEventListener('change', applyEnvironment);
iblIntensityIn.addEventListener('input', applyIBLIntensity);
iblRotationIn.addEventListener('input', applyIBLRotation);
showIconsIn.addEventListener('change', () => {
    scene.showLightIcons = showIconsIn.checked;
    if (!showIconsIn.checked) attachGizmoFor(null);
});
shadowsIn.addEventListener('change', () => {
    sun.castsShadow = shadowsIn.checked;
});

// --- Animation loop --------------------------------------------------------
// Animation pauses whenever a light is selected so the gizmo can
// actually drag it somewhere; resumes when deselected.
let t0 = performance.now();
function frame() {
    scene.setCamera(Camera.orbitViewOpts(cam, canvas));

    const animate = animateIn.checked && !selected;
    if (animate) {
        const t = (performance.now() - t0) / 1000.0;
        const r = 2.5;
        p1.x = Math.cos(t * 0.9) * r;
        p1.z = Math.sin(t * 0.9) * r;
        p2.x = Math.cos(t * 0.9 + Math.PI * 2/3) * r;
        p2.z = Math.sin(t * 0.9 + Math.PI * 2/3) * r;
        p3.x = Math.cos(t * 0.9 + Math.PI * 4/3) * r;
        p3.z = Math.sin(t * 0.9 + Math.PI * 4/3) * r;
        const sx = Math.sin(t * 0.4) * 3.0;
        spot.x = sx;
        spot.direction = [-sx * 0.15, -1, 0];
    }

    requestAnimationFrame(frame);
}
requestAnimationFrame(frame);
