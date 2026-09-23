// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * Every bro.tts loader takes these (paths go through the asset-path resolver).
 * @typedef {Object} KokoroLoadOptions
 * @property {string} [device] `'cuda'` | `'metal'` | `'cpu'`; defaults to CUDA,
 *   then Metal, when available, else CPU.
 * @property {Function} [onReady] `onReady(model)`: load on a background thread;
 *   the loader then returns an AsyncHandle instead of the model.
 * @property {Function} [onError] `onError(message)` for the async load.
 */

/**
 * @typedef {Object} VoiceLoadOptions
 * @property {Function} [onReady] `onReady(voice)`: load on a background thread
 *   and return an AsyncHandle.
 * @property {Function} [onError] `onError(message)`.
 */

/**
 * @typedef {Object} KokoroSynthesizeOptions
 * @property {number} [speed=1]
 * @property {boolean} [trace=false] Attach `stages` to the result.
 * @property {Function} [onDone] `onDone(result, { cancelled, error? })` — the
 *   async forms (`bro.tts.synthesize(kokoro, …)`, `session.synthesize`).
 */

/**
 * @typedef {Object} KokoroSynthesizeResult
 * @property {Float32Array} samples Mono PCM at `sampleRate` (24 kHz).
 * @property {number} sampleRate
 * @property {Int32Array} durations Predicted frames per phoneme id.
 * @property {Array<{name: string, h: number, w: number, data: Float32Array}>} [stages]
 *   The traced pipeline's intermediate tensors (synthesizeTraced / `trace`).
 */

/**
 * @typedef {Object} KokoroStreamOptions
 * @property {number} [speed=1]
 * @property {Function} [onChunk] `onChunk(samples, durations)` per phoneme chunk.
 * @property {Function} [onDone] `onDone(result, info)` with the concatenation.
 */

/**
 * @typedef {KokoroLoadOptions} QwenLoadOptions
 */

/**
 * Qwen3-TTS synthesis options. Which voice knob applies depends on the
 * checkpoint's `variant`: `speaker` (CustomVoice presets, see `speakers()`),
 * `instruct` (VoiceDesign: describe the voice in words), or `xvector` (Base:
 * a speaker embedding from `embedSpeaker`).
 * @typedef {Object} QwenSynthesizeOptions
 * @property {string} [speaker]
 * @property {string} [language='english'] A name from `languages()`.
 * @property {string} [instruct]
 * @property {Float32Array} [xvector] Base: replaces the preset speaker.
 * @property {number} [temperature]
 * @property {number} [topP]
 * @property {number} [topK]
 * @property {number} [repetitionPenalty]
 * @property {number} [adaptive]
 * @property {(number|bigint)} [seed] Sampling seed (a BigInt carries all 64 bits).
 * @property {Object<string, number>} [logitBias] `{ codeId: delta }`, additive
 *   on codebook 0.
 * @property {Float32Array} [voiceSteer]
 * @property {Float32Array} [speakerVector]
 * @property {boolean} [trace=false] Attach `stages` to the result.
 * @property {Function} [onDone] `onDone(result, { cancelled, error? })`, the
 *   async forms.
 */

/**
 * @typedef {Object} QwenSynthesizeResult
 * @property {Float32Array} samples
 * @property {number} sampleRate
 * @property {Array<{name: string, h: number, w: number, data: Float32Array}>} [stages]
 */

/**
 * @typedef {QwenSynthesizeOptions} QwenStreamOptions
 * @property {number} [chunkFrames=25] Codec frames per `onChunk` (~2 s).
 * @property {Function} [onChunk] `onChunk(samples: Float32Array)` per chunk.
 * @property {Function} [onDone] `onDone(result, info)` with the whole buffer.
 */

/**
 * The Qwen 12 Hz codec's codes, codebook-major (`codes[k * numFrames + t]`).
 * @typedef {Object} QwenAudioCodes
 * @property {Int32Array} codes
 * @property {number} numQuantizers
 * @property {number} numFrames
 */

