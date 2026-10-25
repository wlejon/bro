// A scene settled with frames that skip what is still compiling (a window's
// policy, perf.asyncShaders(true)) draws what one settled with frames that
// wait for it (headless's) draws, at the same moment: whatever a frame
// skipped comes back, and no bake keeps what a frame without its pipeline
// made of it. (vulkan/frame/scene_shader_warmup.h, scene_vk_pipeline.h's
// pipeline_async and AsyncComputePipeline; docs/code-cache.md "Shader
// warm-up".) Verifies, on a round body with a land and its processes, a
// surface, grass, a box casting the sun's shadow and a cloud deck that
// drifts and shadows the land:
//   - the window's way skips draws at first (the test exercises it): the
//     scene's world shaders, salted, compile cold
//   - settled both ways and run on to the same time, the two pictures are
//     the same to within two scenes' run-to-run noise, at the same exposure

const N = (type, props = {}) => {
    const n = FastNoise.create(type);
    for (const k in props) n.set(k, props[k]);
    return n;
};
// (A salt in every world module: nothing of this run's is in the shader
// cache, so the window's way meets its sets compiling.)
const SALT = (Math.random() * 1e-6).toExponential(6);
const SURFACE = `
LandSurface landSurface(LandPoint q) {
    float g = landGrid(1, q.at).x;
    vec3 c = mix(vec3(0.12, 0.3, 0.08), vec3(0.45, 0.4, 0.3), smoothstep(0.1, 0.5, 1.0 - q.normal.y));
    LandSurface s = landSurfaceOf(c * (0.9 + 0.2 * g) + ${SALT});
    s.cover = vec4(1.0, 0.0, 1.0, 0.0);
    return s;
}
`;
const PROCESSES = {
    resolution: 64,
    channels: 2,
    source: `
void procInit(inout vec4 v[PROC_CHANNELS]) {
    vec3 p = procPoint();
    v[0] = vec4(8.0 * sin(p.x / 90.0) * cos(p.z / 70.0) + ${SALT}, 1.0, 0.0, 0.0);
    v[1] = vec4(0.5 + 0.5 * sin(p.x / 40.0), 0.0, 0.0, 0.0);
}
`,
};
// A broken deck drifting over the land, its shadow on it.
const CLOUDS = `
vec4 cloudCover(vec3 up) { return vec4(0.5 + ${SALT}, 0.5, 0.6, 0.0); }
float cloudDensity(CloudPoint q) {
    vec4 n = cloudNoise(q.p / 3000.0, 1.0 / 16.0);
    return clamp((n.x - 0.45) * 4.0, 0.0, 1.0) * smoothstep(0.0, 0.3, q.height) * (1.0 - smoothstep(0.7, 1.0, q.height));
}
`;

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
const R = 6371000;
const frames = (n) => { flush(); for (let i = 0; i < n; i++) advanceTime(1000 / 30); };

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
    scn.setBody({ radius: R, at: [0, R, 0] });
    scn.setAtmosphere({});
    scn.setExposure({ mode: 'auto', meter: 'incident' });
    const sun = scn.createLight({ type: 'directional', direction: [-0.4, -0.7, -0.5], color: [1, 1, 1], intensity: 3 });
    sun.castsShadow = true;
    const land = scn.createLand({ height: N('Constant', { Value: 0 }), surface: SURFACE, processes: PROCESSES });
    scn.createGrass({ ground: land });
    scn.setClouds({ module: CLOUDS, bottom: 600, top: 1600, wind: [12, 0, -4] });
    // (As many frames each way, so both scenes' clocks read the same; the
    // box placed once the land's heights answer, as an app stands a thing
    // on it.)
    let skipped = 0;
    const note = () => {
        if (perf.pendingWork().some((r) => /^(pipelines|shaders|clouds: not yet compiled)/.test(r))) skipped++;
    };
    for (let i = 0; i < 10; i++) {
        frames(1);
        note();
    }
    // (On a held clock: however long the window's way waits for its land,
    // in frames, both scenes' clocks still read the same.)
    perf.settle({ maxFrames: 2000, kinds: ['land processes'] });
    if (!land.landStats()?.ready) return { failed: `the land is not ready (${JSON.stringify(land.landStats())})` };
    const y = land.landHeight(0, -12);
    scn.createMesh({ mesh: 'box', color: 'white', x: 0, y: y + 1.5, z: -12, castsShadow: true });
    scn.setCamera({ fov: 60, near: 0.1, far: 2e5, position: [0, land.landHeight(0, 0) + 6, 0],
                    target: [0, y + 40, -60], up: [0, 1, 0] });
    for (let i = 0; i < 20; i++) {
        frames(1);
        if (perf.pendingWork().some((r) => /^(pipelines|shaders|clouds: not yet compiled)/.test(r))) skipped++;
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
    assert(!windowWay.failed && !headlessWay.failed, windowWay.failed || headlessWay.failed);
    console.log(`window's way: ${windowWay.skipped} frames skipped work, settled after ${windowWay.frames} (ev ${windowWay.ev.toFixed(3)}); ` +
                `headless's: settled after ${headlessWay.frames} (ev ${headlessWay.ev.toFixed(3)})`);
    assert(windowWay.skipped > 0, 'the window\'s way skips work while its shaders compile');
    assert(windowWay.settled.settled, `the window's way settles (${windowWay.settled.pending.join('; ')})`);
    assert(headlessWay.settled.settled, `headless's way settles (${headlessWay.settled.pending.join('; ')})`);
    assert(Math.abs(windowWay.ev - headlessWay.ev) < 0.01,
           `the same exposure both ways (${windowWay.ev.toFixed(3)} / ${headlessWay.ev.toFixed(3)})`);
    const d = differ(windowWay.img, headlessWay.img);
    console.log(`the two pictures: ${d.db.toFixed(1)} dB, ${d.over} px over 8 levels, worst ${d.worst}`);
    // (Two scenes settled headless's way in one process differ by some
    // 66 dB, a pixel or two over 8 levels: the grass's and the clouds'
    // last bits. What a skipped draw or a bake made without its pipeline
    // left would be a region of them.)
    assert(d.over < S * S / 500 && d.db > 55,
           `settled both ways, the same picture (${d.db.toFixed(1)} dB, ${d.over} px over 8 levels, worst ${d.worst})`);
    console.log('test_async_settle: OK');
}
