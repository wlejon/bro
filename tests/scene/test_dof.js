// Depth-of-field (scene.setDepthOfField) — depth-based CoC mixes the sharp
// HDR frame toward a half-res Gaussian before tonemap. Two high-frequency
// stripe targets: one at the focus distance (must stay sharp), one far
// beyond it (must blur). Sharpness metric = variance of pixel values along a
// row crossing the stripes; blurring mixes white stripes with the black gaps
// between them, so variance drops hard. All assertions comparative
// (on vs off) so they're robust to GPU variance.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '200');
canvas.setAttribute('height', '200');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('no scene; skipping dof test');
} else {
    scene.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
    scene.setCamera({
        fov: 60, near: 0.1, far: 200,
        position: [0, 0, 10], target: [0, 0, 0], up: [0, 1, 0],
    });
    // Zero-intensity light suppresses the implicit sun; emissive-only
    // stripes keep values deterministic and below the tonemap clamp.
    scene.createLight({ type: 'directional', intensity: 0 });

    // Near stripe group: camera distance 10 (= focusDistance). Vertical
    // white stripes, 0.4 wide, 0.8 apart, against a dark backdrop.
    for (let i = -2; i <= 2; i++) {
        scene.createMesh({
            mesh: Mesh.box(0.2, 2, 0.05), color: [0.8, 0.8, 0.8, 1],
            emissive: 1, x: -2.5 + i * 0.8, y: 0, z: 0,
        });
    }
    scene.createMesh({
        mesh: Mesh.box(3, 3, 0.05), color: [0.02, 0.02, 0.02, 1],
        emissive: 1, x: -2.5, y: 0, z: -0.3,
    });

    // Far stripe group: camera distance 50 (deep out of focus). Scaled up
    // so the stripes still span a few pixels on screen.
    for (let i = -2; i <= 2; i++) {
        scene.createMesh({
            mesh: Mesh.box(1, 10, 0.05), color: [0.8, 0.8, 0.8, 1],
            emissive: 1, x: 12 + i * 4, y: 0, z: -40,
        });
    }
    scene.createMesh({
        mesh: Mesh.box(14, 14, 0.05), color: [0.02, 0.02, 0.02, 1],
        emissive: 1, x: 12, y: 0, z: -41,
    });

    // Row variance of the red channel across [x0, x1) at row y.
    const rowVariance = (img, y, x0, x1) => {
        let sum = 0, n = x1 - x0;
        for (let x = x0; x < x1; x++) sum += img.data[(y * img.width + x) * 4];
        const mean = sum / n;
        let v = 0;
        for (let x = x0; x < x1; x++) {
            const d = img.data[(y * img.width + x) * 4] - mean;
            v += d * d;
        }
        return v / n;
    };

    const NEAR = [100, 35, 80];   // row, x0, x1 — over the near stripes
    const FAR  = [100, 120, 162]; // over the far stripes

    const off = scene.captureFrame();
    scene.setDepthOfField({
        enabled: true, focusDistance: 10, focusRange: 5, maxBlur: 6,
    });
    const on = scene.captureFrame();

    const nearOff = rowVariance(off, ...NEAR);
    const nearOn  = rowVariance(on, ...NEAR);
    const farOff  = rowVariance(off, ...FAR);
    const farOn   = rowVariance(on, ...FAR);
    console.log(`variance near ${nearOff.toFixed(0)} -> ${nearOn.toFixed(0)}, ` +
                `far ${farOff.toFixed(0)} -> ${farOn.toFixed(0)}`);

    // Both stripe groups are high-contrast without DoF.
    assert(nearOff > 2000, `near stripes high-variance in off frame (${nearOff.toFixed(0)})`);
    assert(farOff > 2000, `far stripes high-variance in off frame (${farOff.toFixed(0)})`);

    // Far (out of focus): variance collapses — neighboring stripe/gap pixels
    // smear together.
    assert(farOn < farOff * 0.5,
        `far stripes blurred by DoF (variance ${farOff.toFixed(0)} -> ${farOn.toFixed(0)})`);

    // Near (in focus): stays sharp — variance essentially unchanged.
    assert(nearOn > nearOff * 0.8,
        `focused stripes stay sharp (variance ${nearOff.toFixed(0)} -> ${nearOn.toFixed(0)})`);

    // Off again -> pixel-identical to never-enabled.
    scene.setDepthOfField({ enabled: false });
    const off2 = scene.captureFrame();
    let maxDelta = 0;
    for (let i = 0; i < off.data.length; i += 97) {
        const d = Math.abs(off.data[i] - off2.data[i]);
        if (d > maxDelta) maxDelta = d;
    }
    assert(maxDelta === 0,
        `DoF off is pixel-identical to never-enabled (maxDelta=${maxDelta})`);

    flush();
}

document.body.removeChild(canvas);
