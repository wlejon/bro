/**
 * bro.stt — Speech-to-text (Whisper, Parakeet)
 *
 * Transcribes 16 kHz mono audio to text. Two model families:
 *   - Whisper: encoder/decoder transformer. Prompted (language/task), 30 s
 *     windows with optional sequential long-form decode.
 *   - Parakeet (NVIDIA Parakeet-TDT-0.6B-v3): FastConformer encoder + TDT
 *     transducer. Unconditional (no prompt), multilingual (25 European
 *     languages), single-pass over the whole clip, and reports per-token
 *     encoder-frame positions for word timestamps. Faster than Whisper at
 *     the same size thanks to TDT frame-skipping.
 *
 * Backed by brosoundml (audio-ML inference) on top of brotensor. Defaults to
 * CUDA; pass { device: 'cpu' } to force the CPU backend.
 *
 * A transcription needs two pieces: the model (loadWhisper / loadParakeet)
 * and its tokenizer (loadTokenizer / loadParakeetTokenizer) for decoding the
 * output ids back to text (and, for Whisper, building the decoder prompt).
 *
 * Audio is supplied as { samples: Float32Array, sampleRate: number } in the
 * [-1, 1] range. Both models expect 16 kHz mono — resample/downmix before
 * calling (see tests/smoke_voice_pipeline.js for a WAV reader that does this).
 */


// ── Load ──────────────────────────────────────────────────────────────────

/**
 * Load a Whisper model from a weights directory (model.safetensors + config).
 *
 * @param {string} dir            - Whisper weights directory.
 * @param {Object} [opts]
 * @param {string} [opts.device='cuda'] - 'cuda' or 'cpu'.
 * @param {function} [opts.onReady]     - async load: onReady(whisper).
 * @param {function} [opts.onError]     - async load: onError(message).
 * @returns {WhisperModel|AsyncHandle}  - the model (sync), or an AsyncHandle
 *          (async, when opts.onReady is a function).
 *
 * Two modes:
 *   - Sync (no onReady): blocks the JS thread until the model is loaded and
 *     returns the WhisperModel.
 *   - Async (onReady is a function): the heavy load (file IO + GPU upload) runs
 *     on a background thread; onReady(whisper) / onError(message) fire later on
 *     the JS thread. Returns an AsyncHandle with .cancel() immediately. Loading
 *     several models this way runs them in parallel.
 */
const whisper = bro.stt.loadWhisper('../brosoundml/weights/whisper');
// whisper.dModel === 384 (tiny)

// Async load:
// bro.stt.loadWhisper('../brosoundml/weights/whisper', {
//     onReady: (w) => { whisper = w; },
//     onError: (msg) => console.error('whisper load failed:', msg),
// });

/**
 * Load the Whisper BPE tokenizer.
 *
 * @param {Object} opts
 * @param {string} opts.vocabPath    - path to vocab.json
 * @param {string} opts.mergesPath   - path to merges.txt
 * @param {function} [opts.onReady]  - async load: onReady(tokenizer).
 * @param {function} [opts.onError]  - async load: onError(message).
 * @returns {WhisperTokenizer|AsyncHandle}  - the tokenizer (sync), or an
 *          AsyncHandle (async, when opts.onReady is a function). Same sync/async
 *          convention as loadWhisper.
 */
const tok = bro.stt.loadTokenizer({
    vocabPath:  '../brosoundml/weights/whisper/vocab.json',
    mergesPath: '../brosoundml/weights/whisper/merges.txt',
});


// ── Tokenizer prompt + decode ───────────────────────────────────────────────

/**
 * WhisperTokenizer
 *
 * @method buildPrompt(lang, task, timestamps) → number[]
 *         Build the decoder start prompt. lang is a language code ('en'),
 *         task is 'transcribe' or 'translate', timestamps is a boolean that
 *         toggles the <|notimestamps|> token.
 *
 * @method decode(ids, skipSpecial) → string
 *         Decode output token ids to text. Pass skipSpecial=true to strip
 *         <|...|> control tokens.
 */
const prompt = tok.buildPrompt('en', 'transcribe', /*timestamps=*/false);


