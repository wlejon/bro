// Weights-gated end-to-end test for bro.tts.loadOmniVoice.
//
// Skips (passes with a message) when the OmniVoice weights or a GPU backend
// are absent: the weights live in the brosoundml sibling
// (BRO_WEIGHTS env or ../brosoundml/weights), and OmniVoice's LM runs on the
// GPU only (the CPU is opt-in and not exercised here). Whisper (same weights
// root) transcribes the synthesized clip so the check is on the words, not on
// the waveform.
//
// Wall time on an RTX 4090: ~10 s of model loads + ~15 s of synthesis.

const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const WEIGHTS = process.env.BRO_WEIGHTS || '../brosoundml/weights';
const OMNI_DIR = WEIGHTS + '/omnivoice';
const WHISPER_DIR = WEIGHTS + '/whisper';
const TEXT = 'Hello there, this is a test of the OmniVoice pipeline.';

function ms(t0) { return ((Date.now() - t0) / 1000).toFixed(2) + 's'; }

// advanceTime() drains the async-job queue and fires callbacks; wallSleep()
// gives the worker thread wall-clock time (see docs/headless.md).
function pumpUntil(desc, fn, seconds) {
    const iters = Math.ceil((seconds * 1000) / 16);
    for (let i = 0; i < iters; i++) {
        advanceTime(16);
        wallSleep(16);
        if (fn()) return;
    }
    throw new Error('timeout waiting for ' + desc);
}

// Run one async op to completion: returns [result, info] from onDone.
function run(desc, launch, seconds) {
    let out = null;
    const handle = launch((result, info) => { out = [result, info]; });
    assert(handle && typeof handle.cancel === 'function', desc + ' returns an AsyncHandle');
    pumpUntil(desc, () => out !== null, seconds || 120);
    return out;
}

// 24 kHz -> 16 kHz by linear interpolation: plenty for a transcript check.
function to16k(samples, sr) {
    if (sr === 16000) return samples;
    const ratio = sr / 16000;
    const n = Math.floor(samples.length / ratio);
    const out = new Float32Array(n);
    for (let i = 0; i < n; i++) {
        const x = i * ratio, i0 = Math.floor(x), i1 = Math.min(i0 + 1, samples.length - 1);
        const f = x - i0;
        out[i] = samples[i0] * (1 - f) + samples[i1] * f;
    }
    return out;
}

let transcribe = null;   // lazily loaded Whisper: (samples, sr) -> lower-cased text
function whisperText(samples, sr) {
    if (!transcribe) {
        const t0 = Date.now();
        const whisper = bro.stt.loadWhisper(WHISPER_DIR);
        const tok = bro.stt.loadTokenizer({
            vocabPath: WHISPER_DIR + '/vocab.json', mergesPath: WHISPER_DIR + '/merges.txt',
        });
        const prompt = tok.buildPrompt('en', 'transcribe', false);
        console.log('loadWhisper: ' + ms(t0));
        transcribe = (s, r) => {
            const ids = whisper.transcribe({ samples: to16k(s, r), sampleRate: 16000 }, prompt,
                                           { maxNewTokens: 96 });
            return tok.decode(ids, true).trim().toLowerCase();
        };
    }
    return transcribe(samples, sr);
}