/**
 * HiggsAudio v2 codec codes (the codec OmniVoice speaks), codebook-major
 * (`codes[q * numFrames + t]`), 25 frames/s.
 * @typedef {Object} HiggsCodes
 * @property {Int32Array} codes
 * @property {number} numFrames
 * @property {number} numQuantizers
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
 * masked), `scores` this step's unmask scores, `confidence` this step's raw
 * model confidence — all three `numCodebooks * numFrames` laid out
 * `[q * numFrames + t]`. The copies are made only when `onStep` is set.
 *
 * `scores` and `confidence` are not the same signal:
 *
 * - `scores` is what the step actually ranked cells by: the raw confidence
 *   minus `q * layerPenalty`, then divided by `positionTemperature` with
 *   Gumbel noise added when that is > 0, and `-Infinity` at every cell that is
 *   already fixed. Its range is dominated by the codebook penalty and the
 *   noise, so it says which cell won, not how sure the model was.
 * - `confidence` is the model's raw maximum CFG log-probability at the cell,
 *   before the penalty and before any noise, and it is finite at *every* cell,
 *   already-unmasked ones included (the model predicts every target position on
 *   every forward). This is the signal to colour a heat map with.
 *
 * A long-form `synthesize` restarts the schedule per chunk, so `step` runs
 * `0 … numSteps-1` once per chunk and `chunk` (0-based) / `numChunks` say which
 * one; `generateCodes` and an unchunked `synthesize` always report
 * `chunk: 0, numChunks: 1`.
 *
 * @typedef {Object} OmniVoiceStep
 * @property {number} step Step within this chunk, 0-based.
 * @property {number} numSteps Steps per chunk.
 * @property {number} chunk Long-form chunk index, 0-based.
 * @property {number} numChunks Chunks in this synthesis (1 when unchunked).
 * @property {number} numFrames Frames in THIS chunk.
 * @property {number} numCodebooks
 * @property {number} unmasked Cells fixed by this step.
 * @property {Int32Array} tokens
 * @property {Float32Array} scores
 * @property {Float32Array} confidence Raw max log-prob per cell, no penalty,
 *   no noise; finite everywhere.
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
 * @property {Float32Array} confidence numCodebooks * numFrames raw confidence
 *   (see {@link OmniVoiceStep}), sampled at the step each cell was committed,
 *   so it pairs cell-for-cell with `unmaskStep`. A cell an init grid kept is
 *   never committed and carries the last step's value instead.
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
 * @typedef {KokoroLoadOptions} SupertonicLoadOptions
 */

/**
 * @typedef {Object} SupertonicSynthesizeOptions
 * @property {SupertonicVoice} voice Required.
 * @property {string} [language='en']
 * @property {number} [steps=8] Flow-matching steps (>= 1).
 * @property {number} [speed=1.05]
 * @property {(number|bigint)} [seed=0]
 * @property {number} [guidance=3]
 * @property {boolean} [longForm=false] Split long text and join with gaps.
 * @property {number} [gapSeconds=0.3] Silence between long-form pieces.
 * @property {Function} [onDone] `onDone(result, info)`, the async form
 *   (`bro.tts.synthesize(supertonic, …)`). Not cancellable mid-run.
 */

/**
 * @typedef {Object} SupertonicSynthesizeResult
 * @property {Float32Array} samples
 * @property {number} sampleRate
 */

/**
 * @typedef {KokoroLoadOptions} SpeakerEncoderLoadOptions
 */

/**
 * @typedef {Object} SpeakerEncoderEmbedOptions
 * @property {number} [sampleRate=24000] Rate of the samples.
 * @property {Function} [onDone] `onDone(embedding: Float32Array)`: embed on a
 *   background thread and return an AsyncHandle.
 * @property {Function} [onError] `onError(message)`.
 */

/**
 * The English G2P assets. `root` (or setAssetRoot) points at the
 * brosoundml-data layout; the others override single files.
 * @typedef {Object} TtsAssetsOptions
 * @property {string} [root]
 * @property {string} [lexicon]
 * @property {string} [pos] POS-tagger weights (also accepted as `posTagger`).
 * @property {string} [kokoroConfig] A Kokoro config.json (the phoneme vocab).
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * Kokoro-82M: phoneme ids in, 24 kHz speech out. Single-owner: one op at a
 * time per model (sessions share the gate); the sync methods refuse while an
 * async op is in flight. Returned by `bro.tts.loadKokoro`.
 */