// ── Transcribe ──────────────────────────────────────────────────────────────

/**
 * WhisperModel
 *
 * @property {number} dModel
 *
 * @method transcribe(audio, prompt, opts) → number[]
 *         Run the encoder over `audio` and greedily decode starting from
 *         `prompt`, returning the generated token ids.
 *
 *         audio: { samples: Float32Array, sampleRate: number }  (16 kHz mono)
 *         opts:  { maxNewTokens, timestampBeginId, onToken } — see below.
 *
 * opts.timestampBeginId (= tok.firstTimestampId) turns on Whisper's sequential
 *   long-form decode: when set AND the audio is longer than 30 s, the input is
 *   windowed into 30 s segments and seeked by the last emitted timestamp instead
 *   of being truncated to the first window. Requires a timestamps prompt
 *   (buildPrompt(lang, task, /*withTimestamps=* /true)). Omit (or < 0) for the
 *   legacy single-window behaviour.
 * opts.onToken(id) fires once per decoded token, in order, as it is produced —
 *   detokenize incrementally for a live partial transcript. Runs synchronously
 *   inside transcribe() on this thread; keep it cheap.
 */
const audio = { samples: /* Float32Array */ null, sampleRate: 16000 };
const ids   = whisper.transcribe(audio, prompt, {
    maxNewTokens: 96,
    timestampBeginId: tok.firstTimestampId,   // long-form (>30 s) windowing
    onToken: (id) => { /* incremental decode */ },
});
const text  = tok.decode(ids, /*skipSpecial=*/true);
console.log(text.trim());


// ── Async transcribe (non-blocking) ─────────────────────────────────────────

/**
 * bro.stt.transcribe(whisper, audio, promptIds, opts) → AsyncHandle
 *
 * Runs Whisper's autoregressive decode on a background thread so the JS thread
 * (and the app) stays responsive. opts.onToken(id) streams each decoded token
 * to the JS thread as it is produced (lock-free handoff, drained once per
 * frame). Cancellation (handle.cancel()) is real — the decode loop polls the
 * flag once per token and stops — and drops the result; onDone still fires
 * with { cancelled: true }.
 *
 * @param {WhisperModel} whisper           - from loadWhisper().
 * @param {Float32Array|{samples,sampleRate}} audio - 16 kHz mono; a bare
 *        Float32Array is assumed to be 16 kHz.
 * @param {Int32Array|number[]} promptIds  - decoder prompt (tok.buildPrompt()).
 * @param {Object} [opts]
 * @param {number}   [opts.maxNewTokens=0] - cap on decoded tokens (0 = model max).
 * @param {function} [opts.onToken]        - onToken(id) per decoded token.
 * @param {function} [opts.onDone]         - onDone(ids, info) on the JS thread:
 *        ids  = Int32Array of token ids (includes the prompt prefix).
 *        info = { cancelled: boolean, error?: string }.
 * @returns {AsyncHandle}  - { cancel(): void }. Rejects (throws) if another
 *          transcribe()/op is already in flight on this model.
 */
let lastIds = null;
const handle = bro.stt.transcribe(whisper, audio, prompt, {
    maxNewTokens: 96,
    onDone: (ids, info) => {
        if (info.cancelled) return;           // barge-in: ignore stale result
        if (info.error) { console.error(info.error); return; }
        lastIds = ids;
        console.log(tok.decode(ids, /*skipSpecial=*/true).trim());
    },
});
// handle.cancel();  // e.g. on barge-in — onDone fires with cancelled:true


// ── Parakeet ────────────────────────────────────────────────────────────────

/**
 * Load a Parakeet-TDT model from a weights directory (config.json +
 * model.safetensors — the HF `transformers` checkpoint layout, e.g.
 * nvidia/parakeet-tdt-0.6b-v3 fetched by brosoundml/scripts/download-parakeet.sh).
 *
 * @param {string} dir            - Parakeet weights directory.
 * @param {Object} [opts]
 * @param {string} [opts.device='cuda'] - 'cuda' or 'cpu'.
 * @param {function} [opts.onReady]     - async load: onReady(parakeet).
 * @param {function} [opts.onError]     - async load: onError(message).
 * @returns {ParakeetModel|AsyncHandle} - same sync/async convention as
 *          loadWhisper.
 */
