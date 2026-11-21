// Headless test: zero-recompile runtime @knobs and differentiable mod.knobGrad.
assert(typeof bro.kernel === 'object', 'bro.kernel namespace exists');
if (!bro.kernel.available) {
    skipTest('bro.kernel is compiled out of this build');
} else {
    const mod = bro.kernel.compile(`
        @knob
        const W: f32 = 2.0;

        @knob
        const COLOR: vec3<f32> = vec3<f32>(1.0, 2.0, 3.0);

        @kernel
        fn loss(x: f32) -> f32 {
            let y = W * x * x;
            let dotc = COLOR.x * x + COLOR.y * 2.0 + COLOR.z * 3.0;
            return y + dotc;
        }
    `, { file: 'knobs_ad.bk', runtimeKnobs: true });

    const kLoss = mod.kernel('loss');
    const hashBefore = kLoss.codeHash;
    const v0 = kLoss.eval(2.0); // 2*4 + (1*2 + 4 + 9) = 8 + 15 = 23
    assert(Math.abs(v0 - 23.0) < 1e-5, 'initial runtime knob eval');

    kLoss.setKnobs({ W: 3.0 });
    assert(kLoss.codeHash === hashBefore, 'setKnobs does not change codeHash');
    assert(Math.abs(kLoss.eval(2.0) - 27.0) < 1e-5, 'setKnobs updates value without recompile');

    const sameHandle = kLoss.knobs({ W: 1.5, COLOR: [2.0, 0.0, 0.0] });
    assert(sameHandle === kLoss, 'knobs() updates in place when runtimeKnobs is true');
    // 1.5*4 + 2*2 = 6 + 4 = 10
    assert(Math.abs(kLoss.eval(2.0) - 10.0) < 1e-5, 'in-place knobs() updates scalar & vec3 knobs');

    const dW = mod.knobGrad('loss', 'W');
    assert(Math.abs(dW.eval(3.0) - 9.0) < 1e-4, 'd(loss)/dW at x=3 equals x^2 = 9');

    const grads = mod.knobGrad('loss', ['W', 'COLOR']);
    assert(Math.abs(grads.W.eval(2.0) - 4.0) < 1e-4, 'gradMap W');
    const dc = grads.COLOR.eval(5.0);
    assert(Array.isArray(dc) && Math.abs(dc[0] - 5.0) < 1e-4 && Math.abs(dc[1] - 2.0) < 1e-4 && Math.abs(dc[2] - 3.0) < 1e-4,
           'gradMap COLOR returns vec3 partial derivatives');
}
