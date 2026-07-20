// =============================================================================
// bro.diffusion API Reference
// =============================================================================
//
// Diffusion-model text-to-image inference, backed by the brodiffusion sibling
// library. Five model families are supported:
//   - Stable Diffusion 1.5, CLIP text encoder + U-Net + VAE, DDIM/LCM
//     schedulers, LoRA, INT8 quantization.
//   - Flux: CLIP (pooled) + T5-XXL text encoders + Flux DiT denoiser + VAE,
//     flow-match scheduler.
//   - Sana (NVIDIA): Gemma-2 text encoder + Linear DiT denoiser + DC-AE
//     f32c32 autoencoder (32x latent, vs 8x for SD/Flux), flow-match scheduler.
//     Sana-Sprint is the few-step guidance-distilled variant (SCM scheduler).
//     Sana txt2img runs through the same generate()/prime() path; img2img,
//     inpaint, ControlNet, and LoRA are not wired for Sana.
//   - PixArt-Sigma: T5-XXL text encoder + PixArt DiT (AdaLN-single) + SDXL
//     KL-VAE (8x latent), true classifier-free guidance. The T5-XXL encoder is
//     resolved from $BRODIFFUSION_T5_DIR, a bundled text_encoder/, or a sibling
//     t5-xxl/. txt2img runs through the same generate()/prime() path; img2img,
//     inpaint, ControlNet, LoRA, and the conditioning-control seam are not
//     wired for PixArt.
//   - Krea 2: Qwen3-VL-4B text encoder (single-stream flow DiT, 3-shard
//     transformer/) + Qwen-Image VAE decoder. txt2img runs through the same
//     generate()/prime() path. LoRA is supported as runtime adapters
//     (applyLora / setLoraScale / clearLoras, live rescale, INT8-safe);
//     img2img, inpaint, and ControlNet are not wired for Krea 2.
// loadModel() takes a model directory and auto-detects the family;
// createPipeline() builds the SD1.5 stack explicitly.
//
// The native Pipeline owns the multi-GB model weights. JavaScript never holds
// or moves weight bytes. It holds an opaque *handle* (the Pipeline object).
// Latents and attention maps are small and download to Float32Arrays on
// demand. Only the final decoded image crosses into JS as pixel data.
//
// bro.diffusion is installed in both the main JS context and every Worker
// context, and is available on CPU-only builds (brodiffusion's CPU FP32 path
// is always built). It runs on whatever backend brotensor resolves at
// runtime, CPU FP32 by default, CUDA/Metal FP16 when a GPU build has one.
//
// Two usage modes, one binding:
//   - Main thread:  step-wise prime() -> stepOnce() -> decode(), inspecting
//                   latents and cross-attention traces each step.
//   - Worker:       full generate() off the main thread (see "Workers" below).
//
// Weights are not bundled. brodiffusion ships scripts/download-weights.ps1;
// each model is a diffusers-format export, text_encoder/, unet/, vae/, and a
// tokenizer/ with vocab.json + merges.txt.
//
// =============================================================================


// -----------------------------------------------------------------------------
// bro.diffusion, namespace
// -----------------------------------------------------------------------------

