/**
 * bro.tts — Text-to-speech (Kokoro + Qwen3-TTS)
 *
 * Synthesizes 24 kHz mono speech. Backed by brosoundml (audio-ML inference) on
 * top of brotensor. Defaults to CUDA; pass { device: 'cpu' } to force the CPU
 * backend. Two pipelines are exposed:
 *
 *   Kokoro (loadKokoro) — the 82M phoneme-driven pipeline. It takes phoneme
 *   ids, not raw text: use bro.tts.phonemize(text) to convert a string into the
 *   id sequence Kokoro expects, then synthesize() with a loaded voice. (You can
 *   also feed a known-good id sequence directly — see
 *   tests/smoke_voice_pipeline.js, which reads one from the model's ids.txt.)
 *
 *   Qwen3-TTS (loadQwen) — the 12 Hz multi-codebook model. Text-driven
 *   end-to-end: no phonemize() step, no voice pack. Two variants, picked by the
 *   checkpoint you load:
 *     • CustomVoice — choose a preset speaker by name (opts.speaker).
 *     • VoiceDesign — describe the voice in natural language (opts.instruct),
 *       e.g. "a warm, low-pitched elderly storyteller". No presets.
 *   synthesize() takes the raw string either way. See the "Qwen3-TTS" section at
 *   the bottom of this file.
 *
 * bro.tts.synthesize(model, ...) is the async, cancellable entry point for both
 * — it dispatches on the model type (Kokoro vs QwenTts).
 */


// ── Phonemize (text → phoneme ids) ──────────────────────────────────────────

/**
 * Convert text into the Kokoro phoneme-id sequence.
 *
 * @param {string} text    - input text (English).
 * @param {Object} [opts]  - reserved (opts.lang is parsed but not yet acted on).
 * @returns {Int32Array}   - phoneme ids ready to pass to synthesize().
 */
const phonemeIds = bro.tts.phonemize('Hello, Bro.');

/**
 * Override the brosoundml repo root the phonemizer derives its assets from.
 * The g2p data root is assumed at <dir>/../brosoundml-data and the Kokoro
 * config at <dir>/weights/kokoro/config.json. Optional — defaults resolve
 * against well-known sibling paths. Clears any prior setAssets() override.
 *
 * @param {string} dir
 */
// bro.tts.setAssetRoot('../brosoundml');

/**
 * Set explicit phonemizer asset paths — use this to load from a flat per-user
 * cache that doesn't follow the dev sibling layout. Any explicit file path
 * overrides the root-derived default; omitted keys are left unchanged. Takes
 * precedence over setAssetRoot(). Resets cached state so the next phonemize()
 * rebuilds.
 *
 * @param {Object} opts
 * @param {string} [opts.root]         - sibling-layout base for any asset not given explicitly.
 * @param {string} [opts.lexicon]      - path to g2p lexicon_en_us.bin.
 * @param {string} [opts.posTagger]    - path to the POS-tagger model.bin.
 * @param {string} [opts.kokoroConfig] - path to the Kokoro config.json (phoneme vocab).
 */
// bro.tts.setAssets({
//   lexicon:      cacheDir + '/datasets/wlejon/brosoundml-data/g2p/lexicon_en_us.bin',
//   posTagger:    cacheDir + '/datasets/wlejon/brosoundml-data/pos_tagger/model.bin',
//   kokoroConfig: kokoroDir + '/config.json',
// });


// ── Load model + voice ──────────────────────────────────────────────────────

/**
 * Load a Kokoro model from a weights directory.
 *
 * @param {string} dir            - Kokoro weights directory.
 * @param {Object} [opts]
 * @param {string} [opts.device='cuda'] - 'cuda' or 'cpu'.
 * @param {function} [opts.onReady]     - async load: onReady(kokoro).
 * @param {function} [opts.onError]     - async load: onError(message).
 * @returns {KokoroModel|AsyncHandle}   - the model (sync), or an AsyncHandle
 *          (async, when opts.onReady is a function).
 *
 * Two modes:
 *   - Sync (no onReady): blocks the JS thread until loaded, returns KokoroModel.
 *   - Async (onReady is a function): the heavy load runs on a background thread;
 *     onReady(kokoro) / onError(message) fire later on the JS thread. Returns an
 *     AsyncHandle with .cancel() immediately.
 */
const kokoro = bro.tts.loadKokoro('../brosoundml/weights/kokoro');
// kokoro.nTokens === 178, kokoro.styleDim === 128

