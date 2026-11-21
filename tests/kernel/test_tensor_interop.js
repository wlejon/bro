// Headless test: bro.kernel <-> bro.tensor interop (fromTensor, copyToTensor, copyFromTensor).
assert(typeof bro.kernel === 'object', 'bro.kernel namespace exists');
if (!bro.kernel.available || !bro.tensor || bro.tensor.available === false) {
    skipTest('bro.kernel or bro.tensor is compiled out of this build');
} else {
    bro.tensor.init();
    const mod = bro.kernel.compile(`
        @kernel @grid(1)
        fn k_saxpy(g: grid<1>, x: rw_store<f32, 1>, alpha: f32, beta: f32) {
            let i = g.x();
            x[i] = x[i] * alpha + beta;
        }
    `, { file: 'tensor_interop.bk' });
    const kSaxpy = mod.kernel('k_saxpy');

    const t = bro.tensor.createTensor(4, 2);
    const initial = new Float32Array([1, 2, 3, 4, 5, 6, 7, 8]);
    t.upload(initial);

    const kbuf = bro.kernel.fromTensor(t);
    assert(kbuf.byteLength === initial.byteLength, 'fromTensor matches byteLength');

    // Execute kernel on KernelBuffer (GPU if available, else CPU), then push back to tensor.
    if (bro.kernel.gpu) {
        kSaxpy.dispatch(8, kbuf, 2.0, 1.0);
    } else {
        kSaxpy.run(8, kbuf, 2.0, 1.0);
    }
    bro.kernel.copyToTensor(kbuf, t);

    const afterTo = t.download();
    for (let i = 0; i < 8; i++) {
        const want = initial[i] * 2.0 + 1.0;
        assert(Math.abs(afterTo[i] - want) < 1e-4, `copyToTensor[${i}]=${afterTo[i]} want ${want}`);
    }

    // Mutate tensor on bro.tensor side and sync back to KernelBuffer.
    const nextVals = new Float32Array([10, 20, 30, 40, 50, 60, 70, 80]);
    t.upload(nextVals);
    bro.kernel.copyFromTensor(t, kbuf);
    const readBack = kbuf.read(new Float32Array(8));
    for (let i = 0; i < 8; i++) {
        assert(Math.abs(readBack[i] - nextVals[i]) < 1e-5, `copyFromTensor[${i}]`);
    }

    // Zero-copy dispatchTensors directly over GpuTensor storage (Vulkan BDA or CPU host pointer).
    assert(typeof bro.kernel.dispatchTensors === 'function', 'bro.kernel.dispatchTensors exists');
    const zt = bro.tensor.createTensor(4, 2);
    const seed = new Float32Array([1, 2, 3, 4, 5, 6, 7, 8]);
    zt.upload(seed);
    bro.kernel.dispatchTensors(kSaxpy, 8, zt, 3.0, -2.0);
    const afterDirect = zt.download();
    for (let i = 0; i < 8; i++) {
        const want = seed[i] * 3.0 - 2.0;
        assert(Math.abs(afterDirect[i] - want) < 1e-4, `dispatchTensors[${i}]=${afterDirect[i]} want ${want}`);
    }
}