const diffusion = {

  /** @type {string} brodiffusion library version, e.g. "0.0.1". */
  version,

  /**
   * Initialize the brotensor runtime (probe CUDA/Metal, register backends).
   * Idempotent and thread-safe; createPipeline() also calls it. Exposed so a
   * Worker can warm up before its first message.
   */
  init() {},

  /**
   * Load a complete diffusers model directory and return a fully-ready
   * Pipeline. The directory holds `model_index.json` plus one component
   * subdir each (text_encoder/, unet/ or transformer/, vae/, tokenizer/,
   * scheduler/, ...). The model family is auto-detected from `_class_name`:
   *
   *   - Stable Diffusion → CLIP tokenizer + U-Net + VAE
   *   - Flux             → CLIP (pooled) + T5-XXL tokenizers/encoders +
   *                        Flux DiT + VAE, flow-match scheduler
   *   - Sana             → Gemma-2 tokenizer/encoder + Linear DiT + DC-AE
   *                        decoder, flow-match (or SCM for Sana-Sprint). The
   *                        Gemma-2 text encoder is loaded internally: no
   *                        bro.lm.loadGemma2 needed for Sana txt2img.
   *   - PixArt           → T5-XXL tokenizer/encoder + PixArt DiT + SDXL KL-VAE.
   *                        The T5-XXL encoder resolves from $BRODIFFUSION_T5_DIR,
   *                        a bundled text_encoder/, or a sibling t5-xxl/.
   *   - Krea2            → Qwen3-VL-4B tokenizer/encoder (loaded from
   *                        tokenizer/ + text_encoder/, internally, no
   *                        bro.lm.loadQwen3VL needed) + single-stream flow DiT
   *                        + Qwen-Image VAE decoder.
   *
   * Every weight and tokenizer is loaded by this call, so the returned
   * Pipeline needs no loadWeights(): call generate()/prime() directly. This
   * is the only way to run a Flux, Sana, PixArt, or Krea2 model. Blocking and
   * slow (multi-GB read); run it in a Worker. Check pipeline.config().modelClass
   * for the family ('StableDiffusion' | 'Flux' | 'Sana' | 'PixArt' | 'Krea2').
   *
   * @param {string} modelDir - model directory root
   * @returns {Pipeline}
   *
   * @example
   *   const pipe = bro.diffusion.loadModel('/path/to/flux-schnell');
   *   const img  = pipe.generate('a fox in autumn leaves', { steps: 4 });
   *
   * @example
   *   // Sana: width/height must be multiples of 32 (DC-AE downsamples 32x).
   *   const sana = bro.diffusion.loadModel('../brodiffusion/weights/sana-600m');
   *   const img  = sana.generate('a red panda in a tree',
   *                              { width: 512, height: 512, steps: 20 });
   */
  loadModel(modelDir) {},

  /**
   * Expand an init-noise tensor to an integer-factor larger resolution while
   * preserving the identity/composition it encodes. The source is NCHW raw
   * N(0,1), e.g. `state.latent()` right after prime() (sigma_0 is 1.0 for
   * flow-match, so the latent IS the init noise). The result is exactly
   * i.i.d. N(0,1) whose k×k block means are tied to the source, so the
   * low-frequency structure, which decides the character in the first
   * step or two: carries over. A SEED cannot do this: the same seed at a
   * different latent shape is an unrelated noise field.
   *
   * Feed the result back via `opts.initNoise` at the larger size.
   *
   * @param {Float32Array} src - NCHW noise, length channels*height*width
   * @param {object} opts
   * @param {number} opts.channels - latent channels (16 for Krea 2 / Flux)
   * @param {number} opts.height   - source latent height (image height / 8)
   * @param {number} opts.width    - source latent width  (image width / 8)
   * @param {number} opts.factor   - integer spatial expansion factor (>= 1)
   * @param {number} [opts.seed]   - Philox key for the fine-level complement;
   *        deterministic per (src, seed)
   * @returns {Float32Array} length channels * height*factor * width*factor
   *
   * @example
   *   // Find a character at 512², render it at 1024².
   *   const st = pipe.prime(prompt, { seed, width: 512, height: 512,
   *                                   steps: 8, guidanceScale: 1.0 });
   *   const noise = st.latent();                    // 16×64×64 init noise
   *   while (!st.done) st.stepOnce();               // ... judge st.decode()
   *   const big = bro.diffusion.expandNoise(noise,
   *       { channels: 16, height: 64, width: 64, factor: 2, seed });
   *   const img = pipe.generate(prompt, { initNoise: big,
   *       width: 1024, height: 1024, steps: 8, guidanceScale: 1.0 });
   */
  expandNoise(src, opts) {},

  /**
   * Create a Stable Diffusion 1.5 inference Pipeline. Loads the CLIP
   * tokenizer and builds the model graph (no weights yet. Call
   * pipeline.loadWeights()). For Flux, use loadModel() instead.
   *
   * @param {object} opts
   * @param {string}  opts.vocabPath        - CLIP tokenizer vocab.json (required)
   * @param {string}  opts.mergesPath       - CLIP tokenizer merges.txt (required)
   * @param {string}  [opts.scheduler]      - 'ddim' (default) | 'lcm'
   * @param {boolean} [opts.lcmDistilled]   - true for an LCM-distilled U-Net
   *        checkpoint (sets time_cond_proj_dim = 256). Ignored unless
   *        scheduler is 'lcm'.
   * @param {boolean} [opts.quantizeWeights]- quantize U-Net weights to INT8
   *        (W8A16). GPU-only; silently ignored on the CPU backend.
   * @returns {Pipeline}
   *
   * @example
   *   const W = '/path/to/model';   // diffusers-layout weights directory
   *   const pipe = bro.diffusion.createPipeline({
   *       vocabPath:  W + '/tokenizer/vocab.json',
   *       mergesPath: W + '/tokenizer/merges.txt',
   *   });
   */
  createPipeline(opts) {},
};


