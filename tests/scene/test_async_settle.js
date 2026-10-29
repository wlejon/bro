// A scene settled with frames that skip what is still being built (a
// window's policy, perf.asyncShaders(true)) draws what one settled with
// frames that wait for it (headless's) draws, at the same moment: whatever a
// frame skipped comes back, and no cache (the shadow pages) keeps what a
// frame without its pipeline made of it. (scene_vk_pipeline.h's
// pipeline_async; docs/code-cache.md "Pipelines built off the frame thread".) Verifies, with an
// atmosphere, auto exposure, a ground, boxes casting the sun's shadow (one
// with a salted custom fragment chunk, so its program compiles cold):
//   - settled both ways and run on to the same time, the two pictures are
//     the same to within two scenes' run-to-run noise, at the same exposure
//   - the window's way skipped draws at first; when it skipped none (every
//     pipeline a pipeline-cache hit) the test reports SKIP

// (A salt in the custom chunk: nothing of this run's is in the shader cache.)
const SALT = (Math.random() * 1e-6).toExponential(6);
const FRAG = `
    void userFragment(inout vec3 baseColor, inout vec3 normal,
                      inout float metallic, inout float roughness,
                      inout vec3 emissive, inout float alpha) {
        baseColor = vec3(0.7, 0.35, 0.2) + ${SALT};
        roughness = 0.6;
    }`;

function differ(a, b) {
    let over = 0, worst = 0, se = 0;
    for (let i = 0; i < a.data.length; i += 4) {
        let d = 0;
        for (let k = 0; k < 3; k++) {
            const e = a.data[i + k] - b.data[i + k];
            se += e * e;
            d = Math.max(d, Math.abs(e));
        }
        worst = Math.max(worst, d);
        if (d > 8) over++;
    }
    const mse = se / (3 * a.data.length / 4);
    return { over, worst, db: mse === 0 ? Infinity : 10 * Math.log10(255 * 255 / mse) };
}

const S = 160;
const frames = (n) => { flush(); for (let i = 0; i < n; i++) advanceTime(1000 / 30); };
const skipping = () => perf.pendingWork().some((r) => /^pipelines/.test(r));

// The scene made, the window's way or headless's, settled and run on to
// the same moment of its own clock (every scene's starts at its making).
function settleOnce(async) {
    perf.asyncShaders(async);
    const canvas = document.createElement('canvas');
    canvas.setAttribute('width', String(S));
    canvas.setAttribute('height', String(S));
    document.body.appendChild(canvas);
    flush();
    const scn = canvas.getContext('scene');
    if (!scn) return null;
    scn.setToneMap({ mode: 'aces' });
    scn.setDither(false);
    scn.setAtmosphere({});
    scn.setExposure({ mode: 'auto', meter: 'incident' });
    const sun = scn.createLight({ type: 'directional', direction: [-0.4, -0.7, -0.5], color: [1, 1, 1], intensity: 3 });
    sun.castsShadow = true;
    scn.createMesh({ mesh: 'plane', color: '#5a7040', scale: [80, 1, 80] });
    scn.createMesh({ mesh: 'box', color: 'white', x: -2, y: 1, z: -10, castsShadow: true });
    const custom = scn.createMesh({ mesh: 'box', color: 'white', x: 2, y: 1, z: -8, castsShadow: true });
    custom.setShader({ fragment: FRAG });
    scn.setCamera({ fov: 60, near: 0.1, far: 2e4, position: [0, 4, 2], target: [0, 1, -10], up: [0, 1, 0] });
    // (As many frames each way, so both scenes' clocks read the same.)
    let skipped = 0;
    for (let i = 0; i < 30; i++) {
        frames(1);
        if (skipping()) skipped++;
    }
    const s = perf.settle({ maxFrames: 1500 });
    frames(15);
    const img = scn.captureFrame();
    const cs = scn.cullStats();
    const out = { img, skipped, settled: s, ev: cs.exposureEv, frames: s.frames };
    canvas.remove();
    flush();
    return out;
}

const windowWay = settleOnce(true);
if (!windowWay) {
    missingGpuContext('scene');
} else {
    const headlessWay = settleOnce(false);
    perf.asyncShaders(false);
    console.log(`window's way: ${windowWay.skipped} frames skipped work, settled after ${windowWay.frames} (ev ${windowWay.ev.toFixed(3)}); ` +
                `headless's: settled after ${headlessWay.frames} (ev ${headlessWay.ev.toFixed(3)})`);
    assert(windowWay.settled.settled, `the window's way settles (${windowWay.settled.pending.join('; ')})`);
    assert(headlessWay.settled.settled, `headless's way settles (${headlessWay.settled.pending.join('; ')})`);
    assert(Math.abs(windowWay.ev - headlessWay.ev) < 0.01,
           `the same exposure both ways (${windowWay.ev.toFixed(3)} / ${headlessWay.ev.toFixed(3)})`);
    const d = differ(windowWay.img, headlessWay.img);
    console.log(`the two pictures: ${d.db.toFixed(1)} dB, ${d.over} px over 8 levels, worst ${d.worst}`);
    // (A skipped draw, or a shadow page cached without its caster, would be
    // a region of pixels.)
    assert(d.over < S * S / 500 && d.db > 55,
           `settled both ways, the same picture (${d.db.toFixed(1)} dB, ${d.over} px over 8 levels, worst ${d.worst})`);
    // (With every pipeline in the Vulkan pipeline cache each is built
    // before its first frame records: nothing skipped, the subject untested.)
    if (windowWay.skipped === 0) skipTest('no frame skipped a draw: every pipeline came from the pipeline cache');
    console.log('test_async_settle: OK');
}
