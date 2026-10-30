// Headless test for the SDF tracer's GPU half on the engine's Vulkan device:
// a distance grid and a batch of sphere-traced rays dispatched there agree
// with the graph evaluated on the CPU.
assert(typeof SDFGraph === 'function', 'SDFGraph exists');
if (!bro.mesh.sdfVulkanAvailable()) {
    skipTest('no GPU device for the SDF tracer');
} else {
    const g = new SDFGraph();
    const sph = g.sphere(1);
    const bx = g.translate(g.box([0.5, 0.5, 0.5]), [0.8, 0, 0]);
    g.setRoot(g.opSmoothUnion(sph, bx, 0.2));

    assert(bro.mesh.sdfVulkanDeviceName().length > 0, 'the device has a name');

    // The grid: every cell against evalPoint.
    const n = 12, lo = -2, hi = 2;
    const grid = g.evaluateGridGpu({ dimX: n, dimY: n, dimZ: n, min: [lo, lo, lo], max: [hi, hi, hi] });
    assert(grid.usedVulkanGpu, 'the grid ran on the GPU');
    assert(grid.field.length === n * n * n, 'one distance per cell');
    const step = (hi - lo) / (n - 1);
    let worst = 0;
    for (let z = 0; z < n; z++)
        for (let y = 0; y < n; y++)
            for (let x = 0; x < n; x++) {
                const cpu = g.evalPoint(lo + x * step, lo + y * step, lo + z * step);
                worst = Math.max(worst, Math.abs(cpu - grid.field[(z * n + y) * n + x]));
            }
    assert(worst < 1e-4, `the GPU grid matches evalPoint (worst ${worst})`);

    // Rays: down -z through the scene, against the CPU's own trace of each.
    const rays = [];
    for (let i = 0; i < 16; i++) rays.push(-1.5 + i * 0.2, 0.1, 5, 0, 0, 0, -1, 20);
    const hits = g.traceRaysGpu(new Float32Array(rays));
    assert(bro.mesh.sdfLastTraceUsedGpu(), 'the rays ran on the GPU');
    assert(hits.length === 16 * 8, 'eight floats per hit');
    let hitCount = 0;
    for (let i = 0; i < 16; i++) {
        const r = rays.slice(i * 8, i * 8 + 8);
        const cpu = g.traceRay([r[0], r[1], r[2]], [r[4], r[5], r[6]]);
        const t = hits[i * 8], hit = hits[i * 8 + 6] > 0.5;
        assert(hit === cpu.hit, `ray ${i}: hit agrees (${hit} vs ${cpu.hit})`);
        if (hit) {
            hitCount++;
            assert(Math.abs(t - cpu.t) < 1e-3, `ray ${i}: t agrees (${t} vs ${cpu.t})`);
        }
    }
    assert(hitCount > 4 && hitCount < 16, `some rays hit and some miss (${hitCount})`);
}
