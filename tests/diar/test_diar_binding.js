// Weights-free binding tests for bro.diar (speaker diarization: streaming
// Sortformer + ClusterDiarizer). Exercises the surface that needs no model
// files: namespace presence (or the honest available:false stub when built
// without BRO_WITH_SOUNDML), loader and diarize argument validation, and clean
// errors on nonexistent model paths. No downloads, no diarization with a real
// model.

function expectThrows(fn, what) {
    let err = null;
    try { fn(); } catch (e) { err = e; }
    assert(err !== null, what + ' throws');
    return err;
}

assert(typeof bro === 'object', 'bro global exists');
assert(bro.diar !== undefined && bro.diar !== null, 'bro.diar namespace exists');

if (bro.diar.available === false) {
    const err = expectThrows(() => bro.diar.loadSortformer('x'), 'stub loadSortformer()');
    assert(String(err.message).includes('compiled without'),
           'stub error names the missing build flag: ' + err.message);
    console.log('bro.diar is the unavailable stub; stub contract OK');
} else {
    for (const f of ['init', 'loadSortformer', 'loadClusterDiarizer',
                     'diarize', 'clusterDiarize']) {
        assert(typeof bro.diar[f] === 'function', 'bro.diar.' + f + ' is a function');
    }

    assert(bro.diar.init() === undefined, 'init() returns undefined');

    // ── argument validation — synchronous TypeErrors ─────────────────────────
    let err = expectThrows(() => bro.diar.loadSortformer(), 'loadSortformer() with no args');
    assert(err instanceof TypeError, 'no-args loadSortformer throws TypeError, got: ' + err);
    assert(String(err.message).includes('path'), 'error names the path arg: ' + err.message);

    err = expectThrows(() => bro.diar.loadClusterDiarizer(),
                       'loadClusterDiarizer() with no args');
    assert(err instanceof TypeError, 'no-args loadClusterDiarizer throws TypeError, got: ' + err);
    assert(String(err.message).includes('two paths'),
           'error names both required paths: ' + err.message);

    err = expectThrows(() => bro.diar.diarize(), 'diarize() with no args');
    assert(err instanceof TypeError, 'no-args diarize throws TypeError, got: ' + err);

    err = expectThrows(() => bro.diar.clusterDiarize(), 'clusterDiarize() with no args');
    assert(err instanceof TypeError, 'no-args clusterDiarize throws TypeError, got: ' + err);

    // ── loaders on nonexistent model paths — clean runtime Error, no crash ──
    const bogus = 'tests/diar/__no_such_dir__';
    err = expectThrows(() => bro.diar.loadSortformer(bogus), 'loadSortformer(nonexistent dir)');
    assert(!(err instanceof TypeError),
           'missing model dir is a runtime Error, not a TypeError');
    assert(String(err.message).includes('loadSortformer'),
           'load error carries the loadSortformer prefix: ' + err.message);

    // A failed load must leave the namespace usable.
    err = expectThrows(() => bro.diar.loadSortformer(bogus),
                       'second loadSortformer(nonexistent dir)');
    assert(String(err.message).includes('loadSortformer'),
           'second failed load errors identically');

    console.log('bro.diar binding contract OK (weights-free)');
}
