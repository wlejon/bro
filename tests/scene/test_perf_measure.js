// perf.measure({ frames, warm, step, budget, stepMs }): a run of frames with
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
    const tick = () => {
        rafs++;
        if (k) k.dispatch([65536], buf, rafs);
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
    assert(Math.abs(r.budget - 1000 / 360) < 1e-9, 'the budget is a 360 Hz frame by default');

    // Over a budget of nothing every frame is a spike, with its breakdown.
    const all = perf.measure({ frames: 4, budget: 0 });
    assert(all.over === 4 && all.spikes.length === 4, `every frame over a zero budget (${all.over})`);
    assert(all.spikes.every((s, i) => s.frame === i && s.total === s.gpu + s.kernels), 'a spike names its frame and its ms');

    // A step that throws ends the run with its throw; a step not a function is a TypeError.
    let threw = null;
    try { perf.measure({ frames: 4, step: (i) => { if (i === 2) throw new Error('step 2'); } }); } catch (e) { threw = e; }
    assert(threw && threw.message === 'step 2', `the step's throw is the measure's (${threw})`);
    threw = null;
    try { perf.measure({ frames: 4, step: 3 }); } catch (e) { threw = e; }
    assert(threw instanceof TypeError, 'a step that is not a function is a TypeError');
}