const parakeet = bro.stt.loadParakeet('../brosoundml/weights/parakeet/0.6b-v3');
// parakeet.sampleRate   === 16000
// parakeet.vocabSize    === 8193    (8192 SentencePiece pieces + blank)
// parakeet.frameSeconds === 0.08    (seconds of audio per encoder frame)

/**
 * Load Parakeet's SentencePiece tokenizer (the tokenizer.json beside the
 * checkpoint).
 *
 * @param {string} path              - path to tokenizer.json.
 * @param {Object} [opts]
 * @param {function} [opts.onReady]  - async load: onReady(tokenizer).
 * @param {function} [opts.onError]  - async load: onError(message).
 * @returns {ParakeetTokenizer|AsyncHandle}
 *
 * ParakeetTokenizer:
 * @method decode(ids) → string
 *         Detokenize piece ids to text. Ids outside the vocab (blank/pad) are
 *         skipped, so the raw transcription id stream decodes directly.
 * @method tokenize(text) → Int32Array
 *         Text to unigram piece ids (no eos/padding) — handy for tests.
 * @property {number} vocabCount
 */
const ptok = bro.stt.loadParakeetTokenizer(
    '../brosoundml/weights/parakeet/0.6b-v3/tokenizer.json');

/**
 * ParakeetModel
 *
 * @property {boolean} loaded
 * @property {number}  sampleRate    - 16000 (fixed).
 * @property {number}  vocabSize
 * @property {number}  blankTokenId
 * @property {number}  frameSeconds  - seconds per encoder frame (0.08 for v3);
 *           tokenFrames[i] * frameSeconds = token i's start time.
 *
 * @method transcribe(audio, opts) → { tokenIds, tokenFrames }
 *         Run the full pipeline over `audio` (no prompt — Parakeet is
 *         unconditional). Single pass over the whole clip; no windowing.
 *
 *         audio: Float32Array @ 16 kHz, or { samples, sampleRate } (16 kHz mono)
 *         opts:  { maxNewTokens=0 (0 = whole clip), onToken }
 *         tokenIds:    Int32Array of SentencePiece piece ids (no blank/pad).
 *         tokenFrames: Int32Array — encoder frame each token was emitted at.
 *
 * opts.onToken(id) fires once per emitted token, in order, synchronously on
 *   this thread — detokenize incrementally for a live partial transcript.
 */
const res = parakeet.transcribe(audio);
console.log(ptok.decode(res.tokenIds).trim());
for (let i = 0; i < res.tokenIds.length; i++) {
    const t = res.tokenFrames[i] * parakeet.frameSeconds;
    console.log(t.toFixed(2) + '\t' + ptok.decode([res.tokenIds[i]]));
}

/**
 * bro.stt.transcribe(parakeet, audio, opts) → AsyncHandle
 *
 * Async Parakeet decode on a background thread — same machinery as the
 * Whisper form (the first argument selects the model family; Parakeet takes
 * no promptIds). opts.onToken streams tokens; handle.cancel() is real (the
 * TDT loop polls once per encoder frame).
 *
 * @param {ParakeetModel} parakeet         - from loadParakeet().
 * @param {Float32Array|{samples,sampleRate}} audio - 16 kHz mono.
 * @param {Object} [opts]
 * @param {number}   [opts.maxNewTokens=0] - cap on emitted tokens (0 = whole clip).
 * @param {function} [opts.onToken]        - onToken(id) per emitted token.
 * @param {function} [opts.onDone]         - onDone(result, info) on the JS thread:
 *        result = { tokenIds: Int32Array, tokenFrames: Int32Array }.
 *        info   = { cancelled: boolean, error?: string }.
 * @returns {AsyncHandle}  - { cancel(): void }. Rejects (throws) if another
 *          transcribe() is already in flight on this model.
 */
bro.stt.transcribe(parakeet, audio, {
    onToken: (id) => { /* live partial transcript */ },
    onDone: (result, info) => {
        if (info.cancelled || info.error) return;
        console.log(ptok.decode(result.tokenIds).trim());
    },
});