// Async load:
// bro.tts.loadKokoro('../brosoundml/weights/kokoro', {
//     onReady: (k) => { kokoro = k; },
//     onError: (msg) => console.error('kokoro load failed:', msg),
// });

/**
 * Load a voice embedding (.bin) for this model.
 *
 * @param {string} path             - voice file, e.g. voices/af_heart.bin
 * @param {Object} [opts]
 * @param {function} [opts.onReady] - async load: onReady(voice).
 * @param {function} [opts.onError] - async load: onError(message).
 * @returns {Voice|AsyncHandle}     - the voice (sync), or an AsyncHandle (async,
 *          when opts.onReady is a function). Same sync/async convention as
 *          loadKokoro; the lighter load also offered async for uniformity.
 */
const voice = kokoro.loadVoice('../brosoundml/weights/kokoro/voices/af_heart.bin');


// ── Synthesize ──────────────────────────────────────────────────────────────

/**
 * KokoroModel.synthesize(phonemeIds, voice, opts) → { samples, sampleRate, durations }
 *
 * @param {Int32Array|number[]} phonemeIds - from phonemize() (or a raw id list).
 * @param {Voice}  voice                   - from loadVoice().
 * @param {Object} [opts]
 * @param {number} [opts.speed=1.0]        - speaking-rate multiplier.
 * @returns {{ samples: Float32Array, sampleRate: number, durations: Int32Array }}
 *          samples: 24 kHz mono, [-1, 1].
 *          durations: per-phoneme frame counts, length = phonemeIds.length + 2
 *          (Kokoro wraps the input as [BOS, ...ids, EOS]). Use these to recover
 *          per-phoneme / per-word playback timing — see below.
 */
const out = kokoro.synthesize(phonemeIds, voice, { speed: 1.0 });
console.log(`${out.samples.length} samples @ ${out.sampleRate} Hz`);
// Write out.samples to a WAV, or feed into an AudioContext buffer for playback.


// ── Async synthesize (non-blocking) ─────────────────────────────────────────

/**
 * bro.tts.synthesize(kokoro, phonemeIds, voice, opts) → AsyncHandle
 *
 * Runs Kokoro's forward pass on a background thread so the JS thread stays
 * responsive. Synthesis is a single MONOLITHIC forward (no internal loop
 * exposed), so there is no per-step streaming; cancellation (handle.cancel())
 * drops the result — onDone still fires with { cancelled: true }.
 *
 * @param {KokoroModel} kokoro              - from loadKokoro().
 * @param {Int32Array|number[]} phonemeIds  - from phonemize() (or a raw id list).
 * @param {Voice}  voice                    - from loadVoice().
 * @param {Object} [opts]
 * @param {number}   [opts.speed=1.0]       - speaking-rate multiplier.
 * @param {boolean}  [opts.trace=false]     - also capture the pipeline trace.
 *        When true, result additionally carries `stages` — the same
 *        [{ name, h, w, data }] array the synchronous synthesizeTraced() returns
 *        — built on the background thread, so you get the visualization tensors
 *        without blocking the JS thread. Omitted on a cancelled/errored result.
 * @param {function} [opts.onDone]          - onDone(result, info) on the JS
 *        thread, where result is the SAME shape the sync method returns —
 *        { samples: Float32Array, sampleRate: number, durations: Int32Array }
 *        (+ stages when opts.trace) — and info = { cancelled: boolean, error?: string }.
 * @returns {AsyncHandle}  - { cancel(): void }. Rejects (throws) if another
 *          synthesize()/op is already in flight on this model.
 */
const handle = bro.tts.synthesize(kokoro, phonemeIds, voice, {
    speed: 1.0,
    onDone: (result, info) => {
        if (info.cancelled) return;
        if (info.error) { console.error(info.error); return; }
        console.log(`${result.samples.length} samples @ ${result.sampleRate} Hz`);
        // result.durations: per-phoneme frame counts (see timing section below).
    },
});
// handle.cancel();  // drop the in-flight synthesis; onDone fires cancelled:true

// ── Async re-decode (prosody editing, non-blocking) ─────────────────────────

