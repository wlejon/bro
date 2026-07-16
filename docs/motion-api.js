/**
 * bro.motion — Text-to-motion (nvidia ARDY, Unitree G1 humanoid)
 *
 * Generates a humanoid motion clip from a text prompt: per-frame world-space
 * joint positions (plus the skeleton hierarchy and foot contacts) ready to
 * drive a scene skeleton / mesh. The model is ARDY
 * (nvidia/ARDY-G1-RP-25FPS-Horizon52, "g152") — a text-conditioned
 * autoregressive diffusion motion model for the 34-joint Unitree G1 skeleton
 * at 25 fps.
 *
 * The C++ port is a composition over two siblings — no single library owns it:
 *
 *   Llama-3 tokenizer + LLM2Vec text encoder (brolm) ─→ pooled (4096) text feature
 *   two-stage diffusion denoiser (brodiffusion::ardy) ─┐
 *   FSQ motion decoder                                 ├→ autoregressive window
 *   spaced-DDIM window sampler + AR rollout            │  rollout → explicit
 *   G1 motion rep + forward kinematics                 ┘  features → FK → joints
 *
 * Each piece is golden-tested in its own repo; bro.motion is the glue: encode
 * the prompt, roll out the hybrid motion window by window (52 frames per
 * window, classifier-free-guided DDIM), detokenize to explicit motion
 * features, unnormalize, and run FK to world joint positions.
 *
 * GPU by default (brotensor CUDA/Metal when available, CPU fallback). Heavy —
 * load() reads an 8B text encoder plus the motion model, and generate() is a
 * multi-second synchronous native call — so run the pipeline inside a Worker
 * to keep the UI responsive; the binding is installed in worker contexts too.
 *
 * Everything here is synchronous and throws on error (no callbacks, no
 * promises): load() throws TypeError on malformed options and Error when a
 * model file can't be read; generate() throws TypeError on a missing prompt.
 *
 * Model files (both directories, converted/downloaded offline):
 *   checkpoint   — the ARDY g152 dir. Download with brodiffusion's
 *                  scripts/download-ardy.sh (nvidia/ARDY-G1-RP-25FPS-Horizon52,
 *                  ungated). Contents the binding reads:
 *                    denoiser.safetensors                (two-stage denoiser)
 *                    tokenizer.safetensors               (FSQ motion autoencoder)
 *                    stats/motion/{mean,std}.npy         (418-entry motion stats)
 *                    stats/post_quantization/{mean,std}.npy
 *   textEncoder  — the merged LLM2Vec Llama-3-8B dir: model.safetensors +
 *                  config.json + tokenizer.json. Built offline from
 *                  McGill-NLP/LLM2Vec-Meta-Llama-3-8B-Instruct-mntp (+ the
 *                  -supervised LoRA adapter) by brolm's
 *                  scripts/convert-llm2vec.py, which merges the adapters into a
 *                  plain HF Llama safetensors checkpoint.
 */


// ── Init (optional) ─────────────────────────────────────────────────────────

/**
 * Initialize the brotensor runtime (registers the CUDA/Metal backends).
 * Idempotent, and load() calls it for you — only useful to front-load the
 * driver probe.
 */
bro.motion.init();


// ── Load ────────────────────────────────────────────────────────────────────

/**
 * Load the ARDY pipeline: denoiser + FSQ decoder + normalization stats from
 * the checkpoint dir, and the Llama-3 tokenizer + LLM2Vec encoder from the
 * text-encoder dir. Synchronous and blocking (multi-GB of weights).
 *
 * @param {Object} opts
 * @param {string} opts.checkpoint   - ARDY g152 directory (see header).
 * @param {string} opts.textEncoder  - merged LLM2Vec Llama-3 directory.
 * @param {string} [opts.device]     - 'cuda' | 'metal' | 'cpu'. Default: best
 *        available backend (CUDA, then Metal, then CPU).
 * @returns {ArdyMotionPipeline}
 * @throws {TypeError} when opts is not an object or checkpoint/textEncoder is
 *         missing / not a string.
 * @throws {Error} "motion.load failed: ..." when a model file can't be opened
 *         or a tensor is missing/malformed.
 */
const m = bro.motion.load({
    checkpoint:  '../brodiffusion/weights/ardy-g152',
    textEncoder: '../brolm/weights/llm2vec-llama3-8b',
});
// m.device === 'CUDA' | 'Metal' | 'CPU'  (read-only; where the weights landed)


