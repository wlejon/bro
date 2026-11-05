// perf.measure({ frames, warm, step, budget, stepMs, kernels }): a run of frames with
// step(i) before each, one page frame (rAF) and one render a frame, each
// frame's scene GPU ms and kernel GPU ms waited on, summarised, and the
// frames over the budget listed with what they went on.

const cv = document.createElement('canvas');
cv.setAttribute('width', '64');
cv.setAttribute('height', '64');
document.body.appendChild(cv);
flush();
const scene = cv.getContext('scene');
if (!scene) {
    missingGpuContext('scene');
} else {
    scene.setCamera({ fov: 50, near: 0.1, far: 100, position: [0, 2, 3], target: [0, 0, 0] });
    const box = scene.createMesh({
        positions: new Float32Array([-2, 0, -2, 2, 0, -2, -2, 0, 2, 2, 0, 2]),
        normals: new Float32Array([0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0]),
        indices: new Uint32Array([0, 2, 1, 1, 2, 3]),
        color: [0.5, 0.5, 0.5],
    });
    // A kernel the page dispatches each frame, as a world's producers do.
    const gpuKernel = bro.kernel && bro.kernel.available && bro.kernel.gpu;
    let k = null, buf = null;
    if (gpuKernel) {
        k = bro.kernel.compile(`
            @kernel @grid(1)
            fn k_fill(g: grid<1>, out: write_store<f32, 1>, v: f32) {
                out[g.x()] = v * (g.x() as f32);
            }`).kernel('k_fill');
        buf = bro.kernel.createBuffer(65536 * 4);
    }
    let rafs = 0;
    const bgBuf = gpuKernel ? bro.kernel.createBuffer(65536 * 4) : null;
    // A tile cache updated each frame, once the convergence check sets it.
    let cache = null;
    const view = { position: [500, 5, 500], fov: 60, viewportHeight: 1080 };
    const tick = () => {
        rafs++;
        if (cache) cache.update(view);
        if (k) k.dispatch([65536], buf, rafs);
        // And refinement for the gap after the frame, as a tile cache's production.
        if (k) bro.kernel.background(() => k.dispatch([65536], bgBuf, rafs));
        scene.captureFrame();
        requestAnimationFrame(tick);
    };
    requestAnimationFrame(tick);
    advanceTime(16);

    const seen = [];
    const before = rafs;
    const r = perf.measure({ frames: 12, warm: 3, step: (i) => { seen.push(i); box.rotation = [0, i * 0.1, 0]; } });
    assert(r.frames === 12, `twelve frames measured (${r.frames})`);
    assert(seen.length === 15 && seen.every((v, i) => v === i), `step runs before each frame, warm ones first (${seen})`);
    assert(rafs - before === 15, `one page frame a measured frame (${rafs - before} for 15)`);
    for (const key of ['gpu', 'kernels', 'total', 'wall'])
        assert(['mean', 'median', 'p90', 'p99', 'max'].every((s) => typeof r[key][s] === 'number'), `${key} has its spread`);
    assert(r.gpu.mean > 0 && r.gpu.max >= r.gpu.p90 && r.gpu.p90 >= r.gpu.median, `the scene's GPU ms (${JSON.stringify(r.gpu)})`);
    assert(Math.abs(r.total.mean - r.gpu.mean - r.kernels.mean) < 1e-6, 'total is the scene and the kernels');
    assert(Object.keys(r.passes).length > 0, `the passes' mean ms (${JSON.stringify(r.passes)})`);
    if (gpuKernel) assert(r.kernelsBy.k_fill > 0 && r.kernels.mean > 0, `the page's kernel timed (${JSON.stringify(r.kernelsBy)})`);
    // The refinement is run after each frame, timed whole, and apart from the frame's total.
    if (gpuKernel) {
        assert(r.refine.mean > 0 && r.refine.max >= r.refine.mean, `the refinement's GPU ms a frame (${JSON.stringify(r.refine)})`);
        assert(r.backgroundBy['k_fill'] > 0, `its kernels listed apart (${JSON.stringify(r.backgroundBy)})`);
        assert(typeof r.spill === 'number' && r.spill >= r.over, `frames whose refinement overran the interval (${r.spill})`);
    }
    assert(Math.abs(r.budget - 1000 / 360) < 1e-9, 'the budget is a 360 Hz frame by default');

    // kernels: false runs the dispatches untimed (their stamps serialize
    // them): the refinement still measured whole, the kernels by name not.
    if (gpuKernel) {
        const u = perf.measure({ frames: 6, warm: 1, kernels: false });
        assert(u.refine.mean > 0, `untimed, the refinement's GPU ms (${JSON.stringify(u.refine)})`);
        assert(!(u.backgroundBy['k_fill'] > 0) && u.kernels.mean === 0, `and no kernel's own (${JSON.stringify(u.backgroundBy)})`);
    }

    // Over a budget of nothing every frame is a spike, with its breakdown.
    const all = perf.measure({ frames: 4, budget: 0 });
    assert(all.over === 4 && all.spikes.length === 4, `every frame over a zero budget (${all.over})`);
    assert(all.spill === 4, `and every one spilled (${all.spill})`);
    assert(all.spikes.every((s, i) => s.frame === i && s.total === s.gpu + s.kernels), 'a spike names its frame and its ms');

    // converge: then frames with no step until nothing (of `kinds`) is
    // pending, measured alike: here a tile cache refining for a view.
    if (gpuKernel) {
        cache = bro.tiles.createCache({
            domain: { type: 'plane', origin: [0, 0], size: 1000, roots: [1, 1] },
            tileSize: 8, capacity: 128, maxLevel: 4, pixelsPerCell: 2, layers: { height: 4 }, producePerUpdate: 2,
            produce(batch) { k.dispatch([8 * 8 * batch.count], this.layer('height'), 1); },
        });
        let steps = 0;
        const c = perf.measure({ frames: 2, step: () => { steps++; }, converge: { maxFrames: 400, kinds: ['tiles'] } });
        assert(steps === 2, `step runs for the measured frames only (${steps})`);
        assert(c.converge && c.converge.settled && c.converge.frames > 0,
               `then frames until the tiles are produced (${JSON.stringify(c.converge && { s: c.converge.settled, f: c.converge.frames, p: c.converge.pending })})`);
        assert(cache.pending === 0, `and they are (${cache.pending})`);
        assert(typeof c.converge.total.mean === 'number' && typeof c.converge.refine.mean === 'number' &&
               typeof c.converge.spill === 'number' && typeof c.converge.over === 'number', 'measured as the run is');
        const none = perf.measure({ frames: 1, converge: { kinds: ['tiles'] } });
        assert(none.converge.settled && none.converge.frames === 0, `nothing pending: no frames (${none.converge.frames})`);
        cache = null;
    }

    // A step that throws ends the run with its throw; a step not a function is a TypeError.
    let threw = null;
    try { perf.measure({ frames: 4, step: (i) => { if (i === 2) throw new Error('step 2'); } }); } catch (e) { threw = e; }
    assert(threw && threw.message === 'step 2', `the step's throw is the measure's (${threw})`);
    threw = null;
    try { perf.measure({ frames: 4, step: 3 }); } catch (e) { threw = e; }
    assert(threw instanceof TypeError, 'a step that is not a function is a TypeError');
}
