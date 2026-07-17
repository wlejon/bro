// Exponential-squared + height fog (scene.setFog {density, heightFalloff,
// startDistance}) — exercises the fogFactorFor path in mesh.frag. All
// assertions are comparative (fog on vs off / low vs high) so they're robust
// to GPU variance. Emissive white boxes make the un-fogged base color
// deterministic; linear tonemap + gamma 1 keeps readback values linear.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '160');
canvas.setAttribute('height', '160');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('no scene; skipping fog test');
} else {
    scene.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
    scene.setCamera({
        fov: 60, near: 0.1, far: 200,
        position: [0, 0, 10], target: [0, 0, 0], up: [0, 1, 0],
    });

    const px = (x, y) => {
        const img = scene.captureFrame();
        const i = (y * img.width + x) * 4;
        return { r: img.data[i], g: img.data[i + 1], b: img.data[i + 2],
                 a: img.data[i + 3] };
    };
    // Project world point to pixel via the size the boxes were placed at:
    // we place boxes so they cover known screen regions instead.

    // Near box (camera distance ~4) covering the image center-left; far box
    // (camera distance ~50) center-right. Emissive white -> unfogged pixels
    // read ~white.
    const near = scene.createMesh({
        mesh: 'box', color: [1, 1, 1, 1], emissive: 1,
        x: -1.2, y: 0, z: 6,
    });
    const far = scene.createMesh({
        mesh: Mesh.box(4, 4, 0.5), color: [1, 1, 1, 1], emissive: 1,
        x: 12, y: 0, z: -40,
    });

    const nearPx = [38, 80];   // over the near box
    const farPx = [120, 80];   // over the far box

    // --- Baseline: fog off --------------------------------------------------
    let n0 = px(...nearPx), f0 = px(...farPx);
    assert(n0.a > 0 && n0.r > 200, `near box visible unfogged (r=${n0.r})`);
    assert(f0.a > 0 && f0.r > 200, `far box visible unfogged (r=${f0.r})`);

    // --- Exponential-squared fog: far converges to fog color, near doesn't --
    // Green fog: fogged pixels lose red/blue but keep green.
    scene.setFog({ density: 0.05, color: [0, 1, 0] });
    let n1 = px(...nearPx), f1 = px(...farPx);
    // Far: density*dist ~ 0.05*50 = 2.5 -> factor ~ 1-exp(-6.25) ~ 0.998.
    assert(f1.r < 40, `far box red collapsed toward fog color (r=${f1.r})`);
    assert(f1.g > 150, `far box green held by green fog (g=${f1.g})`);
    // Near: density*dist ~ 0.05*4 = 0.2 -> factor ~ 0.04; nearly unchanged.
    assert(Math.abs(n1.r - n0.r) < 30,
        `near box barely fogged (r ${n0.r} -> ${n1.r})`);
    assert(f0.r - f1.r > 150,
        `fog moved far box strongly toward fog color (${f0.r} -> ${f1.r})`);

    // --- startDistance: pushing the fog-free zone past the far box unfogs it
    scene.setFog({ density: 0.05, startDistance: 60, color: [0, 1, 0] });
    let f2 = px(...farPx);
    assert(f2.r > 200,
        `startDistance beyond the far box removes its fog (r=${f2.r})`);

    // --- Height fog: low box foggier than high box at the same distance -----
    near.destroy();
    far.destroy();
    const low = scene.createMesh({
        mesh: Mesh.box(4, 4, 0.5), color: [1, 1, 1, 1], emissive: 1,
        x: 0, y: -12, z: -40,
    });
    const high = scene.createMesh({
        mesh: Mesh.box(4, 4, 0.5), color: [1, 1, 1, 1], emissive: 1,
        x: 0, y: 12, z: -40,
    });
    // Gentle density so the LOW box lands mid-fog (factor ~0.7) — full fog
    // would fade its alpha to 0 and there'd be nothing left to compare.
    scene.setFog({ density: 0.01, heightFalloff: 0.08, color: [0, 1, 0] });
    // Sample vertically: low box below center, high box above (GL image is
    // returned top-down, so high box = small y).
    const lowP = px(80, 112);
    const highP = px(80, 48);
    assert(lowP.a > 0, 'low box covers its sample pixel');
    assert(highP.a > 0, 'high box covers its sample pixel');
    // Same camera distance; only height differs. Fogginess = red drop.
    assert(highP.r - lowP.r > 40,
        `height fog: low box foggier than high box (low r=${lowP.r}, high r=${highP.r})`);

    // --- Translucent meshes are fogged too ----------------------------------
    low.destroy();
    high.destroy();
    const farT = scene.createMesh({
        mesh: Mesh.box(4, 4, 0.5), color: [1, 1, 1, 0.6], emissive: 1,
        x: 0, y: 0, z: -40,
    });
    scene.setFog({});   // off
    const t0 = px(80, 80);
    scene.setFog({ density: 0.05, color: [0, 1, 0] });
    const t1 = px(80, 80);
    assert(t0.r > 100, `translucent box visible unfogged (r=${t0.r})`);
    assert(t0.r - t1.r > 60,
        `translucent box fogged like opaques (r ${t0.r} -> ${t1.r})`);

    // --- Legacy linear ramp still works -------------------------------------
    scene.setFog({ start: 5, end: 30, color: [0, 1, 0] });
    const t2 = px(80, 80);
    assert(t2.r < t0.r - 60, `legacy linear fog still fogs (r=${t2.r})`);

    farT.destroy();
    flush();
}

document.body.removeChild(canvas);
