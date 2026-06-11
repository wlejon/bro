// Smoke test for bro.stt Qwen3-ASR — async {onReady,onError} loaders, sync
// asr.transcribe (generated ids -> "language <Lang><asr_text>transcript"),
// the encode() latent tap, the QwenAsrStream incremental encoder, and the
// async bro.stt.transcribe(asr, ...) path with onToken streaming.
// Run against the minimal smoke app (avoids loading other models):
//   bro-headless tests/_smoke_app tests/_qwen_asr_smoke.js
// Needs the real checkpoint (brosoundml weights/qwen-asr/0.6B) and the
// Qwen-TTS sample clip ("Hello there, this is a test of the ...").

function pumpUntil(pred, budgetMs) {
    const start = Date.now();
    while (!pred() && (Date.now() - start) < budgetMs) { sleep(20); }
    return pred();
}

const MODEL_DIR = '../brosoundml/weights/qwen-asr/0.6B';
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

// Split the generated stream on the <asr_text> marker id (it detokenizes to
// an empty string, so a text-level split does not work).
function transcriptOf(ids) {
    const arr = Array.from(ids);
    const cut = arr.indexOf(model.asrTextId);
    return {
        language: cut >= 0 ? qtok.decode(arr.slice(0, cut)).trim() : '',
        text:     qtok.decode(cut >= 0 ? arr.slice(cut + 1) : arr).trim(),
    };
}

// ── 1. async-load model; Qwen BPE tokenizer comes from bro.lm ────────────────
let model = null, modelErr = null;
const mh = bro.stt.loadQwenAsr(MODEL_DIR, {
    onReady: (m) => { model = m; },
    onError: (e) => { modelErr = e; },
});
assert(mh && typeof mh.cancel === 'function', 'loadQwenAsr async returns a handle');

const qtok = bro.lm.loadTokenizer({
    vocabPath:  MODEL_DIR + '/vocab.json',
    mergesPath: MODEL_DIR + '/merges.txt',
});
assert(qtok.vocabCount > 150000, 'Qwen tokenizer vocab loaded (' + qtok.vocabCount + ')');

assert(pumpUntil(() => model || modelErr, 300000), 'qwen-asr load finished');
assert(!modelErr, 'qwen-asr load did not error: ' + modelErr);
assert(model.loaded, 'model reports loaded');
assert(model.sampleRate === 16000, 'sampleRate 16000');
assert(model.latentDim === 1024, 'latentDim 1024');
assert(Math.abs(model.latentHz - 12.5) < 1e-6, 'latentHz 12.5');
assert(model.asrTextId === 151704, 'asrTextId 151704');
console.log('[smoke] qwen-asr loaded async');

// ── 2. sync transcribe with sync onToken ─────────────────────────────────────
const audio = readWav16k(WAV);
assert(audio.length > 16000, 'test clip is > 1 s');

const syncTokens = [];
const ids = model.transcribe(audio, { onToken: (id) => syncTokens.push(id) });
assert(ids instanceof Int32Array, 'transcribe returns an Int32Array');
assert(ids.length > 0, 'transcription is non-empty');
assert(syncTokens.length === ids.length, 'sync onToken saw every token');
const sync = transcriptOf(ids);
console.log('[smoke] sync: language="' + sync.language + '" text="' + sync.text + '"');
assert(/english/i.test(sync.language), 'language ID says English');
assert(/hello/i.test(sync.text) && /test/i.test(sync.text),
       'transcript contains the spoken words (got "' + sync.text + '")');

// ── 3. context biasing accepts ids and still transcribes ─────────────────────
const ctxIds = qtok.encode('Hello there, a test of the system');
const ctxOut = transcriptOf(model.transcribe(audio, { contextIds: ctxIds }));
assert(/hello/i.test(ctxOut.text),
       'context-biased transcript still has the words (got "' + ctxOut.text + '")');
console.log('[smoke] context-biased: "' + ctxOut.text + '"');

