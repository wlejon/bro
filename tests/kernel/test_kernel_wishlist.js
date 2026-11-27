// Tests new brokernel language & diagnostic JS APIs: registerConstants, enums,
// newtype, sizeof/offsetof, check, trapNonFinite, probeIf, and compareCpuGpu.
assert(typeof bro.kernel === 'object', 'bro.kernel namespace exists');
if (!bro.kernel.available) {
    skipTest('bro.kernel is compiled out of this build');
} else {
    const regName = bro.kernel.registerConstants('wl::cfg', {
        GRID_SCALE: ['i32', 16],
        GRAVITY: 9.81,
        Biome: { __enum: 'i32', Plains: 0, Forest: 1, Highland: 5 },
    });
    assert(regName === 'wl::cfg', 'registerConstants returns module name');

    const mod = bro.kernel.compile(`
        import wl::cfg::{Biome, GRAVITY};

        pub enum LocalMode : u32 {
            Off = 0u,
            Scan = 4u,
            Lock,
        }

        pub newtype Meters = f32;
        pub newtype Radians = f32;

        struct Sample {
            pos: vec3<f32>,
            flag: i32,
            weight: f32,
        }

        pub const SAMPLE_BYTES: i32 = sizeof(Sample);
        pub const SAMPLE_FLAG_OFF: i32 = offsetof(Sample, flag);
        pub const SAMPLE_WEIGHT_430: i32 = std430_offsetof(Sample, weight);

        @kernel @grid(2)
        fn k_eval_diag(
            g: grid<2>,
            inp: read_store<f32, 2>,
            out: write_store<f32, 2>,
            b: i32,
            mode: u32,
            bad_mode: i32
        ) {
            let x = g.x();
            let y = g.y();
            let v = inp[x, y];
            let m = Meters(v);
            let doubled = (m + Meters(1.0)).0;
            let picked = probe_if("high_cell", x >= 3 && y == 1, vec2<f32>(x as f32, doubled));
            let ok = check("non_neg_input", v >= 0.0 || bad_mode != 0);
            if (bad_mode == 2 && x == 2 && y == 1) {
                out[x, y] = v / v;
            } else if (ok && b == Biome::Highland as i32 && mode == LocalMode::Lock as u32) {
                out[x, y] = picked.y + GRAVITY * 0.0;
            } else {
                out[x, y] = 0.0;
            }
        }
    `, { file: 'wl_diag.bk' });

    assert(mod.consts.SAMPLE_BYTES === 20, 'sizeof(Sample) === 20, got ' + mod.consts.SAMPLE_BYTES);
    assert(mod.consts.SAMPLE_FLAG_OFF === 12, 'offsetof(Sample, flag) === 12');
    assert(mod.consts.SAMPLE_WEIGHT_430 === 20 || mod.consts.SAMPLE_WEIGHT_430 >= 16, 'std430_offsetof computed');
    assert(mod.enums && mod.enums.LocalMode && mod.enums.LocalMode.Lock === 5,
           'mod.enums reflects LocalMode.Lock=5: ' + JSON.stringify(mod.enums));

    // Nominal newtype safety must reject mixing Meters and Radians
    let threwTypeMismatch = false;
    try {
        bro.kernel.compile(`
            newtype Meters = f32;
            newtype Radians = f32;
            @kernel fn bad(a: Meters, b: Radians) -> f32 { return (a + b).0; }
        `);
    } catch (e) {
        threwTypeMismatch = /Meters/.test(e.message) && /Radians/.test(e.message);
    }
    assert(threwTypeMismatch, 'Meters + Radians fails compile with nominal type error');

    const k = mod.kernel('k_eval_diag');
    const W = 5, H = 3;
    const inp = new Float32Array(W * H);
    for (let i = 0; i < inp.length; i++) inp[i] = i + 1;
    const out = new Float32Array(W * H);

    // 1. CPU & GPU parity
    if (bro.kernel.gpu) {
        const rep = k.compareCpuGpu([W, H], inp, out, 5, 5, 0, { absTol: 1e-4, relTol: 1e-4, ulpTol: 32 });
        assert(rep.ok && rep.ranGpu && rep.elementsCompared >= W * H,
               'compareCpuGpu agrees: ' + JSON.stringify(rep));
    }

    // 2. probeIf filters only x >= 3 && y == 1
    const hits = k.probeIf([W, H], inp, out, 5, 5, 0, { gpu: false, maxSamples: 8 });
    assert(hits.length === 2, 'probeIf captured 2 cells, got ' + hits.length);
    assert(hits[0].label === 'high_cell' && hits[0].coord[0] === 3 && hits[0].coord[1] === 1,
           'first hit at (3,1): ' + JSON.stringify(hits[0]));
    assert(hits[1].coord[0] === 4 && hits[1].coord[1] === 1 && hits[1].value[1] === 11,
           'second hit at (4,1): ' + JSON.stringify(hits[1]));

    // 3. check() pass, check failure, and trapNonFinite NaN detection
    const passRep = k.check([W, H], inp, out, 5, 5, 0, { gpu: false, trapNonFinite: true });
    assert(passRep.ok, 'clean run passes check()');

    const badInp = new Float32Array(inp);
    badInp[1 * W + 1] = -2.0;
    const chkFail = k.check([W, H], badInp, out, 5, 5, 0, { gpu: false, trapNonFinite: true });
    assert(!chkFail.ok && chkFail.kind === 'check' && chkFail.label === 'non_neg_input'
           && chkFail.coord[0] === 1 && chkFail.coord[1] === 1,
           'check() caught assertion failure at (1,1): ' + JSON.stringify(chkFail));

    const zeroInp = new Float32Array(inp);
    zeroInp[1 * W + 2] = 0.0;
    const nanFail = k.check([W, H], zeroInp, out, 5, 5, 2, { gpu: false, trapNonFinite: true });
    assert(!nanFail.ok && nanFail.kind === 'non_finite' && nanFail.coord[0] === 2 && nanFail.coord[1] === 1,
           'trapNonFinite caught NaN at (2,1): ' + JSON.stringify(nanFail));

    if (bro.kernel.gpu) {
        const gpuHits = k.probeIf([W, H], inp, out, 5, 5, 0, { gpu: true, maxSamples: 8 });
        assert(gpuHits.length === 2, 'GPU probeIf captured 2 cells, got ' + gpuHits.length);
        const gpuChk = k.check([W, H], badInp, out, 5, 5, 0, { gpu: true, trapNonFinite: true });
        assert(!gpuChk.ok && gpuChk.kind === 'check' && gpuChk.coord[0] === 1 && gpuChk.coord[1] === 1,
               'GPU check() caught assertion failure at (1,1): ' + JSON.stringify(gpuChk));
        const gpuNan = k.check([W, H], zeroInp, out, 5, 5, 2, { gpu: true, trapNonFinite: true });
        assert(!gpuNan.ok && gpuNan.kind === 'non_finite' && gpuNan.coord[0] === 2 && gpuNan.coord[1] === 1,
               'GPU trapNonFinite caught NaN at (2,1): ' + JSON.stringify(gpuNan));
    }

    bro.kernel.clearModules();
}
