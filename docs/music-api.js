/**
 * =============================================================================
 * bro.music — YuE2 song generation and the YuE2 VAE
 * =============================================================================
 *
 * YuE2-3B (m-a-p) writes a song from lyrics and a style. It is one
 * AR–NAR Mixture-of-Transformers backbone (Qwen3-shaped: 2048 hidden,
 * 28 layers) run in three stages, then a VAE:
 *
 *   1. plan        AR sampling of an ABC score (cot 'full' / 'melody';
 *                  skipped for cot 'off' or when opts.abc is supplied)
 *   2. semantic    AR sampling of codec tokens (25 per second), with CFG
 *   3. synthesize  NAR flow matching: a midpoint ODE (odeSteps) turns noise
 *                  into 64-dim latents, 25 frames per second
 *   4. decode      the YuE2 VAE: latents -> 48 kHz stereo
 *
 * generate() runs all of it. plan() / semantic() / synthesize() are the same
 * stages one at a time, so an app can edit the ABC, re-roll the semantic
 * tokens with another seed, or re-synthesize with other ODE settings.
 *
 * Pass a VAE as opts.vae and the job decodes as it goes: latent frames are
 * decoded in tiles as soon as each tile's right-hand context exists, and
 * onAudio receives each chunk, so playback can start before the song is
 * done. The tiles are exact (each carries vae.requiredHalo() frames of
 * context on either side and is cropped to its core), so the chunks put end
 * to end equal a one-pass decode.
 *
 * In the default protocol mode (narChunkFrames 0) the NAR synthesizes the
 * whole song in one bidirectional chunk after the last semantic token, so
 * the first audio arrives after the semantic stage. narChunkFrames > 0
 * synthesizes windows as their tokens arrive (lower latency, a quality
 * trade-off).
 *
 * Weights: the YuE2-3B folder (config.json, model.safetensors, qwen.tiktoken;
 * BF16, CC BY-NC 4.0) and the YuE2-Vae folder (config.json,
 * model.safetensors; FP32). Both load on the GPU by default. Every model runs
 * one op at a time (`busy`); a generate() with opts.vae holds both.
 *
 * Steering and capture work on the residual stream after a decoder layer,
 * on the AR path (plan / semantic) or the NAR path (synthesis): opts.steer
 * adds vectors, and opts.capture returns hidden states in result.hidden. That
 * is how musical attributes are located inside the model and pushed around.
 *
 * Audio is planar: samples holds channel 0's `frames` samples, then
 * channel 1's.
 *
 * @example
 *   // A whole song, played as it is decoded.
 *   const yue = bro.music.loadYuE2(bro.models.find('YuE2-3B'));
 *   const vae = bro.music.loadVae(bro.models.find('yue2-vae'));
 *   const ctx = new AudioContext({ sampleRate: 48000 });
 *   let at = 0;
 *   yue.generate({
 *     style: 'indie folk, warm acoustic guitar, female vocal, 92 bpm',
 *     lyrics: '[verse]\nMorning light on the river\n...\n[chorus]\n...',
 *     vae,
 *     onAbc: (text) => abcView.textContent += text,
 *     onProgress: (p) => bar.value = p.done / p.total,
 *     onAudio: (c) => {
 *       const buf = ctx.createBuffer(2, c.frames, c.sampleRate);
 *       buf.copyToChannel(c.samples.subarray(0, c.frames), 0);
 *       buf.copyToChannel(c.samples.subarray(c.frames), 1);
 *       const src = ctx.createBufferSource();
 *       src.buffer = buf;
 *       src.connect(ctx.destination);
 *       at = Math.max(at, ctx.currentTime + 0.05);
 *       src.start(at);
 *       at += buf.duration;
 *     },
 *     onDone: (r, info) => console.log(info.cancelled ? 'stopped' : 'done', r.timing),
 *   });
 *
 * @example
 *   // Stage by stage: plan, edit the score, then the rest.
 *   yue.plan({ style, lyrics, onDone: (r) => {
 *     const plan = r.plan;
 *     plan.abc = editAbc(plan.abc);   // or pass opts.abc to skip planning
 *     yue.semantic(plan, { style, lyrics, abc: plan.abc, seed: 7, onDone: (s) =>
 *       yue.synthesize(s.plan, s.semantic, { odeSteps: 16, vae, onAudio: play }) });
 *   }});
 *
 * @example
 *   // Re-decode stored latents with streaming playback.
 *   vae.decode(latents, { coreFrames: 25, onChunk: play, onDone: () => {} });
 */

// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * One AR stage's sampling. Defaults differ per stage: the semantic stage's are
 * shown; abcSampling defaults to { temperature 0.7, topP 0.9, topK 30,
 * repetitionPenalty 1.005, penaltyWindow 100, minTokens 32, maxTokens 4096 }.
 * @typedef {Object} YuE2Sampling
 * @property {number} [temperature=1] - 0 = greedy
 * @property {number} [topP=0.95]
 * @property {number} [topK=100]
 * @property {number} [repetitionPenalty=1.2] - Windowed and sign-aware (a negative logit is multiplied, a positive one divided)
 * @property {number} [penaltyWindow=50] - How many recent tokens the penalty sees (1..100)
 * @property {number} [minTokens=200] - The end token is masked before this many
 * @property {number} [maxTokens=9000] - The stage stops here (result.semanticTruncated / plan.truncated)
 */

/**
 * A residual-stream steering vector.
 * @typedef {Object} YuE2Steer
 * @property {number} layer - Decoder layer 0..27; the delta is added to its output
 * @property {Float32Array} delta - 2048 floats, or rows * 2048 with perPosition
 * @property {boolean} [nar=false] - false: the AR path (plan / semantic); true: the NAR path (synthesis)
 * @property {number} [scale=1]
 * @property {boolean} [perPosition=false] - Row r is added at position pos0 + r (AR: token position in the stage's sequence; NAR: latent frame)
 * @property {number} [pos0=0]
 * @property {boolean} [plan=true] - AR: apply during the plan stage
 * @property {boolean} [semantic=true] - AR: apply during the semantic stage
 */

/**
 * Which hidden states to capture (returned in result.hidden).
 * @typedef {Object} YuE2Capture
 * @property {number[]} layers - Layers whose output is captured (AR and NAR)
 * @property {boolean} [plan=true] - Capture during the plan stage
 * @property {boolean} [semantic=true] - Capture during the semantic stage (the conditional branch)
 * @property {number[]} [narEvals] - NAR velocity evaluations to capture, counted per chunk from 0 (two per ODE step: step s is evals 2s and 2s+1); empty = none
 */

