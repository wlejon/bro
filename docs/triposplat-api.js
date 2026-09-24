/**
 * =============================================================================
 * bro.triposplat — single image to a 3D Gaussian splat
 * =============================================================================
 *
 * TripoSplat (VAST-AI/TripoSplat) reconstructed over the bro sibling stack —
 * the binding is installed by brodiffusion, alongside bro.diffusion:
 *
 *   Flux.2 VAE encoder (brodiffusion)      → image-conditioning tokens
 *   flow-matching DiT  (brodiffusion)      → rectified-flow Euler+CFG sampler → latent
 *   octree Gaussian decoder (brodiffusion) → an explicit 3D Gaussian cloud
 *
 * generate() preprocesses the image (alpha-eroded bounding box, cover-fit into
 * a 1024² canvas, composited over black by its own alpha), runs the encoder,
 * draws seeded noise, samples the latent and decodes it to Gaussians. The
 * cloud comes back as typed arrays in the scene's Y-up convention, the subject
 * upright and facing +Z (the image's left on -x, as a camera on +Z sees it) —
 * the sampler works Z-up facing +X and the binding rotates positions and
 * quaternions on the way out — so it feeds scene.createGaussianSplat({ cloud })
 * directly (EWA splatting, see docs/scene-api.js). exportPLY/exportSplat
 * write the same scene-space cloud.
 *
 * GPU by default (FP16), like every bro ML namespace: gate the load on
 * `bro.gpu`. The pipeline is heavy — seconds to a couple of minutes per image —
 * and generate() is ONE synchronous native call, so run it in a Worker (the
 * binding is installed in worker realms too) and cancel from the main thread.
 *
 * Background removal runs only when the pipeline was loaded with a BiRefNet
 * matte model (load({ birefnet })): generate() then replaces the image's alpha
 * with the predicted matte. Without it the preprocessor composites the
 * image's OWN alpha over black, so hand it a pre-masked / foreground-on-black
 * image (bro.vision's BiRefNet matting is the natural upstream step — see
 * docs/vision-api.js).
 *
 * With DINOv3 loaded, its features pass through an affine-free LayerNorm over
 * the channels (the reference's F.layer_norm) before conditioning the flow
 * model.
 *
 * Profiling: run with the environment variable BRO_TRIPOSPLAT_PROFILE=1 and
 * every generate() prints one stderr line per stage — read image, birefnet
 * matte, preprocess, VAE encode, DINOv3 encode, feature1 LayerNorm, feature2
 * assembly, flow sampler (N steps), octree decode, pack — and the TOTAL, in
 * milliseconds. GPU stages are timed after a device sync, so each line is
 * that stage's real cost. The first generate() includes one-time warmup;
 * the second is the steady-state figure.
 *
 * @example
 *   if (!bro.gpu.available) console.warn('No GPU — TripoSplat on CPU is impractical.');
 *
 *   const ts = bro.triposplat.load({
 *     dinov3:  'weights/triposplat/clip_vision/dino_v3_vit_h.safetensors',
 *     vae:     'weights/triposplat/vae/flux2-vae.safetensors',
 *     flow:    'weights/triposplat/diffusion_models/triposplat_fp16.safetensors',
 *     decoder: 'weights/triposplat/vae/triposplat_vae_decoder_fp16.safetensors',
 *   });
 *
 *   const img   = await loadImageData('subject.png');       // { data, width, height }
 *   const cloud = ts.generate(img, { steps: 12, numGaussians: 131072 });
 *   if (cloud.cancelled) return;
 *
 *   const scene = canvas.getContext('scene');
 *   scene.createGaussianSplat({ cloud, scale: 1.0 });       // EWA-splatted live
 *   ts.exportPLY('subject.ply');                            // or .exportSplat()
 */

// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * SplatCloud — the render-ready SoA generate() returns and the export
 * functions consume. All arrays are packed per-splat with `count` splats.
 *
 * @typedef {Object} SplatCloud
 * @property {Float32Array} positions  xyz, 3*count, scene space (Y-up)
 * @property {Float32Array} scales     xyz, 3*count, LINEAR (not log) units
 * @property {Float32Array} rotations  xyzw unit quaternions, 4*count
 * @property {Float32Array} opacities  [0,1], count entries (not logit space)
 * @property {Float32Array} sh         spherical-harmonic coefficients, coefficient-major
 *           and channel-minor (sh[coeff*3 + channel]), stride 3*(shDegree+1)² per splat;
 *           the first three are the DC color term. TripoSplat's decoder emits shDegree 0,
 *           so in practice that is one DC RGB triple per splat.
 * @property {number} shDegree         SH band count - 1 (0 = DC color only)
 * @property {number} count            number of splats
 */

/**
 * SplatImage — what generate() accepts as input.
 *
 * @typedef {string|{data: Uint8ClampedArray|Uint8Array, width: number, height: number}} SplatImage
 *   Either a path to an image file (decoded by broimage; used VERBATIM, so resolve
 *   app-relative paths with bro.resolvePath() first) or an ImageData-shaped object of
 *   RGBA bytes — at least width*height*4 of them. The alpha channel is what isolates
 *   the subject.
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * A loaded TripoSplat pipeline. Created by bro.triposplat.load(); the class
 * object (`bro.triposplat.TripoSplatPipeline`, also the global
 * `TripoSplatPipeline`) is exposed for `instanceof` only and throws TypeError
 * if called as a constructor.
 *
 * The handle also remembers the MOST RECENT generate() result, which is what
 * the no-cloud export methods write.
 */
class TripoSplatPipeline {

  /**
   * Device the models run on: 'CUDA' | 'Metal' | 'CPU'.
   * @readonly
   * @type {string}
   */
  device;

  /**
   * True when a BiRefNet matte model was loaded (`load({ birefnet })`), i.e.
   * generate() can isolate the subject. Lets a UI gate a "remove background"
   * control.
   * @readonly
   * @type {boolean}
   */
  backgroundRemoval;

  /**
   * Reconstruct a Gaussian cloud from a single image. Blocking; the result is
   * also retained on the handle for exportPLY(path) / exportSplat(path).
   *
   * Returns { cancelled: true } instead of a cloud when bro.triposplat.cancel()
   * landed during the run — check for it before reading `.count`.
   *
   * @param {SplatImage} image
   * @param {object} [opts]
   * @param {number} [opts.seed=42]             noise + jitter seed (deterministic)
   * @param {number} [opts.steps=20]            Euler sampler steps (more = finer, ~linear cost)
   * @param {number} [opts.guidanceScale=3.0]   classifier-free guidance; <=1 disables CFG
   * @param {number} [opts.shift=3.0]           flow-matching timestep-schedule shift
   * @param {number} [opts.numGaussians=131072] target splat count, rounded down to a
   *        multiple of the decoder's Gaussians-per-point (32); 32768–262144 is the
   *        useful range
   * @param {boolean} [opts.removeBackground] replace the image's alpha with the
   *        BiRefNet matte before encoding. Default: on when `backgroundRemoval`
   *        is true; pass false for an already-masked input.
   * @returns {SplatCloud|{cancelled: true}}
   */
  generate(image, opts) {}

  /**
   * Exact alias of generate(image, opts).
   * @param {SplatImage} image
   * @param {object} [opts]
   * @returns {SplatCloud|{cancelled: true}}
   */
  imageTo3D(image, opts) {}

  /**
   * Write the most recent generate() result as a binary little-endian .ply
   * (the 3DGS convention: positions, zero normals, f_dc_N and f_rest_N SH, LOGIT
   * opacity, LOG scales, wxyz rotation) — the format the usual viewers and
   * trainers read.
   *
   * Throws if nothing has been generated yet or the file cannot be written.
   * The path is used verbatim.
   *
   * @param {string} path
   * @returns {true}
   */
  exportPLY(path) {}

  /** Alias of exportPLY(path). @param {string} path @returns {true} */
  savePly(path) {}

