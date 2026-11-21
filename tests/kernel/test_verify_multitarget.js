// Headless test: formal SPIR-V verification (kernel.verify) and MSL/PTX/DIS target emission.
assert(typeof bro.kernel === 'object', 'bro.kernel namespace exists');
if (!bro.kernel.available) {
    skipTest('bro.kernel is compiled out of this build');
} else {
    const mod = bro.kernel.compile(`
        shared var tile: array<f32, 64>;

        @kernel @grid(1)
        fn k_verify(g: grid<1>, src: read_store<f32, 1>, dst: write_store<f32, 1>) {
            let lid = g.x() % 64;
            let gx = g.x();
            tile[lid] = src[gx] + 1.0;
            workgroup_barrier();
            dst[gx] = tile[lid];
        }
    `, {
        file: 'verify_targets.bk',
        emitMsl: true,
        emitPtx: true,
        emitSpirvDisassembly: true,
    });

    const k = mod.kernel('k_verify');
    const rep = k.verify();
    assert(rep && rep.spirvParsed === true, 'spirvParsed is true');
    assert(rep.memorySafe === true, 'memorySafe is true');
    assert(rep.barrierUniform === true, 'barrierUniform is true');
    assert(rep.sharedAccessesProven >= 2, 'shared accesses proven: ' + rep.sharedAccessesProven);
    assert(rep.barriersProven >= 1, 'barriers proven: ' + rep.barriersProven);
    assert(Array.isArray(rep.violations) && rep.violations.length === 0, 'no safety violations');

    assert(typeof k.msl === 'string' && k.msl.includes('kernel void'), 'MSL emitted');
    assert(typeof k.mslEntry === 'string' && k.mslEntry.length > 0, 'mslEntry present');
    assert(typeof k.ptx === 'string' && k.ptx.includes('.entry'), 'PTX emitted');
    assert(typeof k.spirvDisassembly === 'string' && k.spirvDisassembly.length > 0, 'SPIR-V disassembly emitted');
}