// -----------------------------------------------------------------------------
// Pipeline
// -----------------------------------------------------------------------------
// Opaque handle to the native diffusion pipeline. The weights live inside it
// and are freed when the handle is garbage-collected. Created via
// bro.diffusion.loadModel() (any family, weights already loaded) or
// bro.diffusion.createPipeline() (SD1.5, then call loadWeights()).

class Pipeline {

  /**
   * Load model weights from safetensors files. Three forms:
   *
   *   loadWeights(checkpointPath)
   *     A single-file full checkpoint (.safetensors).
   *
   *   loadWeights(checkpointPath, { textPrefix, unetPrefix, vaePrefix })
   *     A single file with explicit safetensors key prefixes.
   *
   *   loadWeights(textPath, unetPath, vaePath)
   *     A diffusers three-file export: text_encoder/, unet/, vae/.
   *
   * Must be called before generate()/prime(). The safetensors files are only
   * read during this call; nothing in JS retains them.
   *
   * @example
   *   const W = '/path/to/model';   // diffusers-layout weights directory
   *   pipe.loadWeights(
   *       W + '/text_encoder/model.fp16.safetensors',
   *       W + '/unet/diffusion_pytorch_model.fp16.safetensors',
   *       W + '/vae/diffusion_pytorch_model.fp16.safetensors');
   */
  loadWeights(/* ...paths */) {}

  /**
   * Apply a LoRA file. Two behaviours by model family:
   *
   *   - SD1.5: the deltas are MERGED into the loaded weights (irreversible).
   *   - Krea 2: the file is attached as a RUNTIME-ADAPTER group, the base
   *     weights (possibly INT8-quantized) are untouched and each adapted
   *     linear adds scale * (x @ downT) @ upT per forward. Groups are indexed
   *     in applyLora() call order; rescale live with setLoraScale(index,
   *     scale) (0 disables) and remove them all with clearLoras(). No reload
   *     needed for any of it.
   *
   * Call after loadWeights() / loadModel() and before generate()/prime().
   * Stackable: call repeatedly to layer multiple LoRAs. Key conventions are
   * auto-detected: kohya-ss/A1111 and diffusers/PEFT for SD1.5; diffusers
   * `transformer.`, ComfyUI `diffusion_model.`, bare, and kohya-mangled
   * `lora_unet_transformer_blocks_*` spellings for Krea 2.
   *
   * @param {string} path    - LoRA .safetensors file
   * @param {number} [scale] - multiplier on the per-LoRA alpha/rank factor
   *                           (default 1.0; may be negative to subtract)
   * @returns {number|undefined} the runtime-adapter group index (Krea 2:
   *          pass to setLoraScale), or undefined when merged (SD1.5)
   *
   * @example
   *   // Krea 2: attach, A/B the strength live, then drop it.
   *   const g = pipe.applyLora('/loras/pencil-sketch.safetensors', 1.0);
   *   let img = pipe.generate(prompt, opts);      // with the LoRA
   *   pipe.setLoraScale(g, 0.0);
   *   img = pipe.generate(prompt, opts);          // base model again
   *   pipe.setLoraScale(g, 0.7);                  // softer
   *   pipe.clearLoras();                          // gone entirely
   */
  applyLora(path, scale) {}

