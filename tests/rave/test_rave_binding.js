// Weights-free binding tests for bro.rave (RAVE neural audio autoencoder).
// Exercises the surface that needs no model files: namespace presence (or the
// honest available:false stub when built without BRO_WITH_SOUNDML), loadRave
// argument validation, and clean errors on nonexistent model paths. No
// downloads, no encode/decode with a real model.

function expectThrows(fn, what) {
    let err = null;
    try { fn(); } catch (e) { err = e; }
    assert(err !== null, what + ' throws');
    return err;
}

assert(typeof bro === 'object', 'bro global exists');
assert(bro.rave !== undefined && bro.rave !== null, 'bro.rave namespace exists');

if (bro.rave.available === false) {
    const err = expectThrows(() => bro.rave.loadRave('x'), 'stub loadRave()');
    assert(String(err.message).includes('compiled without'),
           'stub error names the missing build flag: ' + err.message);
    console.log('bro.rave is the unavailable stub; stub contract OK');
} else {
    assert(typeof bro.rave.init === 'function', 'bro.rave.init is a function');
    assert(typeof bro.rave.loadRave === 'function', 'bro.rave.loadRave is a function');

    assert(bro.rave.init() === undefined, 'init() returns undefined');
    assert(bro.rave.init() === undefined, 'init() is idempotent');

    // ── loadRave() argument validation — synchronous TypeErrors ─────────────
    let err = expectThrows(() => bro.rave.loadRave(), 'loadRave() with no args');
    assert(err instanceof TypeError, 'no-args loadRave throws TypeError, got: ' + err);
    assert(String(err.message).includes('path'), 'error names the path arg: ' + err.message);

    err = expectThrows(() => bro.rave.loadRave(42), 'loadRave(42)');
    assert(err instanceof TypeError, 'non-string path throws TypeError, got: ' + err);

    // ── loadRave() on a nonexistent model path — clean runtime Error ────────
    const bogus = 'tests/rave/__no_such_dir__';
    err = expectThrows(() => bro.rave.loadRave(bogus), 'loadRave(nonexistent dir)');
    assert(!(err instanceof TypeError),
           'missing model dir is a runtime Error, not a TypeError');
    assert(String(err.message).includes('loadRave'),
           'load error carries the loadRave prefix: ' + err.message);

    // A failed load must leave the namespace usable.
    err = expectThrows(() => bro.rave.loadRave(bogus), 'second loadRave(nonexistent dir)');
    assert(String(err.message).includes('loadRave'), 'second failed load errors identically');

    console.log('bro.rave binding contract OK (weights-free)');
}
