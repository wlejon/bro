// Headless test for bro.kernel's GPU half: kernels dispatched on the engine's
// Vulkan device over KernelBuffers agree with run() on the CPU (a stencil, a
// gather returning a value per cell, noise over a grid), dispatches run in
// order, stores stay bounded, f64 stays on the CPU, and the errors a script
// gets for bad calls.
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
        fn k_field(g: grid<2>, out: write_store<f32, 2>, origin: vec3<f32>, seed: i32) {
            let p = origin + vec3<f32>(g.x() as f32, g.y() as f32, 0.0) * 0.0375;
            out[g.x(), g.y()] = fbm_simplex<f32, 3>(p, 5, 2.0, 0.5, 0.0, seed);
        }

        // f64 far from the origin: a CPU kernel's, which has no GPU build.
        @kernel @cpu @grid(2)
        fn k_far(g: grid<2>, out: write_store<f32, 2>, origin: vec3<f64>, seed: i32) {
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

    // bro.kernel.background(fn): fn's dispatches run in the gap after the
    // frame, in order with each other, after the work recorded before; the
    // job is done once the GPU has done them, and its results read right
    // then.
    perf.kernelTimes();
    const bgOut = bro.kernel.createBuffer(W * H * 4), bgTwice = bro.kernel.createBuffer(W * H * 4);
    srcBuf.write(src, 0);
    const job = bro.kernel.background(() => {
        blur.dispatch([W, H], srcBuf, bgOut);
        blur.dispatch([W, H], bgOut, bgTwice);
    });
    // Held until the frame's work is all submitted: a graphics readback
    // waited on in the same frame does not let it start.
    outBuf.read(new Float32Array(W));
    assert(!job.done, 'a background job waits for the frame to end');
    let frames = 0;
    const jobT0 = Date.now();
    while (!job.done && Date.now() - jobT0 < 5000) { advanceTime(16); frames++; }
    assert(job.done, `a background job is done within frames (${frames})`);
    agree(bgTwice.read(new Float32Array(W * H)), cpuTwice, 1e-6, 'a background blur of the blur');
    const bgTimes = perf.kernelTimes();
    assert(bgTimes['k_blur (background)']?.dispatches === 2, `its dispatches are timed as background work (${Object.keys(bgTimes)})`);
    let inner = null;
    try { bro.kernel.background(() => { bro.kernel.background(() => {}); }); } catch (e) { inner = e; }
    assert(inner instanceof TypeError, 'a background job inside one is refused');
    let thrown = null;
    try { bro.kernel.background(() => { throw new Error('produce failed'); }); } catch (e) { thrown = e; }
    assert(thrown && thrown.message === 'produce failed', 'what fn throws is rethrown');
    const after = bro.kernel.background(() => {});
    const afterT0 = Date.now();
    while (!after.done && Date.now() - afterT0 < 5000) { advanceTime(16); frames++; }
    assert(after.done, 'and later jobs still run');

    // A job longer than the frames in flight does not hold them: each frame
    // goes on while the GPU still runs it (render::VulkanFrames retires what
    // its slot lent the job instead of waiting), and its result is whole.
    {
        const LW = 8192, LH = 2048;
        let a = bro.kernel.createBuffer(new Float32Array(LW * LH).fill(3)), b = bro.kernel.createBuffer(LW * LH * 4);
        const probe0 = Date.now();
        for (let i = 0; i < 4; i++) { blur.dispatch([LW, LH], a, b); [a, b] = [b, a]; }
        a.read(new Float32Array(1));
        const probeMs = Math.max(1, Date.now() - probe0);
        const PASSES = Math.max(4, Math.min(1500, Math.ceil((150 * 4) / probeMs)));
        const t0 = Date.now();
        const long = bro.kernel.background(() => {
            for (let i = 0; i < PASSES; i++) { blur.dispatch([LW, LH], a, b); [a, b] = [b, a]; }
        });
        while (!long.done && Date.now() - t0 < 30000) advanceTime(16);
        assert(long.done, 'a long background job is done');
        const mid = a.read(new Float32Array(1), (LH / 2 * LW + LW / 2) * 4)[0];
        assert(mid === 3, 'the long job ran every pass: ' + mid);
    }

    // A gather returning one value per cell; an index outside the table reads zero.
    const gather = mod.kernel('k_gather');
    const returns = gather.dispatch(5, bro.kernel.createBuffer(new Float32Array([10, 20, 30])),
                                    bro.kernel.createBuffer(new Int32Array([0, 2, -1, 5, 1])));
    assert(returns && returns.byteLength === 20, 'dispatch returns a KernelBuffer of the cells\' returns');
    const got = returns.read(new Float32Array(5));
    assert([...got].join(',') === '10,30,0,0,20', 'out-of-bounds gathers read zero on the GPU: ' + [...got].join(','));

    // Noise over a grid, with a vector argument. GPU code takes the
    // hardware's fast math (fused multiply-adds, the device's
    // transcendentals), so the builds agree to a few f32 ulps, not to the bit.
    const field = mod.kernel('k_field');
    const FW = 64, FH = 32, origin = [3.25, -1.5, 0.75];
    const fieldBuf = bro.kernel.createBuffer(FW * FH * 4);
    field.dispatch([FW, FH], fieldBuf, origin, 1337);
    const cpuField = new Float32Array(FW * FH);
    field.run([FW, FH], cpuField, origin, 1337);
    assert(cpuField.some((v) => v !== 0), 'the CPU field is not empty');
    // Fast math: the builds fuse multiply-adds at different sites, which the
    // octaves' doubling amplifies (5 octaves: ~3.5e-3 at most), and a point on
    // a simplex's edge may take its neighbour.
    agree(fieldBuf.read(new Float32Array(FW * FH)), cpuField, 5e-3, 'fbm field');

    // f64 is the CPU's: a @cpu kernel runs, and has no GPU build.
    const far = mod.kernel('k_far');
    const farField = new Float32Array(FW * FH);
    far.run([FW, FH], farField, [2293560.0, 3058080.0, 5096800.0], 1337);
    assert(farField.some((v) => v !== 0), 'a @cpu f64 field runs on the CPU');
    let farThrown = null;
    try { far.dispatch([FW, FH], fieldBuf, [0, 0, 0], 1337); } catch (e) { farThrown = e; }
    assert(farThrown && /@cpu/.test(farThrown.message), 'and its dispatch is refused: ' + (farThrown && farThrown.message));
    // A GPU kernel that uses f64 is a compile error naming the site.
    let f64Thrown = null;
    try {
        bro.kernel.compile('@kernel @grid(1)\nfn k_bad(g: grid<1>, out: write_store<f32, 1>, x: f32) {\n    let y = (x as f64) * 2.0;\n    out[g.x()] = y as f32;\n}', { file: 'bad.bk' });
    } catch (e) { f64Thrown = e; }
    assert(f64Thrown && /f64 in GPU kernel 'k_bad'/.test(f64Thrown.message) && f64Thrown.diagnostics?.[0]?.line === 3,
           'f64 in a GPU kernel fails to compile at its line: ' + (f64Thrown && f64Thrown.message));

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

    // KernelBuffer.destroy() releases GPU memory once in-flight work finishes;
    // any later use of the buffer throws.
    const deadBuf = bro.kernel.createBuffer(new Float32Array([1, 2, 3, 4]));
    blur.dispatch([2, 2], deadBuf, outBuf);
    deadBuf.destroy();
    deadBuf.destroy(); // idempotent
    throws(() => deadBuf.byteLength, (e) => /destroyed/.test(e.message), 'byteLength of a destroyed KernelBuffer');
    throws(() => deadBuf.read(new Float32Array(4)), (e) => /destroyed/.test(e.message), 'read of a destroyed KernelBuffer');
    throws(() => deadBuf.write(new Float32Array(4)), (e) => /destroyed/.test(e.message), 'write of a destroyed KernelBuffer');
    throws(() => blur.dispatch([2, 2], deadBuf, outBuf), (e) => /destroyed/.test(e.message),
           'dispatch with a destroyed KernelBuffer');

    // Dropping handles in a loop: forcing a collection frees their Vulkan
    // memory, and allocating past the pressure budget collects automatically
    // without advanceTime() so neither device-local nor upload pool memory
    // grows unboundedly.
    __host.gc();
    const baseGpu = __host.memory().gpu;
    (() => {
        for (let i = 0; i < 16; i++) {
            bro.kernel.createBuffer(1024 * 1024);
        }
    })();
    __host.gc();
    const sweptGpu = __host.memory().gpu;
    assert(sweptGpu.poolAllocations === baseGpu.poolAllocations,
           `GC frees unreferenced KernelBuffers (${baseGpu.poolAllocations} -> ${sweptGpu.poolAllocations} allocations)`);

    const scratch = new Float32Array(1024);
    for (let i = 0; i < 96; i++) {
        const transient = bro.kernel.createBuffer(2 * 1024 * 1024);
        transient.write(scratch);
    }
    const afterLoopGpu = __host.memory().gpu;
    assert(afterLoopGpu.poolDeviceOnlyBytes <= baseGpu.poolDeviceOnlyBytes + 80 * 1024 * 1024,
           `external memory pressure reclaims unreferenced KernelBuffers (${afterLoopGpu.poolDeviceOnlyBytes} bytes vs base ${baseGpu.poolDeviceOnlyBytes})`);

    // A long dispatch chain (> kMaxDispatchesPerSubmit) splits into bounded
    // submissions automatically before read().
    const ping = bro.kernel.createBuffer(src);
    const pong = bro.kernel.createBuffer(W * H * 4);
    let cpuA = new Float32Array(src), cpuB = new Float32Array(W * H);
    for (let i = 0; i < 100; i++) {
        blur.dispatch([W, H], ping, pong);
        blur.dispatch([W, H], pong, ping);
        blur.run([W, H], cpuA, cpuB);
        blur.run([W, H], cpuB, cpuA);
    }
    agree(ping.read(new Float32Array(W * H)), cpuA, 1e-4, '200-dispatch chain');
    ping.destroy();
    pong.destroy();
}
