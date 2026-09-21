/**
 * =============================================================================
 * bro.diffusion — diffusion-model text-to-image inference (brodiffusion)
 * =============================================================================
 *
 * This file documents the CORE surface: the namespace, loading a model, the
 * one-shot generate()/imageToImage()/inpaint() calls, the step-wise
 * prime()/stepOnce()/decode() loop, LoRA, and schedulers.
 *
 * The rest of the surface lives in sibling files:
 *   - docs/diffusion-control-api.js — ControlNet, conditioning-control axes
 *     and control vectors, the Sana identity anchor, attention trace +
 *     steering recipes, the Krea 2 research hooks, and the standalone VAE.
 *   - docs/triposplat-api.js — bro.triposplat (also produced by brodiffusion):
 *     a single image to a 3D Gaussian splat.
 *
 * Six model families are supported; loadModel() auto-detects the family from
 * the directory's `model_index.json` and config().modelClass reports it:
 *   - StableDiffusion — SD1.5: CLIP text encoder + U-Net + VAE, DDIM / LCM /
 *     DPM-Solver schedulers, LoRA (merged), ControlNet, img2img, inpaint,
 *     INT8 (W8A16) quantization.
 *   - Flux — CLIP (pooled) + T5-XXL encoders + Flux DiT + VAE, flow-match
 *     scheduler. txt2img only.
 *   - Sana — Gemma-2 encoder + Linear DiT + DC-AE f32c32 autoencoder (32x
 *     latent, vs 8x for SD/Flux), flow-match (SCM for the guidance-distilled
 *     Sana-Sprint). txt2img plus the identity-anchor seam; no img2img /
 *     inpaint / ControlNet / LoRA.
 *   - PixArt — PixArt-Sigma: T5-XXL + PixArt DiT (AdaLN-single) + SDXL KL-VAE.
 *     txt2img only.
 *   - Krea2 — Qwen3-VL-4B text encoder + single-stream flow DiT + Qwen-Image
 *     VAE decoder. txt2img, runtime-adapter LoRA (live rescale, INT8-safe),
 *     and the krea2* research hooks. No img2img / inpaint / ControlNet.
 *   - QwenImage21 — Qwen-Image 2.1: Qwen3-VL-8B text encoder + a 7.1B
 *     block-causal single-stream DiT (one joint [text ; image] sequence, a
 *     prefix KV cache over the text half) + a 16x RGBA autoencoder. txt2img
 *     and the qwenImage21* research hooks, including a VAE encode/decode seam
 *     and a releasable text encoder. INT8 for both the DiT and the encoder is
 *     the default and the only configuration that fits 1024x1024 on a 24 GB
 *     card. No img2img / inpaint / ControlNet / LoRA.
 *
 * The native Pipeline owns the multi-GB weights. JavaScript never holds or
 * moves weight bytes — it holds an opaque handle (the Pipeline object).
 * Latents and attention maps are small and download to Float32Arrays on
 * demand; only the decoded image crosses into JS as pixel data.
 *
 * bro.diffusion is installed in the main realm AND in every Worker realm (each
 * realm gets its own class objects, so a Pipeline handle never crosses
 * postMessage). Like every bro ML namespace it is CUDA-by-default: brotensor
 * picks CUDA/Metal FP16 when a GPU build has one and falls back to CPU FP32,
 * where a single image takes minutes. Gate any real model load on `bro.gpu`.
 *
 * Weights are not bundled. Each model is a diffusers-format export:
 * model_index.json plus text_encoder/, unet/ (or transformer/), vae/,
 * tokenizer/, scheduler/.
 *
 * @example
 *   // --- Quick start: one image, in a Worker, off the main thread ----------
 *   if (!bro.gpu.available) {
 *     console.warn('No GPU (' + bro.gpu.backend + ') — CPU diffusion is minutes/image.');
 *   }
 *   const pipe = bro.diffusion.loadModel('../brodiffusion/weights/sd15');
 *   const img  = pipe.generate('a cat astronaut, oil painting', {
 *     width: 512, height: 512, steps: 30, guidanceScale: 7.5, seed: 42,
 *   });
 *   if (img.cancelled) throw new Error('generation cancelled');
 *
 *   const cv = document.createElement('canvas');
 *   cv.width = img.width; cv.height = img.height;
 *   const cx = cv.getContext('2d');
 *   const id = cx.createImageData(img.width, img.height);
 *   id.data.set(img.data);                 // Uint8ClampedArray, RGBA (HWC)
 *   cx.putImageData(id, 0, 0);
 *   document.body.appendChild(cv);
 *
 * @example
 *   // --- Step-wise: keep the UI alive, draw every step ----------------------
 *   const st = pipe.prime('a lighthouse at dawn', { steps: 20, seed: 7 });
 *   (function tick() {
 *     if (st.done) { draw(st.decode()); return; }
 *     st.stepOnce();                        // one denoising step per frame
 *     if (st.stepIndex % 4 === 0) draw(st.decode());   // cheap-ish preview
 *     requestAnimationFrame(tick);
 *   })();
 *
 * @example
 *   // --- SD1.5 built by hand, then weights from a 3-file diffusers export ---
 *   const W = '../brodiffusion/weights/sd15';
 *   const pipe = bro.diffusion.createPipeline({
 *     vocabPath:  W + '/tokenizer/vocab.json',
 *     mergesPath: W + '/tokenizer/merges.txt',
 *     scheduler:  'ddim',
 *   });
 *   pipe.loadWeights(
 *     W + '/text_encoder/model.fp16.safetensors',
 *     W + '/unet/diffusion_pytorch_model.fp16.safetensors',
 *     W + '/vae/diffusion_pytorch_model.fp16.safetensors');
 *   const img = pipe.generate('a fox in autumn leaves', { steps: 25 });
 */

// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * GenerateOptions — the `opts` bag shared by generate(), textToImage(),
 * imageToImage(), inpaint(), prime(), setIdentityAnchor() and
 * krea2PrimeFromTaps(). Every key is optional; an absent key keeps the native
 * default. Unknown keys are ignored.
 *
 * Image paths in here (initImagePath / maskImagePath / controls[].imagePath)
 * are passed to brodiffusion VERBATIM — unlike model/LoRA/ControlNet paths
 * they do NOT go through bro's asset-path resolver, so resolve them yourself
 * with bro.resolvePath() if they are app-relative.
 *
 * @typedef {Object} GenerateOptions
 * @property {number}  [width=512]          image width in px (multiple of 8; 32 for Sana's DC-AE)
 * @property {number}  [height=512]         image height in px (same rule)
 * @property {number}  [steps=30]           denoising steps
 * @property {number}  [guidanceScale=7.5]  classifier-free guidance; 1.0 skips the uncond pass
 * @property {string}  [negativePrompt='']  negative prompt
 * @property {number|bigint} [seed=0]       RNG seed for the initial latent noise.
 *           A plain number is exact to 2^53; pass a BigInt for the full 64-bit range.
 * @property {boolean} [includeFp32=false]  also attach the raw NCHW FP32 buffer
 *           (values in [-1,1]) to the result as `fp32`. Read by generate() /
 *           imageToImage() / inpaint() / setIdentityAnchor() / decode() only.
 *
 * @property {string}  [initImagePath]      img2img: VAE-encode this image and noise it
 *           to the right point in the schedule instead of starting from pure Gaussian
 *           noise. Decoded by broimage and resized to width x height. SD1.5 only.
 * @property {number}  [strength=0.8]       0..1 — fraction of the schedule actually
 *           denoised; higher = more freedom from the init. Ignored without initImagePath.
 * @property {boolean} [vaeEncodeSample=false] false = use the VAE mean (deterministic);
 *           true = sample mean + exp(0.5*logvar)*eps from the schedule's Philox stream.
 * @property {string}  [maskImagePath]      inpaint mask; requires initImagePath. Decoded
 *           1-channel and nearest-resized to latent dims: white (>=128) = inpaint,
 *           black = keep. SD1.5 only.
 *
 * @property {Array<{imagePath:string, scale?:number, startStep?:number, endStep?:number}>} [controls]
 *           one entry per registered ControlNet, in addControlNet() order. Defaults per
 *           entry: scale 1.0, startStep 0.0, endStep 1.0 (half-open window on schedule
 *           fraction). See docs/diffusion-control-api.js.
 *
 * @property {string} [noiseSource='internal'] 'internal' (brotensor Philox) or 'torch'
 *           (bit-compatible with a seeded torch.randn reference run). Ignored when
 *           initNoise or initImagePath is set.
 * @property {Float32Array} [initNoise]     explicit initial latent noise in raw N(0,1)
 *           units, NCHW flat, length C_lat*(height/8)*(width/8). Overrides noiseSource;
 *           the scheduler's init_noise_sigma is still applied on top. Cannot be combined
 *           with initImagePath.
 */

/**
 * ImageResult — what generate() / imageToImage() / inpaint() / decode() /
 * setIdentityAnchor() return.
 *
 * @typedef {Object} ImageResult
 * @property {number} width
 * @property {number} height
 * @property {Uint8ClampedArray} data  4*width*height bytes, interleaved RGBA (HWC),
 *           alpha forced to 255. Drop-in for ctx.createImageData() + data.set() +
 *           putImageData().
 * @property {Float32Array} [fp32]     present only when opts.includeFp32 was set:
 *           3*height*width planar NCHW floats in [-1,1], the VAE's own output range.
 */

/**
 * Cancelled — returned INSTEAD of an ImageResult (or instead of a Pipeline, by
 * loadModel) when bro.diffusion.cancel() fires during the call. It has no
 * pixels and no handle, so test for it before touching anything else.
 *
 * @typedef {Object} Cancelled
 * @property {true} cancelled
 */

/**
 * PipelineConfigSnapshot — the read-only object Pipeline.config() returns.
 *
 * @typedef {Object} PipelineConfigSnapshot
 * @property {string}  modelClass       'StableDiffusion' | 'Flux' | 'Sana' | 'PixArt' | 'Krea2' | 'QwenImage21'
 * @property {string}  scheduler        'ddim' | 'lcm' | 'flowmatch' | 'scm'. A pipeline
 *           built with scheduler:'dpm' reports 'ddim' here — the snapshot has no separate
 *           name for DPM-Solver.
 * @property {number}  timeCondProjDim  SD1.5 U-Net time-cond projection dim (256 for an
 *           LCM-distilled checkpoint, else 0). 0 for the DiT families.
 * @property {boolean} quantizeWeights  whether the U-Net was INT8-quantized at load
 * @property {number}  numXAttnBlocks   traceable/steerable cross-attention blocks
 * @property {boolean} weightsLoaded    true after loadModel() or loadWeights()
 * @property {number}  numControlNets   registered ControlNet count
 * @property {boolean} hasControlNet    numControlNets > 0
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * Opaque handle to the native diffusion pipeline. The weights live inside it
 * and are freed when the handle is garbage-collected (or immediately on
 * dispose()). Created by bro.diffusion.loadModel() (any family, weights
 * already loaded) or bro.diffusion.createPipeline() (SD1.5 graph only — then
 * call loadWeights()).
 *
 * `bro.diffusion.Pipeline` is exposed for `instanceof` checks only: calling it
 * as a constructor throws TypeError. It is also registered as the global
 * `Pipeline` in each realm.
 *
 * ControlNet, control axes, the identity anchor and the krea2* hooks are on
 * this same class — see docs/diffusion-control-api.js.
 */
class Pipeline {

  /**
   * Load model weights from safetensors files. Three forms:
   *
   *   loadWeights(checkpointPath)
   *     A single-file full checkpoint. Every prefix stays "" (the root of the
   *     file) — the SD1.5 defaults are NOT substituted, so a root-prefixed
   *     module is honoured.
   *
   *   loadWeights(checkpointPath, { textPrefix, unetPrefix, vaePrefix })
   *     A single file with explicit safetensors key prefixes (any omitted
   *     prefix is "").
   *
   *   loadWeights(textPath, unetPath, vaePath)
   *     A diffusers three-file export: text_encoder/, unet/, vae/.
   *
   * SD1.5 only — this is the createPipeline() path. A loadModel() pipeline is
   * already loaded. Must be called before generate()/prime(). The safetensors
   * files are read only during this call; nothing in JS retains them. Every
   * path goes through bro's asset-path resolver.
   *
   * @param {string} path             checkpoint, or the text_encoder file in the 3-file form
   * @param {string|{textPrefix?:string,unetPrefix?:string,vaePrefix?:string}} [unetPathOrPrefixes]
   * @param {string} [vaePath]
   * @returns {undefined}
   */
  loadWeights(path, unetPathOrPrefixes, vaePath) {}

  /**
   * Krea 2 only. Swap just the Qwen3-VL-4B text backbone, keeping the resident
   * DiT / VAE / vision tower — so a text-encoder quant can be A/B'd without
   * re-reading ~26 GB of transformer shards.
   *
   * `textEncoderPath` accepts a llama.cpp .gguf (text-only: the vision tower
   * still comes from the model dir) or a diffusers .safetensors file/dir. Pass
   * '' (or omit it) to restore the model dir's bundled encoder.
   *
   * @param {string} modelDir              the Krea 2 model directory
   * @param {string} [textEncoderPath='']  replacement backbone; '' = bundled
   * @param {object} [opts]
   * @param {boolean} [opts.quantizeWeights=true] INT8-quantize the swapped-in backbone
   * @returns {undefined}
   *
   * @example
   *   pipe.reloadTextEncoder(DIR, DIR + '/qwen3vl-4b-q8_0.gguf');   // Q8 backbone
   *   const a = pipe.generate(prompt, opts);
   *   pipe.reloadTextEncoder(DIR, '');                              // back to bundled
   *   const b = pipe.generate(prompt, opts);
   */
  reloadTextEncoder(modelDir, textEncoderPath, opts) {}

  /**
   * One-shot text-to-image generation. Blocking: it runs the whole denoising
   * loop synchronously. Intended for a Worker; on the main thread use
   * prime()/stepOnce()/decode() so the event loop keeps turning.
   *
   * If bro.diffusion.cancel() fires (engine teardown, a user abort) the loop
   * stops at the next step and generate() returns { cancelled: true } with no
   * pixels. The cancel flag is cleared at the start of every generate().
   *
   * @param {string} prompt
   * @param {GenerateOptions} [opts]
   * @returns {ImageResult|Cancelled}
   */
  generate(prompt, opts) {}

  /**
   * Exact alias of generate(prompt, opts) — the spelling that reads well next
   * to imageToImage().
   * @param {string} prompt
   * @param {GenerateOptions} [opts]
   * @returns {ImageResult|Cancelled}
   */
  textToImage(prompt, opts) {}

  /**
   * img2img: generate() with opts.initImagePath forced to `imagePath`. The
   * image is VAE-encoded and noised to the point in the schedule `strength`
   * selects, so the result stays near the init. SD1.5 only.
   *
   * `imagePath` is used verbatim (no asset-path resolution).
   *
   * @param {string} imagePath  init image, decoded by broimage and resized to width x height
   * @param {string} prompt
   * @param {GenerateOptions} [opts]   `strength` (default 0.8) is the dial here
   * @returns {ImageResult|Cancelled}
   *
   * @example
   *   const out = pipe.imageToImage('sketch.png', 'a watercolour of the same scene',
   *                                 { strength: 0.55, steps: 30, seed: 3 });
   */
  imageToImage(imagePath, prompt, opts) {}

  /**
   * Inpaint: generate() with both opts.initImagePath and opts.maskImagePath
   * forced. White (>=128) in the mask = repaint, black = keep; at every step
   * but the last the kept region is replaced by a re-noised copy of the
   * encoded init latent. SD1.5 only. Both paths are used verbatim.
   *
   * @param {string} imagePath
   * @param {string} maskPath
   * @param {string} prompt
   * @param {GenerateOptions} [opts]
   * @returns {ImageResult|Cancelled}
   */
  inpaint(imagePath, maskPath, prompt, opts) {}

  /**
   * Begin a step-wise generation: encode the prompt, prime the cross-attention
   * K/V caches, allocate the initial latent. The opts are CAPTURED on the
   * returned state, so its stepOnce()/decode() need none.
   *
   * The state retains this Pipeline (through an internal `__pipeline` link), so
   * the weights outlive the handle you primed from, and clones inherit it.
   * States from different prime() calls can be stepped interleaved — each
   * denoises under its own conditioning, which is what makes dual-state
   * latent blending possible.
   *
   * @param {string} prompt
   * @param {GenerateOptions} [opts]
   * @returns {PipelineState}
   */
  prime(prompt, opts) {}

  /**
   * Pipeline-side convenience form of the step: advance `state` by one
   * denoising step using the opts captured at prime() time. Takes NO control
   * object — for attention trace / attnBias use state.stepOnce(ctrl).
   *
   * @param {PipelineState} state
   * @returns {boolean} true while more steps remain (state.stepIndex < state.numSteps)
   *
   * @example
   *   const st = pipe.prime(prompt, { steps: 12 });
   *   while (pipe.stepOnce(st)) showProgress(st.stepIndex, st.numSteps);
   *   const img = pipe.decode(st);
   */
  stepOnce(state) {}

  /**
   * Pipeline-side convenience form of the decode: VAE-decode `state`'s current
   * latent. Output size is state.latentWidth/Height times the VAE scale factor
   * (8 for the SD/Flux/Krea2 KL-VAEs, 32 for Sana's DC-AE).
   *
   * @param {PipelineState} state
   * @param {object}  [opts]
   * @param {boolean} [opts.includeFp32] also attach the raw NCHW FP32 buffer
   * @returns {ImageResult}
   */
  decode(state, opts) {}

  /**
   * Apply a LoRA file. Two behaviours by family:
   *
   *   - SD1.5: the deltas are MERGED into the loaded weights (irreversible).
   *     Returns undefined.
   *   - Krea 2: the file is attached as a RUNTIME-ADAPTER group — the base
   *     weights (possibly INT8) are untouched and each adapted linear adds
   *     scale * (x @ downT) @ upT per forward. Returns the group index for
   *     setLoraScale(). Rescale live, clear with clearLoras(), no reload.
   *
   * Call after loadWeights()/loadModel() and before generate()/prime().
   * Stackable. Key conventions are auto-detected: kohya-ss/A1111 and
   * diffusers/PEFT for SD1.5; diffusers `transformer.`, ComfyUI
   * `diffusion_model.`, bare, and kohya-mangled `lora_unet_transformer_blocks_*`
   * for Krea 2.
   *
   * @param {string} path      LoRA .safetensors (asset-path resolved)
   * @param {number} [scale=1] multiplier on the per-LoRA alpha/rank factor; may be
   *        negative to subtract. A non-numeric value leaves it at 1.0 rather than 0.
   * @returns {number|undefined} the runtime-adapter group index (Krea 2), or undefined
   *          when merged (SD1.5)
   *
   * @example
   *   const g = pipe.applyLora('/loras/pencil-sketch.safetensors', 1.0);
   *   let img = pipe.generate(prompt, opts);     // with the LoRA
   *   pipe.setLoraScale(g, 0.0);
   *   img = pipe.generate(prompt, opts);         // base model again
   *   pipe.setLoraScale(g, 0.7);                 // softer
   *   pipe.clearLoras();                         // gone entirely
   */
  applyLora(path, scale) {}

  /**
   * Change a runtime LoRA group's multiplier (0 disables, negative subtracts).
   * `index` is applyLora() call order, 0-based. Krea 2 only — SD1.5 LoRAs are
   * merged irreversibly and throw here.
   * @param {number} index
   * @param {number} scale
   * @returns {undefined}
   */
  setLoraScale(index, scale) {}

  /** Drop every runtime LoRA group (frees their factors). Krea 2 only. @returns {undefined} */
  clearLoras() {}

  /** @returns {number} count of attached runtime LoRA groups. Krea 2 only. */
  numLoras() {}

  /**
   * Traceable / steerable cross-attention blocks in the loaded denoiser: 16 for
   * the SD1.5 U-Net, 57 for the Flux DiT, 0 for a denoiser without trace
   * support. Only meaningful once weights are loaded, so query it live — it is
   * the required length of a stepOnce() attnBias array and the length of a
   * returned trace array.
   * @returns {number}
   */
  numXAttnBlocks() {}

  /**
   * The flow-match sigma schedule of the most recent prime()/generate():
   * numSteps + 1 entries with a trailing 0.0, where sigmas()[i] is the noise
   * level entering step i. EMPTY for a non-flow-match scheduler (DDIM / LCM /
   * SCM) or before any schedule has been set.
   *
   * Because the flow-match Euler step is exact, two consecutive latent()
   * snapshots recover the model's velocity and the free x̂0 preview with no
   * extra forward pass:
   *
   *   const s  = pipe.sigmas();
   *   const x0 = st.latent();                    // x_i (before the step)
   *   st.stepOnce();
   *   const x1 = st.latent();                    // x_{i+1}
   *   const k  = s[i] / (s[i + 1] - s[i]);
   *   const p  = x0.map((v, j) => v - k * (x1[j] - v));    // x̂0 preview
   *   const peek = st.clone(); peek.setLatent(p);
   *   const img = peek.decode();                 // what the image is committed to
   *
   * That is the seam behind preview-pruned candidate search: score many init
   * noises after ONE step each and keep the promising ones.
   * @returns {Float32Array}
   */
  sigmas() {}

  /**
   * Read-only snapshot of the resolved pipeline configuration — family,
   * scheduler, quantization, ControlNet count, whether weights are in.
   * @returns {PipelineConfigSnapshot}
   *
   * @example
   *   const c = pipe.config();
   *   if (c.modelClass === 'Krea2') enableKrea2Panel(pipe.krea2NumLayers());
   *   if (c.modelClass === 'QwenImage21') enableQi21Panel(pipe.qwenImage21NumLayers());
   *   if (!c.weightsLoaded) throw new Error('call loadWeights() first');
   */
  config() {}

  /**
   * Release the native pipeline and its weights NOW instead of at the next GC.
   * The handle stays alive but every method on it throws afterwards, and any
   * PipelineState still holding it loses its pipeline link.
   * @returns {undefined}
   */
  dispose() {}
}

/**
 * Opaque handle to a mid-generation state: the working latent plus scheduler
 * progress plus a shared handle to the prepared conditioning. The latent stays
 * native across the whole loop and materializes in JS only if you ask for it
 * with latent(). Created by Pipeline.prime(), Pipeline.krea2PrimeFromTaps() or
 * PipelineState.clone(). Each state keeps its owning Pipeline alive.
 *
 * Cloning is cheap — one latent copy; the conditioning is shared — which is
 * what makes branch-and-score / cross-attention tree search practical.
 *
 * `bro.diffusion.PipelineState` is for `instanceof` only: calling it as a
 * constructor throws TypeError.
 */
class PipelineState {

  /** @type {number} 0-based count of steps run so far. @readonly */
  stepIndex;

  /** @type {number} total scheduled steps for this generation. @readonly */
  numSteps;

  /** @type {number} alias of numSteps, kept for code written against the ported surface. @readonly */
  totalSteps;

  /** @type {boolean} true once stepIndex >= numSteps. @readonly */
  done;

  /** @type {number} latent width (image width / VAE scale factor). @readonly */
  latentWidth;

  /** @type {number} latent height (image height / VAE scale factor). @readonly */
  latentHeight;

  /**
   * Advance one denoising step (mutates this state). The full-fidelity form:
   * it can capture an attention trace and inject a per-layer pre-softmax logit
   * bias. Recipes and worked examples: docs/diffusion-control-api.js.
   *
   * @param {object} [ctrl]
   * @param {boolean} [ctrl.trace] capture the per-layer head-averaged cross-attention
   *        maps. The result's `trace` is an array of numXAttnBlocks() entries, each
   *        { Lq, Lk, data } with `data` a Float32Array of Lq*Lk weights (Lk = the
   *        context length, e.g. 77 for CLIP; Lq = that layer's spatial token count).
   * @param {Array<?{data:Float32Array, Lq:number, Lk:number}>} [ctrl.attnBias]
   *        per-layer bias added to the attention scores before softmax. The array's
   *        length MUST equal numXAttnBlocks() (a mismatch throws RangeError); each
   *        entry is null/undefined (no bias for that layer) or { data, Lq, Lk } with
   *        data.length === Lq*Lk (anything else throws TypeError). Supplying attnBias
   *        forces trace mode internally.
   * @returns {{trace?: Array<{Lq:number, Lk:number, data:Float32Array}>}}
   *
   * @example
   *   const st = pipe.prime('a red apple', { width: 256, height: 256, steps: 8 });
   *   while (!st.done) {
   *     const r = st.stepOnce({ trace: true });
   *     // r.trace[i].data — attention map for cross-attention block i
   *   }
   *   const img = st.decode();
   */
  stepOnce(ctrl) {}

  /**
   * VAE-decode the current latent to an image. Output size is
   * latentWidth/Height times the VAE scale factor (8 for the SD / Flux /
   * Krea 2 KL-VAEs, 32 for Sana's DC-AE).
   * @param {object}  [opts]
   * @param {boolean} [opts.includeFp32] also attach the raw NCHW FP32 buffer as `fp32`
   * @returns {ImageResult}
   */
  decode(opts) {}

  /**
   * Download the working latent as a Float32Array of C_lat * latentHeight *
   * latentWidth values (C_lat is 4 for SD1.5, 16 for Flux / Krea 2, 32 for
   * Sana). FP16/BF16 latents are converted to FP32 on the way out. Small and
   * on demand — for visualization, scoring, or capturing init noise.
   * @returns {Float32Array}
   */
  latent() {}

  /**
   * Overwrite the working latent in place. `data.length` must equal the
   * current latent's element count exactly (TypeError otherwise), and the
   * values are cast back to the state's working dtype.
   *
   * This is the spatial-paint seam: prime two states with different
   * conditioning, step them in lockstep, blend their latents host-side each
   * step, and push the blend back before the next step.
   *
   * @param {Float32Array} data
   * @returns {undefined}
   */
  setLatent(data) {}

  /**
   * The active scheduler's timestep for this state's current stepIndex (on the
   * 0..1000 scale) — the same value the next stepOnce() will feed the
   * denoiser. Krea 2 only; pair it with krea2TimeMod() to build an AdaLN
   * modulation delta for the step that is about to run.
   * @returns {number}
   */
  krea2StepTimestep() {}

  /**
   * The same value for a Qwen-Image 2.1 pipeline — pair it with
   * qwenImage21TimeMod() to build a modulation delta for the step that is
   * about to run. Qwen-Image 2.1 only.
   * @returns {number}
   */
  qwenImage21StepTimestep() {}

  /**
   * Deep-copy this state: one latent clone; counters and RNG state are trivial
   * copies and the prepared conditioning is shared, not re-encoded. The clone
   * advances independently and carries the same owning Pipeline.
   * @returns {PipelineState}
   */
  clone() {}
}

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * brodiffusion library version, e.g. "0.0.1".
 * @readonly
 * @type {string}
 */
bro.diffusion.version;

/**
 * Initialize the brotensor runtime (probe CUDA/Metal, register backends).
 * Idempotent and thread-safe; loadModel() and createPipeline() both call it.
 * Exposed so a Worker can warm up before its first message.
 * @returns {undefined}
 */
bro.diffusion.init = function() {};

/**
 * Load a complete diffusers model directory and return a fully-ready Pipeline:
 * every weight and tokenizer is read here, so the result needs no
 * loadWeights() — call generate()/prime() straight away. This is the ONLY way
 * to run a Flux, Sana, PixArt or Krea 2 model.
 *
 * The family is auto-detected from `model_index.json`'s `_class_name`; the
 * family's own text encoder is loaded internally (no bro.lm.loadGemma2 /
 * loadQwen3VL needed). Check pipeline.config().modelClass for what you got.
 *
 * Blocking and slow (a multi-GB read) — run it in a Worker. The load polls the
 * cancel flag between components (and per transformer block for the big
 * sharded DiTs), so bro.diffusion.cancel() during teardown aborts it promptly
 * and loadModel() returns { cancelled: true } instead of a Pipeline. A missing
 * directory throws.
 *
 * @param {string} modelDir  model directory root (asset-path resolved)
 * @param {object} [opts]
 * @param {boolean} [opts.quantizeWeights=false] INT8 (W8A16) weight-only quantization of
 *        the denoiser — and, for Flux, of T5-XXL — while loading. GPU-only; warned and
 *        ignored on CPU. This is how Flux.1 fits a 24 GB card next to T5 at all.
 * @param {string}  [opts.textEncoderPath]  Krea 2 only: load the Qwen3-VL-4B language
 *        backbone from this .gguf / .safetensors file or dir instead of
 *        <modelDir>/text_encoder. Ignored for other families.
 * @returns {Pipeline|Cancelled}
 *
 * @example
 *   const pipe = bro.diffusion.loadModel('../brodiffusion/weights/flux-schnell');
 *   if (pipe.cancelled) return;
 *   const img = pipe.generate('a fox in autumn leaves', { steps: 4, guidanceScale: 1.0 });
 *
 * @example
 *   // Sana: width/height must be multiples of 32 (the DC-AE downsamples 32x).
 *   const sana = bro.diffusion.loadModel('../brodiffusion/weights/sana-600m');
 *   const img  = sana.generate('a red panda in a tree', { width: 512, height: 512, steps: 20 });
 *
 * @example
 *   // Flux on a 24 GB card.
 *   const flux = bro.diffusion.loadModel(DIR, { quantizeWeights: true });
 */
bro.diffusion.loadModel = function(modelDir, opts) {};

/**
 * Build a Stable Diffusion 1.5 Pipeline explicitly: loads the CLIP tokenizer
 * and constructs the model graph with NO weights — call pipeline.loadWeights()
 * next. For any other family use loadModel().
 *
 * @param {object}  opts
 * @param {string}  opts.vocabPath   CLIP tokenizer vocab.json (required)
 * @param {string}  opts.mergesPath  CLIP tokenizer merges.txt (required)
 * @param {string}  [opts.scheduler='ddim'] 'ddim' | 'lcm' | 'flowmatch' | 'scm' |
 *        'dpm' (alias 'dpmsolver'). Anything else falls back to 'ddim'. Note config()
 *        reports a 'dpm' pipeline as 'ddim'.
 * @param {boolean} [opts.lcmDistilled=false] true for an LCM-distilled U-Net checkpoint
 *        (sets time_cond_proj_dim = 256). Only applied when scheduler is 'lcm'.
 * @param {boolean} [opts.quantizeWeights=false] INT8 (W8A16) U-Net weights. GPU-only.
 * @returns {Pipeline}
 *
 * @example
 *   const W = '../brodiffusion/weights/sd15';
 *   const pipe = bro.diffusion.createPipeline({
 *     vocabPath: W + '/tokenizer/vocab.json',
 *     mergesPath: W + '/tokenizer/merges.txt',
 *     scheduler: 'lcm', lcmDistilled: true,
 *   });
 *   pipe.loadWeights(W + '/lcm-dreamshaper-v7.safetensors');
 *   const img = pipe.generate('a tin robot', { steps: 4, guidanceScale: 1.0 });
 */
bro.diffusion.createPipeline = function(opts) {};

/**
 * Expand an init-noise tensor to an integer-factor larger resolution while
 * preserving the identity / composition it encodes. The source is NCHW raw
 * N(0,1) — e.g. state.latent() right after prime(), since sigma_0 is 1.0 for
 * flow-match so the latent IS the init noise. The result is exactly i.i.d.
 * N(0,1) whose k x k block means are tied to the source, so the low-frequency
 * structure that decides the character in the first step or two carries over.
 * A SEED cannot do this: the same seed at a different latent shape is an
 * unrelated noise field.
 *
 * Feed the result back through opts.initNoise at the larger size.
 *
 * @param {Float32Array} src  NCHW noise, at least channels*height*width long
 * @param {object} [opts]
 * @param {number} [opts.channels=4] latent channels (16 for Krea 2 / Flux)
 * @param {number} [opts.height=64]  source latent height (image height / scale factor)
 * @param {number} [opts.width=64]   source latent width
 * @param {number} [opts.factor=2]   integer spatial expansion factor (>= 1)
 * @param {number|bigint} [opts.seed=0] Philox key for the fine-level complement;
 *        deterministic per (src, seed)
 * @returns {Float32Array} length channels * height*factor * width*factor
 *
 * @example
 *   // Find a character at 512², render it at 1024².
 *   const st = pipe.prime(prompt, { seed, width: 512, height: 512, steps: 8, guidanceScale: 1.0 });
 *   const noise = st.latent();                        // 16x64x64 init noise
 *   while (!st.done) st.stepOnce();                   // ... judge st.decode()
 *   const big = bro.diffusion.expandNoise(noise,
 *       { channels: 16, height: 64, width: 64, factor: 2, seed });
 *   const img = pipe.generate(prompt,
 *       { initNoise: big, width: 1024, height: 1024, steps: 8, guidanceScale: 1.0 });
 */
bro.diffusion.expandNoise = function(src, opts) {};

/**
 * Request that an in-flight generate() / imageToImage() / inpaint() /
 * setIdentityAnchor() / loadModel() abort. Those are single synchronous native
 * calls, so run them in a Worker and call this from the MAIN thread. The
 * cancel is cooperative: it lands between denoising steps (or between model
 * components during a load). The aborted call returns { cancelled: true }.
 *
 * The flag is cleared at the start of each generate-class call, so a stale
 * cancel never kills the next run. The step-wise prime()/stepOnce() loop is
 * NOT cancelled — you own its pacing, so just stop looping.
 * @returns {undefined}
 */
bro.diffusion.cancel = function() {};

/**
 * The Pipeline class object — for `instanceof`. Not constructible (throws
 * TypeError); also registered as the global `Pipeline`.
 * @type {Function}
 */
bro.diffusion.Pipeline;

/**
 * The PipelineState class object — for `instanceof`. Not constructible; also
 * registered as the global `PipelineState`.
 * @type {Function}
 */
bro.diffusion.PipelineState;

/**
 * The standalone KL-VAE class — `new bro.diffusion.VAE()` works. Documented in
 * docs/diffusion-control-api.js.
 * @type {Function}
 */
bro.diffusion.VAE;

// ── Workers: generation off the main thread ──────────────────────────────────
//
// bro.diffusion is installed in Worker realms too, and each realm installs its
// OWN class objects and its own native heap: a Worker must load its own
// weights and own its own Pipeline. Treat the Worker as a long-lived inference
// server — load once, then feed it prompts; never reload per generation.
//
// Only plain cloneable data crosses postMessage. A Pipeline / PipelineState
// handle cannot (and need not) cross — keep it inside the Worker, send the
// prompt and options in, and transfer the result image's buffer back.
//
//   // --- worker script: sd-worker.js ---
//   let pipe = null;
//   self.onmessage = (e) => {
//     const m = e.data;
//     if (m.cmd === 'load') {
//       pipe = bro.diffusion.loadModel(m.dir);
//       self.postMessage({ ready: !pipe.cancelled });
//     } else if (m.cmd === 'generate') {
//       const img = pipe.generate(m.prompt, m.opts);
//       if (img.cancelled) { self.postMessage({ cancelled: true }); return; }
//       self.postMessage({ image: img }, [img.data.buffer]);   // zero-copy
//     }
//   };
//
//   // --- main thread ---
//   const w = new Worker('sd-worker.js');
//   w.onmessage = (e) => {
//     if (e.data.ready) w.postMessage({ cmd: 'generate',
//         prompt: 'a lighthouse at dawn', opts: { steps: 20 } });
//     else if (e.data.image) drawToCanvas(e.data.image);
//   };
//   w.postMessage({ cmd: 'load', dir: '../brodiffusion/weights/sd15' });
//   // From the main thread, bro.diffusion.cancel() aborts the Worker's run.
//
// =============================================================================
