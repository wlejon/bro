// Headless test: a GPU kernel that reads a store's element and then writes
// the same element keeps what it read. MoltenVK 1.4.2's SPIRV-Cross put the
// read at its first use, after the write, so on a Mac such a kernel read what
// it had just written (brass SpirvOptions::loads_through_variable). Small
// Worlds' tile ranges (a span read and zeroed for the next batch) all came
// out empty, and its ground refined a level short.
assert(typeof bro.kernel === 'object', 'bro.kernel namespace exists');
if (!bro.kernel.available) {
    skipTest('bro.kernel is compiled out of this build');
} else if (!bro.kernel.gpu) {
    skipTest('no GPU device for bro.kernel');
} else {
    const mod = bro.kernel.compile(`
        // Read, zero, then use what was read.
        @kernel @grid(1)
        fn k_take(g: grid<1>, span: rw_store<i32, 1>, out: write_store<f32, 1>) {
            let v = span[g.x()];
            span[g.x()] = 0;
            out[g.x()] = v as f32;
        }

        // Two reads of the element, a write between them.
        @kernel @grid(1)
        fn k_swap(g: grid<1>, a: rw_store<f32, 1>, out: write_store<f32, 1>) {
            let before = a[g.x()];
            a[g.x()] = before * 2.0 + 1.0;
            let after = a[g.x()];
            out[g.x()] = after - before;
        }
    `, { file: 'read_then_write.bk' });

    const span = bro.kernel.createBuffer(new Int32Array([7, 8, 9, 10]));
    const took = bro.kernel.createBuffer(16);
    mod.kernel('k_take').dispatch([4], span, took);
    const t = new Float32Array(4); took.read(t);
    const s = new Int32Array(4); span.read(s);
    assert(Array.from(t).join(',') === '7,8,9,10', 'the value read before the write is kept');
    assert(Array.from(s).join(',') === '0,0,0,0', 'the write lands');

    const a = bro.kernel.createBuffer(new Float32Array([1, 2, 3, 4]));
    const diff = bro.kernel.createBuffer(16);
    mod.kernel('k_swap').dispatch([4], a, diff);
    const d = new Float32Array(4); diff.read(d);
    assert(Array.from(d).join(',') === '2,3,4,5', 'a read after the write sees it, one before does not');
}