/**
 * bro.tts.decodeFrom(kokoro, voice, asr, F0, N, nPhonemes, opts) → AsyncHandle
 *
 * The prosody-editing seam: re-run only the decoder BACK HALF (skipping plBERT /
 * encoders / predictor) from EDITED intermediates, on a background thread. This
 * is what lets an editor scrub timing / pitch / energy and re-synthesize on every
 * change without ever blocking the JS thread. The async, latest-wins sibling of
 * the synchronous kokoro.decodeFrom(voice, asr, F0, N, nPhonemes, opts?) method.
 *
 * Inputs are the edited 'asr', 'F0_pred' and 'N_pred' stages from a prior trace:
 *   - asr   length-regulated content, hiddenDim * total floats.
 *   - F0    pitch contour, 2 * total floats. total is inferred as F0.length / 2.
 *   - N     energy contour, 2 * total floats.
 *   - nPhonemes  the 'phonemes' stage length (picks the voice's style row).
 * Editing F0/N only: pass the original asr unchanged. Editing durations: rebuild
 * asr (re-expand the text-encoder features) and resample F0/N to the new frame
 * count first.
 *
 * @param {KokoroModel} kokoro            - from loadKokoro().
 * @param {Voice}  voice                  - from loadVoice()/createVoice().
 * @param {Float32Array} asr, F0, N       - the edited back-half grids (above).
 * @param {number} nPhonemes              - the 'phonemes' stage length.
 * @param {Object} [opts]
 * @param {boolean}  [opts.trace=false]   - also return the re-decoded back-half
 *        `stages` ([{ name, h, w, data }]), built on the background thread.
 * @param {function} [opts.onDone]        - onDone(result, info) on the JS thread:
 *        result = { samples: Float32Array, sampleRate: number } (+ stages when
 *        opts.trace), info = { cancelled: boolean, error?: string }.
 * @returns {AsyncHandle}  - { cancel(): void }. Throws if another op is already
 *          in flight on this model (it is single-owner, like synthesize).
 */
bro.tts.decodeFrom(kokoro, voice, editedAsr, editedF0, editedN, nPhonemes, {
    trace: true,
    onDone: (result, info) => {
        if (info.cancelled || info.error) return;
        // play result.samples; result.stages has the new gen_in/har/audio grids.
    },
});

// ── Word/phoneme timing from durations ───────────────────────────────────────
// The output sample count is a fixed multiple of the summed frame count, so:
//   samplesPerFrame = out.samples.length / sum(out.durations)
//   phoneme i (0-based in phonemeIds) spans durations[i+1] frames, starting at
//   the cumulative frame offset; multiply frame offsets by samplesPerFrame /
//   sampleRate for seconds. Words are separated by the inter-word space token
//   (kokoro.vocab()[' ']) in the phoneme stream, so split phonemeIds on it to
//   group per-word, then drive a highlight from audioCtx.getPlaybackPosition().


// ═════════════════════════════════════════════════════════════════════════════
// Qwen3-TTS (12 Hz multi-codebook) — text in, speech out
// ═════════════════════════════════════════════════════════════════════════════
//
// Qwen3-TTS is text-driven end-to-end: no phonemize(), no voice pack. The model
// runs its own Qwen-BPE tokenizer + an autoregressive Talker / Code Predictor
// over a 12.5 Hz code stream, then a bundled codec decodes to 24 kHz mono. Voice
// is chosen per variant: a preset speaker name (CustomVoice) or a natural-
// language description (VoiceDesign). Like Kokoro it defaults to CUDA.

/**
 * Load a Qwen3-TTS model from a weights directory.
 *
 * @param {string} dir            - model dir: config.json + model.safetensors +
 *        vocab.json + merges.txt + the bundled speech_tokenizer/ codec.
 * @param {Object} [opts]
 * @param {string} [opts.device='cuda'] - 'cuda' or 'cpu'.
 * @param {function} [opts.onReady]     - async load: onReady(qwen).
 * @param {function} [opts.onError]     - async load: onError(message).
 * @returns {QwenTtsModel|AsyncHandle}  - the model (sync), or an AsyncHandle
 *          (async, when opts.onReady is a function). Same sync/async convention
 *          as loadKokoro.
 *
 * QwenTtsModel getters: .loaded, .sampleRate (24000), .variant
 * ('customvoice' | 'base' | 'voicedesign'), .modelSize ('0b6' | '1b7').
 * QwenTtsModel methods:
 *   .speakers()           -> string[]  preset speaker names (CustomVoice; empty
 *                                       for VoiceDesign / Base).
 *   .languages()          -> string[]  selectable language names for opts.language
 *                                       (dialects excluded; "auto" always valid).
 *   .speakerDialect(name) -> string    a preset speaker's dialect tag
 *                                       ("sichuan_dialect" / "beijing_dialect"),
 *                                       or "" if it isn't a dialect voice.
 */
