// CPU NN primitives: PolicyValueNet, FactoredPolicyHead, DeepSetsEncoder, WeightsHandle.

const nn = bro.ai.game.nn;

function allFinite(arr, name) {
    for (let i = 0; i < arr.length; i++) {
        if (!Number.isFinite(arr[i])) {
            assert(false, name + '[' + i + '] non-finite: ' + arr[i]);
            return;
        }
    }
}

// Tensor basics.
{
    const t = nn.createTensor(3, 2);
    assert(t.rows === 3 && t.cols === 2 && t.size === 6, 'tensor shape');
    t.fromArray(new Float32Array([1, 2, 3, 4, 5, 6]));
    const arr = t.toArray();
    assert(arr instanceof Float32Array && arr.length === 6, 'toArray Float32Array len 6');
    assert(arr[0] === 1 && arr[5] === 6, 'roundtrip values');
    t.zero();
    const z = t.toArray();
    for (let i = 0; i < 6; i++) assert(z[i] === 0, 'zero fill');
}

// PolicyValueNet forward.
{
    const inDim = 12, numActions = 4;
    const net = nn.createPolicyValueNet({
        inDim, hidden: [16, 16], valueHidden: 8, numActions, seed: 0xC0DEn,
    });
    assert(net.inDim === inDim, 'pvnet inDim');
    assert(net.numActions === numActions, 'pvnet numActions');
    assert(typeof net.numParams === 'number' && net.numParams > 0, 'numParams > 0');

    const x = nn.createTensor(inDim, 1);
    x.fromArray(new Float32Array(inDim).fill(0.1));
    const logits = nn.createTensor(numActions, 1);
    const v = net.forward(x, logits);
    assert(typeof v === 'number' && Number.isFinite(v), 'value scalar finite, got ' + v);
    assert(v >= -1.0 && v <= 1.0, 'value in [-1,1], got ' + v);
    allFinite(logits.toArray(), 'logits');

    // forwardBatched: B rows in one call must match B independent single-row
    // forward() calls (within float tolerance).
    const B = 3;
    const xB = nn.createTensor(B, inDim);
    const rowsIn = [];
    for (let r = 0; r < B; r++) {
        const row = new Float32Array(inDim).fill(0.1 * (r + 1));
        rowsIn.push(row);
        for (let c = 0; c < inDim; c++) xB.set(r, c, row[c]);
    }
    const logitsB = nn.createTensor(B, numActions);
    const valuesB = nn.createTensor(B, 1);
    net.forwardBatched(xB, logitsB, valuesB);
    for (let r = 0; r < B; r++) {
        const xr = nn.createTensor(inDim, 1);
        xr.fromArray(rowsIn[r]);
        const lr = nn.createTensor(numActions, 1);
        const vr = net.forward(xr, lr);
        assert(Math.abs(valuesB.get(r, 0) - vr) < 1e-3,
            'forwardBatched value row ' + r + ' matches single forward: ' + valuesB.get(r, 0) + ' vs ' + vr);
        for (let c = 0; c < numActions; c++) {
            assert(Math.abs(logitsB.get(r, c) - lr.get(c, 0)) < 1e-3,
                'forwardBatched logits row ' + r + ' col ' + c + ' matches single forward');
        }
    }
}

// FactoredPolicyHead.
{
    const embed = 8;
    const ph = nn.createFactoredPolicyHead(embed, 1n);
    assert(typeof ph.totalLogits === 'number' && ph.totalLogits > 0, 'totalLogits > 0');
    const emb = nn.createTensor(embed, 1);
    emb.fromArray(new Float32Array(embed).fill(0.2));
    const logits = nn.createTensor(ph.totalLogits, 1);
    ph.forward(emb, logits);
    allFinite(logits.toArray(), 'factored logits');
}

// DeepSetsEncoder.
{
    const enc = nn.createDeepSetsEncoder({ hidden: 8, embedDim: 8 }, 1n);
    assert(enc.outDim === 3 * 8, 'enc outDim = 3*embedDim, got ' + enc.outDim);
    const obs = nn.createTensor(bro.ai.game.OBS_TOTAL, 1);
    obs.fromArray(new Float32Array(bro.ai.game.OBS_TOTAL).fill(0.1));
    const embed = nn.createTensor(enc.outDim, 1);
    enc.forward(obs, embed);
    allFinite(embed.toArray(), 'enc embed');
}

// WeightsHandle.
{
    const net = nn.createSingleHeroNet({ enc: { hidden: 8, embedDim: 8 }, trunkHidden: 16, valueHidden: 8, seed: 1n });
    const blob = net.save();
    assert(blob instanceof Uint8Array, 'save() is Uint8Array');
    assert(blob.length > 0, 'blob nonempty');

    const h = nn.createWeightsHandle();
    h.publish(blob, 7n);
    assert(typeof h.version() === 'bigint' || typeof h.version() === 'number',
        'version is bigint/number, got ' + typeof h.version());
    const snap = h.snapshot();
    assert(snap && snap.blob && snap.version !== undefined, 'snapshot has blob, version');
    assert(snap.blob.length === blob.length, 'snap blob same length');

    // Apply back.
    net.load(snap.blob);
}

// Softmax determinism + sums to 1.
{
    const logits = nn.createTensor(4, 1);
    logits.fromArray(new Float32Array([1, 2, 3, 4]));
    const probs = nn.createTensor(4, 1);
    nn.softmaxForward(logits, probs, null);
    const p = probs.toArray();
    let sum = 0; for (let i = 0; i < 4; i++) sum += p[i];
    assert(Math.abs(sum - 1) < 1e-4, 'softmax sums to 1, got ' + sum);
    for (let i = 0; i < 4; i++) assert(p[i] >= 0, 'softmax non-negative');
    assert(p[3] > p[0], 'larger logit -> larger prob');
}

console.log('test_nn_cpu: OK');