/**
 * Options for generate / plan / semantic / synthesize.
 * @typedef {Object} YuE2Options
 * @property {string} [style] - The "[Tags]" text: genre, instruments, voice, tempo...
 * @property {string} [lyrics] - The "[Lyrics]" text, with section tags like [verse] / [chorus]
 * @property {'full'|'melody'|'off'} [cot='full'] - What the plan stage writes: a full ABC score, a melody only, or nothing
 * @property {string} [abc] - Use this ABC score and skip planning
 * @property {number|bigint} [seed=831001] - Each AR stage reseeds with it; the NAR noise too
 * @property {number} [cfgScale] - Semantic CFG scale; below 0 = the protocol default (1.0, or 1.01 for cot 'off')
 * @property {YuE2Sampling} [abcSampling]
 * @property {YuE2Sampling} [semanticSampling]
 * @property {number} [odeSteps=32] - Midpoint ODE steps per NAR chunk
 * @property {number} [narChunkFrames=0] - 0 = the protocol's chunks (whole song); > 0 = streaming windows of this many frames
 * @property {boolean} [runSemantic=true] - generate(): false stops after the plan
 * @property {boolean} [runSynthesize=true] - generate(): false stops after the semantic tokens
 * @property {boolean} [reusePrefixKv=true] - generate(): reuse the semantic stage's KV cache for synthesis instead of prefilling again
 * @property {number} [abcDraftTokens=0] - Speculative ABC planning: 1..7 tokens drafted per forward by prompt lookup
 *   (the score repeats itself) and checked with standard speculative acceptance, so the plan's distribution is
 *   exactly that of no drafting; a given seed draws a different plan than at 0 (greedy gives the same one). 5 is a
 *   good value: ~2.3-2.6 tokens per forward, the plan ~200 -> ~400 tok/s in BF16 (~540 with arWeights 'int8'), and
 *   cot 'full' time to first audio streaming 10.1 s -> 6.7 s (5.0 s with int8) on a 4090. Ignored with cfgScale
 *   (the plan has no CFG). timing.planForwards counts the forwards.
 * @property {YuE2Steer[]} [steer]
 * @property {YuE2Capture} [capture]
 * @property {YuE2Vae} [vae] - Decode the latents as they arrive (onAudio, result.audio)
 * @property {number} [decodeCoreFrames=50] - With vae: the largest decoded tile (smaller = more halo recompute)
 * @property {number} [decodeFirstFrames=8] - With vae: the first tile. Tiles grow from it: the first is decoded as soon
 *   as this many frames have their right context (halo frames past them), each later one waits until it can take twice
 *   the previous tile, up to decodeCoreFrames. Streamed in 50-frame NAR windows, the first window gives a 38-frame
 *   tile at once (~60 ms on a 4090), so audio starts right after the first latents. Output is exact for any tiling.
 * @property {number} [decodeHaloFrames] - With vae: context frames per side (at least vae.requiredHalo())
 * @property {function(string, Int32Array)} [onAbc] - ABC text as it is sampled, batched per tick, with its token ids (never split inside a UTF-8 character)
 * @property {function(Int32Array)} [onSemantic] - Codec ids (0..32767) as they are sampled, batched per tick
 * @property {function({frame0: number, frames: number, latents: Float32Array})} [onLatents] - Final latent frames, contiguous from 0, frame-major (frames * 64)
 * @property {function(YuE2AudioChunk)} [onAudio] - With vae: each decoded chunk, contiguous from sample 0
 * @property {function({stage: string, done: number, total: number})} [onProgress] - Tokens so far / budget per AR stage, ODE steps done / total for 'synthesize'
 * @property {function(YuE2Result, {cancelled: boolean, error: string})} [onDone] - Exactly once, last
 */

/**
 * The plan stage's output; pass it back to semantic() / synthesize().
 * @typedef {Object} YuE2Plan
 * @property {string} abc - The ABC score ('' for cot 'off')
 * @property {Int32Array} abcIds - Its token ids
 * @property {Int32Array} prefix - The token prefix the semantic stage continues
 * @property {boolean} truncated - Planning hit maxTokens before the score ended
 */

/**
 * @typedef {Object} YuE2Timing
 * @property {number} planSeconds
 * @property {number} semanticSeconds
 * @property {number} synthSeconds
 * @property {number} planTtft - Seconds to the first plan token after prefill
 * @property {number} semanticTtft
 * @property {number} planTokens
 * @property {number} semanticTokens
 * @property {number} planTps - Tokens per second
 * @property {number} planForwards - AR forwards the plan took; planTokens / planForwards is the tokens each
 *   forward accepted (1 without abcDraftTokens)
 * @property {number} semanticTps
 * @property {number} firstLatentSeconds - generate(): wall time to the first final latent frame
 * @property {number} narSecondsPerAudioSecond - generate(): NAR cost per second of audio
 */

/**
 * A captured block of hidden states.
 * @typedef {Object} YuE2Hidden
 * @property {'plan'|'semantic'|'synthesize'} stage
 * @property {number} layer
 * @property {number} pos0 - First token position (AR) or latent frame (NAR)
 * @property {number} rows - Positions in data
 * @property {number} hidden - Floats per row (2048)
 * @property {number} narEval - NAR: the velocity evaluation; -1 on the AR path
 * @property {Float32Array} data - rows * hidden
 */