const qwen = bro.tts.loadQwen('../brosoundml/weights/qwen-tts/0.6B-customvoice');
// qwen.speakers()  === ['serena', 'vivian', 'ryan', ...]; qwen.sampleRate === 24000
// qwen.languages() === ['english', 'chinese', 'german', ...]

// VoiceDesign (1.7B) is loaded the same way; it just has no presets and takes a
// description instead of a speaker (see synthesize opts.instruct below):
// const vd = bro.tts.loadQwen('../brosoundml/weights/qwen-tts/1.7B-voicedesign');
// vd.variant === 'voicedesign'; vd.speakers() === []

// Async load:
// bro.tts.loadQwen('../brosoundml/weights/qwen-tts/0.6B-customvoice', {
//     onReady: (q) => { qwen = q; },
//     onError: (msg) => console.error('qwen load failed:', msg),
// });

/**
 * QwenTtsModel.synthesize(text, opts?) → { samples, sampleRate }   (sync, blocking)
 *
 * @param {string} text                  - the text to speak.
 * @param {Object} [opts]
 * @param {string} [opts.speaker]        - preset speaker name (CustomVoice; see
 *        .speakers()). Ignored by VoiceDesign.
 * @param {string} [opts.instruct]       - natural-language voice description
 *        (VoiceDesign), e.g. 'a warm, low-pitched elderly storyteller'. Ignored
 *        by the 0.6B CustomVoice checkpoint.
 * @param {string} [opts.language='english'] - 'english' | 'chinese' | 'auto' | ...
 * @param {number} [opts.temperature=0]  - 0 = greedy/deterministic (default; the
 *        bit-exact upstream policy). >0 makes the autoregressive loop stochastic:
 *        every code is drawn through a seeded sampler, giving take-to-take variation.
 * @param {number} [opts.topK=0]   - keep only the top-K logits when sampling (0 = off).
 * @param {number} [opts.topP=1]   - nucleus cap when sampling (1 = off).
 * @param {number} [opts.seed=0]   - RNG seed; a fixed (temperature, seed) reproduces
 *        the exact utterance, different seeds give different takes.
 * @param {boolean} [opts.trace=false] - also return `stages` — the AR trace for
 *        visualization ("watch it take shape"). Each stage is { name, h, w, data:
 *        Float32Array } (h×w row-major), same shape as Kokoro's trace:
 *          • 'codes'         — the 16×F multi-codebook RVQ raster (row k = codebook
 *                              k, column t = frame t), code ids as floats.
 *          • 'c0_confidence' — 1×F, per-frame top-1 softmax probability of codebook
 *                              0 (how sure the model was that frame).
 *        Also supported on synthesizeFromXvector and the async synthesize.
 * @returns {{ samples: Float32Array, sampleRate: number, stages?: Array }} - 24 kHz
 *        mono, [-1, 1]; `stages` present when opts.trace.
 *
 * No durations: Qwen3-TTS is autoregressive over codec frames, not phonemes, so
 * there is no per-phoneme timing array (unlike Kokoro).
 */
const qout = qwen.synthesize('Hello there.', { speaker: 'serena', language: 'english' });
// A varied take: qwen.synthesize(text, { speaker: 'serena', temperature: 0.8, topP: 0.95, seed: 7 });
console.log(`${qout.samples.length} samples @ ${qout.sampleRate} Hz`);

// VoiceDesign: same call, but describe the voice instead of naming a speaker.
// const vout = vd.synthesize('Hello there.',
//     { instruct: 'a warm, low-pitched elderly storyteller', language: 'english' });

/**
 * QwenTtsModel.synthesizeClone(text, refPath, opts?) → { samples, sampleRate }  (sync, blocking)
 *
 * Zero-shot voice clone — synthesize `text` in the voice of a reference clip.
 * BASE VARIANT ONLY: load the Base checkpoint (e.g. '…/qwen-tts/0.6B-Base'),
 * which bundles the ECAPA-TDNN speaker encoder. The reference WAV is read,
 * downmixed + resampled to 24 kHz internally, encoded to a speaker x-vector,
 * and spliced into the Talker prefill where a CustomVoice preset token would
 * sit (x-vector-only enrollment — no reference transcript needed).
 *
 * @param {string} text      - the text to speak.
 * @param {string} refPath   - path to a 16-bit PCM WAV of the voice to clone
 *        (mono or stereo, any sample rate; downmixed/resampled internally).
 * @param {Object} [opts]
 * @param {string} [opts.language='english'] - 'english' | 'chinese' | 'auto' | ...
 * @returns {{ samples: Float32Array, sampleRate: number }} - 24 kHz mono, [-1, 1].
 *
 * Throws if no model is loaded, the checkpoint is not a Base variant (no speaker
 * encoder), or the WAV can't be read. (Like synthesize(), there is no per-phoneme
 * duration array — recover caption word timing from the audio envelope.)
 */
