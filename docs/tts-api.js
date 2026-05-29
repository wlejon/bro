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
 * Override where the phonemizer loads its lexicon/assets from. Optional —
 * defaults resolve against the app's asset mounts.
 *
 * @param {string} dir
 */
// bro.tts.setAssetRoot('/lib/kokoro');


// ── Load model + voice ──────────────────────────────────────────────────────

/**
 * Load a Kokoro model from a weights directory.
 *
 * @param {string} dir            - Kokoro weights directory.
 * @param {Object} [opts]
 * @param {string} [opts.device='cuda'] - 'cuda' or 'cpu'.
 * @returns {KokoroModel}  - has .nTokens, .styleDim, .loadVoice(), .synthesize()
 */
const kokoro = bro.tts.loadKokoro('../brosoundml/weights/kokoro');
// kokoro.nTokens === 178, kokoro.styleDim === 128

/**
 * Load a voice embedding (.bin) for this model.
 *
 * @param {string} path - voice file, e.g. voices/af_heart.bin
 * @returns {Voice}      - { name, rows, cols } — pass to synthesize().
 */
const voice = kokoro.loadVoice('../brosoundml/weights/kokoro/voices/af_heart.bin');


// ── Synthesize ──────────────────────────────────────────────────────────────

/**
 * KokoroModel.synthesize(phonemeIds, voice, opts) → { samples, sampleRate }
 *
 * @param {Int32Array|number[]} phonemeIds - from phonemize() (or a raw id list).
 * @param {Voice}  voice                   - from loadVoice().
 * @param {Object} [opts]
 * @param {number} [opts.speed=1.0]        - speaking-rate multiplier.
 * @returns {{ samples: Float32Array, sampleRate: number }}  24 kHz mono, [-1, 1].
 */
const out = kokoro.synthesize(phonemeIds, voice, { speed: 1.0 });
console.log(`${out.samples.length} samples @ ${out.sampleRate} Hz`);
// Write out.samples to a WAV, or feed into an AudioContext buffer for playback.