if (bro.tts.available === false) {
    console.log('SKIP: bro.tts is the unavailable stub');
} else if (!fs.existsSync(OMNI_DIR + '/config.json') ||
           !fs.existsSync(OMNI_DIR + '/model.safetensors')) {
    console.log('SKIP: OmniVoice weights not found at ' + OMNI_DIR);
} else if (!fs.existsSync(WHISPER_DIR + '/model.safetensors')) {
    console.log('SKIP: Whisper weights not found at ' + WHISPER_DIR);
} else if (!bro.gpu.available) {
    console.log('SKIP: no GPU backend (' + bro.gpu.backend + '); OmniVoice runs on the GPU only');
} else {
    // ── Load ─────────────────────────────────────────────────────────────────
    let t0 = Date.now();
    const omni = bro.tts.loadOmniVoice(OMNI_DIR, { precision: 'bf16' });
    console.log('loadOmniVoice: ' + ms(t0) + '  device=' + omni.device + ' precision=' + omni.precision);
    assert(omni instanceof bro.tts.OmniVoice, 'handle is an OmniVoice');
    assert(omni.loaded === true, 'loaded');
    assert(omni.precision === 'bf16', 'precision is bf16');
    assert(omni.device !== 'CPU', 'loaded on a GPU: ' + omni.device);
    const cfg = omni.config;
    assert(cfg.sampleRate === 24000 && cfg.frameRate === 25 && cfg.numCodebooks === 8 &&
           cfg.audioVocabSize === 1025 && cfg.maskId === 1024,
           'config: ' + JSON.stringify(cfg));
    assert(cfg.precision === 'bf16' && cfg.device === omni.device && cfg.decoderOnly === false,
           'config mirrors the load options');
    const NQ = cfg.numCodebooks, FRAME = cfg.sampleRate / cfg.frameRate;   // 960 samples per frame

    // ── Model-free helpers ───────────────────────────────────────────────────
    const ids = omni.tokenize(TEXT);
    assert(ids instanceof Int32Array && ids.length > 5 && ids.length < 40,
           'tokenize gives a plausible id count: ' + ids.length);
    // A non-verbal tag tokenizes standalone: its ids never depend on the text
    // around it, so they appear verbatim at the end of a sentence carrying it.
    const tagIds = Array.from(omni.tokenize('[laughter]'));
    const withTag = Array.from(omni.tokenize('Hello there [laughter]'));
    assert(tagIds.length >= 1 && withTag.length > tagIds.length &&
           withTag.slice(-tagIds.length).join(',') === tagIds.join(','),
           'a non-verbal tag tokenizes standalone: ' + tagIds + ' in ' + withTag);
    const langs = omni.languages();
    assert(Array.isArray(langs) && langs.length > 500 && langs.every(s => typeof s === 'string'),
           'languages() lists the 600+ names: ' + langs.length);
    assert(langs.some(s => s.toLowerCase() === 'english'), 'languages() includes English');
    const attrs = omni.instructAttributes();
    assert(Array.isArray(attrs) && attrs.length === 6, 'six instruct categories: ' + attrs.length);
    for (const a of attrs)
        assert(typeof a.name === 'string' && Array.isArray(a.values) && a.values.length > 0,
               'instruct category shape: ' + JSON.stringify(a));
    assert(attrs.some(a => a.name === 'gender'), 'instruct categories include gender');
    const tags = omni.nonverbalTags();
    assert(tags.includes('[laughter]'), 'nonverbalTags includes [laughter]: ' + tags.join(' '));
    const est = omni.estimateFrames(TEXT);
    assert(est > 25 && est < 400, 'estimateFrames without a prompt: ' + est);
    assert(omni.estimateFrames(TEXT, { speed: 2 }) < est, 'speed divides the estimate');
    assert(omni.estimateFrames(TEXT, { duration: 2 }) === 50, 'duration fixes the frames');

    // ── Synthesize with the defaults, then transcribe ────────────────────────
    t0 = Date.now();
    let [out, info] = run('synthesize', done =>
        omni.synthesize(TEXT, { trace: true, onDone: done }));
    console.log('synthesize: ' + ms(t0) + '  ' + out.samples.length + ' samples  lm=' +
                out.trace.lmSeconds.toFixed(2) + 's codec=' + out.trace.codecSeconds.toFixed(2) + 's');
    assert(info.cancelled === false && info.error === undefined, 'synthesize completed: ' + JSON.stringify(info));
    assert(out.sampleRate === 24000, 'sampleRate 24000');
    const secs = out.samples.length / out.sampleRate;
    assert(secs > 1.5 && secs < 10, 'plausible length for one sentence: ' + secs.toFixed(2) + 's');
    let peak = 0;
    for (let i = 0; i < out.samples.length; i++) peak = Math.max(peak, Math.abs(out.samples[i]));
    assert(peak > 0.1 && peak <= 1.0, 'peak-normalised output: ' + peak.toFixed(3));
    const tr = out.trace;
    assert(tr.textIds instanceof Int32Array && tr.textIds.length > ids.length,
           'trace.textIds carries the framed prompt');
    assert(tr.numFrames > 0 && tr.codes.length === NQ * tr.numFrames &&
           tr.unmaskStep.length === tr.codes.length && tr.chunkFrames.length === 1 &&
           tr.chunkFrames[0] === tr.numFrames, 'trace grid shapes');
    for (let i = 0; i < tr.codes.length; i++)
        assert(tr.codes[i] >= 0 && tr.codes[i] < 1024, 'every cell committed to a code');
    const words = whisperText(out.samples, out.sampleRate);
    console.log('whisper: "' + words + '"');
    assert(words.includes('test') && words.includes('pipeline'),
           'transcript contains "test" and "pipeline": ' + words);

    // ── decodeCodes / encodeAudio round the codec ────────────────────────────
    t0 = Date.now();
    const dec = omni.decodeCodes(tr.codes, tr.numFrames);
    console.log('decodeCodes: ' + ms(t0));
    assert(dec.sampleRate === 24000 && dec.samples.length === tr.numFrames * FRAME,
           'decodeCodes gives numFrames * 960 samples: ' + dec.samples.length);
    const dec2 = omni.decodeCodes(tr.codes);
    assert(dec2.samples.length === dec.samples.length, 'numFrames defaults to codes.length / 8');
    t0 = Date.now();
    const enc = omni.encodeAudio(dec.samples, 24000);
    console.log('encodeAudio: ' + ms(t0));
    assert(enc.numCodebooks === NQ && enc.codes instanceof Int32Array &&
           enc.codes.length === NQ * enc.numFrames && Math.abs(enc.numFrames - tr.numFrames) <= 1,
           'encodeAudio frame count matches: ' + enc.numFrames + ' vs ' + tr.numFrames);

    // ── Clone: createPrompt from the output, speak a second sentence ─────────
    t0 = Date.now();
    const prompt = omni.createPrompt(out.samples, { sampleRate: out.sampleRate, refText: TEXT });
    console.log('createPrompt: ' + ms(t0) + '  ' + prompt.numFrames + ' frames rms=' + prompt.rms.toFixed(3));
    assert(prompt.codes instanceof Int32Array && prompt.numFrames > 0 &&
           prompt.codes.length === NQ * prompt.numFrames, 'prompt shape');
    assert(prompt.text === TEXT, 'prompt keeps the transcript (already punctuated): ' + prompt.text);
    assert(prompt.rms > 0, 'prompt rms recorded');
    const estP = omni.estimateFrames('And now the same voice again.', { prompt });
    assert(estP > 10 && estP < 400, 'estimateFrames with a prompt: ' + estP);

    // The .ovcp round trip, through a plain-object copy with codes as a number[].
    const ovcp = path.join(os.tmpdir(), 'bro_test_omnivoice_' + Date.now() + '.ovcp');
    omni.savePrompt({ codes: Array.from(prompt.codes), numFrames: prompt.numFrames,
                      text: prompt.text, rms: prompt.rms }, ovcp);
    const back = omni.loadPrompt(ovcp);
    fs.unlinkSync(ovcp);
    assert(back.numFrames === prompt.numFrames && back.text === prompt.text &&
           Math.abs(back.rms - prompt.rms) < 1e-6 && back.codes.length === prompt.codes.length,
           'ovcp round trip header');
    for (let i = 0; i < prompt.codes.length; i++)
        assert(back.codes[i] === prompt.codes[i], 'ovcp round trip codes');

    const SECOND = 'And now the same voice says something else.';
    t0 = Date.now();
    [out, info] = run('synthesize(prompt)', done =>
        omni.synthesize(SECOND, { prompt, seed: 3, onDone: done }));
    console.log('synthesize(prompt): ' + ms(t0) + '  ' + out.samples.length + ' samples');
    assert(!info.error && !info.cancelled, 'cloned synthesis completed');
    const secs2 = out.samples.length / 24000;
    assert(secs2 > 1 && secs2 < 10, 'plausible cloned length: ' + secs2.toFixed(2) + 's');
    const words2 = whisperText(out.samples, out.sampleRate);
    console.log('whisper(clone): "' + words2 + '"');
    assert(words2.includes('voice'), 'cloned transcript contains "voice": ' + words2);

    // Async createPrompt takes the same route as a synthesis (claims the gate).
    t0 = Date.now();
    let asyncPrompt = null, asyncErr = null;
    const ph = omni.createPrompt(out.samples, { sampleRate: 24000, refText: SECOND,
                                                onDone: p => { asyncPrompt = p; },
                                                onError: m => { asyncErr = m; } });
    assert(ph && typeof ph.cancel === 'function', 'async createPrompt returns a handle');
    let threw = null;
    try { omni.decodeCodes(tr.codes, tr.numFrames); } catch (e) { threw = e; }
    assert(threw && String(threw.message).includes('in flight'),
           'sync codec call is refused while an async op holds the model: ' + threw);
    pumpUntil('async createPrompt', () => asyncPrompt !== null || asyncErr !== null, 60);
    assert(asyncErr === null, 'async createPrompt error: ' + asyncErr);
    assert(asyncPrompt.numFrames > 0 && asyncPrompt.text === SECOND, 'async prompt shape');
    console.log('createPrompt(async): ' + ms(t0));

    // ── generateCodes: exact frames, determinism, onStep, init grid ──────────
    const FR = 50;
    t0 = Date.now();
    let [c1] = run('generateCodes A', done =>
        omni.generateCodes('Hello there.', { frames: FR, gumbelNoise: false, seed: 11, onDone: done }));
    console.log('generateCodes(50 frames): ' + ms(t0));
    assert(c1.numFrames === FR && c1.numCodebooks === NQ && c1.codes instanceof Int32Array &&
           c1.codes.length === NQ * FR, 'generateCodes shape: ' + c1.numFrames + 'x' + c1.numCodebooks);
    let [c2] = run('generateCodes B', done =>
        omni.generateCodes('Hello there.', { frames: FR, gumbelNoise: false, seed: 11, onDone: done }));
    let same = c1.codes.length === c2.codes.length;
    for (let i = 0; same && i < c1.codes.length; i++) same = c1.codes[i] === c2.codes[i];
    assert(same, 'same seed + no noise reproduces the codes exactly');

    // onStep: numSteps calls, the mask count falls monotonically to zero and the
    // per-step unmask counts sum to the grid.
    const STEPS = 16;
    const steps = [];
    let [c3] = run('generateCodes onStep', done =>
        omni.generateCodes('Hello there.', {
            frames: FR, numSteps: STEPS, seed: 5,
            onStep: s => steps.push({ step: s.step, numSteps: s.numSteps, unmasked: s.unmasked,
                                      masked: s.tokens.reduce((n, t) => n + (t === cfg.maskId ? 1 : 0), 0),
                                      tokens: s.tokens.length, scores: s.scores.length,
                                      numFrames: s.numFrames, numCodebooks: s.numCodebooks }),
            onDone: done }));
    assert(steps.length === STEPS, 'onStep fired once per step: ' + steps.length);
    let cum = 0, prevMasked = NQ * FR + 1;
    for (let i = 0; i < steps.length; i++) {
        const s = steps[i];
        assert(s.step === i && s.numSteps === STEPS, 'step index ' + i + ': ' + JSON.stringify(s));
        assert(s.numFrames === FR && s.numCodebooks === NQ && s.tokens === NQ * FR && s.scores === NQ * FR,
               'step arrays are the full grid');
        assert(s.unmasked >= 0, 'unmasked non-negative');
        cum += s.unmasked;
        assert(s.masked <= prevMasked, 'masked cells never increase');
        assert(s.masked === NQ * FR - cum, 'grid mask count matches the cumulative unmasked');
        prevMasked = s.masked;
    }
    assert(cum === NQ * FR && steps[STEPS - 1].masked === 0, 'the last step leaves nothing masked');
    assert(c3.codes.length === NQ * FR, 'onStep run still delivers the codes');

    // Inpainting: keep codebook 0 of run A, re-roll the acoustic codebooks with
    // another seed -> codebook 0 identical, the rest (very likely) not.
    const keep = new Uint8Array(NQ * FR);
    keep.fill(1, 0, FR);                     // codebook 0 = cells [0, FR)
    let [c4] = run('generateCodes init', done =>
        omni.generateCodes('Hello there.', {
            frames: FR, seed: 99, trace: true,
            init: { tokens: c1.codes, keep }, onDone: done }));
    assert(c4.codes.length === NQ * FR, 'init run shape');
    for (let t = 0; t < FR; t++)
        assert(c4.codes[t] === c1.codes[t], 'codebook 0 kept at frame ' + t);
    let diff = 0;
    for (let i = FR; i < NQ * FR; i++) if (c4.codes[i] !== c1.codes[i]) diff++;
    assert(diff > 0, 'the re-rolled codebooks changed (' + diff + ' cells)');
    for (let t = 0; t < FR; t++)
        assert(c4.trace.unmaskStep[t] === -1, 'kept cells report unmaskStep -1');
    assert(c4.trace.unmaskStep.slice(FR).every(s => s >= 0), 're-rolled cells report their step');

    // ── A bad instruct rejects through onError + info.error ──────────────────
    let errMsg = null;
    [out, info] = run('bad instruct', done =>
        omni.synthesize('Hello.', { instruct: 'purple, banana', onError: m => { errMsg = m; }, onDone: done }));
    assert(typeof errMsg === 'string' && errMsg.length > 0, 'onError fired: ' + errMsg);
    assert(info.error === errMsg && info.cancelled === false, 'info.error carries the same message');
    assert(out.samples.length === 0, 'error result is empty');
    // and a valid one, one pick each from two categories, passes validation
    // (short, to keep the run quick)
    const byName = n => attrs.find(a => a.name === n);
    const instruct = byName('gender').values[0] + ', ' + byName('age').values[0];
    [out, info] = run('instruct', done =>
        omni.synthesize('Hello.', { instruct, duration: 1.5, numSteps: 8, onDone: done }));
    assert(!info.error && out.samples.length > 0, 'a valid instruct synthesizes: ' + JSON.stringify(info));

    // ── cancel() mid-synthesis resolves as cancelled, then the model is free ──
    let cancelInfo = null, cancelOut = null;
    // A 20 s target at 64 steps takes seconds; cancelling right away is seen
    // by the first step's poll, so the LM returns an empty buffer.
    const h = omni.synthesize(TEXT + ' ' + TEXT, { duration: 20, numSteps: 64,
                                                   onDone: (r, i) => { cancelOut = r; cancelInfo = i; } });
    let second = null;
    try { omni.synthesize('x', {}); } catch (e) { second = e; }
    assert(second && String(second.message).includes('in flight'), 'second op is rejected: ' + second);
    const tCancel = Date.now();
    h.cancel();
    pumpUntil('cancelled synthesis', () => cancelInfo !== null, 60);
    console.log('cancel resolved after ' + ms(tCancel));
    assert(cancelInfo.cancelled === true, 'cancelled: ' + JSON.stringify(cancelInfo));
    assert(cancelOut.samples.length === 0, 'cancelled result is empty');
    [out, info] = run('after cancel', done =>
        omni.generateCodes('Hi.', { frames: 25, numSteps: 4, onDone: done }));
    assert(!info.error && out.codes.length === NQ * 25, 'the model is usable after a cancel');

    // ── bro.tts.synthesize dispatches on the class ───────────────────────────
    [out, info] = run('bro.tts.synthesize', done =>
        bro.tts.synthesize(omni, 'Hi.', { duration: 1, numSteps: 4, onDone: done }));
    assert(!info.error && out.sampleRate === 24000 && out.samples.length > 0,
           'bro.tts.synthesize(omni, ...) works');

    // ── unload ───────────────────────────────────────────────────────────────
    omni.unload();
    assert(omni.loaded === false, 'unloaded');
    threw = null;
    try { omni.tokenize('x'); } catch (e) { threw = e; }
    assert(threw && String(threw.message).includes('not loaded'), 'methods throw after unload: ' + threw);

    console.log('bro.tts OmniVoice end-to-end OK');
}
