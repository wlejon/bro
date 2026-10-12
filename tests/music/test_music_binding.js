// bro.music binding tests (YuE2 song generation + the YuE2 VAE).
//
// Weights-free: the namespace (or the available:false stub), loader argument
// validation, clean errors on missing model dirs.
// Weights-gated (BRO_WEIGHTS or ../brosoundml/weights, needing yue2-vae and a
// GPU backend): the VAE through JS. A synthetic 2 s stereo signal is encoded,
// then decoded whole, tiled, and streamed in chunks (onChunk); the three must
// agree, the chunks must be contiguous, and async encode must equal sync.
// Numeric parity with the reference lives in brosoundml's test_yue2_vae.

function expectThrows(fn, what) {
    let err = null;
    try { fn(); } catch (e) { err = e; }
    assert(err !== null, what + ' throws');
    return err;
}

assert(typeof bro === 'object', 'bro global exists');
assert(bro.music !== undefined && bro.music !== null, 'bro.music namespace exists');

if (bro.music.available === false) {
    const err = expectThrows(() => bro.music.loadVae('x'), 'stub loadVae()');
    assert(String(err.message).includes('compiled without'), 'stub error names the build flag: ' + err.message);
    console.log('bro.music is the unavailable stub; stub contract OK');
} else {
    assert(typeof bro.music.loadYuE2 === 'function', 'loadYuE2 is a function');
    assert(typeof bro.music.loadVae === 'function', 'loadVae is a function');
    assert(bro.music.init() === undefined, 'init() returns undefined');

    let err = expectThrows(() => bro.music.loadVae(), 'loadVae() with no args');
    assert(err instanceof TypeError, 'no-args loadVae throws TypeError');
    err = expectThrows(() => bro.music.loadYuE2(7), 'loadYuE2(7)');
    assert(err instanceof TypeError, 'non-string path throws TypeError');
    err = expectThrows(() => bro.music.loadVae('tests/music/__none__'), 'loadVae(missing)');
    assert(!(err instanceof TypeError) && String(err.message).includes('loadVae'),
           'missing VAE dir is a runtime Error with the loader prefix: ' + err.message);
    err = expectThrows(() => bro.music.loadYuE2('tests/music/__none__'), 'loadYuE2(missing)');
    assert(String(err.message).includes('loadYuE2'), 'missing model dir names loadYuE2: ' + err.message);
    err = expectThrows(() => bro.music.loadYuE2('tests/music', { arWeights: 'int4' }), 'loadYuE2 arWeights int4');
    assert(String(err.message).includes('arWeights'), 'a bad arWeights names the option: ' + err.message);

    // A stream's knob clamps with no model loaded (a stream's knobRanges is
    // the same object: tests/music/test_music_stream.js), a fresh object each
    // read.
    const kr = bro.music.knobRanges;
    assert(JSON.stringify(kr) === JSON.stringify({
        energy: { min: -2, max: 1 }, texture: { min: -2, max: 2 },
        percussion: { min: -6, max: 6 }, arrangement: { min: -2, max: 1 } }),
        'bro.music.knobRanges is the clamps Yue2Knobs applies: ' + JSON.stringify(kr));
    kr.energy.min = 99;
    assert(bro.music.knobRanges.energy.min === -2, 'each read is a fresh object');

    const fs = require('node:fs');
    const WEIGHTS = process.env.BRO_WEIGHTS || '../brosoundml/weights';
    const VAE_DIR = WEIGHTS + '/yue2-vae';
    if (!fs.existsSync(VAE_DIR + '/model.safetensors')) {
        skipTest('no YuE2 VAE under ' + VAE_DIR);
    } else if (!bro.gpu || !bro.gpu.available) {
        skipTest('no GPU backend');
    } else {
        runVae(bro.music.loadVae(VAE_DIR));
    }
}

function pumpUntil(desc, fn, seconds) {
    const iters = Math.ceil((seconds * 1000) / 16);
    for (let i = 0; i < iters; i++) {
        advanceTime(16);
        wallSleep(16);
        if (fn()) return;
    }
    throw new Error('timeout waiting for ' + desc);
}

function maxDiff(a, b) {
    let m = 0;
    for (let i = 0; i < a.length; i++) m = Math.max(m, Math.abs(a[i] - b[i]));
    return m;
}

