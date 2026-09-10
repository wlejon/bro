// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * @typedef {Object} KokoroLoadOptions
 * @property {string} [device]
 */

/**
 * @typedef {Object} VoiceLoadOptions
 * @property {string} [path]
 */

/**
 * @typedef {Object} KokoroSynthesizeOptions
 * @property {number} [speed=1]
 * @property {Function} [onDone]
 */

/**
 * @typedef {Object} KokoroSynthesizeResult
 * @property {Float32Array} [samples]
 * @property {number} [sampleRate]
 */

/**
 * @typedef {Object} KokoroStreamOptions
 * @property {number} [speed=1]
 * @property {Function} [onChunk]
 * @property {Function} [onDone]
 */

/**
 * @typedef {Object} QwenLoadOptions
 * @property {string} [device]
 */

/**
 * @typedef {Object} QwenSynthesizeOptions
 * @property {string} [speaker]
 * @property {number} [speed=1]
 * @property {Function} [onDone]
 */

/**
 * @typedef {Object} QwenSynthesizeResult
 * @property {Float32Array} [samples]
 * @property {number} [sampleRate]
 */

/**
 * @typedef {Object} QwenStreamOptions
 * @property {string} [speaker]
 * @property {number} [speed=1]
 * @property {Function} [onChunk]
 * @property {Function} [onDone]
 */

/**
 * @typedef {Object} QwenAudioCodes
 * @property {Int32Array} [codes]
 * @property {number} [numFrames]
 */

/**
 * OmniVoice (k2-fsa, 2026): zero-shot masked-diffusion TTS over 600+
 * languages. A Qwen3-0.6B trunk with eight audio heads generates HiggsAudio v2
 * codec codes (25 frames/s x 8 codebooks) by unmasking the most confident
 * (codebook, frame) cells over `numSteps` bidirectional forwards; the codec
 * renders them at 24 kHz. A voice is either an in-context prompt (the codes +
 * transcript of a reference clip, see {@link OmniVoicePrompt}) or a
 * fixed-vocabulary instruct (see `instructAttributes()`); with neither the
 * model picks a voice.
 *
 * @typedef {Object} OmniVoiceLoadOptions
 * @property {string} [device] 'cuda' | 'metal' | 'cpu'. Defaults to the GPU.
 *   Every diffusion step is a full 0.6B-parameter forward, so when the resolved
 *   device is the CPU the load is refused unless 'cpu' is passed explicitly.
 * @property {string} [precision] 'bf16' (default on CUDA: tensor-core GEMMs +
 *   fused attention, ~6x faster than fp32 with the same transcript) or 'fp32'
 *   (bit-exact against the upstream fixtures; the default off CUDA).
 * @property {boolean} [decoderOnly=false] Skip the codec encoder + HuBERT.
 *   Faster load, less memory; `createPrompt` / `encodeAudio` then throw.
 * @property {Function} [onReady] `onReady(omni)`: load on a background thread.
 * @property {Function} [onError] `onError(message)` for the async load.
 */

/**
 * A reference voice: everything the prompt needs and everything a clone needs
 * to persist. A plain object so a lab can store it, JSON it (`codes` as a
 * number[] is accepted back), splice it, or hand-build one. Produced by
 * `createPrompt` / `loadPrompt`; accepted by `synthesize`, `generateCodes`,
 * `estimateFrames` (as `opts.prompt`) and `savePrompt`.
 *
 * @typedef {Object} OmniVoicePrompt
 * @property {(Int32Array|Array<number>)} codes numCodebooks * numFrames codes,
 *   codebook-major: `codes[q * numFrames + t]`.
 * @property {number} [numFrames] Defaults to `codes.length / numCodebooks`.
 * @property {string} [text=''] Transcript of the clip ('' = text-free reference).
 * @property {number} [rms=0] RMS of the preprocessed clip; drives the output
 *   loudness match (an output is scaled to `rms / 0.1` when rms < 0.1).
 */

