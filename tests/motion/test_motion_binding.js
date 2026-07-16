// Weights-free binding tests for bro.motion (ARDY text-to-motion).
// Exercises the surface that needs no model files: namespace presence (or the
// honest available:false stub when built without BRO_WITH_DIFFUSION +
// BRO_WITH_LM), load() argument validation, and clean errors on nonexistent
// model paths. No downloads, no generate() — that needs the g152 checkpoint
// plus the 8B LLM2Vec encoder.

function expectThrows(fn, what) {
    let err = null;
    try { fn(); } catch (e) { err = e; }
    assert(err !== null, what + ' throws');
    return err;
}

assert(typeof bro === 'object', 'bro global exists');
assert(bro.motion !== undefined && bro.motion !== null, 'bro.motion namespace exists');

if (bro.motion.available === false) {
    // Compiled out: the stub must still exist and give a clear error on any
    // call rather than a bare ReferenceError / undefined.
    const err = expectThrows(() => bro.motion.load({}), 'stub load()');
    assert(String(err.message).includes('compiled without'),
           'stub error names the missing build flag: ' + err.message);
    console.log('bro.motion is the unavailable stub; stub contract OK');
} else {
    assert(typeof bro.motion.load === 'function', 'bro.motion.load is a function');
    assert(typeof bro.motion.init === 'function', 'bro.motion.init is a function');

    // init() is idempotent and returns undefined (brotensor runtime init).
    assert(bro.motion.init() === undefined, 'init() returns undefined');
    assert(bro.motion.init() === undefined, 'init() is idempotent');

    // ── load() argument validation — synchronous TypeErrors ─────────────────
    let err = expectThrows(() => bro.motion.load(), 'load() with no args');
    assert(err instanceof TypeError, 'no-args load throws TypeError, got: ' + err);

    err = expectThrows(() => bro.motion.load(42), 'load(42)');
    assert(err instanceof TypeError, 'non-object options throws TypeError, got: ' + err);

    err = expectThrows(() => bro.motion.load({}), 'load({})');
    assert(err instanceof TypeError, 'empty options throws TypeError, got: ' + err);
    assert(String(err.message).includes('checkpoint'),
           'error names the required keys: ' + err.message);

    err = expectThrows(() => bro.motion.load({ checkpoint: 'x' }),
                       'load() missing textEncoder');
    assert(err instanceof TypeError, 'missing textEncoder throws TypeError');

    err = expectThrows(() => bro.motion.load({ checkpoint: 123, textEncoder: 'y' }),
                       'load() non-string checkpoint');
    assert(err instanceof TypeError, 'non-string checkpoint throws TypeError');

    // ── load() on nonexistent model paths — clean runtime Error, no crash ───
    const bogus = { checkpoint:  'tests/motion/__no_such_dir__',
                    textEncoder: 'tests/motion/__no_such_dir__' };
    err = expectThrows(() => bro.motion.load(bogus), 'load(nonexistent paths)');
    assert(!(err instanceof TypeError),
           'missing model files is a runtime Error, not a TypeError');
    assert(String(err.message).includes('motion.load failed'),
           'load error carries the motion.load prefix: ' + err.message);

    // A failed load must leave the namespace usable: a second bad load errors
    // identically instead of crashing on leftover state.
    err = expectThrows(() => bro.motion.load(bogus), 'second load(nonexistent paths)');
    assert(String(err.message).includes('motion.load failed'),
           'second failed load errors identically');

    console.log('bro.motion binding contract OK (weights-free)');
}
