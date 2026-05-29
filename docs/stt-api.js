/**
 * bro.stt — Speech-to-text (Whisper)
 *
 * Transcribes 16 kHz mono audio to text with a Whisper encoder/decoder.
 * Backed by brosoundml (audio-ML inference) on top of brotensor. Defaults to
 * CUDA; pass { device: 'cpu' } to force the CPU backend.
 *
 * A transcription needs two pieces: the Whisper model (loadWhisper) and a
 * BPE tokenizer (loadTokenizer) for building the decoder prompt and decoding
 * the output ids back to text.
 *
 * Audio is supplied as { samples: Float32Array, sampleRate: number } in the
 * [-1, 1] range. Whisper expects 16 kHz mono — resample/downmix before
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
 *         opts:  { maxNewTokens } — cap on decoded tokens.
 */
const audio = { samples: /* Float32Array */ null, sampleRate: 16000 };
const ids   = whisper.transcribe(audio, prompt, { maxNewTokens: 96 });
const text  = tok.decode(ids, /*skipSpecial=*/true);
console.log(text.trim());


// ── Async transcribe (non-blocking) ─────────────────────────────────────────

/**
 * bro.stt.transcribe(whisper, audio, promptIds, opts) → AsyncHandle
 *
 * Runs Whisper's autoregressive decode on a background thread so the JS thread
 * (and the app) stays responsive. STT runs once per turn (pre-reply), so this
 * is a MONOLITHIC op: there is no per-token streaming, and the decode loop is
 * internal to brosoundml. Cancellation (handle.cancel()) drops the result —
 * onDone still fires with { cancelled: true }.
 *
 * @param {WhisperModel} whisper           - from loadWhisper().
 * @param {Float32Array|{samples,sampleRate}} audio - 16 kHz mono; a bare
 *        Float32Array is assumed to be 16 kHz.
 * @param {Int32Array|number[]} promptIds  - decoder prompt (tok.buildPrompt()).
 * @param {Object} [opts]
 * @param {number}   [opts.maxNewTokens=0] - cap on decoded tokens (0 = model max).
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
