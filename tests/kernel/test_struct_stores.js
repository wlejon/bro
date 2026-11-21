// Headless test: structured store sub-element access, mod.structView,
// dual-residency KernelBuffer ('host'|'shared'|'gpu'), fast eval & exportNative.
assert(typeof bro.kernel === 'object', 'bro.kernel namespace exists');
if (!bro.kernel.available) {
    skipTest('bro.kernel is compiled out of this build');
} else {
    const mod = bro.kernel.compile(`
        struct Particle {
            pos: vec3<f32>,
            vel: vec3<f32>,
        }

        @kernel @grid(1)
        fn k_step(g: grid<1>, p: rw_store<Particle, 1>, dt: f32) {
            let i = g.x();
            p[i].pos += p[i].vel * dt;
        }

        @kernel
        fn k_mix_fast(a: f32, b: f32, t: f32) -> f32 {
            return a + (b - a) * t;
        }
    `, { file: 'struct_stores.bk' });

    const N = 4;
    const layout = mod.structLayout('Particle');
    assert(layout && layout.byteSize === 24, 'Particle byteSize === 24');

    const residency = bro.kernel.gpu ? 'shared' : 'host';
    const kbuf = bro.kernel.createBuffer(N * layout.byteSize, { residency });
    assert(kbuf.residency === residency, 'buffer residency matches requested mode');

    const view = mod.structView('Particle', kbuf);
    assert(view.count === N && view.byteStride === 24, 'structView count/stride');
    for (let i = 0; i < N; i++) {
        view.setStruct(i, { pos: [i, 0, 1], vel: [2, 3, -1] });
    }

    const kStep = mod.kernel('k_step');
    kStep.run(N, kbuf, 0.5);
    const p0 = view.getStruct(0);
    assert(Math.abs(p0.pos[0] - 1.0) < 1e-5 && Math.abs(p0.pos[1] - 1.5) < 1e-5 && Math.abs(p0.pos[2] - 0.5) < 1e-5,
           'CPU structured store update via shared/host KernelBuffer');

    if (bro.kernel.gpu) {
        kStep.dispatch(N, kbuf, 0.5);
        const p0gpu = view.getStruct(0);
        assert(Math.abs(p0gpu.pos[0] - 2.0) < 1e-4 && Math.abs(p0gpu.pos[1] - 3.0) < 1e-4,
               'GPU dispatch on shared KernelBuffer synced seamlessly back to structView');
    }

    // Fast scalar eval + exportNative
    const kFast = mod.kernel('k_mix_fast');
    for (let i = 0; i < 10; i++) {
        assert(Math.abs(kFast.eval(1.0, 5.0, 0.25) - 2.0) < 1e-6, 'fast eval');
    }
    kFast.exportNative('bro.__testMixNative');
    const dynRes = eval('bro.__testMixNative(10.0, 20.0, 0.3)');
    assert(Math.abs(dynRes - 13.0) < 1e-5, 'exportNative result accurate via eval: ' + dynRes);
    let dupThrew = false;
    try {
        kFast.exportNative('bro.__testMixNative');
    } catch (e) {
        dupThrew = true;
    }
    assert(dupThrew, 'duplicate exportNative registration rejected');
}
