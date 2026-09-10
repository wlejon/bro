// Weights-free binding tests for bro.tts (text-to-speech: Kokoro / Qwen3-TTS).
// Exercises the surface that needs no model files: namespace presence (or the
// honest available:false stub when built without BRO_WITH_SOUNDML), loader and
// synthesize argument validation, and clean errors on nonexistent model paths.
// No downloads, no synthesis with a real model.

function expectThrows(fn, what) {
    let err = null;
    try { fn(); } catch (e) { err = e; }
    assert(err !== null, what + ' throws');
    return err;
}

assert(typeof bro === 'object', 'bro global exists');
assert(bro.tts !== undefined && bro.tts !== null, 'bro.tts namespace exists');

if (bro.tts.available === false) {
    const err = expectThrows(() => bro.tts.loadKokoro('x'), 'stub loadKokoro()');
    assert(String(err.message).includes('compiled without'),
           'stub error names the missing build flag: ' + err.message);
    console.log('bro.tts is the unavailable stub; stub contract OK');
} else {
    for (const f of ['init', 'loadKokoro', 'loadQwen', 'loadOmniVoice', 'phonemize', 'synthesize']) {
        assert(typeof bro.tts[f] === 'function', 'bro.tts.' + f + ' is a function');
    }

    assert(bro.tts.init() === undefined, 'init() returns undefined');

    // ── argument validation — synchronous TypeErrors ─────────────────────────
    let err = expectThrows(() => bro.tts.loadKokoro(), 'loadKokoro() with no args');
    assert(err instanceof TypeError, 'no-args loadKokoro throws TypeError, got: ' + err);
    assert(String(err.message).includes('path'), 'error names the path arg: ' + err.message);

    err = expectThrows(() => bro.tts.loadQwen(), 'loadQwen() with no args');
    assert(err instanceof TypeError, 'no-args loadQwen throws TypeError, got: ' + err);

    err = expectThrows(() => bro.tts.phonemize(), 'phonemize() with no args');
    assert(err instanceof TypeError, 'no-args phonemize throws TypeError, got: ' + err);

    err = expectThrows(() => bro.tts.synthesize(), 'synthesize() with no args');
    assert(err instanceof TypeError, 'no-args synthesize throws TypeError, got: ' + err);

    err = expectThrows(() => bro.tts.synthesize({}, 'hello'), 'synthesize(non-model)');
    assert(err instanceof TypeError, 'non-model synthesize throws TypeError, got: ' + err);
    assert(String(err.message).includes('Kokoro'),
           'error names the expected model types: ' + err.message);

    // ── loaders on nonexistent model paths — clean runtime Error, no crash ──
    const bogus = 'tests/tts/__no_such_dir__';
    err = expectThrows(() => bro.tts.loadKokoro(bogus), 'loadKokoro(nonexistent dir)');
    assert(!(err instanceof TypeError),
           'missing model dir is a runtime Error, not a TypeError');
    assert(String(err.message).includes('loadKokoro'),
           'load error carries the loadKokoro prefix: ' + err.message);

    err = expectThrows(() => bro.tts.loadQwen(bogus), 'loadQwen(nonexistent dir)');
    assert(!(err instanceof TypeError), 'missing Qwen dir is a runtime Error');

    // ── OmniVoice: argument validation + the missing-dir error, weights-free ──
    err = expectThrows(() => bro.tts.loadOmniVoice(), 'loadOmniVoice() with no args');
    assert(err instanceof TypeError, 'no-args loadOmniVoice throws TypeError, got: ' + err);
    assert(String(err.message).includes('path'), 'error names the path arg: ' + err.message);

    err = expectThrows(() => bro.tts.loadOmniVoice(bogus, { device: 42 }),
                       'loadOmniVoice(bad device)');
    assert(err instanceof TypeError, 'non-string device is a TypeError: ' + err);

    err = expectThrows(() => bro.tts.loadOmniVoice(bogus, { device: 'cuda', precision: 'int8' }),
                       'loadOmniVoice(bad precision)');
    assert(err instanceof TypeError, 'unknown precision is a TypeError: ' + err);
    assert(String(err.message).includes('precision'), 'error names precision: ' + err.message);

    // A missing model dir is a runtime Error carrying the loader's prefix. On a
    // box with no GPU backend the same call is refused before any file is
    // opened (the CPU is opt-in) - also a runtime Error with the same prefix.
    err = expectThrows(() => bro.tts.loadOmniVoice(bogus), 'loadOmniVoice(nonexistent dir)');
    assert(!(err instanceof TypeError), 'missing OmniVoice dir is a runtime Error');
    assert(String(err.message).includes('loadOmniVoice'),
           'load error carries the loadOmniVoice prefix: ' + err.message);
    err = expectThrows(() => bro.tts.loadOmniVoice(bogus, { device: 'cpu' }),
                       'loadOmniVoice(nonexistent dir, cpu)');
    assert(!(err instanceof TypeError), 'missing OmniVoice dir (cpu) is a runtime Error');

    // The dispatching entry point knows the class.
    err = expectThrows(() => bro.tts.synthesize({}, 'hello'), 'synthesize(non-model) names OmniVoice');
    assert(String(err.message).includes('OmniVoice'),
           'synthesize error lists OmniVoice among the model types: ' + err.message);

    // The class object is exposed (for instanceof) and its prototype carries
    // the documented methods + getters without any weights loaded.
    assert(typeof bro.tts.OmniVoice === 'function', 'bro.tts.OmniVoice is the class object');
    err = expectThrows(() => new bro.tts.OmniVoice(), 'new bro.tts.OmniVoice()');
    assert(err instanceof TypeError, 'OmniVoice is not constructible from JS: ' + err);
    const proto = bro.tts.OmniVoice.prototype;
    assert(proto && typeof proto === 'object', 'OmniVoice.prototype exists');
    for (const m of ['synthesize', 'generateCodes', 'decodeCodes', 'encodeAudio',
                     'createPrompt', 'savePrompt', 'loadPrompt', 'estimateFrames',
                     'tokenize', 'languages', 'instructAttributes', 'nonverbalTags',
                     'unload']) {
        assert(typeof proto[m] === 'function', 'OmniVoice.prototype.' + m + ' is a function');
    }
    for (const g of ['loaded', 'device', 'precision', 'sampleRate', 'config']) {
        const d = Object.getOwnPropertyDescriptor(proto, g);
        assert(d && typeof d.get === 'function', 'OmniVoice.prototype.' + g + ' is a getter');
    }
    assert(!(({}) instanceof bro.tts.OmniVoice), 'a plain object is not an OmniVoice');

    // A failed load must leave the namespace usable.
    err = expectThrows(() => bro.tts.loadKokoro(bogus), 'second loadKokoro(nonexistent dir)');
    assert(String(err.message).includes('loadKokoro'), 'second failed load errors identically');

    console.log('bro.tts binding contract OK (weights-free)');
}