// ── Generate ────────────────────────────────────────────────────────────────

/**
 * ArdyMotionPipeline.generate(text, opts?) → clip   (sync, blocking)
 *
 * text → pooled LLM2Vec feature → seeded per-window noise → autoregressive
 * window rollout (classifier-free-guided spaced DDIM) → FSQ detokenize →
 * unnormalize → forward kinematics → per-frame world joint positions.
 *
 * @param {string} text            - the motion prompt, e.g. "a person walks
 *        forward and waves".
 * @param {Object} [opts]
 * @param {number} [opts.frames=104] - requested clip length in 25 fps frames.
 *        Rounded UP to a whole number of 52-frame generation windows (ARDY's
 *        horizon), so clip.frames may exceed the request (e.g. 60 → 104).
 *        Clamped to >= 1.
 * @param {number} [opts.steps=10]   - DDIM denoising steps per window (the
 *        subsampled schedule length; 10 is the model's base schedule).
 * @param {number} [opts.cfg=2.5]    - classifier-free guidance weight on the
 *        text conditioning (each step runs a text and an unconditional pass).
 * @param {number} [opts.seed=0]     - seeds the per-window generation noise.
 *        Deterministic: same (text, opts) → the same clip.
 * @param {number} [opts.heading=0]  - frame-0 facing angle in radians (rotates
 *        the whole clip's initial heading in the world frame).
 * @returns {{
 *   frames: number,                 // actual frame count F (window-rounded)
 *   joints: number,                 // 34 (G1 skeleton, root/pelvis at index 0)
 *   fps: number,                    // 25
 *   positions: Float32Array,        // F*34*3 world joint positions, meters,
 *                                   // row-major [frame][joint][x,y,z]
 *   parents: Int32Array,            // 34 parent indices, -1 for the root;
 *                                   // topologically sorted (parent < child)
 *   footContacts: Float32Array,     // F*4 — L-heel, L-toe, R-heel, R-toe, 0/1
 * }}
 * @throws {TypeError} when text is missing / not a string, or the pipeline
 *         isn't loaded.
 * @throws {Error} "motion.generate failed: ..." on a pipeline error, or
 *         "motion.generate interrupted" when the engine interrupt fires
 *         mid-generate (Ctrl+C / worker terminate).
 */
const clip = m.generate('a person walks forward and waves',
                        { frames: 104, steps: 10, cfg: 2.5, seed: 0 });
console.log(`${clip.frames} frames @ ${clip.fps} fps, ${clip.joints} joints`);

// Play the clip back — index positions per frame and drive scene nodes (one
// small sphere per joint here; a real app would retarget onto a skinned mesh):
const at = (f, j) => {
    const o = (f * clip.joints + j) * 3;
    return [clip.positions[o], clip.positions[o + 1], clip.positions[o + 2]];
};
let frame = 0;
setInterval(() => {
    for (let j = 0; j < clip.joints; j++) {
        const [x, y, z] = at(frame, j);
        const n = jointNodes[j];              // scene nodes created elsewhere
        n.x = x; n.y = y; n.z = z;
    }
    frame = (frame + 1) % clip.frames;
}, 1000 / clip.fps);

// Bones follow from parents: joint j's bone spans at(f, clip.parents[j]) →
// at(f, j) for every j with parents[j] >= 0. footContacts flags which of the
// four foot points (heels/toes) are planted each frame — use it for foot-lock
// IK or footstep sounds.


// ── Worker usage (recommended) ──────────────────────────────────────────────
//
// generate() blocks the calling thread for seconds; in a windowed app, load
// and generate inside a Worker and post the typed arrays back (they are
// structured-clone / transfer friendly):
//
//   // motion_worker.js
//   const m = bro.motion.load({ checkpoint, textEncoder });
//   onmessage = (e) => {
//       const clip = m.generate(e.data.text, e.data.opts);
//       postMessage(clip, [clip.positions.buffer, clip.parents.buffer,
//                          clip.footContacts.buffer]);
//   };
//
// Feature-detect in minimal builds: bro.motion needs BRO_WITH_DIFFUSION +
// BRO_WITH_LM; compiled out, `bro.motion.available === false` and any call
// throws a clear "compiled without" error.