  /**
   * Change a runtime LoRA group's user multiplier (0 disables it; negative
   * subtracts). `index` is the applyLora() call order, 0-based. Krea 2 only,
   * SD1.5 LoRAs are merged irreversibly and throw here.
   *
   * @param {number} index - LoRA group index (applyLora call order)
   * @param {number} scale - new multiplier on the per-LoRA alpha/rank factor
   */
  setLoraScale(index, scale) {}

  /** Drop every runtime LoRA group (frees their factors). Krea 2 only. */
  clearLoras() {}

  /** @returns {number} count of attached runtime LoRA groups. Krea 2 only. */
  numLoras() {}

  /**
   * Register a ControlNet safetensors file. SD1.5 only, throws on Flux. Call
   * after loadWeights(); stackable (call repeatedly to register multiple
   * nets). The returned index is the position into GenerateOptions.controls
   * and the addressing key for removeControlNet(). LCM and trace mode both
   * work alongside ControlNets; INT8 + trace remains unsupported.
   *
   * @param {string} path  - ControlNet .safetensors file
   * @param {object} [cfg] - non-default architecture overrides for unusual
   *        checkpoints. The HF lllyasviel/sd-controlnet-* zoo uses defaults.
   * @param {number} [cfg.inChannels]          - latent input channels (default 4)
   * @param {number} [cfg.controlChannels]     - control image channels (default 3)
   * @param {number} [cfg.layersPerBlock]      - resnet layers per down block (default 2)
   * @param {number} [cfg.crossAttentionDim]   - CLIP context dim (default 768)
   * @param {number} [cfg.transformerNumHeads] - transformer heads (default 8)
   * @returns {number} index of the newly registered ControlNet
   */
  addControlNet(path, cfg) {}

  /**
   * Drop one registered ControlNet by index. Subsequent indices shift down.
   * @param {number} index
   */
  removeControlNet(index) {}

  /**
   * Drop every registered ControlNet.
   */
  clearControlNets() {}

  /**
   * One-shot text-to-image generation. Blocking, runs the full denoising
   * loop synchronously. Intended for a Worker thread; on the main thread use
   * the step-wise prime()/stepOnce()/decode() API so the event loop stays
   * responsive.
   *
   * If the process is shutting down (Ctrl+C / window close / engine teardown)
   * the denoise loop aborts at the next step and generate() returns
   * { cancelled: true } with no pixels. Check for it before using the result.
   *
   * @param {string} prompt
   * @param {object} [opts]                 - see GenerateOptions below
   * @returns {ImageResult} { width, height, data }, or { cancelled: true }
   *
   * @example
   *   const img = pipe.generate('a cat astronaut, oil painting', {
   *       width: 512, height: 512, steps: 30, guidanceScale: 7.5, seed: 42,
   *   });
   *   const cv = document.createElement('canvas');
   *   cv.width = img.width; cv.height = img.height;
   *   const cx = cv.getContext('2d');
   *   const id = cx.createImageData(img.width, img.height);
   *   id.data.set(img.data);
   *   cx.putImageData(id, 0, 0);
   */
  generate(prompt, opts) {}

  /**
   * Begin a step-wise generation. Encodes the prompt, primes the cross-
   * attention K/V caches, and allocates the initial latent noise. The opts
   * are captured on the returned state, so stepOnce()/decode() take none.
   *
   * @param {string} prompt
   * @param {object} [opts] - see GenerateOptions below
   * @returns {PipelineState}
   */
  prime(prompt, opts) {}

  /**
   * Number of cross-attention (Transformer2D) blocks in the loaded model's
   * U-Net. Meaningful only after weights are loaded (returns 1 before). This
   * is the required length of a stepOnce() attnBias array and the length of a
   * trace array. Returns 0 for a Flux model (no U-Net cross-attention).
   * @returns {number}
   */
  numXAttnBlocks() {}

