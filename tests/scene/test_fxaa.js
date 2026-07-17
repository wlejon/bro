// FXAA (scene.setFXAA) — FXAA 3.11 on the final LDR image, last pass in the
// post stack. A high-contrast shallow diagonal edge (white box rotated 10
// degrees over a dark backdrop, MSAA off) rasterizes as a staircase with
// multi-pixel steps — FXAA's sweet spot: the edge-end search distributes
// blend offsets along each step, so the count of INTERMEDIATE-intensity
// pixels rises sharply, while pixels far from any edge stay bit-identical.
// (A perfect 45-degree staircase is FXAA's weakest case — 1px steps end the
// edge search immediately, leaving only the sub-pixel filter.)
// Comparative on/off assertions only.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '160');
canvas.setAttribute('height', '160');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('no scene; skipping fxaa test');
} else {
    scene.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
    scene.setCamera({
        fov: 60, near: 0.1, far: 100,
        position: [0, 0, 6], target: [0, 0, 0], up: [0, 1, 0],
    });
    // Deterministic emissive-only shading (zero-intensity light suppresses
    // the implicit sun). Bright diagonal square over a dark backdrop.
    scene.createLight({ type: 'directional', intensity: 0 });
    scene.createMesh({
        mesh: Mesh.box(1.4, 1.4, 0.05), color: [0.9, 0.9, 0.9, 1],
        emissive: 1, rz: 10,
    });
    scene.createMesh({
        mesh: Mesh.box(4, 4, 0.05), color: [0.05, 0.05, 0.05, 1],
        emissive: 1, z: -0.5,
    });

    // Count pixels whose red channel sits strictly between the two flat
    // levels (dark ~13, bright ~230) — i.e. blended edge pixels.
    const intermediateCount = (img) => {
        let n = 0;
        for (let y = 0; y < img.height; y++) {
            for (let x = 0; x < img.width; x++) {
                const v = img.data[(y * img.width + x) * 4];
                if (v > 40 && v < 200) n++;
            }
        }
        return n;
    };

    const off = scene.captureFrame();
    scene.setFXAA(true);
    const on = scene.captureFrame();

    const nOff = intermediateCount(off);
    const nOn = intermediateCount(on);
    console.log(`intermediate edge pixels: off=${nOff} on=${nOn}`);

    // Without AA almost every edge pixel is one of the two flat levels;
    // with FXAA each multi-pixel stair step gets a blend gradient.
    assert(nOn > nOff * 2 + 20,
        `FXAA blends the diagonal edge (intermediate px ${nOff} -> ${nOn})`);

    // Flat interior far from any edge is untouched (FXAA early-exits on low
    // local contrast): center of the white square.
    const c = (img, x, y) => img.data[(y * img.width + x) * 4];
    assert(c(on, 80, 80) === c(off, 80, 80),
        `flat interior untouched (${c(off, 80, 80)} -> ${c(on, 80, 80)})`);
    assert(c(on, 12, 12) === c(off, 12, 12),
        `flat backdrop corner untouched (${c(off, 12, 12)} -> ${c(on, 12, 12)})`);

    // MSAA + FXAA can both be on (MSAA for geometry edges in HDR, FXAA for
    // the LDR result) — must render without error and still count as
    // anti-aliased.
    scene.setMSAA(4);
    const both = scene.captureFrame();
    assert(intermediateCount(both) > nOff,
        'MSAA + FXAA together still anti-aliased');
    scene.setMSAA(0);

    // Off again -> pixel-identical to never-enabled.
    scene.setFXAA(false);
    const off2 = scene.captureFrame();
    let maxDelta = 0;
    for (let i = 0; i < off.data.length; i += 97) {
        const d = Math.abs(off.data[i] - off2.data[i]);
        if (d > maxDelta) maxDelta = d;
    }
    assert(maxDelta === 0,
        `FXAA off is pixel-identical to never-enabled (maxDelta=${maxDelta})`);

    flush();
}

document.body.removeChild(canvas);
