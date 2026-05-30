/**
 * bro.tts — Text-to-speech (Kokoro)
 *
 * Synthesizes 24 kHz mono speech from a phoneme-id sequence and a voice
 * embedding. Backed by brosoundml (audio-ML inference) on top of brotensor.
 * Defaults to CUDA; pass { device: 'cpu' } to force the CPU backend.
 *
 * Kokoro is phoneme-driven, not text-driven: it takes phoneme ids, not raw
 * text. Use bro.tts.phonemize(text) to convert a string into the id sequence
 * Kokoro expects, then synthesize() with a loaded voice. (You can also feed a
 * known-good id sequence directly — see tests/smoke_voice_pipeline.js, which
 * reads one from the model's ids.txt.)
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
 * @param {function} [opts.onDone]          - onDone(result, info) on the JS
 *        thread, where result is the SAME shape the sync method returns —
 *        { samples: Float32Array, sampleRate: number, durations: Int32Array } —
 *        and info = { cancelled: boolean, error?: string }.
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

// ── Word/phoneme timing from durations ───────────────────────────────────────
// The output sample count is a fixed multiple of the summed frame count, so:
//   samplesPerFrame = out.samples.length / sum(out.durations)
//   phoneme i (0-based in phonemeIds) spans durations[i+1] frames, starting at
//   the cumulative frame offset; multiply frame offsets by samplesPerFrame /
//   sampleRate for seconds. Words are separated by the inter-word space token
//   (kokoro.vocab()[' ']) in the phoneme stream, so split phonemeIds on it to
//   group per-word, then drive a highlight from audioCtx.getPlaybackPosition().
