const canvas = document.getElementById('stage');
const scene = canvas.getContext('scene');

scene.setCamera({
    fov: 50,
    position: [0, 4, 10],
    target: [0, 1, 0],
});
scene.setAmbient([0.03, 0.03, 0.035]);
scene.setToneMap({ mode: 'aces', exposure: 1.0 });

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
    direction: [-0.4, -1.0, -0.3],
    color: [1.0, 0.98, 0.92],
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
modeSel.addEventListener('change', applyToneMap);
exposureIn.addEventListener('input', applyToneMap);
sunIn.addEventListener('input', applySun);
ambientIn.addEventListener('input', applyAmbient);

// --- Animation loop --------------------------------------------------------
let t0 = performance.now();
function frame() {
    const t = (performance.now() - t0) / 1000.0;
    const r = 2.5;
    p1.x = Math.cos(t * 0.9) * r;
    p1.z = Math.sin(t * 0.9) * r;
    p2.x = Math.cos(t * 0.9 + Math.PI * 2/3) * r;
    p2.z = Math.sin(t * 0.9 + Math.PI * 2/3) * r;
    p3.x = Math.cos(t * 0.9 + Math.PI * 4/3) * r;
    p3.z = Math.sin(t * 0.9 + Math.PI * 4/3) * r;

    // Spot sweeps across the scene.
    const sx = Math.sin(t * 0.4) * 3.0;
    spot.x = sx;
    spot.direction = [-sx * 0.15, -1, 0];

    requestAnimationFrame(frame);
}
requestAnimationFrame(frame);