/**
 * The generation parameters, mapped 1:1 onto brosoundml's OmniVoiceParams;
 * omitted keys keep the upstream defaults shown.
 *
 * @typedef {Object} OmniVoiceParams
 * @property {number} [numSteps=32] Diffusion forwards per chunk (16 is acceptable).
 * @property {number} [tShift=0.1] Schedule warp `t' = s*t / (1 + (s-1)*t)`.
 * @property {number} [guidanceScale=2] Classifier-free guidance weight; 0 runs
 *   the conditional row alone (half the work per step).
 * @property {number} [layerPenalty=5] Confidence -= codebook * layerPenalty, so
 *   codebook 0 (the semantic one) unmasks first.
 * @property {number} [positionTemperature=5] Gumbel-noise scale on the unmask order.
 * @property {number} [classTemperature=0] > 0: sample each cell's code from its
 *   top-10% instead of the argmax.
 * @property {boolean} [gumbelNoise=true] false: constant uniforms, a
 *   deterministic (argmax) unmask order regardless of `seed`.
 * @property {number} [seed=0] Seed of the counter-based Gumbel RNG; a seed
 *   reproduces an utterance exactly (CPU and CUDA draw identical noise).
 * @property {number} [speed=1] Divides the estimated frame count.
 * @property {number} [duration=0] Seconds; > 0 fixes the frame count outright.
 * @property {string} [language=''] A name from `languages()` (case-insensitive)
 *   or an ISO id; '' / 'none' is language-agnostic.
 * @property {string} [instruct=''] Voice design: a comma-separated pick of at
 *   most one value per `instructAttributes()` category ('female, young, high
 *   pitch'). Anything else rejects through `onError` / `info.error`.
 * @property {boolean} [denoise=true] Prepend the denoise token when a prompt is present.
 * @property {boolean} [preprocessPrompt=true]
 * @property {boolean} [postprocess=true] Silence trim + RMS/peak match (fade +
 *   pad always apply, as upstream).
 * @property {number} [chunkDuration=15] Seconds per long-form chunk.
 * @property {number} [chunkThreshold=30] Chunk at punctuation when the
 *   estimate exceeds this many seconds; chunks are cross-faded.
 * @property {number} [padDuration=0.1] Seconds of silence each side.
 * @property {number} [fadeDuration=0.1] Seconds of linear fade each side.
 */

/**
 * One observation per diffusion step, delivered on the JS thread through
 * `opts.onStep`. `tokens` is the grid so far (`config.maskId` where still
 * masked), `scores` this step's unmask scores (-Infinity where already fixed),
 * both `numCodebooks * numFrames` laid out `[q * numFrames + t]`. The copies
 * are made only when `onStep` is set.
 *
 * @typedef {Object} OmniVoiceStep
 * @property {number} step
 * @property {number} numSteps
 * @property {number} numFrames
 * @property {number} numCodebooks
 * @property {number} unmasked Cells fixed by this step.
 * @property {Int32Array} tokens
 * @property {Float32Array} scores
 */

/**
 * Diagnostics attached to a result as `result.trace` when `opts.trace` is true.
 *
 * @typedef {Object} OmniVoiceTrace
 * @property {Int32Array} textIds The conditional prompt's text ids.
 * @property {number} numFrames Frames generated (all chunks).
 * @property {Int32Array} codes numCodebooks * numFrames, `[q * numFrames + t]`.
 * @property {Int32Array} unmaskStep Step at which each cell was fixed (-1 =
 *   kept by an init grid).
 * @property {Int32Array} chunkFrames Per-chunk frame counts (one entry when unchunked).
 * @property {number} lmSeconds
 * @property {number} codecSeconds
 */

/**
 * @typedef {OmniVoiceParams} OmniVoiceSynthesizeOptions
 * @property {OmniVoicePrompt} [prompt] Clone this voice (in-context).
 * @property {boolean} [trace=false] Attach an {@link OmniVoiceTrace}.
 * @property {Function} [onStep] `onStep(step: OmniVoiceStep)`, every step.
 * @property {Function} [onDone] `onDone(result, info)`, once; `info =
 *   { cancelled, error? }`. Fires even on error / cancel (empty result).
 * @property {Function} [onError] `onError(message)`, additionally, on error.
 */

