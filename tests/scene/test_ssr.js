// SSR (scene.setSSR) — screen-space reflections on opaque surfaces, marched
// from the opaque depth right after the decal slot and composited over IBL.
//
// Scene: a mirror-smooth metallic floor under a bright emissive cube. With
// no IBL environment and a zero-intensity light (suppresses the implicit
// sun), a fully-metallic floor has NO other light path — its no-IBL ambient
// term is ambient * baseColor * (1 - metallic) == 0 — so the floor renders
// black and ANY floor brightness with SSR on is the marched reflection.
// Linear tonemap + gamma 1 keeps readback linear; all assertions are
// comparative (on vs off) so they're robust to GPU variance.
//
// Covers: reflection appears (mirror floor), the per-pixel reflectance mask
// (rough floor reflects far less), off-is-identical determinism, and an
// orthographic-camera variant (the marching math must not assume
// perspective).

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '200');
canvas.setAttribute('height', '200');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('no scene; skipping ssr test');
} else {
    scene.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
    scene.setCamera({
        fov: 60, near: 0.1, far: 100,
        position: [0, 2.5, 6], target: [0, 1, 0], up: [0, 1, 0],
    });
    scene.createLight({ type: 'directional', intensity: 0 });

    // Mirror floor: metallic 1 -> black without SSR (see header comment).
    const floor = scene.createMesh({
        mesh: Mesh.box(10, 0.25, 10), y: -0.125,
        color: [1, 1, 1, 1], metallic: 1, roughness: 0.05,
    });
    // Bright emissive red cube above the floor (emissiveColor defaults to
    // the base color). Its reflection lands on the floor between the cube
    // and the camera — the lower half of the frame.
    scene.createMesh({
        mesh: Mesh.box(1.2, 1.2, 1.2), y: 1.6,
        color: [1, 0.15, 0.15, 1], emissive: 2,
    });

    // Max red-channel increase (on - off) over the lower half of the frame,
    // and where it happened. The cube renders identically in both frames;
    // the floor is black without SSR, so the increase isolates reflections.
    const maxIncreaseLowerHalf = (offImg, onImg) => {
        let best = 0, bestX = -1, bestY = -1;
        for (let y = Math.floor(offImg.height / 2); y < offImg.height; y++) {
            for (let x = 0; x < offImg.width; x++) {
                const i = (y * offImg.width + x) * 4;
                const d = onImg.data[i] - offImg.data[i];
                if (d > best) { best = d; bestX = x; bestY = y; }
            }
        }
        return { best, x: bestX, y: bestY };
    };

    // --- Section 1: mirror floor reflects the cube ------------------------
    const offImg = scene.captureFrame();
    scene.setSSR({ enabled: true });     // defaults: 30 units, 48 steps
    const onImg = scene.captureFrame();

    const mirror = maxIncreaseLowerHalf(offImg, onImg);
    console.log(`mirror reflection: max red increase ${mirror.best} at ` +
                `(${mirror.x}, ${mirror.y})`);
    assert(mirror.best > 60,
        `SSR on: mirror floor shows the emissive cube's reflection ` +
        `(max increase=${mirror.best})`);

    // The reflection is red like the cube, not white: sample the found pixel.
    const hi = (mirror.y * onImg.width + mirror.x) * 4;
    assert(onImg.data[hi] > onImg.data[hi + 1] + 30,
        `reflection carries the cube's red color ` +
        `(r=${onImg.data[hi]} g=${onImg.data[hi + 1]})`);

    // --- Section 2: reflectance mask — rough floor barely reflects --------
    floor.roughness = 0.85;   // mask ~ (1-0.85)^2 = 0.02 of the mirror's
    const roughImg = scene.captureFrame();
    const rough = maxIncreaseLowerHalf(offImg, roughImg);
    console.log(`rough floor: max red increase ${rough.best}`);
    assert(rough.best < mirror.best * 0.35,
        `rough floor reflects far less than the mirror ` +
        `(rough=${rough.best}, mirror=${mirror.best})`);
    floor.roughness = 0.05;

    // --- Section 3: disabled again is pixel-identical ---------------------
    scene.setSSR({ enabled: false });
    const offImg2 = scene.captureFrame();
    let maxDelta = 0;
    for (let i = 0; i < offImg.data.length; i += 97) {   // sparse sweep
        const d = Math.abs(offImg.data[i] - offImg2.data[i]);
        if (d > maxDelta) maxDelta = d;
    }
    assert(maxDelta === 0,
        `SSR off is pixel-identical to never-enabled (maxDelta=${maxDelta})`);

    // --- Section 4: orthographic camera -----------------------------------
    // Same scene through an ortho camera: the incident ray is the constant
    // view direction and depth reconstruction runs through the full inverse
    // projection — the reflection must still land on the floor.
    scene.setCamera({
        mode: 'ortho', size: 7, near: 0.1, far: 100,
        position: [0, 2.5, 6], target: [0, 1, 0], up: [0, 1, 0],
    });
    const orthoOff = scene.captureFrame();
    scene.setSSR({ enabled: true });
    const orthoOn = scene.captureFrame();
    const ortho = maxIncreaseLowerHalf(orthoOff, orthoOn);
    console.log(`ortho reflection: max red increase ${ortho.best} at ` +
                `(${ortho.x}, ${ortho.y})`);
    assert(ortho.best > 40,
        `SSR works under an orthographic camera ` +
        `(max increase=${ortho.best})`);

    flush();
}

document.body.removeChild(canvas);
