// Headless test for the SDF tracer on the engine's Vulkan device: a distance
// grid and a batch of sphere-traced rays dispatched there agree with the
// graph's distance function and with single traced rays.
assert(typeof SDFGraph === 'function', 'SDFGraph exists');
const g = new SDFGraph();
const sph = g.sphere(1);
const bx = g.translate(g.box([0.5, 0.5, 0.5]), [0.8, 0, 0]);
g.setRoot(g.opSmoothUnion(sph, bx, 0.2));

// The grid: every cell against evalPoint.
const n = 12, lo = -2, hi = 2;
const grid = g.evaluateGrid({ dimX: n, dimY: n, dimZ: n, min: [lo, lo, lo], max: [hi, hi, hi] });
assert(grid && grid.field.length === n * n * n, 'one distance per cell');
const step = (hi - lo) / (n - 1);
let worst = 0;
for (let z = 0; z < n; z++)
    for (let y = 0; y < n; y++)
        for (let x = 0; x < n; x++) {
            const d = g.evalPoint(lo + x * step, lo + y * step, lo + z * step);
            worst = Math.max(worst, Math.abs(d - grid.field[(z * n + y) * n + x]));
        }
assert(worst < 1e-4, `the GPU grid matches evalPoint (worst ${worst})`);

// Rays: down -z through the scene, the batch against each ray traced alone,
// and every hit on the surface (evalPoint ~ 0 there).
const rays = [];
for (let i = 0; i < 16; i++) rays.push(-1.5 + i * 0.2, 0.1, 5, 0, 0, 0, -1, 20);
const hits = g.traceRays(new Float32Array(rays));
assert(hits.length === 16 * 8, 'eight floats per hit');
let hitCount = 0;
for (let i = 0; i < 16; i++) {
    const r = rays.slice(i * 8, i * 8 + 8);
    const one = g.traceRay([r[0], r[1], r[2]], [r[4], r[5], r[6]]);
    const t = hits[i * 8], hit = hits[i * 8 + 6] > 0.5;
    assert(hit === one.hit, `ray ${i}: hit agrees (${hit} vs ${one.hit})`);
    if (hit) {
        hitCount++;
        assert(Math.abs(t - one.t) < 1e-5, `ray ${i}: t agrees (${t} vs ${one.t})`);
        const d = g.evalPoint(r[0], r[1], r[2] - t);
        assert(Math.abs(d) < 1e-3, `ray ${i}: the hit is on the surface (${d})`);
    }
}
assert(hitCount > 4 && hitCount < 16, `some rays hit and some miss (${hitCount})`);
