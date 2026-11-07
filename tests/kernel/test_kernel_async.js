// bro.kernel.compileAsync: the same module compile() gives, compiled off the
// page thread and settled in a frame; several at once; the same Error, with
// diagnostics, on a bad source; a resolver answered on the page thread.
if (!bro.kernel.available) {
    skipTest('bro.kernel is compiled out of this build');
} else {
    const SRC = `
        @kernel @cpu
        fn k_sum(a: f64, b: f64) -> f32 { return (a + b) as f32; }

        @kernel @grid(1)
        fn k_double(g: grid<1>, src: read_store<f32, 1>) -> f32 { return src[g.x()] * 2.0; }
    `;
    const sync = bro.kernel.compile(SRC, { file: 'sync.bk' });

    const p = bro.kernel.compileAsync(SRC, { file: 'async.bk' });
    assert(p instanceof Promise, 'compileAsync returns a Promise');
    const mod = await p;
    assert(JSON.stringify(mod.kernels.slice().sort()) === JSON.stringify(sync.kernels.slice().sort()),
           'the same kernels as compile(): ' + mod.kernels);
    assert(mod.file === 'async.bk', 'the module is compiled as its file');
    assert(mod.kernel('k_sum').eval(2, 3) === sync.kernel('k_sum').eval(2, 3), 'a point kernel evaluates the same');
    const src = new Float32Array([1, 2, 3, 4]);
    const a = mod.kernel('k_double').run(4, src);
    const b = sync.kernel('k_double').run(4, src);
    assert(JSON.stringify(Array.from(a)) === JSON.stringify(Array.from(b)), 'a grid kernel runs the same: ' + Array.from(a));

    // Several at once, one bad: the good ones resolve, the bad one rejects with
    // the Error compile() throws.
    const BAD = '@kernel @cpu fn k_bad(a: f32) -> f32 { return a + nope; }';
    let syncErr = null;
    try {
        bro.kernel.compile(BAD, { file: 'bad.bk' });
    } catch (e) {
        syncErr = e;
    }
    assert(syncErr && Array.isArray(syncErr.diagnostics), 'compile() throws with diagnostics');
    const results = await Promise.allSettled([
        bro.kernel.compileAsync(SRC, { file: 'a1.bk' }),
        bro.kernel.compileAsync(BAD, { file: 'bad.bk' }),
        bro.kernel.compileAsync(SRC, { file: 'a2.bk', knobs: {} }),
    ]);
    assert(results[0].status === 'fulfilled' && results[2].status === 'fulfilled', 'the good compiles resolve');
    assert(results[1].status === 'rejected', 'the bad compile rejects');
    assert(results[1].reason.message === syncErr.message, 'with the message compile() throws: ' + results[1].reason.message);
    assert(Array.isArray(results[1].reason.diagnostics) && results[1].reason.diagnostics.length === syncErr.diagnostics.length,
           'and the same diagnostics');

    // Bad arguments reject rather than throw.
    let threw = false;
    let bad;
    try {
        bad = bro.kernel.compileAsync(42);
    } catch (e) {
        threw = true;
    }
    assert(!threw && bad instanceof Promise, 'a bad argument does not throw');
    let rejected = false;
    try {
        await bad;
    } catch (e) {
        rejected = e instanceof TypeError;
    }
    assert(rejected, 'a bad argument rejects with a TypeError');

    // A module from the app's resolver, asked on the page thread while the
    // compile runs on its own.
    const asked = [];
    bro.kernel.resolve((name) => {
        asked.push(name);
        if (name !== 'ka_scale') return undefined;
        return `
            module ka_scale;
            pub fn scale(x: f64) -> f64 { return x * 10.0; }
        `;
    });
    const viaResolver = await bro.kernel.compileAsync(`
        import ka_scale;
        @kernel @cpu
        fn k_scaled(x: f64) -> f32 { return ka_scale::scale(x) as f32; }
    `, { file: 'resolved.bk' });
    assert(asked.includes('ka_scale'), 'the resolver is asked for the module: ' + asked);
    assert(viaResolver.kernel('k_scaled').eval(1.5) === 15, 'the resolved module compiles in');
    bro.kernel.resolve(undefined);
}
