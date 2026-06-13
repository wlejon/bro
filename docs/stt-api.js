/**
 * bro.stt — Speech-to-text (Whisper, Parakeet, Qwen3-ASR)
 *
 * Transcribes 16 kHz mono audio to text. Three model families:
 *   - Whisper: encoder/decoder transformer. Prompted (language/task), 30 s
 *     windows with optional sequential long-form decode.
 *   - Parakeet (NVIDIA Parakeet-TDT-0.6B-v3): FastConformer encoder + TDT
 *     transducer. Unconditional (no prompt), multilingual (25 European
 *     languages), single-pass over the whole clip, and reports per-token
 *     encoder-frame positions for word timestamps. Faster than Whisper at
 *     the same size thanks to TDT frame-skipping.
 *   - Qwen3-ASR (Qwen/Qwen3-ASR-0.6B / -1.7B): AuT audio encoder + Qwen3
 *     text decoder. Unconditional, 52 languages + language ID, optional
 *     context biasing (names / domain terms), and an encoder-only streaming
 *     latent tap (loadQwenAsrStream) for incremental mic-feed pipelines.
 *
 * Backed by brosoundml (audio-ML inference) on top of brotensor. Defaults to
 * CUDA; pass { device: 'cpu' } to force the CPU backend.
 *
 * A transcription needs two pieces: the model (loadWhisper / loadParakeet /
 * loadQwenAsr) and its tokenizer for decoding the output ids back to text
 * (and, for Whisper, building the decoder prompt). Whisper and Parakeet have
 * dedicated tokenizer loaders (loadTokenizer / loadParakeetTokenizer);
 * Qwen3-ASR uses the Qwen BPE tokenizer already bound as bro.lm.loadTokenizer
 * (vocab.json + merges.txt sit in the model dir).
 *
 * Audio is supplied as { samples: Float32Array, sampleRate: number } in the
 * [-1, 1] range. All models expect 16 kHz mono — resample/downmix before
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


// ── Qwen3-ASR ───────────────────────────────────────────────────────────────

/**
 * Load a Qwen3-ASR model from a weights directory (config.json +
 * model.safetensors — the HF checkpoint layout, e.g. Qwen/Qwen3-ASR-0.6B).
 *
 * @param {string} dir            - Qwen3-ASR weights directory.
 * @param {Object} [opts]
 * @param {string} [opts.device='cuda'] - 'cuda' or 'cpu'.
 * @param {function} [opts.onReady]     - async load: onReady(asr).
 * @param {function} [opts.onError]     - async load: onError(message).
 * @returns {QwenAsrModel|AsyncHandle}  - same sync/async convention as
 *          loadWhisper.
 */
const asr = bro.stt.loadQwenAsr('../brosoundml/weights/qwen-asr/0.6B');
// asr.sampleRate === 16000
// asr.latentDim  === 1024   (decoder hidden width)
// asr.latentHz   === 12.5   (encoder latents per second)

// The Qwen BPE tokenizer files live in the model dir; bro.lm's tokenizer
// loader reads them. encode() also produces contextIds for biasing.
const qtok = bro.lm.loadTokenizer({
    vocabPath:  '../brosoundml/weights/qwen-asr/0.6B/vocab.json',
    mergesPath: '../brosoundml/weights/qwen-asr/0.6B/merges.txt',
});