// ── 4. encode() latent tap ────────────────────────────────────────────────────
const enc = model.encode(audio);
assert(enc.latents instanceof Float32Array, 'latents is a Float32Array');
assert(enc.latentDim === 1024 && Math.abs(enc.latentHz - 12.5) < 1e-6,
       'encode() echoes latent geometry');
assert(enc.frames > 0, 'encode produced latent rows');
assert(enc.latents.length === enc.frames * enc.latentDim, 'latents shape matches');
const expectRows = (audio.length / 16000) * 12.5;
assert(Math.abs(enc.frames - expectRows) < 15,
       'latent rows ~ duration * 12.5 (' + enc.frames + ' vs ~' + expectRows.toFixed(1) + ')');
console.log('[smoke] encode: ' + enc.frames + ' latent rows');

// ── 5. streaming encoder: mic-sized feeds, finalized rows accumulate ─────────
let stream = null, streamErr = null;
bro.stt.loadQwenAsrStream(MODEL_DIR, {
    onReady: (s) => { stream = s; },
    onError: (e) => { streamErr = e; },
});
assert(pumpUntil(() => stream || streamErr, 300000), 'stream encoder load finished');
assert(!streamErr, 'stream load did not error: ' + streamErr);
assert(stream.latentDim === 1024 && stream.blockChunks === 1, 'stream geometry');

let freshTotal = 0, feedCalls = 0, freshCalls = 0;
const CHUNK = 1600;  // 100 ms @ 16 kHz, a typical bro.mic chunk
for (let off = 0; off < audio.length; off += CHUNK) {
    const fresh = stream.feed(audio.subarray(off, Math.min(off + CHUNK, audio.length)));
    feedCalls++;
    if (fresh > 0) {
        freshCalls++;
        const rows = stream.latents(stream.frames - fresh, fresh);
        assert(rows.length === fresh * stream.latentDim, 'incremental slice shape');
    }
    freshTotal += fresh;
}
freshTotal += stream.finish();
assert(freshTotal === stream.frames, 'feed/finish return values sum to frames');
assert(freshCalls > 1, 'multiple blocks finalized across the clip');
assert(Math.abs(stream.frames - enc.frames) <= 8,
       'stream rows ~ one-shot rows (' + stream.frames + ' vs ' + enc.frames + ')');
const allRows = stream.latents();
assert(allRows.length === stream.frames * stream.latentDim, 'latents() full copy shape');
console.log('[smoke] stream: ' + stream.frames + ' rows across ' + freshCalls + ' blocks');

// ── 6. async transcribe with streaming ───────────────────────────────────────
const streamed = [];
let asyncDone = null;
const ah = bro.stt.transcribe(model, audio, {
    onToken: (id) => streamed.push(id),
    onDone:  (resultIds, info) => { asyncDone = { resultIds, info }; },
});
assert(ah && typeof ah.cancel === 'function', 'async transcribe returns a handle');
assert(pumpUntil(() => asyncDone !== null, 300000), 'async transcribe completed');
assert(!asyncDone.info.cancelled, 'not cancelled');
assert(!asyncDone.info.error, 'no error: ' + asyncDone.info.error);
const aIds = asyncDone.resultIds;
assert(aIds instanceof Int32Array && aIds.length > 0, 'async ids non-empty');
assert(streamed.length === aIds.length, 'streamed every token');
for (let i = 0; i < aIds.length; i++)
    assert(streamed[i] === aIds[i], 'streamed order matches result');
const async = transcriptOf(aIds);
assert(/hello/i.test(async.text) && /test/i.test(async.text),
       'async transcript has the words (got "' + async.text + '")');
console.log('[smoke] async transcript: "' + async.text + '" (' + streamed.length + ' tokens)');

// ── 7. busy guard: second op on an in-flight model throws ────────────────────
let busyDone = null;
bro.stt.transcribe(model, audio, { onDone: () => { busyDone = true; } });
let threw = false;
try { bro.stt.transcribe(model, audio, { onDone: () => {} }); }
catch (e) { threw = true; }
assert(threw, 'concurrent transcribe on one model throws');
assert(pumpUntil(() => busyDone !== null, 300000), 'first transcribe still completed');

console.log('[smoke] PASS');