  /**
   * The flow-match sigma schedule of the most recent prime()/generate():
   * a Float32Array of length numSteps + 1 with a trailing 0.0, sigmas()[i]
   * is the noise level entering step i. Empty for non-flow-match schedulers
   * (DDIM/LCM/SCM) or before any schedule has been set.
   *
   * Because the flow-match Euler step is exact, two consecutive latent()
   * snapshots recover the model's velocity and the free x̂0 preview without
   * an extra forward:
   *
   *   const s  = pipe.sigmas();
   *   const x0 = st.latent();                       // x_i  (before the step)
   *   st.stepOnce();
   *   const x1 = st.latent();                       // x_{i+1}
   *   const k  = s[i] / (s[i + 1] - s[i]);
   *   const p  = x0.map((v, j) => v - k * (x1[j] - v));   // x̂0 preview
   *   const peek = st.clone(); peek.setLatent(p);
   *   const img = peek.decode();                    // what the final image
   *                                                 // is already committed to
   *
   * This is the engine seam behind preview-pruned candidate search (score
   * many init noises after ONE step each, keep the promising ones).
   * @returns {Float32Array}
   */
  sigmas() {}

  /**
   * Read-only snapshot of the resolved pipeline configuration. `modelClass`
   * is 'StableDiffusion', 'Flux', 'Sana', 'PixArt', or 'Krea2'; `scheduler` is
   * 'ddim', 'lcm', 'flowmatch', or 'scm' (Sana-Sprint). `timeCondProjDim`,
   * `quantizeWeights`, and `numXAttnBlocks` describe the SD1.5 U-Net and read
   * 0/false for a Flux, Sana, PixArt, or Krea2 model.
   * `numControlNets` is the current registered ControlNet count and
   * `hasControlNet` is true iff that count is > 0.
   * @returns {{modelClass:string, scheduler:string, timeCondProjDim:number,
   *            quantizeWeights:boolean, numXAttnBlocks:number,
   *            weightsLoaded:boolean, numControlNets:number,
   *            hasControlNet:boolean}}
   */
  config() {}

  // ── conditioning-space control axes (the sana-research seam) ──────────────
  // A dictionary of named unit directions in the text encoder's embedding
  // space, injected additively into the positive conditioning at prime()/
  // generate() time. See brodiffusion/cond_control.h for the file format.

  /**
   * Load a BCD1 control dictionary, replacing loaded axes and zeroing weights.
   *
   * With `{merge: true}` the file's axes are ADDED to those already loaded
   * instead (a same-named axis is overwritten, its weight reset). Banks come
   * from different discoveries: a word-derived one, an SAE-discovered one,
   * and stacking them needs no offline concatenation. A merged file must agree
   * on the encoder dim with what is already loaded, or it throws.
   *
   *   pipeline.loadControlDictionary('assets/axes_turbo.bcd1');
   *   pipeline.loadControlDictionary('assets/axes_sae_deck.bcd1', { merge: true });
   *   pipeline.setControl({ 'color.temperature': 0.8, 'sae.4571': -3 });
   *
   * @param {string} path
   * @param {{merge?: boolean}} [opts]
   */
  loadControlDictionary(path, opts) {}

  /**
   * @returns {string[]} names of every registered axis (dictionary + runtime).
   */
  controlAxes() {}

  /**
   * The stored direction + baked scale of one axis, introspection for
   * explaining axes (e.g. cosine-decompose a freshly minted axis against the
   * dictionary's named directions). Throws on unknown name.
   * @param {string} name
   * @returns {{dir: Float32Array, scale: number}}
   */
  controlVector(name) {}

  /**
   * Set one axis weight (natural units), or several at once from a map.
   * Applied to every generation until changed / clearControl().
   * @param {string|Object<string,number>} nameOrMap
   * @param {number} [alpha]
   */
  setControl(nameOrMap, alpha) {}

