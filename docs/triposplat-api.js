// ── Classes & Interfaces ─────────────────────────────────────────────────────

class TripoSplatPipeline {

  /**
   * Target execution device name.
   * @readonly
   * @type {string}
   */
  device;

  /**
   * True if BiRefNet background matte removal preprocessor is active.
   * @readonly
   * @type {boolean}
   */
  backgroundRemoval;

  /**
   * Generate 3D Gaussian Splat cloud from input image.
   *
   * @param {*} image
   * @param {Object} [opts]
   * @returns {Object}
   */
  generate(image, opts) {}

}

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * =============================================================================
 * bro.triposplat — single-image to 3D Gaussian Splat pipeline
 * =============================================================================
 *
 * Feedforward 3D reconstruction pipeline assembling DINOv3 ViT-H, Flux.2 VAE encoder,
 * FlowDiT diffusion transformer, and Octree Gaussian Decoder into real-time 3D Gaussian splats.
 * @example
 * const pipeline = bro.triposplat.load({
 *     dinov3: "weights/dinov3.safetensors",
 *     vae: "weights/vae.safetensors",
 *     flow: "weights/flow.safetensors",
 *     decoder: "weights/decoder.safetensors"
 *   });
 *   const cloud = pipeline.generate(imageBitmap, { steps: 25 });
 *   scene.createGaussianSplat({ cloud, scale: 1.0 });
 */
/**
 * Initialize brotensor acceleration subsystem.
 */
bro.triposplat.init = function() {};

/**
 * Load TripoSplat model checkpoint pipelines.
 *
 * @param {Object} opts
 * @returns {TripoSplatPipeline}
 */
bro.triposplat.load = function(opts) {};

/**
 * Request cancellation of active reconstruction generation.
 */
bro.triposplat.cancel = function() {};