/**
 * @typedef {Object} OmniVoiceSynthesizeResult
 * @property {Float32Array} samples Mono 24 kHz PCM; empty when cancelled.
 * @property {number} sampleRate
 * @property {OmniVoiceTrace} [trace]
 */

/**
 * A starting token grid for `generateCodes` (inpainting / re-roll). Cells with
 * `keep[i] != 0` stay at `tokens[i]` for the whole schedule; the rest start
 * masked. Both arrays are numCodebooks * numFrames, `[q * numFrames + t]`, and
 * must match `frames`. Keep codebook 0 and re-roll the acoustic codebooks,
 * regenerate one time span, or splice takes, all through this one input.
 *
 * @typedef {Object} OmniVoiceInit
 * @property {(Int32Array|Array<number>)} tokens
 * @property {(Uint8Array|Array<number>)} keep
 */

/**
 * @typedef {OmniVoiceSynthesizeOptions} OmniVoiceGenerateCodesOptions
 * @property {number} [frames=0] Exact frame count; <= 0 uses the duration rule
 *   (`estimateFrames`).
 * @property {OmniVoiceInit} [init]
 */

/**
 * @typedef {Object} OmniVoiceCodesResult
 * @property {Int32Array} codes numCodebooks * numFrames, `[q * numFrames + t]`;
 *   empty when cancelled.
 * @property {number} numFrames
 * @property {number} numCodebooks
 * @property {OmniVoiceTrace} [trace]
 */

/**
 * @typedef {Object} OmniVoiceCreatePromptOptions
 * @property {number} [sampleRate=24000] Rate of `samples`; resampled to 24 kHz.
 * @property {string} [refText=''] The clip's transcript. '' yields a text-free
 *   reference (there is no ASR fallback).
 * @property {boolean} [preprocess=true] Upstream preprocessing: RMS boost, trim
 *   an untranscribed clip over 20 s, silence removal, frame alignment,
 *   terminal punctuation on the transcript.
 * @property {Function} [onDone] `onDone(prompt)`: run the encoder on a
 *   background thread (claims the model like a synthesis).
 * @property {Function} [onError] `onError(message)` for the async form.
 */

/**
 * @typedef {Object} OmniVoiceConfig
 * @property {number} sampleRate 24000
 * @property {number} frameRate 25
 * @property {number} numCodebooks 8
 * @property {number} audioVocabSize 1025 (1024 codes + MASK)
 * @property {number} maskId 1024
 * @property {number} hiddenSize
 * @property {number} numLayers
 * @property {string} precision 'bf16' | 'fp32'
 * @property {string} device 'CUDA' | 'Metal' | 'CPU'
 * @property {boolean} decoderOnly
 */

/**
 * @typedef {Object} OmniVoiceInstructCategory
 * @property {string} name 'gender' | 'age' | 'pitch' | 'whisper' | 'accent' | 'dialect'
 * @property {Array<string>} values
 */

/**
 * @typedef {Object} SupertonicLoadOptions
 * @property {string} [device]
 */

/**
 * @typedef {Object} SupertonicSynthesizeOptions
 * @property {SupertonicVoice} [voice]
 * @property {number} [speed=1]
 * @property {Function} [onDone]
 */

/**
 * @typedef {Object} SupertonicSynthesizeResult
 * @property {Float32Array} [samples]
 * @property {number} [sampleRate]
 */

/**
 * @typedef {Object} SpeakerEncoderLoadOptions
 * @property {string} [device]
 */

/**
 * @typedef {Object} SpeakerEncoderEmbedOptions
 * @property {(Float32Array|SttAudioBuffer)} [audio]
 * @property {Function} [onDone]
 */

/**
 * @typedef {Object} TtsAssetsOptions
 * @property {string} [root]
 * @property {string} [lexicon]
 * @property {string} [pos]
 * @property {string} [kokoroConfig]
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

class KokoroModel {

  /**
   * @readonly
   * @type {boolean}
   */
  loaded;

  /**
   * @readonly
   * @type {string}
   */
  device;

  /**
   * @param {string} ipa
   * @returns {Int32Array}
   */
  encodePhonemes(ipa) {}

  /**
   * @param {string} path
   * @returns {Voice}
   */
  loadVoice(path) {}

  /**
   * @returns {KokoroSession}
   */
  createSession() {}

}

