// Headless test: runtime telemetry(slot, val) + setTelemetry + domain stdlibs (std::dsp, std::image, std::sim).
assert(typeof bro.kernel === 'object', 'bro.kernel namespace exists');
if (!bro.kernel.available) {
    skipTest('bro.kernel is compiled out of this build');
} else {
    const mod = bro.kernel.compile(`
        import std::dsp;
        import std::image;
        import std::sim;

        @kernel @grid(1)
        fn k_domain_telem(g: grid<1>, out: write_store<f32, 1>) {
            let i = g.x();
            let a = dsp::soft_clip(f32(i + 1) * 0.5, 2.0);
            let l = image::luma709(image::aces_tonemap(vec3<f32>(a, a, a)));
            let d = sim::sdf_circle(vec2<f32>(f32(i), 0.0), vec2<f32>(0.0, 0.0), 2.0);
            let combined = l + max(0.0, d);
            telemetry(i, combined);
            telemetry(-1, 1234.0);
            telemetry(99, 5678.0);
            out[i] = combined;
        }
    `, { file: 'telemetry_stdlib.bk', telemetrySlots: 16 });

    const k = mod.kernel('k_domain_telem');
    assert(k.telemetrySlots === 16, 'telemetrySlots preserved on kernel');

    const telem = new Float32Array(16);
    telem.fill(-1);
    k.setTelemetry(telem);

    const out = new Float32Array(8);
    k.run(8, out);
    for (let i = 0; i < 8; i++) {
        assert(Math.abs(telem[i] - out[i]) < 1e-5, `slot ${i} matches output`);
    }
    for (let i = 8; i < 16; i++) {
        assert(telem[i] === -1, `slot ${i} left untouched`);
    }

    if (bro.kernel.gpu) {
        const telemBuf = bro.kernel.createBuffer(16 * 4, { residency: 'shared' });
        telemBuf.write(new Float32Array(16).fill(-2));
        const outBuf = bro.kernel.createBuffer(8 * 4);
        k.setTelemetry(telemBuf);
        k.dispatch(8, outBuf);
        const gpuOut = outBuf.read(new Float32Array(8));
        const gpuTelem = telemBuf.read(new Float32Array(16));
        for (let i = 0; i < 8; i++) {
            assert(Math.abs(gpuTelem[i] - gpuOut[i]) < 1e-4, `GPU telemetry slot ${i} matches`);
        }
    }
}
