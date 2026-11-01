// A grid kernel's handle (KernelModule.kernel) starts building its GPU
// pipeline when it is made; one never dispatched still has its pipeline and
// layout destroyed before the device at shutdown (Vulkan validation reports
// the leak otherwise, and run_tests.sh fails on it).
if (!bro.kernel.available) {
    skipTest('bro.kernel is compiled out of this build');
} else if (!bro.kernel.gpu) {
    missingGpuContext('kernel');
} else {
    const mod = bro.kernel.compile(`
        @kernel @grid(1)
        fn k_never(g: grid<1>, out: write_store<f32, 1>) {
            out[g.x()] = g.x() as f32;
        }

        @kernel @grid(1)
        fn k_once(g: grid<1>, out: write_store<f32, 1>) {
            out[g.x()] = 2.0 * (g.x() as f32);
        }
    `, { file: 'undispatched.bk' });
    const never = mod.kernel('k_never');
    const once = mod.kernel('k_once');
    assert(never && once, 'both handles made');
    const buf = bro.kernel.createBuffer(16 * 4);
    once.dispatch([16], buf);
    const out = buf.read(new Float32Array(16));
    assert(out[5] === 10, 'the dispatched kernel ran: ' + out[5]);
}
