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

    // Zero-copy dispatchTensors directly over GpuTensor storage (CUDA PTX, Vulkan BDA, or CPU).
    assert(typeof bro.kernel.dispatchTensors === 'function', 'bro.kernel.dispatchTensors exists');
    const zt = bro.tensor.createTensor(4, 2);
    const seed = new Float32Array([1, 2, 3, 4, 5, 6, 7, 8]);
    zt.upload(seed);
    bro.kernel.dispatchTensors(kSaxpy, 8, zt, 3.0, -2.0);
    // Chain a native bro.tensor op right after dispatchTensors on the same device stream.
    bro.tensor.addScalarInplace(zt, 5.0);
    const afterDirect = zt.download();
    for (let i = 0; i < 8; i++) {
        const want = seed[i] * 3.0 - 2.0 + 5.0;
        assert(Math.abs(afterDirect[i] - want) < 1e-4, `dispatchTensors+tensor1D[${i}]=${afterDirect[i]} want ${want}`);
    }

    // 2D kernel via dispatchTensors + chained matmul (proves 2D grid + multi-store PTX/SPIR-V ABI).
    const mod2d = bro.kernel.compile(`
        @kernel @grid(2)
        fn k_mat_bias(g: grid<2>, out: rw_store<f32, 2>, inp: store<f32, 2>, bias: f32) {
            let x = g.x();
            let y = g.y();
            out[x, y] = inp[x, y] * 2.0 + bias;
        }
    `, { file: 'tensor_interop_2d.bk' });
    const kMat = mod2d.kernel('k_mat_bias');
    const matIn = bro.tensor.createTensor(2, 3);
    matIn.upload(new Float32Array([1, 2, 3, 4, 5, 6]));
    const matOut = bro.tensor.createTensor(2, 3);
    bro.kernel.dispatchTensors(kMat, [3, 2], matOut, matIn, 0.5);
    const out2d = matOut.download();
    for (let i = 0; i < 6; i++) {
        const want = (i + 1) * 2.0 + 0.5;
        assert(Math.abs(out2d[i] - want) < 1e-4, `dispatchTensors2D[${i}]=${out2d[i]} want ${want}`);
    }

    // Mixing CPU and GPU tensors in a single dispatch must throw TypeError.
    const cpuKb = bro.kernel.createBuffer(8 * 4, { residency: 'host' });
    const cpuOnly = bro.kernel.toTensor(cpuKb, [4, 2], { dtype: 'f32', device: 'cpu' });
    if (zt.device !== 'cpu') {
        let threwMix = false;
        try {
            const modCopy = bro.kernel.compile(`
                @kernel @grid(1)
                fn k_copy_t(g: grid<1>, dst: rw_store<f32, 1>, src: store<f32, 1>) {
                    let i = g.x();
                    dst[i] = src[i];
                }
            `, { file: 'tensor_mix.bk' });
            bro.kernel.dispatchTensors(modCopy.kernel('k_copy_t'), 8, zt, cpuOnly);
        } catch (e) {
            threwMix = e instanceof TypeError;
        }
        assert(threwMix, 'mixing CPU and GPU tensors in dispatchTensors throws TypeError');
    }
}
