// Weights-free binding tests for bro.stt (speech-to-text: Whisper / Parakeet /
// Qwen3-ASR). Exercises the surface that needs no model files: namespace
// presence (or the honest available:false stub when built without
// BRO_WITH_SOUNDML), loader argument validation, and clean errors on
// nonexistent model paths. No downloads, no transcribe() with a real model.

function expectThrows(fn, what) {
    let err = null;
    try { fn(); } catch (e) { err = e; }
    assert(err !== null, what + ' throws');
    return err;
}

assert(typeof bro === 'object', 'bro global exists');
assert(bro.stt !== undefined && bro.stt !== null, 'bro.stt namespace exists');

if (bro.stt.available === false) {
    const err = expectThrows(() => bro.stt.loadWhisper('x'), 'stub loadWhisper()');
    assert(String(err.message).includes('compiled without'),
           'stub error names the missing build flag: ' + err.message);
    console.log('bro.stt is the unavailable stub; stub contract OK');
} else {
    for (const f of ['init', 'loadWhisper', 'loadParakeet', 'loadParakeetTokenizer',
                     'loadQwenAsr', 'loadTokenizer', 'transcribe']) {
        assert(typeof bro.stt[f] === 'function', 'bro.stt.' + f + ' is a function');
    }

    assert(bro.stt.init() === undefined, 'init() returns undefined');

    // ── loader argument validation — synchronous TypeErrors ─────────────────
    let err = expectThrows(() => bro.stt.loadWhisper(), 'loadWhisper() with no args');
    assert(err instanceof TypeError, 'no-args loadWhisper throws TypeError, got: ' + err);
    assert(String(err.message).includes('path'), 'error names the path arg: ' + err.message);

    err = expectThrows(() => bro.stt.loadParakeet(), 'loadParakeet() with no args');
    assert(err instanceof TypeError, 'no-args loadParakeet throws TypeError, got: ' + err);

    err = expectThrows(() => bro.stt.loadQwenAsr(), 'loadQwenAsr() with no args');
    assert(err instanceof TypeError, 'no-args loadQwenAsr throws TypeError, got: ' + err);

    err = expectThrows(() => bro.stt.loadTokenizer(), 'loadTokenizer() with no args');
    assert(err instanceof TypeError, 'no-args loadTokenizer throws TypeError, got: ' + err);

    err = expectThrows(() => bro.stt.transcribe(), 'transcribe() with no args');
    assert(err instanceof TypeError, 'no-args transcribe throws TypeError, got: ' + err);

    err = expectThrows(() => bro.stt.transcribe(1, 2), 'transcribe(non-model, non-audio)');
    assert(err instanceof TypeError, 'wrong-typed transcribe throws TypeError, got: ' + err);

    // ── loaders on nonexistent model paths — clean runtime Error, no crash ──
    const bogus = 'tests/stt/__no_such_dir__';
    err = expectThrows(() => bro.stt.loadWhisper(bogus), 'loadWhisper(nonexistent dir)');
    assert(!(err instanceof TypeError),
           'missing model dir is a runtime Error, not a TypeError');
    assert(String(err.message).includes('loadWhisper'),
           'load error carries the loadWhisper prefix: ' + err.message);

    err = expectThrows(() => bro.stt.loadParakeet(bogus), 'loadParakeet(nonexistent dir)');
    assert(!(err instanceof TypeError), 'missing Parakeet dir is a runtime Error');

    err = expectThrows(() => bro.stt.loadQwenAsr(bogus), 'loadQwenAsr(nonexistent dir)');
    assert(!(err instanceof TypeError), 'missing QwenAsr dir is a runtime Error');

    // A failed load must leave the namespace usable.
    err = expectThrows(() => bro.stt.loadWhisper(bogus), 'second loadWhisper(nonexistent dir)');
    assert(String(err.message).includes('loadWhisper'), 'second failed load errors identically');

    console.log('bro.stt binding contract OK (weights-free)');
}