const cloned = qwen.synthesizeClone('Hello there.', 'weights/myvoice.wav', { language: 'english' });
// Requires the Base checkpoint:
// const qbase = bro.tts.loadQwen('../brosoundml/weights/qwen-tts/0.6B-Base');
// qbase.variant === 'base'

/**
 * QwenTtsModel.synthesizeFromXvector(text, xvec, opts?) → { samples, sampleRate }  (sync)
 *
 * Render `text` from a caller-supplied speaker x-vector directly — synthesizeClone
 * without the WAV enrollment. `xvec` is a Float32Array of enc_dim (1024) floats, as
 * embedSpeaker() returns. This is the voice-designer seam: enroll real voices to
 * x-vectors, then interpolate / morph / steer in that continuous space and render
 * the designed point — no reference clip per render. Base variant only.
 *
 * @param {string} text   - the text to speak.
 * @param {Float32Array} xvec - enc_dim (1024) speaker x-vector (from embedSpeaker, or
 *        a blend of several). Throws if the width is wrong or the model isn't Base.
 * @param {Object} [opts]  - opts.language + the sampling controls (temperature/topK/
 *        topP/seed) from synthesize().
 * @returns {{ samples: Float32Array, sampleRate: number }} - 24 kHz mono, [-1, 1].
 */
const va = qwen.embedSpeaker(refA, { sampleRate: 24000 });   // Float32Array(1024)
const vb = qwen.embedSpeaker(refB, { sampleRate: 24000 });
const mix = va.map((x, i) => 0.5 * x + 0.5 * vb[i]);          // morph halfway between A and B
const designed = qwen.synthesizeFromXvector('Hello there.', mix, { language: 'english' });

/**
 * QwenTtsModel.encodeAudio(audio, opts?) → { codes, numQuantizers, numFrames }  (sync)
 * QwenTtsModel.decodeCodes(codes, numQuantizers, numFrames) → { samples, sampleRate }  (sync)
 *
 * The codec analysis / synthesis tail, exposed directly. encodeAudio turns mono PCM
 * (Float32Array; opts.sampleRate default 24000, resampled internally) into the RVQ
 * code stream — `codes` is an Int32Array of numQuantizers*numFrames, codebook-major
 * (codes[k*numFrames + t]). decodeCodes runs the deterministic codec decoder over a
 * code stream back to 24 kHz (numFrames*1920 samples). Same layout both ways, so
 * encode ▸ decode round-trips. Lets an editor splice / prefix-lock / round-trip a
 * code stream and re-render the audio without re-running the autoregressive Talker.
 */
const e = qwen.encodeAudio(somePcm, { sampleRate: 24000 });
const back = qwen.decodeCodes(e.codes, e.numQuantizers, e.numFrames);  // back.samples ≈ somePcm

/**
 * bro.tts.loadSpeakerEncoder(dir, opts?) → SpeakerEncoder   (sync)
 *                                        → AsyncHandle       (async, if opts.onReady)
 *
 * The standalone ECAPA-TDNN speaker encoder on its own — for harvesting a voice's
 * x-vector (e.g. to drive a style adapter like Kokoro's voice_bridge) WITHOUT
 * loading all of Qwen-Base. `dir` is the ~18 MB artifact (config.json +
 * model.safetensors) at brosoundml-data/qwen-tts/speaker-encoder; the x-vector it
 * produces is bit-identical to qwen.embedSpeaker on the full Base checkpoint.
 * Loading the artifact is cheap; the convolution stack that embedSpeaker runs
 * lives on the default device (GPU when available), in FP32.
 *
 * @param {string} dir       - the speaker-encoder artifact directory.
 * @param {Object} [opts]
 * @param {function} [opts.onReady] - onReady(enc): when present, loads on a
 *        background thread and returns an AsyncHandle instead of the encoder.
 * @param {function} [opts.onError] - onError(message) on the JS thread.
 * @returns {SpeakerEncoder|AsyncHandle} - handle with:
 *   enc.embedSpeaker(audio, opts?) → Float32Array(encDim)  (alias: enc.embed)
 *     audio: Float32Array of mono samples; opts.sampleRate (default 24000,
 *     resampled to 24 kHz as needed). Drop-in for qwen.embedSpeaker. The ECAPA
 *     forward is a multi-GFLOP conv stack: pass opts.onDone(embedding) to run it
 *     on a background thread (returns an AsyncHandle), opts.onError(message) for
 *     failures. Without onDone it runs synchronously and returns the Float32Array.
 *   enc.encDim, enc.sampleRate, enc.loaded — read-only props.
 */