  /**
   * Register (or replace) a runtime axis from an explicit direction and set
   * its weight. `dir` is taken as-is (caller normalizes); the injected vector
   * is alpha * scale * dir. Coexists with dictionary axes.
   * @param {string} name
   * @param {Float32Array} dir
   * @param {number} alpha
   * @param {number} [scale=1]
   */
  setControlVector(name, dir, alpha, scale) {}

  /** Remove one axis (runtime or dictionary) by name; no-op if unknown. */
  removeControl(name) {}

  /**
   * Cap how hard a STACK of axes may push, in the same alpha units the weights
   * use. Each axis is added to every token row, so what the denoiser sees is
   * the SUM: ten axes at +2 push harder than one at +10, and past a length of
   * roughly the conditioning's own token norm the injection: not the prompt,
   * is what gets rendered (on Krea 2 a ten-axis deck past ~12 alpha turns any
   * scene into dramatic crowds). Over budget, every active axis is scaled by
   * ONE common factor, so the dialled-in mix is kept and only the overdrive is
   * shed. 0 (the default) leaves the stack uncapped.
   * @param {number} alpha
   */
  setControlBudget(alpha) {}

  /**
   * The current stack's length in alpha units, its budget, and whether the next
   * generation will hold it back: what a UI draws a stack meter from.
   * `scale` is the factor every active axis will be multiplied by (1 when in
   * budget).
   * @returns {{norm: number, budget: number, clamped: boolean, scale: number}}
   */
  controlNorm() {}

  /** Zero every axis weight (keeps the dictionary loaded). */
  clearControl() {}

  /**
   * Capture a reference identity and arm Sana's training-free reference-
   * attention seam. Runs ONE full generation of `prompt` (returned as the
   * anchor image, e.g. a neutral portrait) while recording the DiT's per-step
   * linear-attention summaries. Every subsequent generate()/prime() then adds
   * those summaries back: scaled by setIdentityWeight(). So the subject stays
   * the same person while the prompt and control axes drive pose/expression.
   *
   * Sana only (throws on other model classes). The summaries are token-count
   * independent, so the anchor and later generations may differ in size; match
   * `steps` for tight alignment (a shorter later run reuses the anchor's final
   * step). Sharing `seed` with the target tightens identity coherence.
   *
   * @param {string} prompt - the face/subject to hold (e.g. a neutral portrait)
   * @param {object} [opts] - GenerateOptions; use the same steps/seed as later runs
   * @returns {ImageResult} the anchor image, or { cancelled: true }
   *
   * @example
   *   const neutral = pipe.setIdentityAnchor('a portrait of a woman, neutral', { steps: 8, seed: 1 });
   *   pipe.setIdentityWeight(1.0);
   *   pipe.setControl('smile', 3.0);           // push emotion to the extreme…
   *   const held = pipe.generate('a portrait of a woman', { steps: 8, seed: 1 }); // …face holds
   */
  setIdentityAnchor(prompt, opts) {}

  /**
   * Injection strength for the armed identity anchor. 0 (default) disables
   * injection even with an anchor set: a true no-op; ~1 holds identity
   * faithfully; higher over-anchors (identity locked, the edit damped). Takes
   * effect on the next generate()/prime(). Sana only.
   * @param {number} weight
   */
  setIdentityWeight(weight) {}

  /**
   * @returns {boolean} whether an identity anchor has been captured + armed.
   */
  hasIdentityAnchor() {}

  /**
   * Drop the cached anchor and zero the weight (frees the summary cache).
   */
  clearIdentityAnchor() {}
}


