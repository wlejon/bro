// Weights-gated test for Qwen3-TTS determinism, word timings and Parakeet's
// resampling and forced alignment.
//
// 1. A seeded synthesis is a function of (text, voice, seed) alone. The Code
//    Predictor's CUDA graph once baked in the seed of the first sampled call
//    and replayed it for every later seed, so the same request gave different
//    audio depending on what the process had synthesized before: here a take
//    rendered after another seed must match, byte for byte, the same take
//    rendered by a freshly loaded model.
// 2. synthesize(text, { align: parakeet }) answers `words`: one entry per
//    whitespace word, in order, inside the clip.
// 3. Parakeet takes any sample rate (24 kHz TTS output, 48 kHz) and resamples
//    itself; align() on the 48 kHz copy lands within a frame of the 24 kHz one.
//
// Skips without the weights (BRO_WEIGHTS or ../brosoundml/weights) or a GPU.

const fs = require('node:fs');

const WEIGHTS = process.env.BRO_WEIGHTS || '../brosoundml/weights';
const QWEN_DIR = WEIGHTS + '/qwen-tts/0.6B-customvoice';
const PARAKEET_DIR = WEIGHTS + '/parakeet/0.6b-v3';
const TEXT_A = 'Every street on the map.';
const TEXT_B = 'Earned in the field, built to your loadout.';

function hash(f) {
    const u = new Uint32Array(f.buffer, f.byteOffset, f.length);
    let h = 2166136261 >>> 0;
    for (let i = 0; i < u.length; i++) { h ^= u[i]; h = Math.imul(h, 16777619) >>> 0; }
    return h.toString(16) + ':' + f.length;
}

function upsample2(samples) {
    const out = new Float32Array(samples.length * 2);
    for (let i = 0; i < samples.length; i++) {
        const a = samples[i], b = i + 1 < samples.length ? samples[i + 1] : a;
        out[2 * i] = a;
        out[2 * i + 1] = 0.5 * (a + b);
    }
    return out;
}

if (bro.tts.available === false || bro.stt.available === false) {
    skipTest('bro.tts / bro.stt are the unavailable stubs');
} else if (!fs.existsSync(QWEN_DIR + '/model.safetensors')) {
    skipTest('Qwen3-TTS weights not found at ' + QWEN_DIR);
} else if (!fs.existsSync(PARAKEET_DIR + '/model.safetensors')) {
    skipTest('Parakeet weights not found at ' + PARAKEET_DIR);
} else if (!bro.gpu.available) {
    skipTest('no GPU backend (' + bro.gpu.backend + ')');
} else {
    const q1 = bro.tts.loadQwen(QWEN_DIR);
    const speaker = q1.speakers()[0];
    const opts = seed => ({ speaker, language: 'english', temperature: 0.8, topP: 0.95, topK: 50,
                            repetitionPenalty: 1.05, seed });

    const a1 = q1.synthesize(TEXT_A, opts(11));
    const b1 = q1.synthesize(TEXT_B, opts(22));
    const a2 = q1.synthesize(TEXT_A, opts(11));
    assert(hash(a1.samples) === hash(a2.samples), 'same request twice in one process is identical');

    const q2 = bro.tts.loadQwen(QWEN_DIR);
    const b2 = q2.synthesize(TEXT_B, opts(22));
    assert(hash(b1.samples) === hash(b2.samples),
           'a seed-22 take after a seed-11 take matches a fresh model\'s seed-22 take: ' +
           hash(b1.samples) + ' vs ' + hash(b2.samples));
    const a3 = q2.synthesize(TEXT_A, opts(11));
    assert(hash(a1.samples) === hash(a3.samples), 'the seed-11 take is the same on both models');
    console.log('determinism: A ' + hash(a1.samples) + '  B ' + hash(b1.samples));

    const pk = bro.stt.loadParakeet(PARAKEET_DIR);
    assert(pk.tokenizer instanceof bro.stt.ParakeetTokenizer, 'the model carries its tokenizer.json');

    const r = q1.synthesize(TEXT_B, Object.assign(opts(22), { align: pk }));
    assert(hash(r.samples) === hash(b1.samples), 'align does not change the audio');
    const dur = r.samples.length / r.sampleRate;
    const expect = TEXT_B.split(/\s+/);
    assert(Array.isArray(r.words) && r.words.length === expect.length,
           'one timing per word: ' + JSON.stringify(r.words));
    let prev = 0;
    r.words.forEach((w, i) => {
        assert(w.text === expect[i], 'word ' + i + ' keeps its text: ' + w.text);
        assert(w.start >= prev - 1e-9 && w.end >= w.start && w.end <= dur + 1e-6,
               'word ' + i + ' is ordered and inside the clip: ' + JSON.stringify(w));
        prev = w.end;
    });
    assert(r.words[0].start < 0.8, 'the first word starts near the top: ' + r.words[0].start);
    assert(r.words[r.words.length - 1].end > dur * 0.6, 'the last word ends near the end');
    assert(q1.synthesize(TEXT_A, opts(11)).words === undefined, 'no words without align');
    console.log('words: ' + r.words.map(w => w.text + '@' + w.start.toFixed(2)).join(' '));

    const t24 = pk.tokenizer.decode(pk.transcribe({ samples: r.samples, sampleRate: 24000 }).tokenIds);
    const hi = upsample2(r.samples);
    const t48 = pk.tokenizer.decode(pk.transcribe({ samples: hi, sampleRate: 48000 }).tokenIds);
    assert(/loadout/i.test(t24) && /field/i.test(t24), 'Parakeet hears 24 kHz: ' + t24);
    assert(t48 === t24, 'Parakeet hears the 48 kHz copy the same: ' + t48 + ' | ' + t24);

    const al = pk.align({ samples: hi, sampleRate: 48000 }, TEXT_B);
    assert(typeof al.logProb === 'number' && al.words.length === expect.length, 'align() on 48 kHz');
    al.words.forEach((w, i) => {
        assert(Math.abs(w.start - r.words[i].start) <= 0.09,
               'word ' + i + ' start agrees across rates: ' + w.start + ' vs ' + r.words[i].start);
    });

    let err = null;
    try { q1.synthesize(TEXT_A, Object.assign(opts(11), { align: {} })); } catch (e) { err = e; }
    assert(err instanceof TypeError, 'a bad align option is a TypeError: ' + err);
}
