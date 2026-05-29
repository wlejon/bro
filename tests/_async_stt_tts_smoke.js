// Smoke test for async, cancellable bro.stt.transcribe + bro.tts.synthesize,
// plus the {onReady,onError} async-load convention on their loaders.
// Run against the minimal smoke app (avoids loading other models):
//   bro-headless tests/_smoke_app tests/_async_stt_tts_smoke.js
// Model/asset paths mirror broworkshop/ai/voice-pipeline/voice-worker.js.

// Bound by REAL wall-clock (Date.now() is real in headless), since the async
// jobs run on real background threads. sleep() advances virtual time and drives
// the engine tick (which calls tickAsync to drain results) but returns instantly,
// so the loop spins for real seconds, pumping delivery while the bg thread works.
function pumpUntil(pred, budgetMs) {
    const start = Date.now();
    while (!pred() && (Date.now() - start) < budgetMs) { sleep(20); }
    return pred();
}

const WHISPER_DIR = '../brosoundml/weights/whisper';
const VOCAB       = '../brosoundml/weights/whisper/vocab.json';
const MERGES      = '../brosoundml/weights/whisper/merges.txt';
const KOKORO_DIR  = '../brosoundml/weights/kokoro';
const VOICE_PATH  = '../brosoundml/weights/kokoro/voices/af_heart.bin';

// ── 1. async-load whisper + tokenizer (parallel) ─────────────────────────────
let whisper = null, whisperErr = null;
const wh = bro.stt.loadWhisper(WHISPER_DIR, {
    onReady: (w) => { whisper = w; },
    onError: (m) => { whisperErr = m; },
});
assert(wh && typeof wh.cancel === 'function', 'loadWhisper async returns a handle');

let stok = null, stokErr = null;
const th = bro.stt.loadTokenizer({
    vocabPath: VOCAB, mergesPath: MERGES,
    onReady: (t) => { stok = t; },
    onError: (m) => { stokErr = m; },
});
assert(th && typeof th.cancel === 'function', 'loadTokenizer async returns a handle');

assert(pumpUntil(() => whisper || whisperErr, 180000), 'whisper load finished');
assert(!whisperErr, 'whisper load did not error: ' + whisperErr);
assert(pumpUntil(() => stok || stokErr, 60000), 'tokenizer load finished');
assert(!stokErr, 'tokenizer load did not error: ' + stokErr);
console.log('[smoke] whisper + tokenizer loaded async');

// ── 2. async-load kokoro + voice ─────────────────────────────────────────────
bro.tts.setAssetRoot('../brosoundml');
let kokoro = null, kokoroErr = null;
const kh = bro.tts.loadKokoro(KOKORO_DIR, {
    onReady: (k) => { kokoro = k; },
    onError: (m) => { kokoroErr = m; },
});
assert(kh && typeof kh.cancel === 'function', 'loadKokoro async returns a handle');
assert(pumpUntil(() => kokoro || kokoroErr, 180000), 'kokoro load finished');
assert(!kokoroErr, 'kokoro load did not error: ' + kokoroErr);

let voice = null, voiceErr = null;
const vh = kokoro.loadVoice(VOICE_PATH, {
    onReady: (v) => { voice = v; },
    onError: (m) => { voiceErr = m; },
});
assert(vh && typeof vh.cancel === 'function', 'loadVoice async returns a handle');
assert(pumpUntil(() => voice || voiceErr, 60000), 'voice load finished');
assert(!voiceErr, 'voice load did not error: ' + voiceErr);
console.log('[smoke] kokoro + voice loaded async');

// ── 3. async STT transcription ───────────────────────────────────────────────
const promptIds = stok.buildPrompt('en', 'transcribe', false);
assert(promptIds.length > 0, 'buildPrompt yielded a non-empty prompt');

// 1 second of 16 kHz audio (a quiet sine — content is irrelevant to the smoke).
const audio = new Float32Array(16000);
for (let i = 0; i < audio.length; ++i)
    audio[i] = 0.01 * Math.sin(2 * Math.PI * 220 * i / 16000);

let sttDone = null;
const sh = bro.stt.transcribe(whisper, audio, promptIds, {
    maxNewTokens: 32,
    onDone: (ids, info) => { sttDone = { ids, info }; },
});
assert(sh && typeof sh.cancel === 'function', 'transcribe returns a handle');
assert(pumpUntil(() => sttDone !== null, 120000), 'transcribe completed');
assert(!sttDone.info.cancelled, 'transcribe not cancelled');
assert(!sttDone.info.error, 'transcribe had no error: ' + sttDone.info.error);
assert(sttDone.ids instanceof Int32Array, 'transcribe onDone gave an Int32Array');
assert(sttDone.ids.length >= promptIds.length,
       'transcribe ids include the prompt prefix (' + sttDone.ids.length +
       ' >= ' + promptIds.length + ')');
console.log('[smoke] stt transcribe -> ' + sttDone.ids.length + ' ids');

// ── 4. async TTS synthesis ───────────────────────────────────────────────────
const phonemeIds = bro.tts.phonemize('hello there');
assert(phonemeIds.length > 0, 'phonemize yielded phoneme ids');

let ttsDone = null;
const tsh = bro.tts.synthesize(kokoro, phonemeIds, voice, {
    onDone: (result, info) => { ttsDone = { result, info }; },
});
assert(tsh && typeof tsh.cancel === 'function', 'synthesize returns a handle');
assert(pumpUntil(() => ttsDone !== null, 120000), 'synthesize completed');
assert(!ttsDone.info.cancelled, 'synthesize not cancelled');
assert(!ttsDone.info.error, 'synthesize had no error: ' + ttsDone.info.error);
assert(ttsDone.result.samples instanceof Float32Array, 'synthesize gave Float32Array samples');
assert(ttsDone.result.samples.length > 0, 'synthesize produced audio samples');
assert(ttsDone.result.sampleRate === 24000, 'synthesize sampleRate is 24000');
assert(ttsDone.result.durations instanceof Int32Array, 'synthesize gave Int32Array durations');
assert(ttsDone.result.durations.length === phonemeIds.length + 2,
       'durations length === phonemeIds + 2 (got ' + ttsDone.result.durations.length +
       ', expected ' + (phonemeIds.length + 2) + ')');
console.log('[smoke] tts synthesize -> ' + ttsDone.result.samples.length +
            ' samples, ' + ttsDone.result.durations.length + ' durations');

console.log('[smoke] PASS');
