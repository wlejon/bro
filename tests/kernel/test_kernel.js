// Headless test for bro.kernel: compiling, a point kernel's eval, grid kernels
// run over typed arrays (a stencil writing a store, a gather returning a value
// per cell), bounded stores, and the errors a script gets for bad calls.
assert(typeof bro.kernel === 'object', 'bro.kernel namespace exists');
if (!bro.kernel.available) {
    skipTest('bro.kernel is compiled out of this build');
} else {
    const mod = bro.kernel.compile(`
        import std::noise::*;

        @kernel
        fn k_height(x: f64, y: f64, z: f64, seed: i32) -> f32 {
            return fbm_simplex<f64, 3>(vec3<f64>(x, y, z) * 0.001, 5, 2.0, 0.5, 0.0, seed) as f32;
        }

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
    `, { file: 'test.bk' });

    const names = mod.kernels;
    assert(names.includes('k_height') && names.includes('k_blur') && names.includes('k_gather'), 'every kernel is compiled');

    // A point kernel: the same position gives the same height, another seed another one.
    const height = mod.kernel('k_height');
    assert(height.gridDim === 0 && height.returns === 'f32', 'k_height is a point kernel returning f32');
    assert(height.params.length === 4 && height.params[0].kind === 'scalar' && height.params[0].type === 'f64',
           'k_height takes four scalars');
    const h1 = height.eval(1000.5, 2000.25, 3000.125, 7);
    assert(typeof h1 === 'number' && h1 >= -1 && h1 <= 1, 'eval returns a height in [-1, 1]: ' + h1);
    assert(height.eval(1000.5, 2000.25, 3000.125, 7) === h1, 'eval is deterministic');
    assert(height.eval(1000.5, 2000.25, 3000.125, 8) !== h1, 'another seed gives another height');

    // A stencil over a 2D grid, its edges clamped, writing a store in place.
    const blur = mod.kernel('k_blur');
    const W = 13, H = 2;
    const src = new Float32Array(W * H);
    for (let i = 0; i < src.length; i++) src[i] = i * 3;
    const out = new Float32Array(W * H);
    assert(blur.run([W, H], src, out) === undefined, 'a kernel without a return value returns nothing');
    for (let y = 0; y < H; y++) {
        for (let x = 0; x < W; x++) {
            const at = (xx) => src[y * W + Math.min(Math.max(xx, 0), W - 1)];
            const want = Math.fround(Math.fround(Math.fround(at(x - 1) + at(x)) + at(x + 1)) / 3);
            assert(out[y * W + x] === want, `blur (${x}, ${y}): ${out[y * W + x]}, want ${want}`);
        }
    }
    const outParam = blur.params.find((p) => p.name === 'out');
    assert(outParam.kind === 'store' && outParam.access === 'write' && outParam.cells === 2,
           'out is a write store accessed at the grid\'s cells');

    // A gather returning one value per cell; an index outside the table reads zero.
    const gather = mod.kernel('k_gather');
    const got = gather.run(5, new Float32Array([10, 20, 30]), new Int32Array([0, 2, -1, 5, 1]));
    assert(got instanceof Float32Array && got.length === 5, 'run returns a Float32Array of the cells');
    assert([...got].join(',') === '10,30,0,0,20', 'out-of-bounds gathers read zero: ' + [...got].join(','));

    // The errors a script gets.
    const throws = (fn, check, what) => {
        try { fn(); } catch (e) { assert(check(e), what + ': ' + e.message); return; }
        assert(false, what + ': did not throw');
    };
    throws(() => blur.run([W, H], new Float64Array(W * H), out), (e) => e instanceof TypeError && /Float32Array/.test(e.message),
           'a store of the wrong element type');
    throws(() => blur.run([W, H], src, new Float32Array(W * H - 1)), (e) => /'out'/.test(e.message),
           'a store smaller than the grid it is written at');
    throws(() => blur.run([W, H], src), (e) => e instanceof TypeError, 'a missing argument');
    throws(() => blur.eval(src, out), (e) => /run\(\)/.test(e.message), 'eval of a grid kernel');
    throws(() => bro.kernel.compile('fn broken( {', { file: 'broken.bk' }),
           (e) => Array.isArray(e.diagnostics) && e.diagnostics.length > 0 && e.diagnostics[0].file === 'broken.bk' &&
                  e.diagnostics[0].line === 1,
           'a compile error lists its diagnostics');

    // A module given as source, imported by name.
    const withModule = bro.kernel.compile(`
        from world::shape import doubled;
        @kernel
        fn k_twice(v: f32) -> f32 { return doubled(v); }
    `, { modules: { 'world::shape': 'module world::shape; pub fn doubled(v: f32) -> f32 { return v * 2.0; }' } });
    assert(withModule.kernel('k_twice').eval(21) === 42, 'an imported module resolves from the source given');
}
