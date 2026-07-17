// Weights-free binding tests for bro.lm (text-generation: Qwen3 / Mistral /
// Qwen3.5). Exercises the surface that needs no model files: namespace
// presence (or the honest available:false stub when built without
// BRO_WITH_LM), loader argument validation, and clean errors on nonexistent
// model paths. No downloads, no generate() with a real model.

function expectThrows(fn, what) {
    let err = null;
    try { fn(); } catch (e) { err = e; }
    assert(err !== null, what + ' throws');
    return err;
}

assert(typeof bro === 'object', 'bro global exists');
assert(bro.lm !== undefined && bro.lm !== null, 'bro.lm namespace exists');

if (bro.lm.available === false) {
    const err = expectThrows(() => bro.lm.loadQwen('x'), 'stub loadQwen()');
    assert(String(err.message).includes('compiled without'),
           'stub error names the missing build flag: ' + err.message);
    console.log('bro.lm is the unavailable stub; stub contract OK');
} else {
    for (const f of ['init', 'loadQwen', 'loadMistral', 'loadQwen35',
                     'loadTokenizer', 'generate']) {
        assert(typeof bro.lm[f] === 'function', 'bro.lm.' + f + ' is a function');
    }

    assert(bro.lm.init() === undefined, 'init() returns undefined');
    assert(bro.lm.init() === undefined, 'init() is idempotent');

    // ── loader argument validation — synchronous TypeErrors ─────────────────
    let err = expectThrows(() => bro.lm.loadQwen(), 'loadQwen() with no args');
    assert(err instanceof TypeError, 'no-args loadQwen throws TypeError, got: ' + err);
    assert(String(err.message).includes('path'), 'error names the path arg: ' + err.message);

    err = expectThrows(() => bro.lm.loadQwen(42), 'loadQwen(42)');
    assert(err instanceof TypeError, 'non-string path throws TypeError, got: ' + err);

    err = expectThrows(() => bro.lm.loadMistral(), 'loadMistral() with no args');
    assert(err instanceof TypeError, 'no-args loadMistral throws TypeError, got: ' + err);

    err = expectThrows(() => bro.lm.loadMistral('x.gguf'), 'loadMistral() missing opts');
    assert(err instanceof TypeError, 'missing tokenizerPath opts throws TypeError, got: ' + err);
    assert(String(err.message).includes('tokenizerPath'),
           'error names the required opts key: ' + err.message);

    err = expectThrows(() => bro.lm.loadQwen35(), 'loadQwen35() with no args');
    assert(err instanceof TypeError, 'no-args loadQwen35 throws TypeError, got: ' + err);

    err = expectThrows(() => bro.lm.loadTokenizer(), 'loadTokenizer() with no args');
    assert(err instanceof TypeError, 'no-args loadTokenizer throws TypeError, got: ' + err);

    err = expectThrows(() => bro.lm.generate(), 'generate() with no args');
    assert(err instanceof TypeError, 'no-args generate throws TypeError, got: ' + err);

    err = expectThrows(() => bro.lm.generate({}), 'generate({}) without a model');
    assert(err instanceof TypeError, 'model-less generate throws TypeError, got: ' + err);

    // ── loaders on nonexistent model paths — clean runtime Error, no crash ──
    const bogus = 'tests/lm/__no_such_dir__/model.gguf';
    err = expectThrows(() => bro.lm.loadQwen(bogus), 'loadQwen(nonexistent path)');
    assert(!(err instanceof TypeError),
           'missing model file is a runtime Error, not a TypeError');
    assert(String(err.message).includes('loadQwen'),
           'load error carries the loadQwen prefix: ' + err.message);

    err = expectThrows(() => bro.lm.loadQwen35('tests/lm/__no_such_dir__'),
                       'loadQwen35(nonexistent dir)');
    assert(!(err instanceof TypeError),
           'missing checkpoint dir is a runtime Error, not a TypeError');

    // A failed load must leave the namespace usable.
    err = expectThrows(() => bro.lm.loadQwen(bogus), 'second loadQwen(nonexistent path)');
    assert(String(err.message).includes('loadQwen'), 'second failed load errors identically');

    console.log('bro.lm binding contract OK (weights-free)');
}
