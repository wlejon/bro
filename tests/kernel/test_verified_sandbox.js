// Headless test: kernel.verifySandbox() and bro.kernel.dispatchTensorsVerified().
assert(typeof bro.kernel === 'object', 'bro.kernel namespace exists');
if (!bro.kernel.available || !bro.tensor || bro.tensor.available === false) {
    skipTest('bro.kernel or bro.tensor is compiled out of this build');
} else {
    bro.tensor.init();

    const mod = bro.kernel.compile(`
        shared var tile: array<f32, 64>;

        @kernel @grid(1)
        fn k_smooth_scale(g: grid<1>, out: rw_store<f32, 1>, inp: store<f32, 1>, scale: f32) {
            let i = g.x();
            let lid = i % 64;
            tile[lid] = inp[i];
            workgroup_barrier();
            out[i] = tile[lid] * scale + 1.0;
        }
    `, { file: 'verified_sandbox.bk' });
    const kSmooth = mod.kernel('k_smooth_scale');
    assert(typeof kSmooth.verifySandbox === 'function', 'kernel.verifySandbox exists');
    const sb = kSmooth.verifySandbox();
    assert(sb.passed === true, 'verifySandbox passed on portable GPU kernel: ' + sb.diagnostic);
    assert(sb.portabilityOk === true, 'verifySandbox portabilityOk');
    assert(sb.spirvParsed === true, 'verifySandbox spirvParsed');
    assert(sb.memorySafe === true, 'verifySandbox memorySafe');
    assert(sb.barrierUniform === true, 'verifySandbox barrierUniform');
    assert(sb.barriersProven >= 1, 'verifySandbox barriersProven >= 1');
    assert(sb.proofTier === 3, 'verifySandbox proofTier === FullSmt');
    assert(sb.hardeningTier === 1, 'verifySandbox hardeningTier === PureSmtZeroOverhead');
    assert(sb.globalBoundsProven === true, 'verifySandbox globalBoundsProven');

    assert(typeof bro.kernel.dispatchTensorsVerified === 'function',
        'bro.kernel.dispatchTensorsVerified exists');
    const src = bro.tensor.createTensor(16);
    const dst = bro.tensor.createTensor(16);
    const seed = new Float32Array(16);
    for (let i = 0; i < 16; i++) seed[i] = i * 0.5;
    src.upload(seed);

    // First dispatch populates sandbox cache; second dispatch exercises cache hit.
    bro.kernel.dispatchTensorsVerified(kSmooth, 16, dst, src, 2.0);
    bro.kernel.dispatchTensorsVerified(kSmooth, 16, dst, src, 2.0);
    const got = dst.download();
    for (let i = 0; i < 16; i++) {
        const want = seed[i] * 2.0 + 1.0;
        assert(Math.abs(got[i] - want) < 1e-4, `verified[${i}]=${got[i]} want ${want}`);
    }

    // Out-of-bounds gridShape > store length must fail dispatch bounds check cleanly
    // on a grid-covered @grid(1) kernel without barriers.
    const gridMod = bro.kernel.compile(`
        @kernel @grid(1)
        fn k_grid_scale(g: grid<1>, out: rw_store<f32, 1>, inp: store<f32, 1>, scale: f32) {
            let i = g.x();
            out[i] = inp[i] * scale;
        }
    `, { file: 'verified_grid_bounds.bk' });
    const kGrid = gridMod.kernel('k_grid_scale');
    const gsb = kGrid.verifySandbox();
    assert(gsb.passed === true && gsb.globalBoundsProven === true, 'k_grid_scale sandbox passes');
    bro.kernel.dispatchTensorsVerified(kGrid, 16, dst, src, 2.0);
    let threwOob = false;
    try {
        bro.kernel.dispatchTensorsVerified(kGrid, 32, dst, src, 2.0);
    } catch (e) {
        threwOob = e instanceof Error && e.message.indexOf('dispatch bounds') >= 0;
    }
    assert(threwOob, 'dispatchTensorsVerified must throw when gridShape exceeds proved store bounds');

    // hardenSandbox option clamps unproven dynamic accesses and instruments unbounded loops.
    const dynMod = bro.kernel.compile(`
        @kernel @grid(1)
        fn k_dyn_clamp(g: grid<1>, out: rw_store<f32, 1>, inp: store<f32, 1>, n: i32) {
            let i = g.x();
            var acc: f32 = 0.0;
            for (var k: i32 = 0; k < n; k = k + 1) {
                acc = acc + inp[k];
            }
            out[i] = acc;
        }
    `, { file: 'verified_harden_sandbox.bk', hardenSandbox: true, sandboxMaxLoopIterations: 4096 });
    const kDyn = dynMod.kernel('k_dyn_clamp');
    const dsb = kDyn.verifySandbox();
    assert(dsb.passed === true, 'hardened dynamic kernel passes verifySandbox: ' + dsb.diagnostic);
    assert(dsb.hardeningTier === 2, 'hardened dynamic kernel has HybridSmtClamped tier');
    bro.kernel.dispatchTensorsVerified(kDyn, 16, dst, src, 4);

    // @cpu kernel must fail verifySandbox() and throw in dispatchTensorsVerified().
    const cpuMod = bro.kernel.compile(`
        @cpu @kernel @grid(1)
        fn k_cpu_only(g: grid<1>, x: rw_store<f32, 1>) {
            let i = g.x();
            x[i] = x[i] + 1.0;
        }
    `, { file: 'verified_sandbox_cpu.bk' });
    const kCpu = cpuMod.kernel('k_cpu_only');
    const cpuSb = kCpu.verifySandbox();
    assert(cpuSb.passed === false, '@cpu kernel must fail verifySandbox');
    let threwCpu = false;
    try {
        bro.kernel.dispatchTensorsVerified(kCpu, 16, dst);
    } catch (e) {
        threwCpu = e instanceof Error;
    }
    assert(threwCpu, 'dispatchTensorsVerified must throw on @cpu kernel');
}