/**
 * @typedef {Object} YuE2Result
 * @property {YuE2Plan} plan
 * @property {string} abc - Same as plan.abc
 * @property {Int32Array} semantic - Codec ids
 * @property {boolean} semanticTruncated
 * @property {Float32Array} latents - frames * 64, frame-major
 * @property {number} frames
 * @property {YuE2Timing} timing
 * @property {boolean} cancelled
 * @property {YuE2Audio} [audio] - With vae: the whole decoded song
 * @property {YuE2Hidden[]} [hidden] - With capture
 */

/**
 * Planar stereo PCM.
 * @typedef {Object} YuE2Audio
 * @property {Float32Array} samples - channels * frames, planar (channel 0 then channel 1)
 * @property {number} channels - 2
 * @property {number} frames - Samples per channel
 * @property {number} sampleRate - 48000
 */

/**
 * A streamed chunk: YuE2Audio plus where it starts.
 * @typedef {Object} YuE2AudioChunk
 * @property {Float32Array} samples
 * @property {number} channels
 * @property {number} frames
 * @property {number} sampleRate
 * @property {number} sample0 - Offset of the chunk's first sample in the song
 */

/**
 * @typedef {Object} YuE2VaeDecodeOptions
 * @property {number} [coreFrames] - Tile the decode with this many frames per tile (exact); 0 = one pass (or 50 when streaming)
 * @property {number} [haloFrames] - Context per side; at least requiredHalo(coreFrames)
 * @property {number} [firstFrames=8] - With onChunk: the first tile; tiles double from it up to coreFrames, so the
 *   first chunk arrives after one small tile instead of a full core
 * @property {function(YuE2AudioChunk)} [onChunk] - Stream: chunks in order as tiles finish (async)
 * @property {function({done: number, total: number})} [onProgress] - Tiles done (async, without onChunk)
 * @property {function(YuE2Audio|undefined, {cancelled: boolean, error: string})} [onDone] - Async; the result is undefined when streaming
 */

/**
 * @typedef {Object} YuE2LoadOptions
 * @property {string} [device] - 'cuda', 'vulkan', 'metal' or 'cpu'; defaults to the GPU
 * @property {boolean} [decoderOnly=false] - loadVae: skip the encoder
 * @property {string} [arWeights='bf16'] - loadYuE2: the weights the one-token AR decode (plan + semantic sampling)
 *   reads. 'int8' / 'fp8' keep per-output-row quantised copies of every layer's AR projections and lm_head (~1.9 GB
 *   more VRAM) and roughly halve the bytes each token reads: the ABC plan goes from ~200 to ~314 tok/s on a 4090
 *   (cot 'full' time to first latent ~10.1 s -> ~8.1 s). Prefill, synthesis and the VAE stay BF16. 'int8' is the
 *   closer of the two (a sampled token differs from BF16's distribution ~0.3% of the time in the plan, ~1.5% in
 *   semantic sampling; fp8 0.4% / 2.5%) at the same speed.
 * @property {Function} [onReady] - Load on a worker thread; called with the model
 * @property {Function} [onError]
 */

// ── Classes ──────────────────────────────────────────────────────────────────

/**
 * A loaded YuE2-3B backbone.
 */
class YuE2 {
  /** @readonly @type {boolean} */ loaded;
  /** @readonly @type {string} */ device;
  /** True while an op runs on this model. @readonly @type {boolean} */ busy;
  /** The AR decode weights it was loaded with: 'bf16' | 'int8' | 'fp8'. @readonly @type {string} */ arWeights;
  /** 64. @readonly @type {number} */ latentDim;
  /** Latent frames per second (25). @readonly @type {number} */ frameRate;

  /**
   * The whole pipeline. Cancel with handle.cancel(): the job stops at the next
   * token or ODE step, and onDone gets what exists with cancelled = true.
   * @param {YuE2Options} opts
   * @returns {{cancel: function(), wait: function()}} AsyncHandle
   */
  generate(opts) {}

  /**
   * The plan stage only; onDone's result.plan is the score.
   * @param {YuE2Options} opts
   * @returns {{cancel: function(), wait: function()}} AsyncHandle
   */
  plan(opts) {}

