// Headless test for bro.kernel.reload: a module's new source swapped into the
// modules that read it, their kernels following in place, the kernels whose
// code changed named, a kernel's own knobs kept, and a broken source changing
// nothing.
if (!bro.kernel.available) {
    skipTest('bro.kernel is compiled out of this build');
} else {
    const LIB = (a) => `
        module lib;
        pub fn a(x: f32) -> f32 { return ${a}; }
        pub fn b(x: f32) -> f32 { return x * 2.0; }
    `;
    const ROOT = (b) => `
        import lib;
        @knob const S: f32 = 1.0;
        @kernel fn k_a(x: f32) -> f32 { return lib::a(x); }
        @kernel fn k_b(x: f32) -> f32 { return ${b}; }
        @kernel @grid(1)
        fn k_grid(g: grid<1>, out: write_store<f32, 1>) { out[g.x()] = lib::a(g.x() as f32); }
    `;
    const mod = bro.kernel.compile(ROOT('lib::b(x) * S'), { file: 'reload.bk', modules: { lib: LIB('x + 1.0') } });
    const other = bro.kernel.compile(`@kernel fn k_c(x: f32) -> f32 { return x; }`, { file: 'other.bk' });
    assert(mod.file === 'reload.bk', `mod.file is its file (${mod.file})`);
    assert(Array.isArray(mod.imports) && mod.imports.includes('lib'), `mod.imports names lib (${mod.imports})`);
    assert(other.imports.length === 0, `a module importing nothing has no imports (${other.imports})`);

    const ka = mod.kernel('k_a'), kb = mod.kernel('k_b'), kg = mod.kernel('k_grid');
    assert(ka.eval(1) === 2 && kb.eval(1) === 2, `before: k_a(1) ${ka.eval(1)}, k_b(1) ${kb.eval(1)}`);
    kb.knobs({ S: 4 });
    assert(kb.eval(1) === 8, `k_b's own knob: ${kb.eval(1)}`);

    // lib's a changes: k_a and k_grid read it, k_b does not.
    let r = bro.kernel.reload({ lib: LIB('x + 10.0') });
    assert(r.length === 1 && r[0].file === 'reload.bk', `only the module importing lib recompiled: ${JSON.stringify(r)}`);
    const changed = [...r[0].kernels].sort().join(',');
    assert(changed === 'k_a,k_grid', `the kernels whose code changed: ${changed}`);
    assert(ka.eval(1) === 11, `k_a follows in place: ${ka.eval(1)}`);
    assert(kb.eval(1) === 8, `k_b keeps its own knob across the reload: ${kb.eval(1)}`);
    const out = new Float32Array(4);
    kg.run(4, out);
    assert(out[3] === 13, `k_grid follows in place: ${out[3]}`);
    assert(mod.kernel('k_a').eval(1) === 11, 'a kernel taken after the reload is the new one');

    // A broken source throws and changes nothing.
    let threw = false;
    try { bro.kernel.reload({ lib: LIB('x +') }); } catch (e) { threw = true; }
    assert(threw, 'a source that does not compile throws');
    assert(ka.eval(1) === 11, `after the failed reload k_a is as it was: ${ka.eval(1)}`);

    // The root by its file name.
    r = bro.kernel.reload({ 'reload.bk': ROOT('(lib::b(x) + 1.0) * S') });
    assert(r.length === 1 && r[0].kernels.join(',') === 'k_b', `the root's change names k_b: ${JSON.stringify(r)}`);
    assert(kb.eval(1) === 12, `k_b is the new source with its own knob: ${kb.eval(1)}`);
    assert(ka.eval(1) === 11, 'k_a keeps the reloaded lib');

    // Nothing reads a name: nothing recompiles.
    r = bro.kernel.reload({ nothing: 'module nothing;' });
    assert(r.length === 0, `a name nothing reads recompiles nothing: ${JSON.stringify(r)}`);
    console.log('kernel reload tests passed');
}
