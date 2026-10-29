// Headless test for bro.kernel's GPU half: kernels dispatched on the engine's
// Vulkan device over KernelBuffers agree with run() on the CPU (a stencil, a
// gather returning a value per cell, f64 noise over a grid), dispatches run
// in order, stores stay bounded, and the errors a script gets for bad calls.
assert(typeof bro.kernel === 'object', 'bro.kernel namespace exists');
if (!bro.kernel.available) {
    skipTest('bro.kernel is compiled out of this build');
} else if (!bro.kernel.gpu) {
    skipTest('no GPU device for bro.kernel');
} else {
    const mod = bro.kernel.compile(`
        import std::noise::*;

        @kernel @grid(2)
        fn k_blur(g: grid<2>, src: stencil<f32, 2>, out: write_store<f32, 2>) {
            let x = g.x();
            let y = g.y();
            out[x, y] = (src[-1, 0] + src[0, 0] + src[1, 0]) / 3.0;
        }

        @kernel @grid(1)
        fn k_gather(g: grid<1>, table: read_store<f32, 1>, idx: read_store<i32, 1>) -> f32 {
            return table[idx[g.x()]];
        }

        @kernel @grid(2)
        fn k_field(g: grid<2>, out: write_store<f32, 2>, origin: vec3<f64>, seed: i32) {
            let p = origin + vec3<f64>(g.x() as f64, g.y() as f64, 0.0) * 37.5;
            out[g.x(), g.y()] = fbm_simplex<f64, 3>(p * 0.001, 5, 2.0, 0.5, 0.0, seed) as f32;
        }
    `, { file: 'gpu.bk' });

    const near = (a, b, tol) => Math.abs(a - b) <= tol * Math.max(1, Math.abs(b));
    const agree = (gpu, cpu, tol, what) => {
        assert(gpu.length === cpu.length, what + ': lengths');
        let worst = 0, at = -1;
        for (let i = 0; i < cpu.length; i++) {
            const d = Math.abs(gpu[i] - cpu[i]) / Math.max(1, Math.abs(cpu[i]));
            if (d > worst) { worst = d; at = i; }
        }
        assert(worst <= tol, `${what}: GPU and CPU differ by ${worst} at ${at} (${gpu[at]} vs ${cpu[at]})`);
    };

    // A buffer from a typed array holds its bytes; one from a length reads zero.
    const W = 61, H = 7;
    const src = new Float32Array(W * H);
    for (let i = 0; i < src.length; i++) src[i] = Math.sin(i * 0.37) * 100;
    const srcBuf = bro.kernel.createBuffer(src);
    assert(srcBuf instanceof Object && srcBuf.byteLength === src.byteLength, 'createBuffer(typedArray) has its byteLength');
    assert([...srcBuf.read(new Float32Array(W * H))].every((v, i) => v === src[i]), 'a buffer reads back what it was made with');
    const outBuf = bro.kernel.createBuffer(W * H * 4);
    assert(outBuf.read(new Float32Array(W * H)).every((v) => v === 0), 'createBuffer(byteLength) reads zero');

    // A stencil, on the GPU and the CPU.
    const blur = mod.kernel('k_blur');
    assert(blur.dispatch([W, H], srcBuf, outBuf) === undefined, 'a kernel without a return value returns nothing');
    const cpuOut = new Float32Array(W * H);
    blur.run([W, H], src, cpuOut);
    agree(outBuf.read(new Float32Array(W * H)), cpuOut, 1e-6, 'blur');

    // Dispatches run in order: blur the result again, from the GPU's buffer.
    const twice = bro.kernel.createBuffer(W * H * 4);
    blur.dispatch([W, H], outBuf, twice);
    const cpuTwice = new Float32Array(W * H);
    blur.run([W, H], cpuOut, cpuTwice);
    agree(twice.read(new Float32Array(W * H)), cpuTwice, 1e-6, 'blur of the blur');

    // write() lands before a later dispatch reads it.
    srcBuf.write(new Float32Array(W).fill(3), 0);
    blur.dispatch([W, H], srcBuf, outBuf);
    const row = outBuf.read(new Float32Array(W));
    assert(row.every((v) => near(v, 3, 1e-6)), 'a write is seen by the next dispatch: ' + row[5]);

    // A gather returning one value per cell; an index outside the table reads zero.
    const gather = mod.kernel('k_gather');
    const returns = gather.dispatch(5, bro.kernel.createBuffer(new Float32Array([10, 20, 30])),
                                    bro.kernel.createBuffer(new Int32Array([0, 2, -1, 5, 1])));
    assert(returns && returns.byteLength === 20, 'dispatch returns a KernelBuffer of the cells\' returns');
    const got = returns.read(new Float32Array(5));
    assert([...got].join(',') === '10,30,0,0,20', 'out-of-bounds gathers read zero on the GPU: ' + [...got].join(','));

    // f64 noise over a grid far from the origin, with a vector argument.
    const field = mod.kernel('k_field');
    const FW = 64, FH = 32, origin = [2293560.0, 3058080.0, 5096800.0];
    const fieldBuf = bro.kernel.createBuffer(FW * FH * 4);
    field.dispatch([FW, FH], fieldBuf, origin, 1337);
    const cpuField = new Float32Array(FW * FH);
    field.run([FW, FH], cpuField, origin, 1337);
    assert(cpuField.some((v) => v !== 0), 'the CPU field is not empty');
    agree(fieldBuf.read(new Float32Array(FW * FH)), cpuField, 1e-5, 'f64 fbm field');

    // The errors a script gets.
    const throws = (fn, check, what) => {
        try { fn(); } catch (e) { assert(check(e), what + ': ' + e.message); return; }
        assert(false, what + ': did not throw');
    };
    throws(() => blur.dispatch([W, H], src, outBuf), (e) => e instanceof TypeError && /KernelBuffer/.test(e.message),
           'a typed array where a KernelBuffer goes');
    throws(() => blur.dispatch([W, H], srcBuf, bro.kernel.createBuffer(W * H * 4 - 4)), (e) => /'out'/.test(e.message),
           'a buffer smaller than the grid it is written at');
    throws(() => outBuf.read(new Float32Array(W * H + 1)), (e) => e instanceof RangeError, 'a read past the end');
    throws(() => outBuf.write(new Float32Array(2), W * H * 4 - 4), (e) => e instanceof RangeError, 'a write past the end');
    throws(() => bro.kernel.compile('@kernel fn k_p(v: f32) -> f32 { return v; }').kernel('k_p').dispatch(1, 1),
           (e) => e instanceof TypeError && /eval\(\)/.test(e.message), 'dispatch of a point kernel');
}
