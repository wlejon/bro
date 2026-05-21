// =============================================================================
// bro.diffusion API Reference
// =============================================================================
//
// Diffusion-model text-to-image inference, backed by the brodiffusion sibling
// library. Two model families are supported:
//   - Stable Diffusion 1.5 — CLIP text encoder + U-Net + VAE, DDIM/LCM
//     schedulers, LoRA, INT8 quantization.
//   - Flux — CLIP (pooled) + T5-XXL text encoders + Flux DiT denoiser + VAE,
//     flow-match scheduler.
// loadModel() takes a diffusers model directory and auto-detects the family;
// createPipeline() builds the SD1.5 stack explicitly.
//
// The native Pipeline owns the multi-GB model weights. JavaScript never holds
// or moves weight bytes — it holds an opaque *handle* (the Pipeline object).
// Latents and attention maps are small and download to Float32Arrays on
// demand. Only the final decoded image crosses into JS as pixel data.
//
// bro.diffusion is installed in both the main JS context and every Worker
// context, and is available on CPU-only builds (brodiffusion's CPU FP32 path
// is always built). It runs on whatever backend brotensor resolves at
// runtime — CPU FP32 by default, CUDA/Metal FP16 when a GPU build has one.
//
// Two usage modes, one binding:
//   - Main thread:  step-wise prime() -> stepOnce() -> decode(), inspecting
//                   latents and cross-attention traces each step.
//   - Worker:       full generate() off the main thread (see "Workers" below).
//
// Weights are not bundled. brodiffusion ships scripts/download-weights.ps1;
// each model is a diffusers-format export — text_encoder/, unet/, vae/, and a
// tokenizer/ with vocab.json + merges.txt.
//
// =============================================================================


// -----------------------------------------------------------------------------
// bro.diffusion — namespace
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
   *
   * Every weight and tokenizer is loaded by this call, so the returned
   * Pipeline needs no loadWeights() — call generate()/prime() directly. This
   * is the only way to run a Flux model. Blocking and slow (multi-GB read);
   * run it in a Worker. Check pipeline.config().modelClass for the family.
   *
   * @param {string} modelDir - diffusers model directory root
   * @returns {Pipeline}
   *
   * @example
   *   const pipe = bro.diffusion.loadModel('/path/to/flux-schnell');
   *   const img  = pipe.generate('a fox in autumn leaves', { steps: 4 });
   */
  loadModel(modelDir) {},

  /**
   * Create a Stable Diffusion 1.5 inference Pipeline. Loads the CLIP
   * tokenizer and builds the model graph (no weights yet — call
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
   *     A diffusers three-file export — text_encoder/, unet/, vae/.
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
   * Merge a LoRA file's deltas into the loaded weights. Call after
   * loadWeights() and before generate()/prime(). Stackable — call repeatedly
   * to layer multiple LoRAs. Both kohya-ss/A1111 and diffusers/PEFT key
   * conventions are auto-detected.
   *
   * @param {string} path    - LoRA .safetensors file
   * @param {number} [scale] - multiplier on the per-LoRA alpha/rank factor
   *                           (default 1.0; may be negative to subtract)
   */
  applyLora(path, scale) {}

  /**
   * One-shot text-to-image generation. Blocking — runs the full denoising
   * loop synchronously. Intended for a Worker thread; on the main thread use
   * the step-wise prime()/stepOnce()/decode() API so the event loop stays
   * responsive.
   *
   * @param {string} prompt
   * @param {object} [opts]                 - see GenerateOptions below
   * @returns {ImageResult} { width, height, data }
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
   * Read-only snapshot of the resolved pipeline configuration. `modelClass`
   * is 'StableDiffusion' or 'Flux'; `scheduler` is 'ddim', 'lcm', or
   * 'flowmatch'. `timeCondProjDim`, `quantizeWeights`, and `numXAttnBlocks`
   * describe the SD1.5 U-Net and read 0/false for a Flux model.
   * @returns {{modelClass:string, scheduler:string, timeCondProjDim:number,
   *            quantizeWeights:boolean, numXAttnBlocks:number,
   *            weightsLoaded:boolean}}
   */
  config() {}
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
//   includeFp32    boolean  generate()/decode() only — also attach the raw
//                           NCHW FP32 buffer ([-1,1]) to the result as `fp32`.


// -----------------------------------------------------------------------------
// ImageResult  (returned by generate() and PipelineState.decode())
// -----------------------------------------------------------------------------
//
//   width   number              image width in pixels
//   height  number              image height in pixels
//   data    Uint8ClampedArray   4 * width * height bytes, interleaved RGBA
//                               (HWC) — drop-in for ctx.createImageData() +
//                               id.data.set(data) + putImageData().
//   fp32    Float32Array?       present only when opts.includeFp32 was set —
//                               raw 3*H*W planar NCHW values in [-1, 1].


// -----------------------------------------------------------------------------
// PipelineState
// -----------------------------------------------------------------------------
// Opaque handle to a mid-generation state — the working latent plus scheduler
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
   *       // r.trace[i].data — attention map for cross-attn block i
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
   * copied). The clone advances independently — the basis for branch-and-
   * score / cross-attention tree search. The owning Pipeline is shared.
   * @returns {PipelineState}
   */
  clone() {}
}


// -----------------------------------------------------------------------------
// Workers — running generation off the main thread
// -----------------------------------------------------------------------------
//
// bro.diffusion is installed in Worker contexts too. A Worker is an isolated
// JS context with its own native heap: it must load its own weights and own
// its own Pipeline. Treat the Worker as a long-lived inference server — load
// the weights once, then feed it prompts; never reload per generation.
//
// Only plain cloneable data crosses postMessage. A Pipeline / PipelineState
// handle cannot (and need not) cross — keep it inside the Worker. Send the
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
// recommended for interactive use — build bro with -DBROGAMEAGENT_WITH_CUDA=ON,
// which also compiles brodiffusion's fused CUDA kernels.
//
// =============================================================================
