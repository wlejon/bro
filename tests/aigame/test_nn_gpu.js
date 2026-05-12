// GPU NN primitives — skip cleanly when CUDA not present.

const gpu = bro.ai.game.nn.gpu;

if (!gpu || gpu.available !== true) {
    console.log('test_nn_gpu: GPU unavailable, skipping');
} else {
    try {
        gpu.init();
    } catch (e) {
        console.log('test_nn_gpu: gpu.init threw (' + e + '), skipping');
    }

    // createTensor.
    let okBasic = true;
    try {
        const t = gpu.createTensor(3, 4);
        assert(t.rows === 3 && t.cols === 4 && t.size === 12, 'gpu tensor shape');
        t.zero();
        const back = t.download();
        assert(back instanceof Float32Array, 'download returns Float32Array');
        assert(back.length === 12, 'download len = 12');
        for (let i = 0; i < 12; i++) assert(back[i] === 0, 'zero fill');

        t.upload(new Float32Array([1,2,3,4,5,6,7,8,9,10,11,12]));
        const back2 = t.download();
        for (let i = 0; i < 12; i++) assert(back2[i] === i + 1, 'roundtrip[' + i + ']');
    } catch (e) {
        okBasic = false;
        assert(false, 'BUG: gpu basic tensor ops threw: ' + e);
    }

    if (okBasic) {
        // Dense forward y = W*x + b.
        const inDim = 4, outDim = 2;
        const W = gpu.createTensor(outDim, inDim);
        const b = gpu.createTensor(outDim, 1);
        const x = gpu.createTensor(inDim, 1);
        const y = gpu.createTensor(outDim, 1);
        W.upload(new Float32Array([1,0,0,0, 0,1,0,0]));   // identity-like 2x4
        b.upload(new Float32Array([0, 0]));
        x.upload(new Float32Array([5, 6, 7, 8]));
        gpu.linearForward(W, b, x, y);
        gpu.sync();
        const yr = y.download();
        assert(Math.abs(yr[0] - 5) < 1e-4 && Math.abs(yr[1] - 6) < 1e-4,
            'gpu dense forward, got [' + yr[0] + ',' + yr[1] + ']');

        // Softmax.
        const logits = gpu.createTensor(4, 1);
        const probs = gpu.createTensor(4, 1);
        logits.upload(new Float32Array([1, 2, 3, 4]));
        gpu.softmaxForward(logits, probs, null);
        gpu.sync();
        const pr = probs.download();
        let sum = 0; for (let i = 0; i < 4; i++) sum += pr[i];
        assert(Math.abs(sum - 1) < 1e-3, 'gpu softmax sums to 1, got ' + sum);

        // Adam step shouldn't NaN weights.
        const p = gpu.createTensor(4, 1); p.upload(new Float32Array([1, 1, 1, 1]));
        const g = gpu.createTensor(4, 1); g.upload(new Float32Array([0.1, -0.1, 0.2, -0.2]));
        const mT = gpu.createTensor(4, 1); mT.zero();
        const vT = gpu.createTensor(4, 1); vT.zero();
        gpu.adamStep(p, g, mT, vT, 0.01, 0.9, 0.999, 1e-8, 1);
        gpu.sync();
        const pr2 = p.download();
        for (let i = 0; i < 4; i++) assert(Number.isFinite(pr2[i]), 'adam result finite[' + i + ']');
    }

    console.log('test_nn_gpu: OK');
}