// -----------------------------------------------------------------------------
// GenerateOptions  (the `opts` object for generate() and prime())
// -----------------------------------------------------------------------------
//
//   width          number   image width  in pixels (default 512; multiple of 8)
//   height         number   image height in pixels (default 512; multiple of 8)
//   steps          number   denoising steps (default 30)
//   guidanceScale  number   classifier-free guidance (default 7.5; 1.0 disables
//                           the unconditional pass)
//   negativePrompt string   negative prompt (default "")
//   seed           number   RNG seed for the initial latent noise (default 0).
//                           Pass a BigInt for seeds above 2^53.
//   includeFp32    boolean  generate()/decode() only, also attach the raw
//                           NCHW FP32 buffer ([-1,1]) to the result as `fp32`.
//
//   ── img2img / inpaint (SD1.5 only) ───────────────────────────────────────
//   initImagePath  string   if set, prime() VAE-encodes this image and noises
//                           it to the appropriate point in the schedule
//                           instead of starting from pure Gaussian noise.
//                           Decoded by broimage and resized to width×height.
//   strength       number   0..1, fraction of the schedule used for
//                           denoising; higher = more freedom from the init.
//                           Default 0.8. Ignored when initImagePath is empty.
//   vaeEncodeSample boolean false (default) = use the VAE mean; true = sample
//                           mean + exp(0.5*logvar)*eps from the schedule's
//                           Philox stream. Ignored when initImagePath is empty.
//   maskImagePath  string   inpaint mask path. Requires initImagePath. White
//                           (>=128) = inpaint, black = keep. The unmasked
//                           region is re-noised to the next timestep at each
//                           step except the final one.
//
//   ── ControlNet (SD1.5 only) ──────────────────────────────────────────────
//   controls       Array    one entry per registered ControlNet, in
//                           registration order. Length must equal
//                           pipeline.numControlNets() (brodiffusion throws
//                           otherwise). Each entry:
//                             imagePath   string   conditioning image path
//                             scale       number   per-net conditioning_scale
//                                                  (default 1.0; 0.0 disables)
//                             startStep   number   schedule fraction in [0,1)
//                                                  at which this net begins
//                                                  contributing (default 0.0)
//                             endStep     number   schedule fraction in (0,1]
//                                                  at which this net stops
//                                                  (default 1.0; half-open)
//
//   ── noise control (advanced) ─────────────────────────────────────────────
//   noiseSource    string   'internal' (default, brotensor Philox) or
//                           'torch' (bit-compatible with torch.randn for a
//                           seeded reference run). Ignored when initNoise or
//                           initImagePath is set.
//   initNoise      Float32Array   explicit initial latent noise in raw N(0,1)
//                           units, NCHW flat, length C_lat*(H/8)*(W/8).
//                           Overrides noiseSource; cannot combine with
//                           initImagePath. The scheduler's init_noise_sigma
//                           is still applied on top.


// -----------------------------------------------------------------------------
// ImageResult  (returned by generate() and PipelineState.decode())
// -----------------------------------------------------------------------------
//
//   width   number              image width in pixels
//   height  number              image height in pixels
//   data    Uint8ClampedArray   4 * width * height bytes, interleaved RGBA
//                               (HWC), drop-in for ctx.createImageData() +
//                               id.data.set(data) + putImageData().
//   fp32    Float32Array?       present only when opts.includeFp32 was set,
//                               raw 3*H*W planar NCHW values in [-1, 1].


// -----------------------------------------------------------------------------
// PipelineState
// -----------------------------------------------------------------------------
// Opaque handle to a mid-generation state, the working latent plus scheduler
// progress. The working latent stays native across the whole denoising loop;
// it only materializes in JS if you call latent(). Created via Pipeline.prime()
// or PipelineState.clone(). Each state keeps its owning Pipeline alive.

class PipelineState {

  /** @type {number} 0-based count of stepOnce() calls run so far. */
  stepIndex;
  /** @type {number} total scheduled steps for this generation. */
  numSteps;
  /** @type {boolean} true once stepIndex === numSteps. */
  done;
  /** @type {number} latent width  (image width  / 8). */
  latentWidth;
  /** @type {number} latent height (image height / 8). */
  latentHeight;