/**
 * QwenAsrModel
 *
 * @property {boolean} loaded
 * @property {number}  sampleRate - 16000 (fixed).
 * @property {number}  latentDim  - encoder latent width (= decoder hidden).
 * @property {number}  latentHz   - encoder latents per second (12.5).
 * @property {number}  vocabSize
 * @property {number}  asrTextId  - the <asr_text> marker id (151704).
 *
 * @method transcribe(audio, opts) → Int32Array
 *         Run the full pipeline (mel → AuT encoder → autoregressive Qwen3
 *         decode) and return the GENERATED token ids only. The stream is the
 *         model's native "language <Language><asr_text>transcript" format —
 *         split the ID STREAM on asrTextId, then decode each side with the
 *         Qwen tokenizer. (The marker detokenizes to an empty string, so a
 *         text-level split does not work.)
 *
 *         audio: Float32Array @ 16 kHz, or { samples, sampleRate } (16 kHz mono)
 *         opts:  { maxNewTokens=0 (0 = 1024), contextIds, onToken }
 *
 * opts.contextIds (Int32Array from qtok.encode(text)) biases recognition
 *   toward names / domain terms — the ids land in the chat template's system
 *   block.
 * opts.onToken(id) fires once per decoded token, in order, synchronously on
 *   this thread — detokenize incrementally for a live partial transcript.
 *
 * @method encode(audio) → { latents, frames, latentDim, latentHz }
 *         Latent tap: AuT encoder + projector only (no decoder). `latents` is
 *         a row-major (frames, latentDim) Float32Array on the host — the rows
 *         transcribe() splices over the <|audio_pad|> block. For bridge
 *         pipelines that drive a separate decoder.
 */
const asrIds = Array.from(
    asr.transcribe(audio, { contextIds: qtok.encode('Jonny Brannum') }));
const cut = asrIds.indexOf(asr.asrTextId);
console.log(qtok.decode(asrIds.slice(cut + 1)).trim());   // transcript
console.log(qtok.decode(asrIds.slice(0, cut)).trim());    // "language English"

/**
 * bro.stt.transcribe(asr, audio, opts) → AsyncHandle
 *
 * Async Qwen3-ASR decode on a background thread — same machinery as the
 * Whisper/Parakeet forms (the first argument selects the model family; Qwen3-ASR
 * takes no promptIds). opts.onToken streams tokens; handle.cancel() is real
 * (the greedy loop polls once per token).
 *
 * @param {QwenAsrModel} asr               - from loadQwenAsr().
 * @param {Float32Array|{samples,sampleRate}} audio - 16 kHz mono.
 * @param {Object} [opts]
 * @param {number}   [opts.maxNewTokens=0] - cap on decoded tokens (0 = 1024).
 * @param {Int32Array|number[]} [opts.contextIds] - context biasing ids.
 * @param {function} [opts.onToken]        - onToken(id) per decoded token.
 * @param {function} [opts.onDone]         - onDone(ids, info) on the JS thread:
 *        ids  = Int32Array of generated token ids.
 *        info = { cancelled: boolean, error?: string }.
 * @returns {AsyncHandle}  - { cancel(): void }. Rejects (throws) if another
 *          transcribe() is already in flight on this model.
 */
bro.stt.transcribe(asr, audio, {
    onDone: (ids, info) => {
        if (info.cancelled || info.error) return;
        const a = Array.from(ids);
        console.log(qtok.decode(a.slice(a.indexOf(asr.asrTextId) + 1)).trim());
    },
});

/**
 * Load the encoder-only streaming tap (no decoder weights). Feed mic chunks;
 * latent rows finalize per block (~blockChunks seconds) and never change —
 * the encoder's attention is windowed within a block, so nothing is
 * re-encoded. A single-block clip streams bit-identically to asr.encode().
 *
 * @param {string} dir            - same Qwen3-ASR weights directory.
 * @param {Object} [opts]
 * @param {number} [opts.blockChunks=1] - block size in ~1 s conv-chunks
 *        (clamped to the model's attention-window cap; 1 = lowest latency,
 *        ~13 latents per block).
 * @param {string} [opts.device='cuda'] - 'cuda' or 'cpu'.
 * @param {function} [opts.onReady]     - async load: onReady(stream).
 * @param {function} [opts.onError]     - async load: onError(message).
 * @returns {QwenAsrStream|AsyncHandle}
 *
 * QwenAsrStream:
 * @property {boolean} loaded
 * @property {number}  sampleRate  - 16000 (fixed).
 * @property {number}  latentDim
 * @property {number}  latentHz
 * @property {number}  frames      - total finalized latent rows so far.
 * @property {number}  blockChunks - block size in conv-chunks.
 * @property {number}  blockFrames - mel frames per full block.
 *
 * @method feed(samples) → number
 *         Feed mono 16 kHz Float32Array samples (e.g. a bro.mic chunk).
 *         Returns the number of newly finalized latent rows (0 if no block
 *         boundary was crossed).
 * @method finish() → number
 *         Flush the trailing partial block as a final, shorter block.
 * @method latents(startFrame=0, count=rest) → Float32Array
 *         Copy of finalized rows [startFrame, startFrame+count), row-major
 *         (count, latentDim).
 */
