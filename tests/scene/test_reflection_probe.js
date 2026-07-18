// Local reflection probes (scene.createReflectionProbe) — captured,
// GGX-prefiltered, box-projected local specular that replaces the global IBL
// specular for meshes inside the probe's box volume.
//
// Scene: a closed room of emissive red walls (the +X wall green for
// asymmetry) with a mirror-metallic white sphere inside. No IBL environment
// and a zero-intensity light (suppresses the implicit sun), so a fully
// metallic sphere has NO light path at all without a probe — its no-IBL
// ambient term is ambient * baseColor * (1 - metallic) == 0 — and renders
// black. ANY sphere brightness therefore isolates the probe contribution.
// The camera sits inside the room looking -Z at the sphere, so the screen-
// center pixel is the sphere's front surface, whose reflection points back
// at the +Z wall behind the camera. Linear tonemap + gamma 1 keeps readback
// linear; assertions are comparative so they're robust to GPU variance.
//
// Covers: manual mode waits for capture(); capture() lights the sphere with
// the wall's reflection (red, not green/white at center); boxProjection
// on/off changes the sampled result; a scene change (wall emissive off) is
// stale until an explicit recapture picks it up; destroy() restores the
// global (black) fallback; 'once' mode captures on its first visible frame.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '200');
canvas.setAttribute('height', '200');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('no scene; skipping reflection probe test');
} else {
    scene.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
    scene.setCamera({
        fov: 60, near: 0.1, far: 100,
        position: [0.8, 0, 3.5], target: [0.8, 0, 0], up: [0, 1, 0],
    });
    scene.createLight({ type: 'directional', intensity: 0 });

    // Room: 10x10x10 interior, six emissive walls (self-lit so the capture
    // is deterministic — no shadow/lighting dependence). +X wall green, the
    // rest red; the +Z wall (behind the camera) is what the sphere's
    // screen-center reflection sees.
    const wall = (x, y, z, sx, sy, sz, color) => scene.createMesh({
        mesh: Mesh.box(sx, sy, sz), x, y, z, color, emissive: 2,
    });
    const red = [1, 0.1, 0.1, 1];
    wall(0, -5, 0, 10.4, 0.2, 10.4, red);              // floor
    wall(0,  5, 0, 10.4, 0.2, 10.4, red);              // ceiling
    wall(-5, 0, 0, 0.2, 10.4, 10.4, red);              // -X
    wall( 5, 0, 0, 0.2, 10.4, 10.4, [0.1, 1, 0.1, 1]); // +X (green)
    wall(0, 0, -5, 10.4, 10.4, 0.2, red);              // -Z
    const wallZ = wall(0, 0, 5, 10.4, 10.4, 0.2, red); // +Z (behind camera)

    // Mirror sphere: metallic 1 -> black without a probe (see header).
    scene.createMesh({
        mesh: Mesh.sphere(1), x: 0.8, y: 0, z: 0,
        color: [1, 1, 1, 1], metallic: 1, roughness: 0.05,
    });

    const px = (img, x, y) => {
        const i = (y * img.width + x) * 4;
        return { r: img.data[i], g: img.data[i + 1], b: img.data[i + 2] };
    };
    const center = (img) => px(img, 100, 100);
    // Max per-channel abs difference over the central sphere region.
    const maxDiffRegion = (a, b) => {
        let best = 0;
        for (let y = 40; y < 160; y++) {
            for (let x = 40; x < 160; x++) {
                const i = (y * a.width + x) * 4;
                for (let c = 0; c < 3; c++) {
                    const d = Math.abs(a.data[i + c] - b.data[i + c]);
                    if (d > best) best = d;
                }
            }
        }
        return best;
    };

    // --- Section 1: control — no probe, mirror sphere is black ------------
    const control = scene.captureFrame();
    const c0 = center(control);
    console.log(`control sphere pixel: r=${c0.r} g=${c0.g} b=${c0.b}`);
    assert(c0.r < 20 && c0.g < 20 && c0.b < 20,
        `no probe: metallic sphere is black (r=${c0.r} g=${c0.g} b=${c0.b})`);

    // --- Section 2: 'manual' probe waits for capture() --------------------
    const probe = scene.createReflectionProbe({
        size: 10, resolution: 128, updateMode: 'manual',
    });
    assert(probe.type === 'reflectionProbe', 'node type is reflectionProbe');
    assert(probe.updateMode === 'manual', 'updateMode reads back');
    assert(probe.resolution === 128, 'resolution reads back');
    assert(probe.boxProjection === true, 'boxProjection defaults true');

    const manualIdle = scene.captureFrame();
    const c1 = center(manualIdle);
    assert(c1.r < 20,
        `manual probe without capture() stays inactive (r=${c1.r})`);

    // --- Section 3: capture() -> sphere reflects the red room -------------
    probe.capture();
    const lit = scene.captureFrame();
    const c2 = center(lit);
    console.log(`probe-lit sphere pixel: r=${c2.r} g=${c2.g} b=${c2.b}`);
    assert(c2.r > c0.r + 40,
        `captured probe lights the mirror sphere (r=${c2.r} vs control ${c0.r})`);
    assert(c2.r > c2.g + 30,
        `screen-center reflection carries the red +Z wall (r=${c2.r} g=${c2.g})`);

    // --- Section 4: boxProjection on/off changes the sample ---------------
    // Sampling-time only — no recapture. The sphere is offset from the probe
    // origin, so parallax correction must land on different cubemap texels.
    probe.boxProjection = false;
    const noBoxProj = scene.captureFrame();
    probe.boxProjection = true;
    const diff = maxDiffRegion(lit, noBoxProj);
    console.log(`boxProjection on/off max diff over sphere region: ${diff}`);
    assert(diff > 8,
        `boxProjection changes the sampled reflection (maxDiff=${diff})`);

    // --- Section 5: scene change is stale until an explicit recapture -----
    wallZ.emissive = 0;   // the wall the center reflection sees goes dark
    const stale = scene.captureFrame();
    const c3 = center(stale);
    assert(Math.abs(c3.r - c2.r) < 20,
        `probe is stale after a scene change without capture() ` +
        `(r=${c3.r} vs lit ${c2.r})`);

    probe.capture();
    const recaptured = scene.captureFrame();
    const c4 = center(recaptured);
    console.log(`recaptured sphere pixel: r=${c4.r} (was ${c2.r})`);
    assert(c4.r < c2.r - 40,
        `recapture picks up the darkened wall (r=${c4.r} vs ${c2.r})`);

    // --- Section 6: destroy restores the global fallback ------------------
    wallZ.emissive = 2;   // relight the wall for the 'once' test below
    probe.destroy();
    const destroyed = scene.captureFrame();
    const c5 = center(destroyed);
    assert(c5.r < 20,
        `destroyed probe restores the global (black) fallback (r=${c5.r})`);

    // --- Section 7: 'once' mode captures on its first visible frame -------
    const probe2 = scene.createReflectionProbe({ size: 10 });   // default 'once'
    const once = scene.captureFrame();
    const c6 = center(once);
    console.log(`'once' probe sphere pixel: r=${c6.r}`);
    assert(c6.r > c5.r + 40,
        `'once' probe captured automatically on its first frame (r=${c6.r})`);
    probe2.destroy();

    flush();
}

document.body.removeChild(canvas);
