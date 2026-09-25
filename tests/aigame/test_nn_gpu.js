// bro.ai.game.nn on the GPU backend (CUDA or Metal, whichever bro.gpu reports).
//
// There is no separate nn.gpu namespace any more: the free ops take bro.tensor
// GpuTensors, which live on the active brotensor device, and the bundled nets
// move there with to("gpu"). Every GPU result here is checked against the same
// op run on the CPU (a Float32Array argument is viewed in place as a CPU
// tensor), so a backend that computes the wrong thing fails, not just one
// that throws. Skips only when the tower is compiled out or no GPU is present.

const nn = bro.ai.game.nn;

if (!nn || nn.available === false || !bro.tensor || bro.tensor.available === false) {
    console.log('test_nn_gpu: nn / tensor tower not built, skipping');
} else {
    bro.tensor.init();   // before reading backend, or a pre-init probe answers "cpu"
    if (!bro.gpu.available || bro.tensor.backend === 'cpu') {
        console.log('test_nn_gpu: no GPU backend (' + bro.gpu.backend + '), skipping');
    } else {
        runGpuTests(nn, bro.tensor);
        console.log('test_nn_gpu: OK on ' + bro.tensor.backend);
    }
}

function runGpuTests(nn, T) {
    const close = (a, b, tol) => Math.abs(a - b) <= tol * Math.max(1, Math.abs(b));
    function near(got, want, tol, what) {
        assert(got.length >= want.length, what + ': length ' + got.length + ' < ' + want.length);
        for (let i = 0; i < want.length; i++) {
            if (!close(got[i], want[i], tol)) {
                assert(false, what + '[' + i + ']: gpu ' + got[i] + ' vs cpu ' + want[i]);
                return;
            }
        }
    }
    function allFinite(arr, what) {
        for (let i = 0; i < arr.length; i++) {
            if (!Number.isFinite(arr[i])) { assert(false, what + '[' + i + '] non-finite: ' + arr[i]); return; }
        }
    }
    function dev(rows, cols, data) {
        const t = T.createTensor(rows, cols);
        if (data) t.upload(data); else t.zero();
        return t;
    }
    function ramp(n, scale, bias) {
        const a = new Float32Array(n);
        for (let i = 0; i < n; i++) a[i] = Math.sin(i * 1.7 + bias) * scale;
        return a;
    }

    // ── device tensor roundtrip ─────────────────────────────────────────
    {
        const t = nn.createTensor(3, 4);
        assert(t instanceof T.GpuTensor, 'nn.createTensor answers a GpuTensor');
        assert(t.rows === 3 && t.cols === 4 && t.size === 12, 'tensor shape');
        const src = new Float32Array(12).map((_, i) => i + 1);
        t.upload(src);
        near(t.download(), src, 0, 'roundtrip');
        t.zero();
        near(t.download(), new Float32Array(12), 0, 'zero fill');
    }

    // ── free ops: GPU tensors vs the CPU Float32Array path ──────────────
    const inDim = 16, outDim = 8;
    const Wh = ramp(outDim * inDim, 0.3, 0.1), bh = ramp(outDim, 0.1, 0.7), xh = ramp(inDim, 1.0, 1.3);

    // linearForward: y = W x + b.
    // A Float32Array is viewed as n x 1, so it cannot stand in for the
    // (out, in) W: the reference is computed by hand.
    const yRef = new Float32Array(outDim);
    for (let o = 0; o < outDim; o++) {
        let s = bh[o];
        for (let i = 0; i < inDim; i++) s += Wh[o * inDim + i] * xh[i];
        yRef[o] = s;
    }
    const W = dev(outDim, inDim, Wh), b = dev(outDim, 1, bh), x = dev(inDim, 1, xh), y = dev(outDim, 1);
    nn.linearForward(W, b, x, y);
    T.sync();
    near(y.download(), yRef, 1e-4, 'linearForward');

    // linearBackward: dX = W^T dY, dW += dY x^T, dB += dY.
    {
        const dYh = ramp(outDim, 0.5, 2.1);
        const dX = dev(inDim, 1), dW = dev(outDim, inDim), dB = dev(outDim, 1);
        nn.linearBackward(W, x, dev(outDim, 1, dYh), dX, dW, dB);
        T.sync();
        const dXr = new Float32Array(inDim), dWr = new Float32Array(outDim * inDim);
        for (let o = 0; o < outDim; o++) {
            for (let i = 0; i < inDim; i++) {
                dXr[i] += Wh[o * inDim + i] * dYh[o];
                dWr[o * inDim + i] = dYh[o] * xh[i];
            }
        }
        near(dX.download(), dXr, 1e-4, 'linearBackward dX');
        near(dW.download(), dWr, 1e-4, 'linearBackward dW');
        near(dB.download(), dYh, 1e-4, 'linearBackward dB');
    }

    // relu / tanh forward + backward, against the Float32Array (CPU) path.
    for (const [name, fwd, bwd, bwdTakesY] of [
        ['relu', nn.reluForward, nn.reluBackward, false],
        ['tanh', nn.tanhForward, nn.tanhBackward, true],
    ]) {
        const n = 32, inH = ramp(n, 2.0, 0.4), dYh = ramp(n, 1.0, 0.9);
        const outC = new Float32Array(n), dXC = new Float32Array(n);
        fwd(inH, outC);
        bwd(bwdTakesY ? outC : inH, dYh, dXC);

        const inG = dev(n, 1, inH), outG = dev(n, 1), dXG = dev(n, 1);
        fwd(inG, outG);
        bwd(bwdTakesY ? outG : inG, dev(n, 1, dYh), dXG);
        T.sync();
        near(outG.download(), outC, 1e-5, name + 'Forward');
        near(dXG.download(), dXC, 1e-5, name + 'Backward');
    }

    // softmax, and the fused softmax + xent, unmasked and masked. The mask
    // is brought to the op's device: a Float32Array mask is uploaded for a
    // GPU op (brotensor reads it as a device pointer), and a GpuTensor mask
    // is used in place. Masked entries come back exactly 0.
    {
        const n = 6, logH = new Float32Array([1, 2, 3, 4, -1, 0.5]);
        const maskH = new Float32Array([1, 0, 1, 0, 1, 1]);
        const target = new Float32Array([0, 0, 0.25, 0, 0, 0.75]);
        for (const [what, mask] of [['', null], [' masked', maskH], [' tensor-masked', dev(n, 1, maskH)]]) {
            const pC = new Float32Array(n);
            nn.softmaxForward(logH, pC, mask ? maskH : null);
            const pG = dev(n, 1);
            nn.softmaxForward(dev(n, 1, logH), pG, mask);
            T.sync();
            const pr = pG.download();
            near(pr, pC, 1e-5, 'softmaxForward' + what);
            let sum = 0; for (let i = 0; i < n; i++) sum += pr[i];
            assert(Math.abs(sum - 1) < 1e-4, 'softmax' + what + ' sums to 1, got ' + sum);

            const pXC = new Float32Array(n), dC = new Float32Array(n);
            const lossC = nn.softmaxXent(logH, target, pXC, dC, mask ? maskH : null);
            const pXG = dev(n, 1), dG = dev(n, 1);
            const lossG = nn.softmaxXent(dev(n, 1, logH), dev(n, 1, target), pXG, dG, mask);
            assert(close(lossG, lossC, 1e-4), 'softmaxXent' + what + ' loss gpu ' + lossG + ' vs cpu ' + lossC);
            const pXr = pXG.download(), dr = dG.download();
            near(pXr, pXC, 1e-5, 'softmaxXent' + what + ' probs');
            near(dr, dC, 1e-5, 'softmaxXent' + what + ' dLogits');
            if (mask) {
                for (let i = 0; i < n; i++) {
                    if (maskH[i] === 0) {
                        assert(pr[i] === 0 && pXr[i] === 0 && dr[i] === 0,
                               'masked entry ' + i + what + ': ' + pr[i] + ' ' + pXr[i] + ' ' + dr[i]);
                    }
                }
            }
        }
    }

    // factoredSoftmax / factoredXent over the move | attack | ability blocks,
    // with attack and ability masks (the trailing no-op class is always legal).
    {
        const nM = nn.N_MOVE, nA = nn.N_ATTACK, nB = nn.N_ABILITY, total = nM + nA + nB;
        const logH = ramp(total, 2.0, 0.3);
        const aMask = new Float32Array(nA - 1).fill(1), bMask = new Float32Array(nB - 1).fill(1);
        aMask[0] = 0; bMask[nB - 2] = 0;
        const soft = (len, illegal) => {
            const t = new Float32Array(len);
            let s = 0;
            for (let i = 0; i < len; i++) { t[i] = illegal.includes(i) ? 0 : i + 1; s += t[i]; }
            return t.map(v => v / s);
        };
        const mT = soft(nM, []), aT = soft(nA, [0]), bT = soft(nB, [nB - 2]);

        const pC = new Float32Array(total);
        nn.factoredSoftmax(logH, pC, aMask, bMask);
        const pG = dev(total, 1);
        nn.factoredSoftmax(dev(total, 1, logH), pG, aMask, bMask);
        T.sync();
        const pr = pG.download();
        near(pr, pC, 1e-5, 'factoredSoftmax masked');
        assert(pr[nM] === 0 && pr[nM + nA + nB - 2] === 0, 'factoredSoftmax masked entries are 0');

        const pXC = new Float32Array(total), dC = new Float32Array(total);
        const lossC = nn.factoredXent(logH, mT, aT, bT, pXC, dC, aMask, bMask);
        const pXG = dev(total, 1), dG = dev(total, 1);
        const lossG = nn.factoredXent(dev(total, 1, logH), dev(nM, 1, mT), dev(nA, 1, aT), dev(nB, 1, bT),
                                      pXG, dG, dev(nA - 1, 1, aMask), bMask);
        assert(close(lossG, lossC, 1e-4), 'factoredXent loss gpu ' + lossG + ' vs cpu ' + lossC);
        near(pXG.download(), pXC, 1e-5, 'factoredXent probs');
        near(dG.download(), dC, 1e-5, 'factoredXent dLogits');
    }

    // ── PolicyValueNet on the GPU matches the same weights on the CPU ───
    {
        const opts = { inDim: 12, hidden: [32, 32], valueHidden: 16, numActions: 5, seed: 0xC0DEn };
        const cpu = nn.createPolicyValueNet(opts);
        const gpu = nn.createPolicyValueNet(opts);
        gpu.load(cpu.save());
        assert(cpu.device === 'cpu', 'fresh net is on the cpu, got ' + cpu.device);
        gpu.to('gpu');
        assert(gpu.device === 'gpu', 'to("gpu") moves the net, device ' + gpu.device);

        const obsH = ramp(opts.inDim, 1.0, 0.2);
        const logitsC = new Float32Array(opts.numActions);
        const vC = cpu.forward(obsH, logitsC);

        const logitsG = dev(opts.numActions, 1);
        const vG = gpu.forward(dev(opts.inDim, 1, obsH), logitsG);
        assert(close(vG, vC, 1e-3), 'pvnet value gpu ' + vG + ' vs cpu ' + vC);
        near(logitsG.download(), logitsC, 1e-3, 'pvnet logits');

        // Batched forward: B rows in one dispatch, each row equal to the single forward.
        const B = 4;
        const xB = new Float32Array(B * opts.inDim);
        for (let r = 0; r < B; r++) xB.set(ramp(opts.inDim, 1.0, 0.2 + r), r * opts.inDim);
        const logitsB = dev(B, opts.numActions), valuesB = dev(B, 1);
        gpu.forwardBatched(dev(B, opts.inDim, xB), logitsB, valuesB);
        T.sync();
        const lB = logitsB.download(), vB = valuesB.download();
        for (let r = 0; r < B; r++) {
            const lr = new Float32Array(opts.numActions);
            const vr = cpu.forward(xB.subarray(r * opts.inDim, (r + 1) * opts.inDim), lr);
            assert(close(vB[r], vr, 1e-3), 'forwardBatched value[' + r + '] gpu ' + vB[r] + ' vs cpu ' + vr);
            near(lB.subarray(r * opts.numActions, (r + 1) * opts.numActions), lr, 1e-3,
                 'forwardBatched logits[' + r + ']');
        }

        // One SGD step on both from the same gradient: the weights stay equal.
        gpu.forward(dev(opts.inDim, 1, obsH), logitsG);
        cpu.forward(obsH, logitsC);
        const dLh = ramp(opts.numActions, 0.2, 1.1);
        cpu.zeroGrad(); gpu.zeroGrad();
        cpu.backward(0.5, dLh);
        gpu.backward(0.5, dev(opts.numActions, 1, dLh));
        cpu.sgdStep(0.05, 0.0);
        gpu.sgdStep(0.05, 0.0);
        const v2C = cpu.forward(obsH, logitsC);
        const v2G = gpu.forward(dev(opts.inDim, 1, obsH), logitsG);
        assert(close(v2G, v2C, 1e-3), 'after sgdStep value gpu ' + v2G + ' vs cpu ' + v2C);
        assert(!close(v2C, vC, 1e-7), 'sgdStep changed the net');
        near(logitsG.download(), logitsC, 1e-3, 'after sgdStep logits');

        // The GPU net's blob round-trips through a WeightsHandle into a CPU net.
        const handle = nn.createWeightsHandle();
        handle.publish(gpu.save(), 1n);
        const snap = handle.snapshot();
        assert(snap && snap.version === 1n, 'weights handle snapshot');
        const back = nn.createPolicyValueNet(opts);
        back.load(snap.blob);
        const lBack = new Float32Array(opts.numActions);
        const vBack = back.forward(obsH, lBack);
        assert(close(vBack, v2G, 1e-3), 'gpu blob loads on the cpu: ' + vBack + ' vs ' + v2G);
    }

    // ── Adam on device tensors stays finite and moves against the gradient ──
    {
        const p = dev(4, 1, new Float32Array([1, 1, 1, 1]));
        const g = dev(4, 1, new Float32Array([0.1, -0.1, 0.2, -0.2]));
        const m = dev(4, 1), v = dev(4, 1);
        T.adamStep(p, g, m, v, 0.01, 0.9, 0.999, 1e-8, 1);
        T.sync();
        const pr = p.download();
        allFinite(pr, 'adam');
        assert(pr[0] < 1 && pr[1] > 1 && pr[2] < 1 && pr[3] > 1, 'adam steps against the gradient: ' + Array.from(pr));
    }
}