const stream = bro.stt.loadQwenAsrStream('../brosoundml/weights/qwen-asr/0.6B');
bro.mic.start({ chunkFrames: 160, onChunk: (chunk) => {
    const fresh = stream.feed(chunk.samples);
    if (fresh > 0) {
        const rows = stream.latents(stream.frames - fresh, fresh);
        // hand `rows` to a downstream decoder bridge
    }
}});

/**
 * ── Multi-stream sessions (load-once weights, N decode streams) ──────────────
 *
 * model.createSession() turns one loaded model into N independent transcription
 * streams over ONE shared weight set — the STT analog of N wake detectors on a
 * single shared net. Each session owns its own decode state (Whisper/QwenAsr KV
 * cache; Parakeet TDT prediction state); the immutable weights stay read-only in
 * the model. Use it to transcribe several mic / system / per-NPC streams without
 * copying the weights once per stream.
 *
 * Concurrency: SERIALIZED, INDEPENDENT STATE. Every inference over one model —
 * the module-level bro.stt.transcribe(model, ...) AND each session.transcribe()
 * — shares a single in-flight gate, because the GPU runs one stream and the
 * captured decoder step-graph is shared across sessions of a model. A second
 * call while one is in flight throws ("an operation is already in flight on this
 * model"); drive the streams from one worker / queue (run them back-to-back, or
 * await each onDone before starting the next). Sessions isolate STATE, not
 * parallel execution — but a session's transcript is bit-identical to the same
 * call on a fresh model, so interleaving streams never cross-talks.
 *
 * The model handle may be dropped while a session is still alive — the session
 * keeps the weights (and the shared gate) alive on its own.
 *
 * @method Whisper#createSession() → WhisperSession
 * @method Parakeet#createSession() → ParakeetSession
 * @method QwenAsr#createSession() → QwenAsrSession
 *
 * Each session is driven with the SAME async dispatch as bro.stt.transcribe:
 * @method WhisperSession#transcribe(audio, promptIds, opts?) → AsyncHandle
 *         opts: { maxNewTokens, timestampBeginId, onToken(id), onDone(ids, info) }.
 * @method ParakeetSession#transcribe(audio, opts?) → AsyncHandle
 *         opts: { maxNewTokens, onToken(id), onDone({tokenIds,tokenFrames}, info) }.
 * @method QwenAsrSession#transcribe(audio, opts?) → AsyncHandle
 *         opts: { maxNewTokens, contextIds, onToken(id), onDone(ids, info) }.
 *         AsyncHandle.cancel() aborts the in-flight decode; info = { cancelled,
 *         error? }. The session methods are async only (the streaming path).
 * @method *Session#reset()
 *         Clear the session's decode state for a fresh, unrelated clip. Throws
 *         if a transcribe is in flight on the model.
 * @property *Session.loaded {boolean}
 */
// Two NPC mics transcribed over ONE Parakeet weight set, driven serially:
const parakeet = bro.stt.loadParakeet('../brosoundml/weights/parakeet');
const tok      = bro.stt.loadParakeetTokenizer('../brosoundml/weights/parakeet/tokenizer.json');
const sessionA = parakeet.createSession();   // stream A's TDT state
const sessionB = parakeet.createSession();   // stream B's TDT state — independent
sessionA.transcribe(clipA, { onDone(a) {
    console.log('A:', tok.decode(Array.from(a.tokenIds)));
    // start B only after A finishes (one model = one in-flight op)
    sessionB.transcribe(clipB, { onDone(b) {
        console.log('B:', tok.decode(Array.from(b.tokenIds)));
    }});
}});
