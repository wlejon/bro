/**
 * =============================================================================
 * bro.motion — Text-to-Motion Generation (NVIDIA ARDY, Unitree G1 Skeleton)
 * =============================================================================
 *
 * Generates humanoid motion clips from text prompts using the NVIDIA ARDY-G1
 * autoregressive diffusion motion model at 25 fps.
 *
 * @example
 *   // Load ARDY motion pipeline and generate walking clip
 *   const m = bro.motion.load({
 *     checkpoint: '../brodiffusion/weights/ardy-g152',
 *     textEncoder: '../brolm/weights/llm2vec-llama3-8b'
 *   });
 *   const clip = m.generate('a person walks forward and waves', {
 *     frames: 104,
 *     steps: 10,
 *     cfg: 2.5,
 *     seed: 0
 *   });
 *   console.log('Generated frames:', clip.frames, 'joints:', clip.joints, 'fps:', clip.fps);
 *
 * @example
 *   // Probe runtime backends
 *   bro.motion.init();
 */

// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * Configuration options for loading an ARDY motion model pipeline.
 * @typedef {Object} MotionLoadOptions
 * @property {string} [checkpoint] -  Path to ARDY g152 weights directory (denoiser, tokenizer, stats)
 * @property {string} [textEncoder] -  Path to merged LLM2Vec Llama-3-8B text encoder weights directory
 * @property {string} [device] -  Optional target device ('cuda', 'metal', or 'cpu')
 */

/**
 * Generation options for ARDY text-to-motion inference.
 * @typedef {Object} MotionGenerateOptions
 * @property {number} [frames=104] -  Requested clip length in frames at 25 fps (rounded to multiple of 52)
 * @property {number} [steps=10] -  DDIM denoising steps per window
 * @property {number} [cfg=2.5] -  Classifier-free guidance weight
 * @property {number} [seed=0] -  RNG seed for noise reproducibility
 * @property {number} [heading=0] -  Frame-0 heading angle in radians
 */

/**
 * Generated motion clip containing skeletal transforms and foot contact state.
 * @typedef {Object} MotionClip
 * @property {number} [frames] -  Actual frame count
 * @property {number} [joints] -  Joint count (34 for Unitree G1)
 * @property {number} [fps] -  Frames per second (25)
 * @property {Float32Array} [positions] -  World joint positions (frames * joints * 3 floats in meters)
 * @property {Int32Array} [parents] -  Joint parent indices (34 entries, -1 for root)
 * @property {Float32Array} [footContacts] -  Foot contact indicator flags (frames * 4 floats: L-heel, L-toe, R-heel, R-toe)
 * @property {Float32Array} [rootPositions] -  World root (pelvis) translation (frames * 3 floats in meters)
 * @property {Float32Array} [localRotations] -  Parent-relative joint rotations, row-major 3x3 (frames * joints * 9 floats)
 * @property {Float32Array} [globalRotations] -  World-space joint rotations, row-major 3x3 (frames * joints * 9 floats)
 * @property {Float32Array} [localQuaternions] -  `localRotations` as quaternions, (x, y, z, w) (frames * joints * 4 floats)
 * @property {Float32Array} [globalQuaternions] -  `globalRotations` as quaternions, (x, y, z, w) (frames * joints * 4 floats)
 */

/**
 * The G1Skeleton34 rest pose and topology, independent of any loaded checkpoint.
 * @typedef {Object} MotionSkeleton
 * @property {string} [name] -  Skeleton identifier ('g1skel34')
 * @property {number} [joints] -  Joint count (34)
 * @property {number} [root] -  Root joint index (0, the pelvis)
 * @property {string[]} [names] -  Joint names in bone order, e.g. 'pelvis', 'left_knee', 'right_hand_roll'
 * @property {Int32Array} [parents] -  Joint parent indices (-1 for root); always topologically sorted, so FK is one pass
 * @property {Float32Array} [neutral] -  Rest offsets relative to the root (joints * 3 floats in meters)
 * @property {Float32Array} [localOffsets] -  Rest offsets relative to each joint's parent — the bind-pose bone vectors (joints * 3 floats)
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * ARDY text-to-motion generation pipeline instance.
 */
class ArdyMotionPipeline {

  /**
   *  Execution device ('CUDA', 'Metal', or 'CPU')
   * @readonly
   * @type {string}
   */
  device;

  /**
   * Generates a motion clip from a text prompt.
   *
   * @param {string} text - Motion prompt description
   * @param {MotionGenerateOptions} [opts] - Generation configuration options
   * @returns {MotionClip} Generated motion clip object
   */
  generate(text, opts) {}

}

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * Motion generation engine namespace.
 */
/**
 * Initializes the brotensor runtime backend probe.
 */
bro.motion.init = function() {};

/**
 * Loads an ARDY motion model pipeline.
 *
 * @param {MotionLoadOptions} opts - Configuration paths and device
 * @returns {ArdyMotionPipeline} Loaded motion generation pipeline
 */
bro.motion.load = function(opts) {};

/**
 * Returns the G1Skeleton34 rest pose and topology. Baked-in model architecture
 * rather than trained weights, so it needs no checkpoint, no GPU, and no
 * `load()` — a tool can build a rig against it before any model exists.
 *
 * @returns {MotionSkeleton} Joint names, parents, and rest offsets
 */
bro.motion.skeleton = function() {};

