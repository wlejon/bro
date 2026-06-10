// Smoke test for bro.stt Parakeet — async {onReady,onError} loaders, sync
// parakeet.transcribe (token ids + frame timestamps), and the async
// bro.stt.transcribe(parakeet, ...) path with onToken streaming.
// Run against the minimal smoke app (avoids loading other models):
//   bro-headless tests/_smoke_app tests/_parakeet_smoke.js
// Needs the real checkpoint (brosoundml/scripts/download-parakeet.sh) and the
// Qwen-TTS sample clip ("Hello there, this is a test of the ...").

function pumpUntil(pred, budgetMs) {
    const start = Date.now();
    while (!pred() && (Date.now() - start) < budgetMs) { sleep(20); }
    return pred();
}

const MODEL_DIR = '../brosoundml/weights/parakeet/0.6b-v3';
const WAV       = '../brosoundml/weights/qwen-tts-hello-there-this-is-a-test-of-th.wav';

// ── WAV reader: 16-bit PCM mono -> Float32, linear-resampled to 16 kHz ──────
function readWav16k(path) {
    const fs = require('fs');
    const buf = fs.readFileSync(path);
    const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
    assert(dv.getUint32(0, false) === 0x52494646, 'RIFF magic');
    const channels = dv.getUint16(22, true);
    const rate     = dv.getUint32(24, true);
    const bits     = dv.getUint16(34, true);
    assert(bits === 16, 'expected 16-bit PCM, got ' + bits);
    // Find the data chunk (fmt may be followed by other chunks).
    let off = 12;
    while (off + 8 <= dv.byteLength) {
        const id = dv.getUint32(off, false), sz = dv.getUint32(off + 4, true);
        if (id === 0x64617461) {  // 'data'
            const n = Math.floor(sz / 2 / channels);
            const mono = new Float32Array(n);
            for (let i = 0; i < n; i++) {
                let s = 0;
                for (let c = 0; c < channels; c++)
                    s += dv.getInt16(off + 8 + (i * channels + c) * 2, true);
                mono[i] = (s / channels) / 32768;
            }
            if (rate === 16000) return mono;
            const ratio = 16000 / rate, m = Math.floor(n * ratio);
            const out = new Float32Array(m);
            for (let i = 0; i < m; i++) {
                const t = i / ratio, j = t | 0, f = t - j;
                out[i] = mono[j] * (1 - f) + (mono[j + 1] !== undefined ? mono[j + 1] : mono[j]) * f;
            }
            return out;
        }
        off += 8 + sz + (sz & 1);
    }
    throw new Error('no data chunk in ' + path);
}

// ── 1. async-load model + tokenizer (parallel) ───────────────────────────────
let model = null, modelErr = null;
const mh = bro.stt.loadParakeet(MODEL_DIR, {
    onReady: (m) => { model = m; },
    onError: (e) => { modelErr = e; },
});
assert(mh && typeof mh.cancel === 'function', 'loadParakeet async returns a handle');

let tok = null, tokErr = null;
const th = bro.stt.loadParakeetTokenizer(MODEL_DIR + '/tokenizer.json', {
    onReady: (t) => { tok = t; },
    onError: (e) => { tokErr = e; },
});
assert(th && typeof th.cancel === 'function', 'loadParakeetTokenizer async returns a handle');

assert(pumpUntil(() => model || modelErr, 300000), 'parakeet load finished');
assert(!modelErr, 'parakeet load did not error: ' + modelErr);
assert(pumpUntil(() => tok || tokErr, 60000), 'tokenizer load finished');
assert(!tokErr, 'tokenizer load did not error: ' + tokErr);
assert(model.loaded, 'model reports loaded');
assert(model.sampleRate === 16000, 'sampleRate 16000');
assert(model.vocabSize === 8193, 'vocabSize 8193');
assert(Math.abs(model.frameSeconds - 0.08) < 1e-9, 'frameSeconds 0.08');
assert(tok.vocabCount > 8000, 'tokenizer vocab loaded (' + tok.vocabCount + ')');
console.log('[smoke] parakeet + tokenizer loaded async');

// ── 2. sync transcribe with timestamps + sync onToken ───────────────────────
const audio = readWav16k(WAV);
assert(audio.length > 16000, 'test clip is > 1 s');

const syncTokens = [];
const res = model.transcribe(audio, { onToken: (id) => syncTokens.push(id) });
assert(res.tokenIds instanceof Int32Array, 'tokenIds is an Int32Array');
assert(res.tokenFrames instanceof Int32Array, 'tokenFrames is an Int32Array');
assert(res.tokenIds.length > 0, 'transcription is non-empty');
assert(res.tokenFrames.length === res.tokenIds.length, 'one frame per token');
assert(syncTokens.length === res.tokenIds.length, 'sync onToken saw every token');
for (let i = 1; i < res.tokenFrames.length; i++)
    assert(res.tokenFrames[i] >= res.tokenFrames[i - 1], 'frames non-decreasing');
const text = tok.decode(res.tokenIds).trim();
console.log('[smoke] sync transcript: "' + text + '"');
assert(/hello/i.test(text) && /test/i.test(text),
       'transcript contains the spoken words (got "' + text + '")');
const lastT = res.tokenFrames[res.tokenFrames.length - 1] * model.frameSeconds;
assert(lastT > 0 && lastT <= audio.length / 16000 + 0.5,
       'last token timestamp within clip duration (' + lastT.toFixed(2) + ' s)');

// ── 3. async transcribe with streaming ───────────────────────────────────────
const streamed = [];
let asyncDone = null;
const ah = bro.stt.transcribe(model, audio, {
    onToken: (id) => streamed.push(id),
    onDone:  (result, info) => { asyncDone = { result, info }; },
});
assert(ah && typeof ah.cancel === 'function', 'async transcribe returns a handle');
assert(pumpUntil(() => asyncDone !== null, 300000), 'async transcribe completed');
assert(!asyncDone.info.cancelled, 'not cancelled');
assert(!asyncDone.info.error, 'no error: ' + asyncDone.info.error);
const aIds = asyncDone.result.tokenIds;
assert(aIds instanceof Int32Array && aIds.length === res.tokenIds.length,
       'async ids match sync length');
assert(streamed.length === aIds.length, 'streamed every token');
for (let i = 0; i < aIds.length; i++)
    assert(streamed[i] === aIds[i], 'streamed order matches result');
assert(tok.decode(Array.from(aIds)).trim() === text, 'async transcript matches sync');
console.log('[smoke] async transcript matches, ' + streamed.length + ' tokens streamed');

// ── 4. busy guard: second op on an in-flight model throws ────────────────────
let busyDone = null;
bro.stt.transcribe(model, audio, { onDone: () => { busyDone = true; } });
let threw = false;
try { bro.stt.transcribe(model, audio, { onDone: () => {} }); }
catch (e) { threw = true; }
assert(threw, 'concurrent transcribe on one model throws');
assert(pumpUntil(() => busyDone !== null, 300000), 'first transcribe still completed');

console.log('[smoke] PASS');