  /**
   * The semantic stage from a plan; onDone's result.semantic.
   * @param {YuE2Plan} plan
   * @param {YuE2Options} opts
   * @returns {{cancel: function(), wait: function()}} AsyncHandle
   */
  semantic(plan, opts) {}

  /**
   * The NAR stage from a plan and semantic ids; onDone's result.latents (and
   * result.audio with opts.vae).
   * @param {YuE2Plan} plan
   * @param {Int32Array} semantic
   * @param {YuE2Options} opts
   * @returns {{cancel: function(), wait: function()}} AsyncHandle
   */
  synthesize(plan, semantic, opts) {}

  /** @param {string} text @returns {Int32Array} */
  tokenize(text) {}

  /** @param {Int32Array} ids @returns {string} */
  detokenize(ids) {}
}

/**
 * The YuE2 VAE: 48 kHz stereo <-> 64-dim latents at 25 frames per second
 * (1920 samples a frame). A decode of T frames is 1920*T - 64 samples per
 * channel.
 */
class YuE2Vae {
  /** @readonly @type {boolean} */ loaded;
  /** @readonly @type {string} */ device;
  /** @readonly @type {boolean} */ busy;
  /** False when loaded decoderOnly. @readonly @type {boolean} */ hasEncoder;
  /** 48000. @readonly @type {number} */ sampleRate;
  /** 2. @readonly @type {number} */ channels;
  /** 64. @readonly @type {number} */ latentDim;
  /** 1920. @readonly @type {number} */ downsamplingRatio;
  /** 25. @readonly @type {number} */ frameRate;

  /**
   * The smallest halo that makes a tile of coreFrames exact (12).
   * @param {number} [coreFrames]
   * @returns {number}
   */
  requiredHalo(coreFrames) {}

  /**
   * Samples per channel decoded from `frames` latents.
   * @param {number} frames
   * @returns {number}
   */
  outputLength(frames) {}

  /**
   * Latents (frame-major, frames * 64) to audio. Sync without onDone /
   * onChunk; otherwise an AsyncHandle.
   * @param {Float32Array} latents
   * @param {YuE2VaeDecodeOptions} [opts]
   * @returns {YuE2Audio|{cancel: function(), wait: function()}}
   */
  decode(latents, opts) {}

  /**
   * Audio to posterior-mean latents (for covers and edits). `audio` is planar
   * { samples, channels, sampleRate }, or mono (a Float32Array at 48 kHz, or
   * { samples, sampleRate }); other rates are resampled, and mono feeds both
   * channels. Encodes in exact tiles, so a whole song fits on the GPU. Sync
   * without opts.onDone.
   * @param {YuE2Audio|Float32Array|{samples: Float32Array, sampleRate: number}} audio
   * @param {{onDone?: function({latents: Float32Array, frames: number}|undefined, {cancelled: boolean, error: string})}} [opts]
   * @returns {{latents: Float32Array, frames: number}|{cancel: function(), wait: function()}}
   */
  encode(audio, opts) {}
}

// ── Namespace ────────────────────────────────────────────────────────────────

/** False in a build without BRO_WITH_SOUNDML (every call then throws). @type {boolean} */
bro.music.available;

/** Probe and initialise the tensor backends (idempotent). */
bro.music.init = function() {};

/**
 * Load YuE2-3B from its folder.
 * @param {string} modelDir - config.json, model.safetensors, qwen.tiktoken
 * @param {YuE2LoadOptions} [opts]
 * @returns {YuE2|{cancel: function(), wait: function()}} the model, or an AsyncHandle with opts.onReady
 */
bro.music.loadYuE2 = function(modelDir, opts) {};

/**
 * Load the YuE2 VAE from its folder.
 * @param {string} vaeDir - config.json, model.safetensors
 * @param {YuE2LoadOptions} [opts]
 * @returns {YuE2Vae|{cancel: function(), wait: function()}} the VAE, or an AsyncHandle with opts.onReady
 */
bro.music.loadVae = function(vaeDir, opts) {};
