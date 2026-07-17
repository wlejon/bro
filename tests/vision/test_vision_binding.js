// Weights-free binding tests for bro.vision (vision-model inference: SAM,
// depth, normals, BiRefNet, ControlNet annotators). Exercises the surface that
// needs no model files: namespace presence (or the honest available:false stub
// when built without BRO_WITH_VISIONML), loader argument validation, and clean
// errors on nonexistent model paths. No downloads, no inference with a real
// model.

function expectThrows(fn, what) {
    let err = null;
    try { fn(); } catch (e) { err = e; }
    assert(err !== null, what + ' throws');
    return err;
}

assert(typeof bro === 'object', 'bro global exists');
assert(bro.vision !== undefined && bro.vision !== null, 'bro.vision namespace exists');

if (bro.vision.available === false) {
    const err = expectThrows(() => bro.vision.loadSam('x'), 'stub loadSam()');
    assert(String(err.message).includes('compiled without'),
           'stub error names the missing build flag: ' + err.message);
    console.log('bro.vision is the unavailable stub; stub contract OK');
} else {
    const loaders = ['loadSam', 'loadDepth', 'loadNormal', 'loadBirefnet',
                     'loadHed', 'loadLineart', 'loadMlsd', 'loadOpenpose',
                     'loadSegformer'];
    assert(typeof bro.vision.init === 'function', 'bro.vision.init is a function');
    for (const f of loaders) {
        assert(typeof bro.vision[f] === 'function', 'bro.vision.' + f + ' is a function');
    }

    assert(bro.vision.init() === undefined, 'init() returns undefined');

    // ── every loader validates its path argument with a TypeError ───────────
    for (const f of loaders) {
        const err = expectThrows(() => bro.vision[f](), f + '() with no args');
        assert(err instanceof TypeError, 'no-args ' + f + ' throws TypeError, got: ' + err);
        assert(String(err.message).includes('path'),
               f + ' error names the path arg: ' + err.message);
    }

    // ── loaders on nonexistent model paths — clean runtime Error, no crash ──
    const bogus = 'tests/vision/__no_such_dir__';
    let err = expectThrows(() => bro.vision.loadSam(bogus), 'loadSam(nonexistent dir)');
    assert(!(err instanceof TypeError),
           'missing model dir is a runtime Error, not a TypeError');
    assert(String(err.message).includes('loadSam'),
           'load error carries the loadSam prefix: ' + err.message);

    err = expectThrows(() => bro.vision.loadDepth(bogus), 'loadDepth(nonexistent dir)');
    assert(!(err instanceof TypeError), 'missing depth dir is a runtime Error');

    // A failed load must leave the namespace usable.
    err = expectThrows(() => bro.vision.loadSam(bogus), 'second loadSam(nonexistent dir)');
    assert(String(err.message).includes('loadSam'), 'second failed load errors identically');

    console.log('bro.vision binding contract OK (weights-free)');
}