class Voice {

  /**
   * @readonly
   * @type {boolean}
   */
  loaded;

  /**
   * @readonly
   * @type {string}
   */
  name;

}

class KokoroSession {

  /**
   * @readonly
   * @type {boolean}
   */
  loaded;

  /**
   * @param {(Int32Array|Array<number>)} phonemes
   * @param {Voice} voice
   * @param {KokoroSynthesizeOptions} [opts]
   * @returns {AsyncHandle}
   */
  synthesize(phonemes, voice, opts) {}

  reset() {}

}

class QwenTtsModel {

  /**
   * @readonly
   * @type {boolean}
   */
  loaded;

  /**
   * @readonly
   * @type {string}
   */
  device;

  /**
   * @readonly
   * @type {string}
   */
  variant;

  /**
   * @returns {QwenTtsSession}
   */
  createSession() {}

  /**
   * @param {Float32Array} audio
   * @returns {QwenAudioCodes}
   */
  encodeAudio(audio) {}

  /**
   * @param {Int32Array} codes
   * @returns {Float32Array}
   */
  decodeCodes(codes) {}

}

class QwenTtsSession {

  /**
   * @readonly
   * @type {boolean}
   */
  loaded;

  /**
   * @readonly
   * @type {string}
   */
  variant;

  /**
   * @param {string} text
   * @param {QwenSynthesizeOptions} [opts]
   * @returns {AsyncHandle}
   */
  synthesize(text, opts) {}

  reset() {}

}

/**
 * One loaded OmniVoice pipeline (LM + codec). Single-owner: one async op at a
 * time (`synthesize` / `generateCodes` / async `createPrompt`); a second call
 * while one is in flight throws, and the sync codec methods refuse to run
 * while an async op holds the model. The gate releases before `onDone` fires,
 * so a callback may start the next op. Returned by `bro.tts.loadOmniVoice`.
 */
class OmniVoice {

  /**
   * @readonly
   * @type {boolean}
   */
  loaded;

  /**
   * 'CUDA' | 'Metal' | 'CPU'
   * @readonly
   * @type {string}
   */
  device;

  /**
   * 'bf16' | 'fp32'
   * @readonly
   * @type {string}
   */
  precision;

  /**
   * @readonly
   * @type {number}
   */
  sampleRate;

  /**
   * @readonly
   * @type {OmniVoiceConfig}
   */
  config;

  /**
   * Text -> 24 kHz PCM on a background thread (long text is chunked at
   * punctuation and cross-faded). `.cancel()` on the handle stops at the next
   * diffusion step; `onDone` then reports `info.cancelled` with empty samples.
   * `bro.tts.synthesize(omni, text, opts)` is the same call.
   * @param {string} text
   * @param {OmniVoiceSynthesizeOptions} [opts]
   * @returns {AsyncHandle}
   */
  synthesize(text, opts) {}

  /**
   * The LM alone, one chunk, no codec / post-processing: run the schedule for
   * `text` at exactly `opts.frames` frames and deliver the codes to `onDone`
   * as an {@link OmniVoiceCodesResult}. The seam a lab drives: `opts.init`
   * seeds the grid, `onStep` watches it fill, `decodeCodes` renders it.
   * @param {string} text
   * @param {OmniVoiceGenerateCodesOptions} [opts]
   * @returns {AsyncHandle}
   */
  generateCodes(text, opts) {}

  /**
   * The codec decoder on its own (sync, ~60 ms): codes `[q * numFrames + t]`
   * -> `{ samples, sampleRate }`. `numFrames` defaults to
   * `codes.length / numCodebooks`. Throws while an async op is in flight.
   * @param {(Int32Array|Array<number>)} codes
   * @param {number} [numFrames]
   * @returns {OmniVoiceSynthesizeResult}
   */
  decodeCodes(codes, numFrames) {}