  /**
   * Write the most recent generate() result as a compact .splat file: 32 bytes
   * per splat — float position xyz, float scale xyz, RGBA bytes (SH DC term
   * converted to sRGB-ish color plus opacity), and 4 quantized rotation bytes.
   * Roughly an order of magnitude smaller than the .ply, at viewer fidelity.
   *
   * Throws if nothing has been generated yet or the file cannot be written.
   *
   * @param {string} path
   * @returns {true}
   */
  exportSplat(path) {}

  /** Alias of exportSplat(path). @param {string} path @returns {true} */
  saveSplat(path) {}
}

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * Initialize the brotensor runtime (probe CUDA/Metal, register backends).
 * Idempotent; load() calls it too. Exposed so a Worker can warm up before its
 * first message.
 * @returns {undefined}
 */
bro.triposplat.init = function() {};

/**
 * Load the TripoSplat checkpoints and place them on the compute device.
 * Blocking and multi-GB — run it in a Worker. Every path given must exist or
 * it throws; paths go through the asset-path resolver.
 *
 * @param {object} paths
 * @param {string} [paths.dinov3] DINOv3 ViT-H weights (brovisionml): a
 *        .safetensors file or a directory. Also accepted as `paths.dino`.
 *        Optional: when given, generate() encodes the image with it as the
 *        flow model's first conditioning; without it that slot is zeros.
 * @param {string} paths.vae      Flux.2 VAE encoder safetensors (brodiffusion weights)
 * @param {string} paths.flow     flow-DiT safetensors (brodiffusion weights)
 * @param {string} paths.decoder  octree Gaussian decoder safetensors (brodiffusion weights)
 * @param {string} [paths.birefnet] optional BiRefNet (Swin-L) matte safetensors
 *        (brovisionml weights); enables background removal in generate().
 * @param {string} [paths.device] 'cuda' | 'metal' | 'cpu'. Default: the best backend
 *        brotensor reports available.
 * @returns {TripoSplatPipeline}
 */
bro.triposplat.load = function(paths) {};

/**
 * Request that an in-flight generate() abort. generate() is a single
 * synchronous native call: run it inside a Worker and call this from the MAIN
 * thread. The cancel is cooperative — it lands after the VAE encode, between
 * Euler sampler steps, or at the next stage boundary, so a run already inside
 * the octree decode finishes that stage first. The aborted generate() returns
 * { cancelled: true }.
 *
 * With no argument it stops every generate() in flight in the process — the
 * main thread's way to reach a Worker's run. With a TripoSplatPipeline it
 * stops only that pipeline's run. A generate() that starts after the call is
 * not affected.
 * @param {TripoSplatPipeline} [pipeline]
 * @returns {undefined}
 */
bro.triposplat.cancel = function(pipeline) {};

/**
 * Write ANY SplatCloud to a binary .ply — the free-function form, for a cloud
 * you are holding (edited, loaded from elsewhere, or from another pipeline
 * handle) rather than the one a pipeline last produced.
 *
 * Only `positions` is mandatory in the object; missing scales / rotations /
 * opacities / sh stay empty and `shDegree` defaults to the cloud's own value.
 * An object without a positions Float32Array throws.
 *
 * @param {SplatCloud} splats
 * @param {string} path
 * @returns {true}
 *
 * @example
 *   const cloud = ts.generate(img);
 *   const keep = { ...cloud };                     // e.g. after filtering splats
 *   bro.triposplat.exportPLY(keep, 'filtered.ply');
 */
bro.triposplat.exportPLY = function(splats, path) {};

/**
 * Write ANY SplatCloud to a compact 32-byte-per-splat .splat file. Same
 * argument rules as bro.triposplat.exportPLY().
 * @param {SplatCloud} splats
 * @param {string} path
 * @returns {true}
 */
bro.triposplat.exportSplat = function(splats, path) {};

/**
 * The TripoSplatPipeline class object — for `instanceof`. Not constructible
 * (throws TypeError); also registered as the global `TripoSplatPipeline`.
 * @type {Function}
 */
bro.triposplat.TripoSplatPipeline;

// =============================================================================
