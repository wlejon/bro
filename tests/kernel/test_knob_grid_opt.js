// Headless test: mod.knobGradGrid multi-step gradient descent without recompilation.
assert(typeof bro.kernel === 'object', 'bro.kernel namespace exists');
if (!bro.kernel.available) {
    skipTest('bro.kernel is compiled out of this build');
} else {
    const src = `
        @knob const A: f32 = 0.0;
        @knob const B: vec3<f32> = vec3<f32>(0.0, 0.0, 0.0);

        pub fn cell_loss(g: grid<1>, target: read_store<f32, 1>, inv_n: f32) -> f32 {
            let i: i32 = g.x();
            let t: f32 = f32(i) * 0.09817477;
            let pred: f32 = A * cos(t) + B.x + B.y * sin(t) + B.z * cos(2.0 * t);
            let diff: f32 = pred - target[i];
            return 0.5 * diff * diff * inv_n;
        }

        @kernel @grid(1)
        pub fn eval_loss(g: grid<1>, target: read_store<f32, 1>, inv_n: f32, out_loss: rw_store<f32, 1>) {
            out_loss.atomic_add(0, cell_loss(g, target, inv_n));
        }
    `;

    const mod = bro.kernel.compile(src, { file: 'knob_grid_opt.bk', runtimeKnobs: true });
    const kLoss = mod.kernel('eval_loss');
    const kGrad = mod.knobGradGrid('cell_loss', ['A', 'B'], 1);
    assert(kGrad && kGrad.gridDim === 1, 'knobGradGrid returned @grid(1) kernel');
    const hash0 = kGrad.codeHash;

    const N = 64;
    const invN = 1.0 / N;
    const trueA = 1.5, trueBx = -0.4, trueBy = 0.7, trueBz = 0.5;
    const target = new Float32Array(N);
    for (let i = 0; i < N; i++) {
        const t = i * 0.09817477;
        target[i] = trueA * Math.cos(t) + trueBx + trueBy * Math.sin(t) + trueBz * Math.cos(2.0 * t);
    }

    const computeLoss = (a, bx, by, bz) => {
        kLoss.setKnobs({ A: a, B: [bx, by, bz] });
        const out = new Float32Array(1);
        kLoss.run(N, target, invN, out);
        return out[0];
    };

    const p = [0, 0, 0, 0];
    const m = [0, 0, 0, 0];
    const v = [0, 0, 0, 0];
    const loss0 = computeLoss(p[0], p[1], p[2], p[3]);
    assert(loss0 > 0.5, `initial loss should be large, got ${loss0}`);

    const gradOut = new Float32Array(4);
    const lr = 0.08, b1 = 0.9, b2 = 0.999, eps = 1e-8;
    for (let step = 1; step <= 160; step++) {
        kGrad.setKnobs({ A: p[0], B: [p[1], p[2], p[3]] });
        gradOut.fill(0);
        kGrad.run(N, target, invN, gradOut);
        const bc1 = 1 - Math.pow(b1, step);
        const bc2 = 1 - Math.pow(b2, step);
        for (let j = 0; j < 4; j++) {
            m[j] = b1 * m[j] + (1 - b1) * gradOut[j];
            v[j] = b2 * v[j] + (1 - b2) * gradOut[j] * gradOut[j];
            p[j] -= lr * (m[j] / bc1) / (Math.sqrt(v[j] / bc2) + eps);
        }
    }
    assert(kGrad.codeHash === hash0, 'codeHash must remain invariant across runtime knob updates');
    const lossFinal = computeLoss(p[0], p[1], p[2], p[3]);
    assert(lossFinal * 100 < loss0 && lossFinal < 1e-4, `Adam failed to converge: loss0=${loss0} -> lossFinal=${lossFinal}`);
    assert(Math.abs(p[0] - trueA) < 0.05, `A=${p[0]} want ${trueA}`);
    assert(Math.abs(p[1] - trueBx) < 0.05, `B.x=${p[1]} want ${trueBx}`);
    assert(Math.abs(p[2] - trueBy) < 0.05, `B.y=${p[2]} want ${trueBy}`);
    assert(Math.abs(p[3] - trueBz) < 0.05, `B.z=${p[3]} want ${trueBz}`);
}
