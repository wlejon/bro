// Weights-free binding tests for bro.diffusion (text-to-image inference).
// Exercises the surface that needs no model files: namespace presence (or the
// honest available:false stub when built without BRO_WITH_DIFFUSION),
// loadModel/createPipeline/expandNoise argument validation, and clean errors
// on nonexistent model paths. No downloads, no generation.

function expectThrows(fn, what) {
    let err = null;
    try { fn(); } catch (e) { err = e; }
    assert(err !== null, what + ' throws');
    return err;
}

assert(typeof bro === 'object', 'bro global exists');
assert(bro.diffusion !== undefined && bro.diffusion !== null,
       'bro.diffusion namespace exists');

if (bro.diffusion.available === false) {
    const err = expectThrows(() => bro.diffusion.loadModel('x'), 'stub loadModel()');
    assert(String(err.message).includes('compiled without'),
           'stub error names the missing build flag: ' + err.message);
    console.log('bro.diffusion is the unavailable stub; stub contract OK');
} else {
    for (const f of ['init', 'loadModel', 'createPipeline', 'expandNoise']) {
        assert(typeof bro.diffusion[f] === 'function',
               'bro.diffusion.' + f + ' is a function');
    }
    assert(typeof bro.diffusion.version === 'string' && bro.diffusion.version.length > 0,
           'bro.diffusion.version is a non-empty string');

    assert(bro.diffusion.init() === undefined, 'init() returns undefined');

    // ── argument validation — synchronous TypeErrors ─────────────────────────
    let err = expectThrows(() => bro.diffusion.loadModel(), 'loadModel() with no args');
    assert(err instanceof TypeError, 'no-args loadModel throws TypeError, got: ' + err);
    assert(String(err.message).includes('path'), 'error names the path arg: ' + err.message);

    err = expectThrows(() => bro.diffusion.loadModel(42), 'loadModel(42)');
    assert(err instanceof TypeError, 'non-string path throws TypeError, got: ' + err);

    err = expectThrows(() => bro.diffusion.createPipeline(), 'createPipeline() with no args');
    assert(err instanceof TypeError, 'no-args createPipeline throws TypeError, got: ' + err);

    err = expectThrows(() => bro.diffusion.createPipeline({}), 'createPipeline({})');
    assert(err instanceof TypeError, 'empty-opts createPipeline throws TypeError, got: ' + err);
    assert(String(err.message).includes('vocabPath'),
           'error names the required opts key: ' + err.message);

    err = expectThrows(() => bro.diffusion.expandNoise(), 'expandNoise() with no args');
    assert(err instanceof TypeError, 'no-args expandNoise throws TypeError, got: ' + err);

    // ── loadModel() on a nonexistent model dir — clean runtime Error ────────
    const bogus = '__no_such_dir__/model';
    err = expectThrows(() => bro.diffusion.loadModel(bogus), 'loadModel(nonexistent dir)');
    assert(!(err instanceof TypeError),
           'missing model dir is a runtime Error, not a TypeError');
    assert(String(err.message).includes('loadModel'),
           'load error carries the loadModel prefix: ' + err.message);

    // A failed load must leave the namespace usable.
    err = expectThrows(() => bro.diffusion.loadModel(bogus), 'second loadModel(nonexistent dir)');
    assert(String(err.message).includes('loadModel'), 'second failed load errors identically');

    console.log('bro.diffusion binding contract OK (weights-free)');
}