  /**
   * The codec encoder (DAC + HuBERT) on its own (sync): mono PCM at any rate
   * -> `{ codes, numFrames, numCodebooks }`. Throws when loaded `decoderOnly`.
   * @param {(Float32Array|OmniVoiceSynthesizeResult)} samples
   * @param {number} [sampleRate=24000]
   * @returns {{codes: Int32Array, numFrames: number, numCodebooks: number}}
   */
  encodeAudio(samples, sampleRate) {}

  /**
   * Build a voice prompt from a reference clip and its transcript. Sync unless
   * `opts.onDone` is a function. Throws when loaded `decoderOnly`.
   * @param {(Float32Array|OmniVoiceSynthesizeResult)} samples
   * @param {OmniVoiceCreatePromptOptions} [opts]
   * @returns {(OmniVoicePrompt|AsyncHandle)}
   */
  createPrompt(samples, opts) {}

  /**
   * Write a prompt as a small self-describing `.ovcp` file (magic "OVCP").
   * Paths resolve app-relative like the loaders.
   * @param {OmniVoicePrompt} prompt
   * @param {string} path
   */
  savePrompt(prompt, path) {}

  /**
   * @param {string} path
   * @returns {OmniVoicePrompt}
   */
  loadPrompt(path) {}

  /**
   * The duration rule: frames `synthesize` would generate for `text` given
   * `opts.prompt` / `speed` / `duration` (25 frames per second).
   * @param {string} text
   * @param {OmniVoiceSynthesizeOptions} [opts]
   * @returns {number}
   */
  estimateFrames(text, opts) {}

  /**
   * Text -> the prompt's text ids (non-verbal tags tokenize standalone).
   * @param {string} text
   * @returns {Int32Array}
   */
  tokenize(text) {}

  /**
   * Language names accepted by `opts.language`.
   * @returns {Array<string>}
   */
  languages() {}

  /**
   * The voice-design vocabulary an `instruct` picks from.
   * @returns {Array<OmniVoiceInstructCategory>}
   */
  instructAttributes() {}

  /**
   * The non-verbal tags the tokenizer keeps whole ('[laughter]', ...).
   * @returns {Array<string>}
   */
  nonverbalTags() {}

  /**
   * Drop the weights (GPU memory included); `loaded` becomes false and every
   * method throws. Refused while an op is in flight.
   */
  unload() {}

}

class SupertonicModel {

  /**
   * @readonly
   * @type {boolean}
   */
  loaded;

  /**
   * @readonly
   * @type {string}
   */
  device;

  /**
   * @param {string} path
   * @returns {SupertonicVoice}
   */
  loadVoiceStyle(path) {}

}

class SupertonicVoice {

  /**
   * @readonly
   * @type {boolean}
   */
  loaded;

  /**
   * @readonly
   * @type {string}
   */
  name;

}

class SpeakerEncoder {

  /**
   * @readonly
   * @type {boolean}
   */
  loaded;

  /**
   * @readonly
   * @type {string}
   */
  device;

  /**
   * @param {Float32Array} audio
   * @param {SpeakerEncoderEmbedOptions} [opts]
   * @returns {AsyncHandle}
   */
  embedSpeaker(audio, opts) {}

}

// ── Namespaces ───────────────────────────────────────────────────────────────

bro.tts.init = function() {};

/**
 * @param {string} dir
 * @param {KokoroLoadOptions} [opts]
 * @returns {(KokoroModel|AsyncHandle)}
 */
bro.tts.loadKokoro = function(dir, opts) {};

/**
 * @param {string} dir
 * @param {QwenLoadOptions} [opts]
 * @returns {(QwenTtsModel|AsyncHandle)}
 */
bro.tts.loadQwen = function(dir, opts) {};