class KokoroModel {

  /** @readonly @type {boolean} */ loaded;
  /** @readonly @type {string} */ device;
  /** @readonly @type {number} */ sampleRate;
  /** @readonly @type {number} */ nTokens;
  /** @readonly @type {number} */ hiddenDim;
  /** @readonly @type {number} */ styleDim;
  /** @readonly @type {number} */ nLayer;

  /**
   * The codepoint -> id stage only (no G2P): IPA in, Kokoro ids out. For
   * English text use `bro.tts.phonemize`.
   * @param {string} ipa
   * @returns {Int32Array}
   */
  encodePhonemes(ipa) {}

  /**
   * The phoneme vocabulary, `{ phoneme: id }`.
   * @returns {Object<string, number>}
   */
  vocab() {}

  /**
   * Load a voice pack (`voices/<name>.bin`).
   * @param {string} path
   * @param {VoiceLoadOptions} [opts]
   * @returns {(Voice|AsyncHandle)}
   */
  loadVoice(path, opts) {}

  /**
   * A voice from raw style floats: `styleDim*2` values broadcast across the
   * rows, or a whole `rows * cols` table (e.g. a blend of `voice.data`s).
   * @param {(Float32Array|Array<number>)} data
   * @param {string} [name='custom']
   * @returns {Voice}
   */
  createVoice(data, name) {}

  /**
   * Blocking synthesis. For a background synthesis use
   * `bro.tts.synthesize(kokoro, ids, voice, { onDone })`.
   * @param {(Int32Array|Array<number>)} phonemeIds
   * @param {Voice} voice
   * @param {{speed?: number}} [opts]
   * @returns {KokoroSynthesizeResult}
   */
  synthesize(phonemeIds, voice, opts) {}

  /**
   * As synthesize, plus `stages` (the pipeline's intermediate tensors).
   * @param {(Int32Array|Array<number>)} phonemeIds
   * @param {Voice} voice
   * @param {{speed?: number}} [opts]
   * @returns {KokoroSynthesizeResult}
   */
  synthesizeTraced(phonemeIds, voice, opts) {}

  /**
   * Re-run the decoder from edited prosody (the traced `asr`, `F0` and `N`
   * stages): the prosody-editing seam. `F0` and `N` hold `2 * total` values.
   * Blocking; `bro.tts.decodeFrom(kokoro, …)` is the background form.
   * @param {Voice} voice
   * @param {Float32Array} asr
   * @param {Float32Array} F0
   * @param {Float32Array} N
   * @param {number} nPhonemes
   * @param {{trace?: boolean}} [opts]
   * @returns {{samples: Float32Array, sampleRate: number, stages?: Array}}
   */
  decodeFrom(voice, asr, F0, N, nPhonemes, opts) {}

  /**
   * @param {Voice} [voice] May also be given later, per synthesize or setVoice.
   * @returns {KokoroSession}
   */
  createSession(voice) {}

}

class Voice {

  /** @readonly @type {boolean} */ loaded;
  /** @readonly @type {string} */ name;
  /** @readonly @type {number} */ rows;
  /** @readonly @type {number} */ cols;

  /**
   * The full style table, row-major `rows * cols`, for blending / perturbing
   * and feeding back through `kokoro.createVoice()`.
   * @readonly
   * @type {Float32Array}
   */
  data;

}

class KokoroSession {

  /** @readonly @type {boolean} */ loaded;

  /**
   * Background synthesis; `opts.onDone(result, info)`.
   * @param {(Int32Array|Array<number>)} phonemeIds
   * @param {Voice} [voice] Required unless the session already has one.
   * @param {KokoroSynthesizeOptions} [opts]
   * @returns {AsyncHandle}
   */
  synthesize(phonemeIds, voice, opts) {}

