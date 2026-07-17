// Weights-free binding tests for bro.triposplat (single image → 3D Gaussian
// Splat). Exercises the surface that needs no model files: namespace presence
// (or the honest available:false stub when built without BRO_WITH_DIFFUSION +
// BRO_WITH_VISIONML), load() argument validation, and clean errors on
// nonexistent model paths. No downloads, no generate().

function expectThrows(fn, what) {
    let err = null;
    try { fn(); } catch (e) { err = e; }
    assert(err !== null, what + ' throws');
    return err;
}

assert(typeof bro === 'object', 'bro global exists');
assert(bro.triposplat !== undefined && bro.triposplat !== null,
       'bro.triposplat namespace exists');

if (bro.triposplat.available === false) {
    const err = expectThrows(() => bro.triposplat.load({}), 'stub load()');
    assert(String(err.message).includes('compiled without'),
           'stub error names the missing build flag: ' + err.message);
    console.log('bro.triposplat is the unavailable stub; stub contract OK');
} else {
    for (const f of ['init', 'load', 'cancel']) {
        assert(typeof bro.triposplat[f] === 'function',
               'bro.triposplat.' + f + ' is a function');
    }

    assert(bro.triposplat.init() === undefined, 'init() returns undefined');

    // ── load() argument validation — synchronous TypeErrors ─────────────────
    let err = expectThrows(() => bro.triposplat.load(), 'load() with no args');
    assert(err instanceof TypeError, 'no-args load throws TypeError, got: ' + err);

    err = expectThrows(() => bro.triposplat.load(42), 'load(42)');
    assert(err instanceof TypeError, 'non-object options throws TypeError, got: ' + err);

    err = expectThrows(() => bro.triposplat.load({}), 'load({})');
    assert(err instanceof TypeError, 'empty options throws TypeError, got: ' + err);
    assert(String(err.message).includes('dinov3'),
           'error names the required keys: ' + err.message);

    err = expectThrows(() => bro.triposplat.load({ dinov3: 'x' }),
                       'load() missing vae/flow/decoder');
    assert(err instanceof TypeError, 'partial options throws TypeError, got: ' + err);

    // ── load() on nonexistent model paths — clean runtime Error, no crash ───
    const no = 'tests/triposplat/__no_such_dir__';
    const bogus = { dinov3: no, vae: no, flow: no, decoder: no };
    err = expectThrows(() => bro.triposplat.load(bogus), 'load(nonexistent paths)');
    assert(!(err instanceof TypeError),
           'missing model files is a runtime Error, not a TypeError');
    assert(String(err.message).includes('triposplat.load failed'),
           'load error carries the triposplat.load prefix: ' + err.message);

    // A failed load must leave the namespace usable.
    err = expectThrows(() => bro.triposplat.load(bogus), 'second load(nonexistent paths)');
    assert(String(err.message).includes('triposplat.load failed'),
           'second failed load errors identically');

    console.log('bro.triposplat binding contract OK (weights-free)');
}
