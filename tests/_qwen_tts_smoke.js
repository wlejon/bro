// Smoke test for the Qwen3-TTS bindings: bro.tts.loadQwen (async-load
// convention), the sync QwenTts.synthesize method, the async/cancellable
// bro.tts.synthesize(qwen, text, opts) free function, and speakers().
// Run (GPU — Qwen needs it) against the minimal smoke app:
//   bro-headless tests/_smoke_app tests/_qwen_tts_smoke.js
// Assumes the dev weights are present (a source checkout has them):
//   ../brosoundml/weights/qwen-tts/0.6B-customvoice

// Bound by REAL wall-clock (Date.now() is real in headless), since the async
// jobs run on real background threads. sleep() advances virtual time and drives
// the engine tick (which calls tickAsync to drain results) but returns instantly.
function pumpUntil(pred, budgetMs) {
    const start = Date.now();
    while (!pred() && (Date.now() - start) < budgetMs) { sleep(20); }
    return pred();
}

const QWEN_DIR = '../brosoundml/weights/qwen-tts/0.6B-customvoice';

// ── 1. async-load qwen ───────────────────────────────────────────────────────
let qwen = null, qwenErr = null;
const lh = bro.tts.loadQwen(QWEN_DIR, {
    onReady: (q) => { qwen = q; },
    onError: (m) => { qwenErr = m; },
});
assert(lh && typeof lh.cancel === 'function', 'loadQwen async returns a handle');
assert(pumpUntil(() => qwen || qwenErr, 180000), 'qwen load finished');
assert(!qwenErr, 'qwen load did not error: ' + qwenErr);
assert(qwen.loaded, 'qwen reports loaded');
assert(qwen.sampleRate === 24000, 'qwen sampleRate is 24000');
console.log('[smoke] qwen loaded async: variant=' + qwen.variant +
            ' size=' + qwen.modelSize);

// ── 2. speakers() ────────────────────────────────────────────────────────────
const speakers = qwen.speakers();
assert(Array.isArray(speakers) && speakers.length > 0, 'speakers() lists presets');
const speaker = speakers[0];
console.log('[smoke] speakers (' + speakers.length + '): ' + speakers.join(', '));

// ── 3. sync synthesize (the blocking method) ─────────────────────────────────
const sync = qwen.synthesize('Hello there.', { speaker, language: 'english' });
assert(sync.samples instanceof Float32Array, 'sync synth gave Float32Array samples');
assert(sync.samples.length > 0, 'sync synth produced audio');
assert(sync.sampleRate === 24000, 'sync synth sampleRate is 24000');
console.log('[smoke] sync synth -> ' + sync.samples.length + ' samples');

// ── 4. async synthesize (non-blocking, polymorphic free function) ────────────
let done = null;
const sh = bro.tts.synthesize(qwen, 'Async synthesis works too.', {
    speaker,
    onDone: (result, info) => { done = { result, info }; },
});
assert(sh && typeof sh.cancel === 'function', 'synthesize returns a handle');
assert(pumpUntil(() => done !== null, 120000), 'async synth completed');
assert(!done.info.cancelled, 'async synth not cancelled');
assert(!done.info.error, 'async synth had no error: ' + done.info.error);
assert(done.result.samples instanceof Float32Array, 'async synth gave Float32Array');
assert(done.result.samples.length > 0, 'async synth produced audio');
assert(done.result.sampleRate === 24000, 'async synth sampleRate is 24000');
console.log('[smoke] async synth -> ' + done.result.samples.length + ' samples');

// ── 5. cancellation: a long utterance cancelled at launch returns empty ──────
const longText = ('This is a deliberately long sentence that should take many ' +
    'frames to synthesize so the cancel flag is observed mid-loop. ').repeat(6);
let cdone = null;
const ch = bro.tts.synthesize(qwen, longText, {
    speaker,
    onDone: (result, info) => { cdone = { result, info }; },
});
ch.cancel();   // flip the per-frame cancel flag immediately
assert(pumpUntil(() => cdone !== null, 120000), 'cancelled synth fired onDone');
assert(cdone.info.cancelled, 'cancelled synth reports cancelled=true');
assert(cdone.result.samples.length === 0, 'cancelled synth returns an empty buffer');
console.log('[smoke] cancel -> cancelled=' + cdone.info.cancelled +
            ', samples=' + cdone.result.samples.length);

// ── 6. single-owner guard: a second op while one is in flight throws ─────────
let threw = false;
const bh = bro.tts.synthesize(qwen, 'first one in flight.', { speaker, onDone: () => {} });
try { bro.tts.synthesize(qwen, 'second should be rejected.', { speaker, onDone: () => {} }); }
catch (e) { threw = true; }
assert(threw, 'a concurrent synthesize on the same model is rejected');
bh.cancel();   // release the model
console.log('[smoke] single-owner guard rejects a concurrent op');

console.log('[smoke] PASS');