  /** @param {Voice} voice */
  setVoice(voice) {}

  /** A no-op: Kokoro sessions carry no decode state. */
  reset() {}

}

/**
 * Qwen3-TTS (12 Hz codec). `variant` is `'base'` (voice clones from an
 * x-vector), `'customvoice'` (preset speakers) or `'voicedesign'` (instruct
 * voices), from the checkpoint. Single-owner like every model. Returned by
 * `bro.tts.loadQwen`.
 */
class QwenTtsModel {

  /** @readonly @type {boolean} */ loaded;
  /** @readonly @type {string} */ device;
  /** @readonly @type {number} */ sampleRate;
  /** @readonly @type {string} */ variant;
  /** e.g. `'0.6B'`. @readonly @type {string} */ modelSize;

  /**
   * Blocking synthesis. `bro.tts.synthesize(qwen, text, { onDone })` is the
   * background form.
   * @param {string} text
   * @param {QwenSynthesizeOptions} [opts]
   * @returns {QwenSynthesizeResult}
   */
  synthesize(text, opts) {}

  /**
   * Base variant: clone the voice of a reference clip, blocking.
   * @param {string} text
   * @param {(string|Float32Array|SttAudioBuffer)} ref A WAV path, samples at
   *   `opts.sampleRate` (default 24000), or `{ samples, sampleRate }`.
   * @param {QwenSynthesizeOptions & {sampleRate?: number}} [opts]
   * @returns {QwenSynthesizeResult}
   */
  synthesizeClone(text, ref, opts) {}

  /**
   * Base variant: synthesize from a speaker embedding (`embedSpeaker`).
   * @param {string} text
   * @param {Float32Array} xvector
   * @param {QwenSynthesizeOptions} [opts]
   * @returns {QwenSynthesizeResult}
   */
  synthesizeFromXvector(text, xvector, opts) {}

  /**
   * Base variant: the speaker embedding of a clip.
   * @param {(Float32Array|SttAudioBuffer)} audio
   * @param {(number|{sampleRate: number})} [sampleRate=24000]
   * @returns {Float32Array}
   */
  embedSpeaker(audio, sampleRate) {}

  /** CustomVoice preset names. @returns {string[]} */
  speakers() {}

  /** @returns {string[]} */
  languages() {}

  /**
   * A preset speaker's dialect tag (`'sichuan_dialect'`, `'beijing_dialect'`),
   * or `''`.
   * @param {string} name
   * @returns {string}
   */
  speakerDialect(name) {}

  /** @returns {QwenTtsSession} */
  createSession() {}

  /**
   * @param {(Float32Array|SttAudioBuffer)} audio
   * @param {(number|{sampleRate: number})} [sampleRate=24000]
   * @returns {QwenAudioCodes}
   */
  encodeAudio(audio, sampleRate) {}

  /**
   * @param {(Int32Array|Array<number>)} codes Codebook-major.
   * @param {number} [numQuantizers] Defaults to the codec's.
   * @param {number} [numFrames] Defaults to `codes.length / numQuantizers`.
   * @returns {{samples: Float32Array, sampleRate: number}}
   */
  decodeCodes(codes, numQuantizers, numFrames) {}

}

class QwenTtsSession {

  /** @readonly @type {boolean} */ loaded;
  /** @readonly @type {string} */ variant;

  /**
   * Background synthesis; `opts.onDone(result, info)`.
   * @param {string} text
   * @param {QwenSynthesizeOptions} [opts]
   * @returns {AsyncHandle}
   */
  synthesize(text, opts) {}

  reset() {}

}

/**
 * The HiggsAudio v2 tokenizer alone — 24 kHz mono PCM <-> 8-codebook RVQ codes
 * at 25 frames/s (a frame is 960 samples) — without OmniVoice's language
 * model. OmniVoice's `encodeAudio` / `decodeCodes` are the same codec, but need
 * the full OmniVoice load. encode / decode are synchronous; one op at a time
 * per handle. Returned by `bro.tts.loadHiggsCodec`.
 */
class HiggsCodec {