/**
 * Load OmniVoice from a model dir (config.json + tokenizer.json +
 * model.safetensors + audio_tokenizer/). Sync unless `opts.onReady` is a
 * function. The path resolves app-relative like `fs.existsSync`.
 *
 * @example
 * // Speak, clone the result's voice, then say something else in it.
 * const omni = bro.tts.loadOmniVoice('../brosoundml/weights/omnivoice');   // CUDA, bf16
 * omni.synthesize('Hello there, this is a test of the OmniVoice pipeline.', {
 *   numSteps: 32, seed: 7,
 *   onStep: s => status(`step ${s.step + 1}/${s.numSteps}: +${s.unmasked} cells`),
 *   onDone: (out, info) => {
 *     if (info.error) return fail(info.error);
 *     play(out.samples, out.sampleRate);                       // 24 kHz mono
 *     const prompt = omni.createPrompt(out.samples, {
 *       sampleRate: out.sampleRate,
 *       refText: 'Hello there, this is a test of the OmniVoice pipeline.',
 *     });
 *     omni.savePrompt(prompt, 'voices/test.ovcp');             // { codes, numFrames, text, rms }
 *     omni.synthesize('And this is the same voice again.', { prompt,
 *       onDone: (out2) => play(out2.samples, out2.sampleRate) });
 *   },
 * });
 *
 * // Voice design instead of a clone:
 * omni.synthesize('Whispered, to a friend.', { instruct: 'female, young, whisper', onDone });
 *
 * // The lab seam: codes at an exact length, keep codebook 0, re-roll the rest.
 * omni.generateCodes('Hello there.', { frames: 50, seed: 1, onDone: (r) => {
 *   const keep = new Uint8Array(r.codes.length);
 *   keep.fill(1, 0, r.numFrames);                             // codebook 0 = first numFrames cells
 *   omni.generateCodes('Hello there.', { frames: 50, seed: 2,
 *     init: { tokens: r.codes, keep },
 *     onDone: (r2) => play(omni.decodeCodes(r2.codes, r2.numFrames).samples, 24000) });
 * }});
 *
 * @param {string} dir
 * @param {OmniVoiceLoadOptions} [opts]
 * @returns {(OmniVoice|AsyncHandle)}
 */
bro.tts.loadOmniVoice = function(dir, opts) {};

/**
 * The OmniVoice class object, for `handle instanceof bro.tts.OmniVoice` and
 * prototype inspection. Not constructible: instances come from loadOmniVoice.
 * @type {Function}
 */
bro.tts.OmniVoice = OmniVoice;

/**
 * @param {string} dir
 * @param {SupertonicLoadOptions} [opts]
 * @returns {(SupertonicModel|AsyncHandle)}
 */
bro.tts.loadSupertonic = function(dir, opts) {};

/**
 * @param {string} dir
 * @param {SpeakerEncoderLoadOptions} [opts]
 * @returns {(SpeakerEncoder|AsyncHandle)}
 */
bro.tts.loadSpeakerEncoder = function(dir, opts) {};

/**
 * @param {string} text
 * @param {Object} [opts]
 * @returns {Int32Array}
 */
bro.tts.phonemize = function(text, opts) {};

/**
 * @param {string} dir
 */
bro.tts.setAssetRoot = function(dir) {};

/**
 * @param {TtsAssetsOptions} opts
 */
bro.tts.setAssets = function(opts) {};

/**
 * @param {Object} model
 * @param {*} textOrPhonemes
 * @param {*} voiceOrOpts
 * @param {Object} [opts]
 * @returns {AsyncHandle}
 */
bro.tts.synthesize = function(model, textOrPhonemes, voiceOrOpts, opts) {};

/**
 * @param {Object} model
 * @param {*} textOrChunks
 * @param {*} voiceOrOpts
 * @param {Object} [opts]
 * @returns {AsyncHandle}
 */
bro.tts.synthesizeStream = function(model, textOrChunks, voiceOrOpts, opts) {};

/**
 * @param {KokoroModel} kokoro
 * @param {Voice} voice
 * @param {Float32Array} asr
 * @param {Float32Array} F0
 * @param {Float32Array} N
 * @param {number} nPhonemes
 * @param {Object} [opts]
 * @returns {AsyncHandle}
 */
bro.tts.decodeFrom = function(kokoro, voice, asr, F0, N, nPhonemes, opts) {};

