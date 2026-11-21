// Test bro.eye.fit closed-loop CMA-ES convergence and determinism.

const W = 48, H = 48;
function makeSolidRgb(r, g, b) {
    const data = new Uint8ClampedArray(W * H * 4);
    const ri = Math.max(0, Math.min(255, Math.round(r)));
    const gi = Math.max(0, Math.min(255, Math.round(g)));
    const bi = Math.max(0, Math.min(255, Math.round(b)));
    for (let i = 0; i < data.length; i += 4) {
        data[i] = ri; data[i + 1] = gi; data[i + 2] = bi; data[i + 3] = 255;
    }
    return { width: W, height: H, data };
}

const target = makeSolidRgb(64, 175, 210);
const fitOpts = {
    params: [
        { name: 'r', min: 0, max: 255, start: 220 },
        { name: 'g', min: 0, max: 255, start: 40 },
        { name: 'b', min: 0, max: 255, start: 40 },
    ],
    reference: target,
    maxEvaluations: 160,
    targetDistance: 0.02,
    seed: 1337,
};

const run1 = bro.eye.fit((p) => makeSolidRgb(p.r, p.g, p.b), fitOpts);
const run2 = bro.eye.fit((p) => makeSolidRgb(p.r, p.g, p.b), fitOpts);

assert(run1.ok === true, 'fit succeeded');
assert(run1.distance < 0.06, `CMA-ES converged tightly, distance=${run1.distance}`);
assert(Math.abs(run1.params.r - 64) < 12 && Math.abs(run1.params.g - 175) < 12 && Math.abs(run1.params.b - 210) < 12,
    `recovered params close to target: ${JSON.stringify(run1.params)}`);
assert(run1.distance === run2.distance &&
       run1.params.r === run2.params.r &&
       run1.params.g === run2.params.g &&
       run1.params.b === run2.params.b,
    'fit is bit-deterministic for fixed seed');

// Also verify target measures optimization (target middle luminance)
const lumaRun = bro.eye.fit(
    (p) => makeSolidRgb(p.v, p.v, p.v),
    {
        params: [{ name: 'v', min: 0, max: 255, start: 15 }],
        measures: { lumaMean: 0.65 },
        maxEvaluations: 60,
        targetDistance: 0.03,
        seed: 7,
    }
);
assert(Math.abs(lumaRun.frame.lumaMean - 0.65) < 0.03,
    `measure target converged to lumaMean ~0.65, got ${lumaRun.frame.lumaMean}`);