  /** @readonly @type {boolean} */ loaded;
  /** @readonly @type {string} */ device;
  /** False when loaded with `decoderOnly` (encode then throws). @readonly @type {boolean} */ hasEncoder;
  /** @readonly @type {boolean} */ busy;
  /** @readonly @type {number} */ sampleRate;
  /** @readonly @type {number} */ hopLength;
  /** @readonly @type {number} */ frameRate;
  /** @readonly @type {number} */ numQuantizers;
  /** @readonly @type {number} */ codebookSize;

  /**
   * PCM -> codes. Resampled to 24 kHz mono and right-padded to a whole frame.
   * @param {(Float32Array|SttAudioBuffer)} samples
   * @param {(number|{sampleRate: number})} [sampleRate=24000]
   * @returns {HiggsCodes}
   */
  encode(samples, sampleRate) {}

  /**
   * Codes -> 24 kHz PCM, `numFrames * 960` samples. Fewer RVQ levels than the
   * codec's give coarser audio.
   * @param {(Int32Array|Array<number>)} codes Codebook-major, each in
   *   `[0, codebookSize)`.
   * @param {(number|{numFrames?: number, numQuantizers?: number})} [numFramesOrOpts]
   *   `numQuantizers` defaults to the codec's, `numFrames` to
   *   `codes.length / numQuantizers`.
   * @returns {{samples: Float32Array, sampleRate: number}}
   */
  decode(codes, numFramesOrOpts) {}

  /** Release the weights now rather than at GC; refused while an op runs. */
  dispose() {}

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

/** Supertonic-3: flow-matching multilingual TTS, text in (no phoneme step). */
class SupertonicModel {

  /** @readonly @type {boolean} */ loaded;
  /** @readonly @type {string} */ device;
  /** @readonly @type {number} */ sampleRate;

  /**
   * A voice preset, `voice_styles/<name>.json`.
   * @param {string} path
   * @returns {SupertonicVoice}
   */
  loadVoiceStyle(path) {}

  /**
   * A voice from raw style matrices (e.g. a blend of two voices' `ttl`/`dp`).
   * @param {Float32Array} ttl 50 * 256 floats.
   * @param {Float32Array} dp 8 * 16 floats.
   * @param {string} [name='custom']
   * @returns {SupertonicVoice}
   */
  createVoice(ttl, dp, name) {}

  /**
   * Blocking synthesis; `bro.tts.synthesize(supertonic, text, opts)` is the
   * background form.
   * @param {string} text
   * @param {SupertonicSynthesizeOptions} opts `voice` is required.
   * @returns {SupertonicSynthesizeResult}
   */
  synthesize(text, opts) {}

}

class SupertonicVoice {

  /** @readonly @type {boolean} */ loaded;
  /** @readonly @type {string} */ name;
  /** Row-major 50x256 style matrix. @readonly @type {Float32Array} */ ttl;
  /** Row-major 8x16 duration style matrix. @readonly @type {Float32Array} */ dp;
  /** @readonly @type {number} */ ttlRows;
  /** @readonly @type {number} */ ttlCols;
  /** @readonly @type {number} */ dpRows;
  /** @readonly @type {number} */ dpCols;

}

/**
 * The ECAPA-TDNN speaker encoder, standalone. It places its conv stack on
 * brotensor's default device, which `device` reports.
 */
class SpeakerEncoder {

  /** @readonly @type {boolean} */ loaded;
  /** @readonly @type {string} */ device;
  /** Embedding width. @readonly @type {number} */ encDim;
  /** @readonly @type {number} */ sampleRate;

