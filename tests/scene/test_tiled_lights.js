// Verifies tiled local light culling (> 32 bounded non-shadow Point/Spot
// lights) and that bounded explicit lights prevent implicitSun_ fallback.

function samplePixel(img, x, y) {
    const i = (y * img.width + x) * 4;
    return [img.data[i], img.data[i + 1], img.data[i + 2]];
}

function avgPatch(img, cx, cy, r) {
    let sr = 0, sg = 0, sb = 0, n = 0;
    for (let y = cy - r; y <= cy + r; y++) {
        for (let x = cx - r; x <= cx + r; x++) {
            const [pr, pg, pb] = samplePixel(img, x, y);
            sr += pr; sg += pg; sb += pb; n++;
        }
    }
    return { r: sr / n, g: sg / n, b: sb / n };
}

const cv = document.createElement('canvas');
cv.setAttribute('width', '256');
cv.setAttribute('height', '256');
document.body.appendChild(cv);
flush();

const scn = cv.getContext('scene');
if (!scn) {
    missingGpuContext('scene');
} else {
    scn.setToneMap({ mode: 'linear', gamma: 1.0 });
    scn.setAmbient({ color: [0, 0, 0], intensity: 0 });
    scn.setCamera({ fov: 60, near: 0.1, far: 200, position: [0, 32, 34], target: [0, 0, 0] });

    // Large dark matte floor inside [-40, 40]^2.
    scn.createMesh({ mesh: Mesh.plane(80, 80), color: [0.85, 0.85, 0.85], roughness: 0.9, castsShadow: false });

    // Spawn 256 bounded non-shadow lights across the floor (indices 0..255).
    // Even lights are Point, odd lights are Spot aimed downward. Put distinct
    // bright green tint on indices >= 192 (well past the 32-light UBO limit)
    // near the bottom-right quadrant of the view so we verify late indices render.
    const cols = 16;
    const spacing = 2.6;
    for (let i = 0; i < 256; i++) {
        const gx = (i % cols) - (cols - 1) * 0.5;
        const gz = Math.floor(i / cols) - (cols - 1) * 0.5;
        const x = gx * spacing;
        const z = gz * spacing;
        const isSpot = (i & 1) === 1;
        const lateGreen = i >= 192;
        scn.createLight({
            type: isSpot ? 'spot' : 'point',
            position: [x, 1.8, z],
            direction: [0, -1, 0],
            color: lateGreen ? [0.1, 1.0, 0.2] : [1.0, 0.6, 0.2],
            intensity: 45,
            range: 4.5,
            innerAngle: 0.4,
            outerAngle: 0.85,
            castsShadow: false,
        });
    }

    // Place a tall slab off to the far-left outside every light's range to prove
    // implicitSun_ did NOT trigger when only bounded local lights exist.
    scn.createMesh({ mesh: Mesh.box(4, 2, 4), position: [-28, 1, 0], color: [1, 1, 1], castsShadow: false });

    for (let f = 0; f < 3; f++) scn.captureFrame();
    const img = scn.captureFrame();

    // Top-center / late rows vs unlit corner: check green-tinted lights (indices >= 192)
    // located at positive Z (lower half of the screen).
    let greenPixels = 0;
    let darkCorner = 0;
    for (let y = 135; y < 210; y += 2) {
        for (let x = 64; x < 192; x += 2) {
            const [r, g, b] = samplePixel(img, x, y);
            if (g > r + 15 && g > 40) greenPixels++;
        }
    }
    const edgeDark = avgPatch(img, 10, 40, 4);
    darkCorner = Math.max(edgeDark.r, edgeDark.g, edgeDark.b);

    assert(greenPixels > 40, `lights with index >= 192 visibly illuminate screen tiles (${greenPixels} green-lit pixels)`);
    assert(darkCorner < 10, `unlit background outside light ranges stays dark without implicitSun_ (${darkCorner.toFixed(1)})`);

    const st = scn.cullStats();
    assert(Number.isFinite(st.frameGpuMs || 0), 'stats valid after tiled light frame');

    document.body.removeChild(cv);
}
