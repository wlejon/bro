// ── Classes & Interfaces ─────────────────────────────────────────────────────

class Pipeline {

  /**
   * True if model weights are loaded and ready for inference.
   * @readonly
   * @type {boolean}
   */
  weightsLoaded;

  /**
   * Load model safetensors weights file into pipeline.
   *
   * @param {string} path
   */
  loadWeights(path) {}

  /**
   * Generate image from text prompt.
   *
   * @param {Object} [opts]
   * @returns {Object}
   */
  generate(opts) {}

}

class PipelineState {

  /**
   * Current denoising step index.
   * @readonly
   * @type {number}
   */
  step;

  /**
   * True if denoising trajectory has completed.
   * @readonly
   * @type {boolean}
   */
  done;

}

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * =============================================================================
 * bro.diffusion — text-to-image neural diffusion inference pipeline
 * =============================================================================
 *
 * Multi-architecture neural diffusion pipeline supporting SD1.5, LCM, SCM, FlowMatch,
 * and Flux DiT architectures. Supports one-shot and step-wise latent sampling, ControlNet,
 * LoRA adapters, cross-attention steering, and noise expansion.
 * @example
 * const pipe = bro.diffusion.loadModel("weights/sd15", { device: "cuda" });
 *   const img = pipe.generate({
 *     prompt: "a golden retriever in a field of sunflowers",
 *     steps: 20,
 *     guidanceScale: 7.5
 *   });
 */
/**
 * Brodiffusion version string.
 * @readonly
 * @type {string}
 */
bro.diffusion.version;

/**
 * Initialize brotensor inference acceleration.
 */
bro.diffusion.init = function() {};

/**
 * Instantiate diffusion pipeline from model architecture configuration.
 *
 * @param {Object} config
 * @returns {Pipeline}
 */
bro.diffusion.createPipeline = function(config) {};

/**
 * Load complete diffusion model checkpoint directory.
 *
 * @param {string} dir
 * @param {Object} [opts]
 * @returns {Pipeline}
 */
bro.diffusion.loadModel = function(dir, opts) {};

/**
 * Transport identity latent noise across resolution upscaling factors.
 *
 * @param {Float32Array} src
 * @param {Object} opts
 * @returns {Float32Array}
 */
bro.diffusion.expandNoise = function(src, opts) {};

