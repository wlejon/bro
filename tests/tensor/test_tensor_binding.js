// Weights-free binding tests for bro.tensor (brotensor GPU tensor + ops).
// Exercises the surface that needs no model files: namespace presence (or the
// honest available:false stub when built without BRO_WITH_TENSOR), op argument
// validation, and a tiny createTensor/upload/matmul/download round-trip on the
// default device. No model weights involved anywhere.

function expectThrows(fn, what) {
    let err = null;
    try { fn(); } catch (e) { err = e; }
    assert(err !== null, what + ' throws');
    return err;
}

assert(typeof bro === 'object', 'bro global exists');
assert(bro.tensor !== undefined && bro.tensor !== null, 'bro.tensor namespace exists');

if (bro.tensor.available === false) {
    const err = expectThrows(() => bro.tensor.createTensor(1, 1), 'stub createTensor()');
    assert(String(err.message).includes('compiled without'),
           'stub error names the missing build flag: ' + err.message);
    console.log('bro.tensor is the unavailable stub; stub contract OK');
} else {
    assert(bro.tensor.available === true, 'real binding reports available === true');
    assert(typeof bro.tensor.backend === 'string', 'bro.tensor.backend is a string');
    assert(typeof bro.tensor.dtype === 'object' && bro.tensor.dtype !== null,
           'bro.tensor.dtype enum object exists');
    for (const f of ['init', 'createTensor', 'matmul', 'linearForward',
                     'softmaxForward', 'layernormForward', 'openSafetensors',
                     'randn', 'sync']) {
        assert(typeof bro.tensor[f] === 'function', 'bro.tensor.' + f + ' is a function');
    }

    assert(bro.tensor.init() === undefined, 'init() returns undefined');
    assert(bro.tensor.init() === undefined, 'init() is idempotent');

    // ── op argument validation — synchronous TypeErrors ──────────────────────
    let err = expectThrows(() => bro.tensor.matmul(), 'matmul() with no args');
    assert(err instanceof TypeError, 'no-args matmul throws TypeError, got: ' + err);

    err = expectThrows(() => bro.tensor.matmul(1, 2, 3), 'matmul(non-tensors)');
    assert(err instanceof TypeError, 'non-tensor matmul throws TypeError, got: ' + err);

    err = expectThrows(() => bro.tensor.createTensor(-1, 2), 'createTensor(-1, 2)');
    assert(err instanceof RangeError, 'negative dim throws RangeError, got: ' + err);

    err = expectThrows(() => bro.tensor.openSafetensors('tests/tensor/__no_such__.st'),
                       'openSafetensors(nonexistent file)');
    assert(String(err.message).includes('cannot open'),
           'missing safetensors file gives a clean error: ' + err.message);

    // ── tiny compute round-trip on the default device (no weights) ───────────
    // C(2,2) = A(2,3) @ B(3,2)
    const A = bro.tensor.createTensor(2, 3);
    const B = bro.tensor.createTensor(3, 2);
    const C = bro.tensor.createTensor(2, 2);
    assert(A.rows === 2 && A.cols === 3, 'createTensor shape is (2,3)');
    A.upload(new Float32Array([1, 2, 3, 4, 5, 6]));
    B.upload(new Float32Array([7, 8, 9, 10, 11, 12]));
    bro.tensor.matmul(A, B, C);
    const out = C.download();
    assert(out instanceof Float32Array && out.length === 4,
           'download() returns a Float32Array of 4');
    const want = [58, 64, 139, 154];
    for (let i = 0; i < 4; i++) {
        assert(Math.abs(out[i] - want[i]) < 1e-4,
               'matmul[' + i + '] = ' + out[i] + ', want ' + want[i]);
    }
    bro.tensor.sync();

    console.log('bro.tensor binding contract OK (weights-free, backend=' +
                bro.tensor.backend + ')');
}
