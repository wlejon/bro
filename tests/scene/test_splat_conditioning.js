// Numerical conditioning of the gaussian-splat 2x2 eigen-decomposition —
// exercises the principal-axis derivation in the splat vertex shader
// (src/scene/gaussian_splat_node.cpp). For an elongated splat whose major
// axis is NEARLY x-aligned (tiny but nonzero rotation), the off-diagonal b
// is tiny and (l1 - a) suffers catastrophic cancellation; taking the
// eigenvector from the ill-conditioned row could rotate the computed major
// axis arbitrarily, up to an effective axis swap. The shader now picks the
// better-conditioned row, so the rendered ellipse must stay a wide, flat
// horizontal streak.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '128');
canvas.setAttribute('height', '128');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene || typeof scene.createGaussianSplat !== 'function') {
    console.log('no scene / createGaussianSplat; skipping splat conditioning test');
} else {
    scene.setCamera({
        fov: 60, near: 0.1, far: 100,
        position: [0, 0, 5], target: [0, 0, 0], up: [0, 1, 0],
    });
    scene.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });

    // One white splat at the origin, strongly elongated along local x
    // (25:1), rotated about z by a tiny angle so the screen covariance is
    // near-axis-aligned but NOT exactly (b tiny and nonzero — the
    // ill-conditioned regime).
    const C0 = 0.28209479177387814;
    const eps = 0.002; // radians
    const cloud = {
        positions: new Float32Array([0, 0, 0]),
        scales: new Float32Array([0.5, 0.02, 0.02]),
        rotations: new Float32Array([0, 0, Math.sin(eps / 2), Math.cos(eps / 2)]),
        opacities: new Float32Array([1]),
        sh: new Float32Array(3).fill((1 - 0.5) / C0), // white
        shDegree: 0,
    };
    const node = scene.createGaussianSplat({ cloud });

    const img = scene.captureFrame();
    const alphaAt = (x, y) => img.data[(y * img.width + x) * 4 + 3];

    // Coverage along the center row (major axis) vs the center column
    // (minor axis). Camera at z=5, fov 60, 128 px -> focal = 110.85 px, so
    // sigma_x = 110.85 * 0.5 / 5 ~ 11 px (visible run ~55 px at the alpha
    // threshold) while sigma_y ~ 0.5 px (a few pixels).
    let rowCount = 0, colCount = 0;
    for (let x = 0; x < img.width; x++) if (alphaAt(x, 64) > 10) rowCount++;
    for (let y = 0; y < img.height; y++) if (alphaAt(64, y) > 10) colCount++;

    assert(rowCount >= 40,
        `major axis spans the center row (got ${rowCount} px)`);
    assert(colCount > 0 && colCount <= 12,
        `minor axis stays thin on the center column (got ${colCount} px)`);
    assert(rowCount > 3 * colCount,
        `ellipse is horizontal, not axis-swapped (row=${rowCount} col=${colCount})`);

    node.destroy();
    flush();
}

document.body.removeChild(canvas);