const enc = bro.tts.loadSpeakerEncoder('../brosoundml-data/qwen-tts/speaker-encoder');
// async (recommended — keeps the UI responsive):
enc.embedSpeaker(monoSamples, { sampleRate: 24000, onDone: (xvec) => {/* Float32Array(1024) */} });
// sync:
const xvec = enc.embedSpeaker(monoSamples, { sampleRate: 24000 });  // Float32Array(1024)

/**
 * bro.tts.synthesize(qwen, text, opts) → AsyncHandle   (non-blocking, cancellable)
 *
 * Runs the autoregressive loop on a background thread; the cancel flag is polled
 * once per 12.5 Hz frame, so handle.cancel() aborts mid-utterance (returns an
 * empty buffer, onDone fires { cancelled: true }) — real barge-in, not a
 * post-hoc discard.
 *
 * @param {QwenTtsModel} qwen     - from loadQwen().
 * @param {string} text           - the text to speak.
 * @param {Object} [opts]
 * @param {string} [opts.speaker]  - preset speaker name (CustomVoice).
 * @param {string} [opts.instruct] - natural-language voice description (VoiceDesign).
 * @param {string} [opts.language='english']
 * @param {function} [opts.onDone] - onDone(result, info) on the JS thread, where
 *        result = { samples: Float32Array, sampleRate: number } and
 *        info = { cancelled: boolean, error?: string }.
 * @returns {AsyncHandle}  - { cancel(): void }. Rejects (throws) if another op is
 *          already in flight on this model.
 */
const qhandle = bro.tts.synthesize(qwen, 'Hello there.', {
    speaker: 'serena',
    onDone: (result, info) => {
        if (info.cancelled) return;
        if (info.error) { console.error(info.error); return; }
        console.log(`${result.samples.length} samples @ ${result.sampleRate} Hz`);
    },
});
// qhandle.cancel();  // abort mid-utterance; onDone fires cancelled:true

/**
 * bro.tts.synthesizeStream(qwen, text, opts) → AsyncHandle   (streaming, QwenTts only)
 *
 * Like the async synthesize, but the audio is delivered in chunks AS the
 * autoregressive loop produces it, so playback can start before generation
 * finishes — the lowest-latency path. opts.onChunk(samples) fires on the JS thread
 * each time the codec decodes a new chunk (the codec is causal, so chunk samples
 * are final and can be queued straight into an AudioContext); opts.onDone(result,
 * info) fires once at the end with the complete buffer. .cancel() is real barge-in:
 * the AR loop stops within a frame, onDone fires { cancelled: true }, and chunks
 * already delivered stay played.
 *
 * @param {QwenTtsModel} qwen
 * @param {string} text
 * @param {Object} [opts]
 * @param {number}   [opts.chunkFrames=25] - 12.5 Hz frames per chunk (25 ≈ 2 s; a
 *        smaller value = lower first-audio latency, more onChunk calls).
 * @param {function} [opts.onChunk]  - onChunk(samples: Float32Array) per decoded
 *        chunk, 24 kHz mono, in order.
 * @param {function} [opts.onDone]   - onDone(result, info); result = { samples, sampleRate }
 *        is the whole utterance, info = { cancelled, error? }.
 * @param {string}   [opts.speaker] / [opts.language] / [opts.instruct]   - as synthesize().
 * @param {number}   [opts.temperature] / [opts.topK] / [opts.topP] / [opts.seed]  - sampling.
 * @returns {AsyncHandle} - { cancel(): void }. Throws if another op is in flight,
 *          or if `qwen` is not a QwenTts (Kokoro synthesis is monolithic).
 */
const shandle = bro.tts.synthesizeStream(qwen, 'A longer line that streams as it generates.', {
    speaker: 'serena',
    chunkFrames: 8,
    onChunk: (samples) => { /* queue into an AudioContext buffer for gapless playback */ },
    onDone: (result, info) => { if (!info.cancelled && !info.error) console.log('done', result.samples.length); },
});
// shandle.cancel();  // barge-in: stops within a frame; already-streamed chunks keep playing