  /**
   * The speaker embedding of a clip: blocking, or in the background with
   * `opts.onDone`. `embed` is the same call.
   * @param {(Float32Array|SttAudioBuffer)} audio
   * @param {(number|SpeakerEncoderEmbedOptions)} [opts] A sample rate, or options.
   * @returns {(Float32Array|AsyncHandle)}
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
 * Load the HiggsAudio v2 codec alone from an `audio_tokenizer/` directory
 * (config.json + model.safetensors) — e.g. OmniVoice's
 * `<model>/audio_tokenizer`.
 * @example
 *   const codec = bro.tts.loadHiggsCodec('../brosoundml/weights/omnivoice/audio_tokenizer');
 *   const { codes, numFrames } = codec.encode(samples, 24000);
 *   const { samples: back } = codec.decode(codes, numFrames);
 * @param {string} dir
 * @param {KokoroLoadOptions & {decoderOnly?: boolean}} [opts] `decoderOnly`
 *   skips the encoder + HuBERT (encode then throws).
 * @returns {(HiggsCodec|AsyncHandle)}
 */
bro.tts.loadHiggsCodec = function(dir, opts) {};

/**
 * English text -> Kokoro phoneme ids through the built-in G2P (lexicon + POS
 * tagger + morphology), built lazily on first use from the assets set by
 * setAssetRoot / setAssets (else the sibling brosoundml-data layout).
 * @param {string} text
 * @returns {Int32Array}
 */
bro.tts.phonemize = function(text) {};

/**
 * Root of the G2P assets (resets any setAssets overrides and the built G2P).
 * @param {string} dir
 */
bro.tts.setAssetRoot = function(dir) {};

/**
 * @param {TtsAssetsOptions} opts
 */
bro.tts.setAssets = function(opts) {};

/**
 * Background synthesis for any TTS model, dispatched on its class; returns an
 * AsyncHandle and reports through `opts.onDone(result, { cancelled, error? })`.
 *   `synthesize(kokoro, phonemeIds, voice, opts?)` — KokoroSynthesizeOptions.
 *   `synthesize(qwen, text, opts?)` — QwenSynthesizeOptions.
 *   `synthesize(supertonic, text, opts)` — SupertonicSynthesizeOptions (`voice` required).
 *   `synthesize(omni, text, opts?)` — OmniVoiceSynthesizeOptions.
 * One op per model at a time; a second call while one is in flight throws.
 * @param {(KokoroModel|QwenTtsModel|SupertonicModel|OmniVoice)} model
 * @param {((Int32Array|Array<number>)|string)} textOrPhonemes
 * @param {(Voice|Object)} [voiceOrOpts]
 * @param {Object} [opts]
 * @returns {AsyncHandle}
 */
bro.tts.synthesize = function(model, textOrPhonemes, voiceOrOpts, opts) {};

/**
 * Streaming background synthesis:
 *   `synthesizeStream(kokoro, phonemeChunks, voice, opts?)` — an array of id
 *     chunks (or one flat id array); KokoroStreamOptions.
 *   `synthesizeStream(qwen, text, opts?)` — QwenStreamOptions.
 * @param {(KokoroModel|QwenTtsModel)} model
 * @param {(Array<Int32Array>|Int32Array|string)} textOrChunks
 * @param {(Voice|Object)} [voiceOrOpts]
 * @param {Object} [opts]
 * @returns {AsyncHandle}
 */
bro.tts.synthesizeStream = function(model, textOrChunks, voiceOrOpts, opts) {};

/**
 * Background form of `KokoroModel.decodeFrom`; `opts.onDone(result, info)`,
 * `opts.trace` attaches `stages`.
 * @param {KokoroModel} kokoro
 * @param {Voice} voice
 * @param {Float32Array} asr
 * @param {Float32Array} F0
 * @param {Float32Array} N
 * @param {number} nPhonemes
 * @param {{trace?: boolean, onDone?: Function}} [opts]
 * @returns {AsyncHandle}
 */
bro.tts.decodeFrom = function(kokoro, voice, asr, F0, N, nPhonemes, opts) {};

/**
 * The class objects, for `instanceof` and prototype inspection. None is
 * constructible; instances come from the loaders.
 * @type {Function}
 */
bro.tts.KokoroModel; bro.tts.Voice; bro.tts.KokoroSession; bro.tts.QwenTtsModel;
bro.tts.QwenTtsSession; bro.tts.SupertonicModel; bro.tts.SupertonicVoice;
bro.tts.SpeakerEncoder; bro.tts.HiggsCodec;