  /**
   * Advance one denoising step (mutates this state).
   *
   * @param {object} [ctrl]
   * @param {boolean} [ctrl.trace] - capture the per-layer head-averaged cross-
   *        attention maps. The returned object's `trace` is an array of
   *        numXAttnBlocks() entries, each { Lq, Lk, data }: `data` is a
   *        Float32Array of Lq*Lk attention weights (Lk = 77, the CLIP context
   *        length; Lq is that layer's spatial token count).
   * @param {Array<?{data:Float32Array, Lq:number, Lk:number}>} [ctrl.attnBias]
   *        per-layer pre-softmax logit bias. Length must equal
   *        numXAttnBlocks(); each entry is null (no bias) or a same-shaped
   *        (Lq, Lk) bias added to that layer's attention scores. Supplying
   *        attnBias forces trace mode internally.
   * @returns {{trace?: Array<{Lq:number, Lk:number, data:Float32Array}>}}
   *
   * @example
   *   // Step-wise loop with attention readout:
   *   const st = pipe.prime('a red apple', { width: 256, height: 256, steps: 8 });
   *   while (!st.done) {
   *       const r = st.stepOnce({ trace: true });
   *       // r.trace[i].data, attention map for cross-attn block i
   *   }
   *   const img = st.decode();
   *
   * @example
   *   // Attention steering: trace first to learn each layer's Lq, then bias.
   *   const probe = st.stepOnce({ trace: true });
   *   const bias = probe.trace.map(t => ({
   *       Lq: t.Lq, Lk: t.Lk, data: new Float32Array(t.Lq * t.Lk),
   *   }));
   *   bias[6].data[/* Lq*token + tokenIdx *​/ 0] = 2.0;  // boost a token
   *   st.stepOnce({ attnBias: bias });
   */
  stepOnce(ctrl) {}

  /**
   * VAE-decode the current latent to an image.
   * @param {object}  [opts]
   * @param {boolean} [opts.includeFp32] - also attach the raw FP32 buffer.
   * @returns {ImageResult} { width, height, data }
   */
  decode(opts) {}

  /**
   * Download the working latent as a Float32Array (4 * latentHeight *
   * latentWidth values). Small; for visualization / inspection.
   * @returns {Float32Array}
   */
  latent() {}

  /**
   * Deep-copy this state (one latent clone; RNG and counters are trivially
   * copied). The clone advances independently, the basis for branch-and-
   * score / cross-attention tree search. The owning Pipeline is shared.
   * @returns {PipelineState}
   */
  clone() {}
}


// -----------------------------------------------------------------------------
// Workers, running generation off the main thread
// -----------------------------------------------------------------------------
//
// bro.diffusion is installed in Worker contexts too. A Worker is an isolated
// JS context with its own native heap: it must load its own weights and own
// its own Pipeline. Treat the Worker as a long-lived inference server, load
// the weights once, then feed it prompts; never reload per generation.
//
// Only plain cloneable data crosses postMessage. A Pipeline / PipelineState
// handle cannot (and need not) cross, keep it inside the Worker. Send the
// prompt and options in; send the result image's typed array back (transfer
// it for zero-copy).
//
//   // --- worker script: sd-worker.js ---
//   let pipe = null;
//   self.onmessage = (e) => {
//       const m = e.data;
//       if (m.cmd === 'load') {
//           pipe = bro.diffusion.createPipeline({
//               vocabPath: m.vocab, mergesPath: m.merges });
//           pipe.loadWeights(m.text, m.unet, m.vae);
//           self.postMessage({ ready: true });
//       } else if (m.cmd === 'generate') {
//           const img = pipe.generate(m.prompt, m.opts);
//           self.postMessage({ image: img }, [img.data.buffer]);
//       }
//   };
//
//   // --- main thread ---
//   const w = new Worker('sd-worker.js');
//   w.onmessage = (e) => {
//       if (e.data.ready) w.postMessage({ cmd: 'generate',
//           prompt: 'a lighthouse at dawn', opts: { steps: 20 } });
//       else drawToCanvas(e.data.image);
//   };
//   w.postMessage({ cmd: 'load', vocab: '...', merges: '...',
//                   text: '...', unet: '...', vae: '...' });
//
// CPU generation is slow (minutes per image); a GPU build is strongly
// recommended for interactive use, build bro with -DBROGAMEAGENT_WITH_CUDA=ON,
// which also compiles brodiffusion's fused CUDA kernels.
//
// =============================================================================
