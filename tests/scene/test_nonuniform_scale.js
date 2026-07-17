// Per-axis createMesh scale — `scale: [x, y, z]` used to NaN-poison the
// node transform (the parser coerced the array with JS_ToFloat64 → NaN),
// so the mesh silently vanished. Now the array form sets per-axis scale,
// and the renderer transforms normals with the model's inverse-transpose
// (uNormalMat in mesh.vert) so lighting stays correct under non-uniform
// scale instead of skewing normals toward the stretched axis.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '128');
canvas.setAttribute('height', '128');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('scene context not available (no GPU) — skipping');
} else {
    // Linear tonemap so pixel comparisons are monotonic in shading.
    scene.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
    scene.setAmbient(0, 0, 0);

    const px = (x, y) => {
        const img = scene.captureFrame();
        const i = (y * img.width + x) * 4;
        return { r: img.data[i], g: img.data[i + 1], b: img.data[i + 2],
                 a: img.data[i + 3] };
    };
    const lum = (p) => p.r + p.g + p.b;

    // =====================================================================
    // 1. Transform values: array scale lands per-axis on the node (the old
    //    parser would have set NaN on all three axes).
    // =====================================================================
    const m = scene.createMesh({ mesh: 'box', scale: [2, 3, 4] });
    assert(Math.abs(m.scaleX - 2) < 1e-6, `scaleX = 2 (got ${m.scaleX})`);
    assert(Math.abs(m.scaleY - 3) < 1e-6, `scaleY = 3 (got ${m.scaleY})`);
    assert(Math.abs(m.scaleZ - 4) < 1e-6, `scaleZ = 4 (got ${m.scaleZ})`);
    m.destroy();

    // Uniform number form unchanged.
    const u = scene.createMesh({ mesh: 'box', scale: 2.5 });
    assert(Math.abs(u.scaleX - 2.5) < 1e-6, 'uniform scale x');
    assert(Math.abs(u.scaleY - 2.5) < 1e-6, 'uniform scale y');
    assert(Math.abs(u.scaleZ - 2.5) < 1e-6, 'uniform scale z');
    u.destroy();

    // Short array: missing entries default to 1.
    const s2 = scene.createMesh({ mesh: 'box', scale: [2, 3] });
    assert(Math.abs(s2.scaleZ - 1) < 1e-6, 'missing array entry defaults to 1');
    s2.destroy();

    // =====================================================================
    // 2. Rendered footprint: a box scaled [3, 1, 1] must be wide, not tall
    //    (and must render at all — the NaN regression drew nothing).
    //    Camera at z=8, fov 60 → 13.86 px per world unit at the origin
    //    plane. Box half-extents (1.5, 0.5) → edges at ±20.8 px / ±6.9 px.
    // =====================================================================
    scene.setCamera({
        fov: 60, near: 0.1, far: 100,
        position: [0, 0, 8], target: [0, 0, 0], up: [0, 1, 0],
    });
    const wide = scene.createMesh({
        mesh: 'box', color: [1, 0, 0, 1], emissive: 1, scale: [3, 1, 1],
    });
    flush();

    let p = px(64, 64);
    assert(p.r > 100, `center covered by scaled box (r=${p.r})`);
    p = px(64 + 14, 64);
    assert(p.r > 100, `x extent stretched to 3x (r=${p.r} at +14px)`);
    p = px(64, 64 + 14);
    assert(p.r < 50, `y extent NOT stretched (r=${p.r} at +14px below)`);
    p = px(64 + 30, 64);
    assert(p.r < 50, `box ends where scale 3 says it should (r=${p.r})`);
    wide.destroy();

    // =====================================================================
    // 3. Normal correctness under non-uniform scale. A quad whose normal is
    //    (1,1,1)/√3, lit by a directional light straight down (L = +y):
    //      correct (inverse-transpose): scaling x by 3 ROTATES the normal
    //        away from x, toward the light → N·L rises 0.577 → 0.688.
    //      broken (raw model 3x3): the normal skews INTO the stretched axis
    //        → N·L collapses to 0.302 — visibly darker.
    //    So the scaled quad must render at least as bright as the uniform
    //    one; the broken transform fails this by ~2x.
    // =====================================================================
    scene.createLight({ type: 'directional', direction: [0, -1, 0],
                        color: [1, 1, 1], intensity: 1.0 });

    // Quad spanned by u=(1,-1,0)/√2, v=(1,1,-2)/√6, normal n=(1,1,1)/√3.
    // CCW from the +n side, and n points toward the +z camera.
    const U = [0.70711, -0.70711, 0], V = [0.40825, 0.40825, -0.81650];
    const corner = (a, b) => [a * U[0] + b * V[0],
                              a * U[1] + b * V[1],
                              a * U[2] + b * V[2]];
    const quad = (opts) => {
        const c = [corner(-1, -1), corner(1, -1), corner(1, 1), corner(-1, 1)];
        const n = 0.57735;
        return scene.createMesh(Object.assign({
            positions: new Float32Array([].concat(...c)),
            normals: new Float32Array([n, n, n, n, n, n, n, n, n, n, n, n]),
            indices: new Uint32Array([0, 1, 2, 0, 2, 3]),
            color: [1, 1, 1, 1], roughness: 1, metallic: 0,
        }, opts));
    };

    const flat = quad({});
    flush();
    const lumUniform = lum(px(64, 64));
    flat.destroy();

    const stretched = quad({ scale: [3, 1, 1] });
    flush();
    const lumScaled = lum(px(64, 64));
    stretched.destroy();

    assert(lumUniform > 30, `uniform quad is lit (lum=${lumUniform})`);
    assert(lumScaled > 30, `scaled quad is lit (lum=${lumScaled})`);
    assert(lumScaled >= lumUniform * 0.95,
        `non-uniform scale keeps normals correct: scaled lum ${lumScaled} ` +
        `vs uniform ${lumUniform} (broken transform darkens ~2x)`);

    flush();
}

document.body.removeChild(canvas);
