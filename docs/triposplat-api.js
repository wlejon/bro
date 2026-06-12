// =============================================================================
// bro.triposplat — single image -> 3D Gaussian Splat
// =============================================================================
//
// TripoSplat (VAST-AI/TripoSplat) reconstructed as a composition over the bro
// sibling stack — no single library owns it:
//
//   DINOv3 ViT-H (brovisionml)  ─┐
//   Flux.2 VAE encoder (brodiffusion) ─┤→ two image-conditioning features
//   flow-matching DiT (brodiffusion)  ─┤→ rectified-flow Euler+CFG sampler → latent
//   octree Gaussian decoder (brodiffusion) ─┘→ explicit 3D Gaussian cloud
//
// Each piece is golden-tested in its own repo; bro.triposplat is the glue:
// preprocess the image, run the encoders, draw seeded noise, sample the latent,
// decode to a Gaussian cloud, and return the splats as typed arrays ready for
// the scene GaussianSplatNode (EWA splatting) via scene.createGaussianSplat.
//
// GPU by default (FP16). The pipeline is heavy (a few seconds to a couple of
// minutes per image, dominated by the DINOv3 encode and the per-block rotary);
// run generate() inside a Worker to keep the UI responsive — the binding is
// installed in the worker context too.
//
// Background removal: pass the optional `birefnet` checkpoint to load() and the
// upstream BiRefNet (Swin-L + ASPP-deformable) matte is predicted per image and
// used to isolate the subject before the cover-fit / composite-over-black step.
// Without it, the preprocessor only cover-fits to 1024² and composites the
// image's own alpha over black — so give a pre-masked / foreground-on-black
// image for the best result.

// -----------------------------------------------------------------------------
// bro.triposplat
// -----------------------------------------------------------------------------

/**
 * Initialize the brotensor runtime (idempotent). Optional — load() calls it.
 */
bro.triposplat.init = function () {};

/**
 * Load the four TripoSplat checkpoints and place them on the compute device.
 *
 * @param {object} paths
 * @param {string} paths.dinov3   DINOv3 ViT-H safetensors (brovisionml weights).
 * @param {string} paths.vae      Flux.2 VAE safetensors (brodiffusion weights).
 * @param {string} paths.flow     flow-DiT safetensors (brodiffusion weights).
 * @param {string} paths.decoder  octree-decoder safetensors (brodiffusion weights).
 * @param {string} [paths.birefnet] Optional BiRefNet bg-removal safetensors
 *        (brovisionml weights). When given, generate() replaces the input alpha
 *        with BiRefNet's predicted matte before compositing.
 * @param {string} [paths.device] "cuda" | "metal" | "cpu". Default: best available.
 * @returns {TripoSplatPipeline}
 */
bro.triposplat.load = function (paths) {};

// -----------------------------------------------------------------------------
// TripoSplatPipeline
// -----------------------------------------------------------------------------

class TripoSplatPipeline {
  /** Device the models run on ("CUDA" | "Metal" | "CPU"). */
  get device() {}

  /** True when a BiRefNet matte model was loaded (the `birefnet` path was
   *  given to load()) — i.e. generate() can isolate the subject. Use it to
   *  gate a "remove background" toggle in a UI. */
  get backgroundRemoval() {}

  /**
   * Reconstruct a Gaussian cloud from a single image.
   *
   * @param {ImageBitmap | {data: Uint8ClampedArray, width: number, height: number}} image
   *        RGBA pixels (ImageBitmap or ImageData shape).
   * @param {object} [opts]
   * @param {number} [opts.seed=42]            Noise + jitter seed (deterministic).
   * @param {number} [opts.steps=20]           Euler sampler steps (more = finer; ~linear cost).
   * @param {number} [opts.guidanceScale=3.0]  Classifier-free guidance; <=1 disables CFG.
   * @param {number} [opts.shift=3.0]          Flow-matching timestep-schedule shift.
   * @param {number} [opts.numGaussians=131072] Target splat count (rounded down to a
   *        multiple of 32; 32768–262144 recommended).
   * @param {boolean} [opts.removeBackground] Run BiRefNet matting on this image
   *        before reconstruction. Defaults to true when load() was given a
   *        `birefnet` checkpoint, false otherwise. Set false to skip matting for
   *        an already-masked / foreground-on-black input (no reload needed).
   * @returns {{positions: Float32Array, scales: Float32Array, rotations: Float32Array,
   *           opacities: Float32Array, sh: Float32Array, shDegree: number, count: number}
   *           | {cancelled: true}}
   *   Render-ready SoA (positions xyz / scales xyz linear / rotations xyzw unit /
   *   opacities [0,1] / SH-DC color). Feeds scene.createGaussianSplat({ cloud }).
   *   Returns `{ cancelled: true }` instead (no cloud) when bro.triposplat.cancel()
   *   was called during the run — check for it before reading `.count`.
   */
  generate(image, opts) {}
}

/**
 * Request that an in-flight generate() abort. generate() is a single synchronous
 * native call — run it inside a Worker and call this from the MAIN thread to
 * interrupt it. The cancel is cooperative: it lands at the next stage boundary
 * (after the DINOv3 / VAE encoders) or between Euler sampler steps, so a run
 * stuck in the octree decode finishes that stage first. The aborted generate()
 * resolves with `{ cancelled: true }`. No-op when nothing is running; the flag
 * is cleared at the start of each generate().
 *
 * @function cancel
 * @memberof bro.triposplat
 */

// -----------------------------------------------------------------------------
// Example
// -----------------------------------------------------------------------------
//
//   const ts = bro.triposplat.load({
//     dinov3:  "weights/triposplat/clip_vision/dino_v3_vit_h.safetensors",
//     vae:     "weights/triposplat/vae/flux2-vae.safetensors",
//     flow:    "weights/triposplat/diffusion_models/triposplat_fp16.safetensors",
//     decoder: "weights/triposplat/vae/triposplat_vae_decoder_fp16.safetensors",
//   });
//
//   const img   = await loadImageData("subject.png");          // { data, width, height }
//   const cloud = ts.generate(img, { steps: 12, numGaussians: 131072 });
//
//   const scene = canvas.getContext("scene");
//   scene.createGaussianSplat({ cloud, scale: 1.0 });          // EWA-splatted live
