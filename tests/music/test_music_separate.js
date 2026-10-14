// bro.music.loadSeparator binding tests (HTDemucs htdemucs_ft source separation).
//
// Weights-free: the loader exists, argument validation, a clean error on a
// missing model dir.
// Weights-gated (BRO_WEIGHTS or ../brosoundml/weights, needing htdemucs_ft from
// brosoundml's tools/htdemucs_convert.py, and a GPU backend): a synthetic 10 s
// 48 kHz stereo mix separated through JS. Sync vocals; async all four stems
// with progress, which must give the same vocals and stems that sum back to
// about the mix; a raw mono Float32Array at another rate; cancel; a stem the
// separator did not load. Numeric parity with the reference lives in
// brosoundml's test_htdemucs.

function expectThrows(fn, what) {
    let err = null;
    try { fn(); } catch (e) { err = e; }
    assert(err !== null, what + ' throws');
    return err;
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

assert(typeof bro === 'object' && bro.music, 'bro.music namespace exists');

if (bro.music.available === false) {
    const err = expectThrows(() => bro.music.loadSeparator('x'), 'stub loadSeparator()');
    assert(String(err.message).includes('compiled without'), 'stub error names the build flag: ' + err.message);
    console.log('bro.music is the unavailable stub; stub contract OK');
} else {
    assert(typeof bro.music.loadSeparator === 'function', 'loadSeparator is a function');
    let err = expectThrows(() => bro.music.loadSeparator(), 'loadSeparator() with no args');
    assert(err instanceof TypeError, 'no-args loadSeparator throws TypeError');
    err = expectThrows(() => bro.music.loadSeparator('tests/music/__none__'), 'loadSeparator(missing)');
    assert(!(err instanceof TypeError) && String(err.message).includes('loadSeparator'),
           'missing model dir is a runtime Error with the loader prefix: ' + err.message);
    err = expectThrows(() => bro.music.loadSeparator('tests/music', { stems: 7 }), 'loadSeparator stems: 7');
    assert(err instanceof TypeError, 'a non-string stems option throws TypeError');

    const fs = require('node:fs');
    const WEIGHTS = process.env.BRO_WEIGHTS || '../brosoundml/weights';
    const DIR = WEIGHTS + '/htdemucs_ft';
    if (!fs.existsSync(DIR + '/config.json')) {
        skipTest('no htdemucs_ft under ' + DIR);
    } else if (!bro.gpu || !bro.gpu.available) {
        skipTest('no GPU backend');
    } else {
        run(DIR);
    }
}

// 10 s of planar stereo at 48 kHz: a harmonic line with vibrato, a bass line,
// a kick every half second and a pad, panned apart.
function synth(sr, seconds) {
    const n = Math.round(sr * seconds);
    const s = new Float32Array(2 * n);
    const notes = [0, 3, 5, 7, 5, 3, 0, -2].map((k) => 220 * Math.pow(2, k / 12));
    let ph = 0, bph = 0;
    for (let i = 0; i < n; i++) {
        const t = i / sr;
        const f0 = notes[Math.floor(t / 0.45) % notes.length] * (1 + 0.012 * Math.sin(2 * Math.PI * 5.5 * t));
        ph += 2 * Math.PI * f0 / sr;
        let voice = 0;
        for (let k = 1; k < 9; k++) voice += Math.sin(k * ph) / k;
        voice *= 0.25 * Math.min(1, 1.6 * Math.sin(Math.PI * ((t / 0.45) % 1)));
        bph += 2 * Math.PI * 55 * Math.pow(2, [0, 5, 7, 3][Math.floor(t / 0.9) % 4] / 12) / sr;
        const bass = 0.25 * (Math.sin(bph) + 0.3 * Math.sin(2 * bph));
        const tk = t % 0.5;
        const kick = 0.5 * Math.sin(2 * Math.PI * (50 * tk + 70 * (1 - Math.exp(-tk * 30)) / 30)) * Math.exp(-tk * 12);
        const pad = 0.03 * (Math.sign(Math.sin(2 * Math.PI * 261.6 * t)) + Math.sign(Math.sin(2 * Math.PI * 392 * t)));
        s[i] = 0.6 * voice + 0.8 * bass + 0.7 * kick + 0.9 * pad;
        s[n + i] = 0.8 * voice + 0.6 * bass + 0.7 * kick + 0.4 * pad;
    }
    return { samples: s, channels: 2, sampleRate: sr, frames: n };
}

function run(dir) {
    const sep = bro.music.loadSeparator(dir);
    assert(sep.loaded && sep.sampleRate === 44100, 'separator loaded, model rate 44100');
    assert(JSON.stringify(sep.sources) === '["drums","bass","other","vocals"]', 'sources: ' + JSON.stringify(sep.sources));
    assert(JSON.stringify(sep.stems) === JSON.stringify(sep.sources), 'all four networks loaded');

    const mix = synth(48000, 10);
    const N = mix.frames;
    const v = sep.separate(mix, { stems: 'vocals' });
    assert(JSON.stringify(v.names) === '["vocals"]', 'sync result names');
    const voc = v.stems.vocals;
    assert(voc && voc.frames === N && voc.channels === 2 && voc.sampleRate === 48000 && voc.samples.length === 2 * N,
           'vocals at the input rate and length');
    let finite = true;
    for (let i = 0; i < voc.samples.length; i++) finite = finite && Number.isFinite(voc.samples[i]);
    assert(finite, 'vocals are finite');

    // Async, all four stems, with progress.
    let result = null, info = null, progress = [];
    const h = sep.separate(mix, {
        onProgress: (p) => progress.push([p.done, p.total]),
        onDone: (r, i) => { result = r; info = i; },
    });
    assert(h && typeof h.cancel === 'function', 'async separate returns an AsyncHandle');
    assert(sep.busy, 'separator busy while a job runs');
    expectThrows(() => sep.separate(mix), 'separate while busy');
    pumpUntil('async separate', () => info !== null, 120);
    assert(!info.cancelled && !info.error, 'async separate finished cleanly: ' + JSON.stringify(info));
    assert(JSON.stringify(result.names) === '["drums","bass","other","vocals"]', 'all stems in source order');
    const last = progress[progress.length - 1];
    assert(last && last[0] === last[1] && last[1] >= 4, 'progress reaches its total: ' + JSON.stringify(progress));
    const dv = maxDiff(result.stems.vocals.samples, voc.samples);
    assert(dv < 1e-5, 'async vocals equal sync vocals, max|d| ' + dv);
    // The stems sum back to about the mix.
    let num = 0, den = 0;
    for (let i = 0; i < 2 * N; i++) {
        let sum = 0;
        for (const name of result.names) sum += result.stems[name].samples[i];
        num += mix.samples[i] * mix.samples[i];
        den += (mix.samples[i] - sum) * (mix.samples[i] - sum);
    }
    const sumDb = 10 * Math.log10(num / den);
    assert(sumDb > 15, 'stems sum to the mix within 15 dB, got ' + sumDb.toFixed(1) + ' dB');

    // A raw mono Float32Array at 44.1 kHz.
    const mono = new Float32Array(44100 * 3);
    for (let i = 0; i < mono.length; i++) mono[i] = 0.3 * Math.sin(2 * Math.PI * 110 * i / 44100);
    const m = sep.separate(mono, { stems: ['bass', 'vocals'], sampleRate: 44100 });
    assert(JSON.stringify(m.names) === '["bass","vocals"]', 'subset in the order asked');
    assert(m.stems.bass.frames === mono.length && m.stems.bass.channels === 2 && m.stems.bass.sampleRate === 44100,
           'mono input gives stereo stems at its rate and length');

    // Cancel.
    let cinfo = null;
    const hc = sep.separate(mix, { onDone: (r, i) => { cinfo = [r, i]; } });
    hc.cancel();
    pumpUntil('cancelled separate', () => cinfo !== null, 120);
    assert(cinfo[1].cancelled && cinfo[0] === undefined, 'cancelled separate reports cancelled, no result');

    // A vocals-only separator cannot be asked for drums.
    const vs = bro.music.loadSeparator(dir, { stems: ['vocals'] });
    assert(JSON.stringify(vs.stems) === '["vocals"]', 'vocals-only load');
    const err = expectThrows(() => vs.separate(mix, { stems: 'drums' }), 'drums from a vocals-only separator');
    assert(err instanceof TypeError, 'an unloaded stem throws TypeError');
    const vv = vs.separate(mix);
    assert(maxDiff(vv.stems.vocals.samples, voc.samples) < 1e-5, 'vocals-only separator gives the same vocals');

    console.log('bro.music separator OK: stems sum to the mix at ' + sumDb.toFixed(1) + ' dB, ' +
                progress.length + ' progress events');
}