function runVae(vae) {
    assert(vae.loaded && vae.hasEncoder, 'VAE loaded with its encoder');
    assert(vae.sampleRate === 48000 && vae.channels === 2 && vae.latentDim === 64, 'VAE shape');
    assert(vae.frameRate === 25 && vae.downsamplingRatio === 1920, 'VAE rate');
    assert(vae.requiredHalo(16) === 12, 'requiredHalo(16) is 12, got ' + vae.requiredHalo(16));
    assert(vae.outputLength(10) === 1920 * 10 - 64, 'outputLength');

    // 2 s of planar stereo: a chord, a chirp on the left, pulses on the right.
    const S = 96000, sr = 48000;
    const samples = new Float32Array(2 * S);
    for (let i = 0; i < S; i++) {
        const t = i / sr;
        const chord = 0.12 * (Math.sin(2 * Math.PI * 220 * t) + Math.sin(2 * Math.PI * 277.18 * t) +
                              Math.sin(2 * Math.PI * 329.63 * t));
        samples[i] = chord + 0.2 * Math.sin(2 * Math.PI * (80 * t + 1480 * t * t));
        samples[S + i] = chord + (Math.sin(2 * Math.PI * 2 * t) > 0.95 ? 0.3 * Math.sin(2 * Math.PI * 1000 * t) : 0);
    }
    const audio = { samples, channels: 2, sampleRate: sr };

    const enc = vae.encode(audio);
    assert(enc.frames === 50 && enc.latents.length === 50 * 64, 'encode: 50 frames of 64, got ' + enc.frames);

    const full = vae.decode(enc.latents);
    const N = vae.outputLength(enc.frames);
    assert(full.frames === N && full.channels === 2 && full.samples.length === 2 * N, 'decode shape');
    let peak = 0;
    for (let i = 0; i < full.samples.length; i++) peak = Math.max(peak, Math.abs(full.samples[i]));
    assert(peak > 0.1 && peak < 2, 'decoded audio is not silent or exploded, peak ' + peak);

    const tiled = vae.decode(enc.latents, { coreFrames: 16 });
    assert(tiled.frames === N, 'tiled decode length');
    const dTiled = maxDiff(tiled.samples, full.samples);
    assert(dTiled < 1e-4, 'tiled decode matches full, max|d| ' + dTiled);

    // Streamed decode: chunks in order, planar, concatenating to the full decode.
    const left = new Float32Array(N), right = new Float32Array(N);
    let next = 0, chunks = 0, finished = null, contiguous = true, firstChunk = 0;
    const h = vae.decode(enc.latents, {
        coreFrames: 16,
        firstFrames: 4,
        onChunk: (c) => {
            if (!chunks) firstChunk = c.frames;
            contiguous = contiguous && c.sample0 === next && c.channels === 2;
            left.set(c.samples.subarray(0, c.frames), c.sample0);
            right.set(c.samples.subarray(c.frames, 2 * c.frames), c.sample0);
            next = c.sample0 + c.frames;
            chunks++;
        },
        onDone: (r, info) => { finished = info; },
    });
    assert(h && typeof h.cancel === 'function', 'streamed decode returns an AsyncHandle');
    pumpUntil('streamed decode', () => finished !== null, 60);
    assert(!finished.cancelled && !finished.error, 'streamed decode finished cleanly: ' + JSON.stringify(finished));
    assert(contiguous && next === N, 'chunks contiguous from 0 to N, ended at ' + next);
    // Tiles grow 4, 8, 16, 16, ... (the last one takes the remainder).
    assert(firstChunk === 4 * 1920, 'first chunk is the 4-frame first tile, got ' + firstChunk + ' samples');
    assert(chunks > Math.ceil(enc.frames / 16), 'growing tiles: more chunks than 16-frame cores, got ' + chunks);
    const dStream = Math.max(maxDiff(left, full.samples.subarray(0, N)), maxDiff(right, full.samples.subarray(N)));
    assert(dStream < 1e-4, 'streamed decode matches full, max|d| ' + dStream);

    // Async encode equals sync encode.
    let asyncEnc = null;
    vae.encode(audio, { onDone: (r, info) => { asyncEnc = [r, info]; } });
    assert(vae.busy, 'VAE is busy while an async op runs');
    expectThrows(() => vae.decode(enc.latents), 'decode while busy');
    pumpUntil('async encode', () => asyncEnc !== null, 60);
    assert(asyncEnc[0] && asyncEnc[0].frames === enc.frames, 'async encode result');
    assert(maxDiff(asyncEnc[0].latents, enc.latents) < 1e-5, 'async encode equals sync');

    // Shape errors.
    let err = expectThrows(() => vae.decode(new Float32Array(65)), 'decode of a ragged latent array');
    assert(err instanceof TypeError, 'ragged latents throw TypeError');
    err = expectThrows(() => vae.decode(enc.latents, { coreFrames: 16, haloFrames: 3 }), 'decode with a short halo');
    assert(err instanceof TypeError, 'short halo throws TypeError');

    console.log('bro.music VAE OK: tiled ' + dTiled.toExponential(2) + ', stream ' + dStream.toExponential(2) +
                ' over ' + chunks + ' chunks');
}
