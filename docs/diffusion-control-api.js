/**
 * =============================================================================
 * bro.diffusion — conditioning control, ControlNet, attention, Krea 2, VAE
 * =============================================================================
 *
 * The steering half of the brodiffusion surface. Everything in the `Pipeline`
 * block below is a method on the SAME Pipeline object documented in
 * docs/diffusion-api.js — split across two files only to keep each readable.
 *
 * Read first:
 *   - docs/diffusion-api.js — the namespace, loadModel()/createPipeline(),
 *     generate(), the step-wise prime()/stepOnce()/decode() loop, LoRA,
 *     GenerateOptions and ImageResult (referenced throughout this file).
 *   - docs/triposplat-api.js — bro.triposplat, the other namespace brodiffusion
 *     installs.
 *
 * What is in here:
 *   1. ControlNet          — structural conditioning from a control image (SD1.5).
 *   2. Control axes        — named directions in the text encoder's embedding
 *                            space, injected additively into the positive
 *                            conditioning. Dictionary axes (a .bcd1 bank) and
 *                            runtime axes you mint yourself.
 *   3. Identity anchor     — Sana's training-free reference-attention seam.
 *   4. Attention trace     — per-layer cross-attention readout and pre-softmax
 *                            logit-bias steering, through PipelineState.stepOnce().
 *   5. Krea 2 hooks        — AdaLN / gate dials, raw per-layer text taps,
 *                            image-as-prompt, priming from edited taps.
 *   6. VAE                 — the standalone KL-VAE encoder/decoder handle.
 *
 * Like the rest of the ML namespaces this is CUDA-by-default; gate real model
 * loads on `bro.gpu`. Sections 2, 4 and 5 move only small tensors across the
 * boundary (directions, attention maps, taps), so they are cheap to drive from
 * JS every step.
 *
 * @example
 *   // --- Control axes in three lines ----------------------------------------
 *   pipe.loadControlDictionary('assets/axes_turbo.bcd1');
 *   pipe.setControl({ 'color.temperature': 0.8, 'style.painterly': 1.5 });
 *   const img = pipe.generate('a harbour at dusk', { steps: 20, seed: 4 });
 *
 * @example
 *   // --- ControlNet: hold a pose, restyle everything else --------------------
 *   const i = pipe.addControlNet('/controlnets/sd-controlnet-openpose.safetensors');
 *   const img = pipe.generate('a knight in a cathedral', {
 *     steps: 30,
 *     controls: [{ imagePath: 'pose.png', scale: 1.0, startStep: 0.0, endStep: 0.8 }],
 *   });
 *   pipe.removeControlNet(i);
 */

// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * Tensor2D — the plain host-FP32 matrix shape this surface passes both ways.
 * `data` is row-major with exactly rows*cols elements; FP16/BF16 device
 * tensors are converted to FP32 on the way out and cast back on the way in.
 *
 * @typedef {Object} Tensor2D
 * @property {number} rows
 * @property {number} cols
 * @property {Float32Array} data  length rows*cols
 */

/**
 * TextConditioning — the { embeds, mask } pair Krea 2's raw-tap calls return
 * and krea2EncodeText() / krea2PrimeFromTaps() consume.
 *
 * @typedef {Object} TextConditioning
 * @property {Tensor2D} embeds  raw per-layer Qwen3-VL taps, token-major / layer-minor
 * @property {Tensor2D} mask    the matching validity mask
 */

/**
 * ControlNetInput — one entry of GenerateOptions.controls, one per registered
 * ControlNet in addControlNet() order.
 *
 * @typedef {Object} ControlNetInput
 * @property {string} imagePath   conditioning image (canny edges, a depth map, a pose
 *           skeleton, ...). Used VERBATIM — not asset-path resolved — so resolve it
 *           yourself with bro.resolvePath() if it is app-relative.
 * @property {number} [scale=1]      per-net conditioning_scale (diffusers parity);
 *           0.0 disables this net for the run, 1.0 is the HF default.
 * @property {number} [startStep=0]  schedule fraction in [0,1] at which this net starts
 *           contributing.
 * @property {number} [endStep=1]    schedule fraction at which it stops. The window is
 *           half-open on the right: a step at fraction f runs the net iff
 *           startStep <= f < endStep. Outside it the net is skipped and its residual
 *           stays zero — the usual trick is to release structure late (endStep ~0.7)
 *           so the model can finish freely.
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * The same Pipeline class as in docs/diffusion-api.js — these are its
 * steering methods. Every one of them requires a pipeline whose weights are
 * loaded (loadModel(), or createPipeline() + loadWeights()).
 */
class Pipeline {

  // ── 1. ControlNet (SD1.5 only) ─────────────────────────────────────────────

  /**
   * Register a ControlNet safetensors file. SD1.5 only (throws on the DiT
   * families). Call after loadWeights()/loadModel(); stackable — call again to
   * register more nets. The returned index is both the position into
   * GenerateOptions.controls and the key for removeControlNet().
   *
   * LCM and trace mode both work alongside ControlNets; INT8 + trace does not.
   *
   * @param {string} path   ControlNet .safetensors (asset-path resolved)
   * @param {object} [cfg]  architecture overrides for unusual checkpoints — the
   *        lllyasviel/sd-controlnet-* zoo needs none of them.
   * @param {number} [cfg.inChannels=4]          latent input channels
   * @param {number} [cfg.controlChannels=3]     control-image channels
   * @param {number} [cfg.layersPerBlock=2]      resnet layers per down block
   * @param {number} [cfg.crossAttentionDim=768] CLIP context dim
   * @param {number} [cfg.transformerNumHeads=8] transformer heads
   * @returns {number} index of the newly registered ControlNet
   */
  addControlNet(path, cfg) {}

  /**
   * Drop one registered ControlNet by index. Later indices SHIFT DOWN, so a
   * stored index is only valid until the next removal — re-read
   * config().numControlNets after removing.
   * @param {number} index
   * @returns {undefined}
   */
  removeControlNet(index) {}

  /**
   * Drop every registered ControlNet. After this, config().hasControlNet is
   * false and generate() wants an empty (or absent) opts.controls again.
   * @returns {undefined}
   */
  clearControlNets() {}

  // ── 2. conditioning-space control axes ─────────────────────────────────────
  //
  // A dictionary of named directions in the text encoder's embedding space,
  // added to every token row of the positive conditioning at prime()/generate()
  // time. The applied vector for one axis is alpha * scale * dir. Weights
  // persist across generations until changed or cleared, so a UI can hold a
  // rack of sliders and just call generate() again.

  /**
   * Load a BCD1 control dictionary (a bank of named direction axes built
   * offline), replacing the loaded axes and zeroing every weight.
   *
   * With { merge: true } the file's axes are ADDED to those already loaded
   * instead — a same-named axis is overwritten and its weight reset — so banks
   * of different provenance (a word-derived one, an SAE-discovered one) stack
   * without being concatenated offline. The dictionary's dim must match the
   * model's text encoder, and a merged file must agree with what is loaded, or
   * it throws.
   *
   * @param {string} path  .bcd1 dictionary (asset-path resolved)
   * @param {object} [opts]
   * @param {boolean} [opts.merge=false] add to the loaded axes instead of replacing them
   * @returns {undefined}
   *
   * @example
   *   pipe.loadControlDictionary('assets/axes_turbo.bcd1');
   *   pipe.loadControlDictionary('assets/axes_sae_deck.bcd1', { merge: true });
   *   pipe.setControl({ 'color.temperature': 0.8, 'sae.4571': -3 });
   */
  loadControlDictionary(path, opts) {}

  /**
   * Set one axis weight in natural alpha units, or several at once from a map.
   * Applies to every generation until changed or clearControl().
   *
   * Two forms:
   *   setControl(name, alpha)
   *   setControl({ name: alpha, ... })  — non-numeric values are skipped
   *
   * An unknown axis name throws. In the map form EVERY entry is attempted
   * first and the first failure is reported at the end, so one typo does not
   * silently skip the rest of the rack.
   *
   * @param {string|Object<string,number>} nameOrMap
   * @param {number} [alpha]  required in the (name, alpha) form
   * @returns {undefined}
   */
  setControl(nameOrMap, alpha) {}

  /**
   * Zero every axis weight. The loaded dictionary is kept — only the weights
   * are cleared, so the rack's names survive for the UI.
   * @returns {undefined}
   */
  clearControl() {}

  /**
   * @returns {string[]} names of every registered axis (dictionary + runtime),
   *          empty when nothing is loaded. The list a control panel is built from.
   */
  controlAxes() {}

  /**
   * The stored direction and baked scale of one axis — introspection for
   * explaining axes, e.g. cosine-decomposing a freshly minted search axis
   * against the dictionary's named directions. Throws on an unknown name.
   * @param {string} name
   * @returns {{dir: Float32Array, scale: number}}
   */
  controlVector(name) {}

  /**
   * Register (or replace) a RUNTIME axis from an explicit direction and set its
   * weight in one call. `dir` is taken as-is (normalize it yourself if you want
   * alpha to mean what it means for dictionary axes); the injected vector is
   * alpha * scale * dir. Runtime axes coexist with dictionary axes and show up
   * in controlAxes().
   *
   * @param {string} name        axis name (replaces an existing axis of the same name)
   * @param {Float32Array} dir   non-empty; width = the encoder's hidden dim
   * @param {number} alpha       weight, in the same units setControl() uses
   * @param {number} [scale=1]   baked scale stored with the axis
   * @returns {undefined}
   *
   * @example
   *   // Mint an axis as the diff of means of two phrase sets, in the encoder's
   *   // own space, then drive it like any dictionary axis.
   *   const mean = (phrases) => {
   *     let acc = null;
   *     for (const p of phrases) {
   *       const t = pipe.encodeConditioning(p);          // { rows, cols, data }
   *       acc = acc || new Float32Array(t.cols);
   *       for (let r = 0; r < t.rows; r++)
   *         for (let c = 0; c < t.cols; c++)
   *           acc[c] += t.data[r * t.cols + c] / (t.rows * phrases.length);
   *     }
   *     return acc;
   *   };
   *   const a = mean(['a sunlit meadow', 'a bright summer field']);
   *   const b = mean(['a stormy moor', 'a dark overcast heath']);
   *   const dir = a.map((v, i) => v - b[i]);
   *   let sq = 0; for (const v of dir) sq += v * v;
   *   const n = Math.sqrt(sq) || 1;
   *   pipe.setControlVector('weather.bright', dir.map(v => v / n), 1.5);
   */
  setControlVector(name, dir, alpha, scale) {}

  /**
   * Remove one axis by name, runtime or dictionary. No-op if unknown — so a
   * lab can drop a built search axis without reloading the bank.
   * @param {string} name
   * @returns {undefined}
   */
  removeControl(name) {}

  /**
   * Cap how hard a STACK of axes may push, in the same alpha units the weights
   * use. Each axis is added to every token row, so what the denoiser sees is
   * the SUM: ten axes at +2 push harder than one at +10, and past roughly the
   * conditioning's own token norm the injection, not the prompt, is what gets
   * rendered. Over budget, every active axis is scaled by ONE common factor, so
   * the dialled-in mix is kept and only the overdrive is shed.
   *
   * @param {number} alpha  0 (the default) leaves the stack uncapped
   * @returns {undefined}
   */
  setControlBudget(alpha) {}

  /**
   * The current stack's length in alpha units, what it may spend, and whether
   * the next generation will hold it back — the numbers a stack meter is drawn
   * from, with no render needed. `scale` is the factor every active axis will
   * be multiplied by (1.0 when in budget).
   * @returns {{norm: number, budget: number, clamped: boolean, scale: number}}
   *
   * @example
   *   const m = pipe.controlNorm();
   *   meter.style.width = (100 * Math.min(1, m.norm / (m.budget || m.norm))) + '%';
   *   meter.classList.toggle('over', m.clamped);
   */
  controlNorm() {}

  /**
   * Encode a prompt to the (L, hidden) embeddings the denoiser cross-attends
   * to — row 0 is BOS — downloaded to host FP32. This is the primitive control
   * directions are built from: everything setControlVector() takes lives in
   * this space. Requires loaded weights.
   * @param {string} prompt
   * @returns {Tensor2D}
   */
  encodeConditioning(prompt) {}

  // ── 3. identity anchor (Sana only) ─────────────────────────────────────────

  /**
   * Capture a reference identity and arm Sana's training-free
   * reference-attention seam. Runs ONE full generation of `prompt` — the
   * returned image IS the anchor, e.g. a neutral portrait — while recording the
   * DiT's per-step linear-attention summaries. Every later generate()/prime()
   * then adds those summaries back, scaled by setIdentityWeight(), so the
   * subject stays the same person while the prompt and control axes drive pose
   * and expression.
   *
   * Sana only (throws elsewhere). The summaries are token-count independent, so
   * the anchor and later runs may differ in size; matching `steps` aligns them
   * tightly (a shorter later run reuses the anchor's final step) and sharing
   * `seed` tightens identity coherence further. Honours bro.diffusion.cancel().
   *
   * @param {string} prompt  the subject to hold
   * @param {GenerateOptions} [opts]  use the same steps/seed as the later runs
   * @returns {ImageResult|Cancelled} the anchor image
   *
   * @example
   *   const neutral = pipe.setIdentityAnchor('a portrait of a woman, neutral', { steps: 8, seed: 1 });
   *   pipe.setIdentityWeight(1.0);
   *   pipe.setControl('smile', 3.0);                   // push emotion to the extreme...
   *   const held = pipe.generate('a portrait of a woman', { steps: 8, seed: 1 });  // ...face holds
   */
  setIdentityAnchor(prompt, opts) {}

  /**
   * Injection strength for the armed anchor. 0 (the default) disables it even
   * with an anchor set — a true no-op; ~1 holds identity faithfully; higher
   * over-anchors (identity locked, the edit damped). Takes effect on the next
   * generate()/prime(). Sana only.
   * @param {number} weight
   * @returns {undefined}
   */
  setIdentityWeight(weight) {}

  /** @returns {boolean} whether an anchor has been captured and armed. */
  hasIdentityAnchor() {}

  /**
   * Drop the cached anchor and zero the weight (frees the summary cache).
   * @returns {undefined}
   */
  clearIdentityAnchor() {}

  // ── 5. Krea 2 research hooks ───────────────────────────────────────────────
  //
  // Every krea2* method is Krea 2 only: the native call throws a clear error
  // for any other model class. They expose the DiT's AdaLN modulation and
  // attention gates as editable tensors, and the Qwen3-VL text tower's raw
  // per-layer taps as an editable conditioning you can prime from.
  //
  // Shapes: krea2HiddenSize() is the DiT hidden size (6144), krea2NumLayers()
  // the transformer depth (28). Block ranges are [blockLo, blockHi] indices
  // into those layers.

  /**
   * Override the AdaLN modulation for a range of transformer blocks by adding
   * `delta` to what krea2TimeMod() produced for the step.
   * @param {?Tensor2D} delta  (1, 6*krea2HiddenSize()); null/undefined clears the override
   * @param {number} blockLo   first block index (inclusive)
   * @param {number} blockHi   last block index (inclusive)
   * @returns {undefined}
   */
  krea2SetModDelta(delta, blockLo, blockHi) {}

  /**
   * The time embedding and AdaLN modulation the DiT would use at `timestep`
   * (0..1000 scale) — the tensors krea2SetModDelta() offsets. Pair it with
   * state.krea2StepTimestep() to build a delta for the step about to run.
   * @param {number} timestep
   * @returns {{temb: Tensor2D, mod: Tensor2D}}
   *
   * @example
   *   const t = st.krea2StepTimestep();
   *   const { mod } = pipe.krea2TimeMod(t);
   *   const delta = { rows: 1, cols: mod.cols, data: new Float32Array(mod.cols) };
   *   for (let i = 0; i < mod.cols; i++) delta.data[i] = 0.05 * mod.data[i];   // +5% AdaLN
   *   pipe.krea2SetModDelta(delta, 8, 19);
   *   st.stepOnce();
   *   pipe.krea2SetModDelta(null, 8, 19);                                      // release
   */
  krea2TimeMod(timestep) {}

  /**
   * Scale the text and image attention gates over a block range — the blunt
   * dial for "how much does the prompt still matter here".
   * @param {number} txtScale
   * @param {number} imgScale
   * @param {number} blockLo
   * @param {number} blockHi
   * @returns {undefined}
   */
  krea2SetGateScale(txtScale, imgScale, blockLo, blockHi) {}

  /**
   * Per-position gate mask over a block range: the fine-grained form of
   * krea2SetGateScale().
   * @param {?Tensor2D} mask  holds (text_seq + img_len) values; null/undefined clears
   * @param {number} blockLo
   * @param {number} blockHi
   * @returns {undefined}
   */
  krea2SetGateMask(mask, blockLo, blockHi) {}

  /**
   * Turn gate capture on or off. While on, every stepOnce() overwrites the
   * single gate sink krea2Gates() reads — so read it right after the step you
   * care about.
   * @param {boolean} enable
   * @returns {undefined}
   */
  krea2CaptureGates(enable) {}

  /**
   * The captured gates from the most recent step: rows = krea2NumLayers(),
   * cols = text_seq + img_len (inferred from the flat buffer length).
   * @returns {Tensor2D}
   */
  krea2Gates() {}

  /** @returns {number} the DiT hidden size (6144). */
  krea2HiddenSize() {}

  /** @returns {number} the transformer depth (28). */
  krea2NumLayers() {}

  /**
   * Raw per-layer Qwen3-VL taps for a prompt, PRE-fusion (token-major /
   * layer-minor). Edit rows and feed the result to krea2EncodeText() (to see
   * the fused conditioning) or straight to krea2PrimeFromTaps(). Requires
   * loaded weights.
   * @param {string} prompt
   * @returns {TextConditioning}
   */
  krea2EncodePromptTaps(prompt) {}

  /**
   * Fuse raw taps into the (n_valid, krea2HiddenSize()) conditioning the DiT
   * cross-attends to — the same space the control axes (setControl /
   * setControlVector) apply in.
   * @param {Tensor2D} embeds
   * @param {Tensor2D} mask
   * @returns {Tensor2D}
   */
  krea2EncodeText(embeds, mask) {}

  /**
   * Image-as-prompt: run an image through Krea 2's own Qwen3-VL vision tower
   * and get back the SAME raw-tap shape krea2EncodePromptTaps() produces for
   * text — so an image and a phrase can be blended row-wise before priming.
   * Requires loaded weights.
   *
   * @param {Float32Array} pixels  FP32 CHW in [0,1], length exactly 3*H*W
   * @param {number} H
   * @param {number} W
   * @returns {TextConditioning}
   *
   * @example
   *   const a = pipe.krea2EncodeImagePrompt(chw, 512, 512);
   *   const b = pipe.krea2EncodePromptTaps('in the style of a woodcut');
   *   const mix = { rows: a.embeds.rows, cols: a.embeds.cols,
   *                 data: a.embeds.data.map((v, i) => 0.6 * v + 0.4 * b.embeds.data[i]) };
   *   const st = pipe.krea2PrimeFromTaps(mix, a.mask, { steps: 8, guidanceScale: 1.0 });
   */
  krea2EncodeImagePrompt(pixels, H, W) {}

  /**
   * Prime a step-wise generation from caller-supplied raw taps instead of a
   * prompt string — the entry point for edited / blended / image-derived
   * conditioning. Returns an ordinary PipelineState (see docs/diffusion-api.js),
   * which retains this Pipeline.
   *
   * Omit the uncond pair to fall back to encoding opts.negativePrompt normally;
   * BOTH uncond arguments must be objects for them to be used.
   *
   * @param {Tensor2D} embeds
   * @param {Tensor2D} mask
   * @param {GenerateOptions} [opts]
   * @param {Tensor2D} [uncondEmbeds]
   * @param {Tensor2D} [uncondMask]
   * @returns {PipelineState}
   */
  krea2PrimeFromTaps(embeds, mask, opts, uncondEmbeds, uncondMask) {}
}

/**
 * Standalone KL-VAE handle: the SD/Flux-family autoencoder on its own, without
 * a diffusion pipeline around it. Useful for latent-space tools — encode an
 * image, edit the latent, decode it back — and for feeding hand-built latents
 * to decode().
 *
 * Constructible: `new bro.diffusion.VAE()` (also the global `VAE`). It builds
 * both a decoder and an encoder with default configs; loadWeights() then fills
 * them from a checkpoint.
 *
 * This class predates the asset-path resolver: its paths are used VERBATIM, so
 * pass an absolute path or one from bro.resolvePath().
 */
class VAE {

  /**
   * Load decoder weights from a safetensors checkpoint under `prefix`, then
   * TRY the matching encoder prefix — "encoder." normally, or
   * "first_stage_model.encoder." when prefix is "first_stage_model.decoder.".
   * A checkpoint with no encoder subtree still loads: the encoder simply stays
   * unusable and encode() throws.
   *
   * Throws if the file does not exist.
   *
   * @param {string} path
   * @param {string} [prefix='decoder.'] safetensors key prefix for the decoder
   * @returns {undefined}
   */
  loadWeights(path, prefix) {}

  /**
   * Decode a latent to an image at 8x the latent dims. The latent may be a
   * bare Float32Array or an object { data, width, height }; a second argument
   * { width, height } overrides the dims either way. Both dims default to 64
   * when nothing supplies them, and the buffer must hold at least
   * in_channels * height * width values.
   *
   * Unlike PipelineState.decode() this has no includeFp32 option — the result
   * is always { width, height, data }.
   *
   * @param {Float32Array|{data: Float32Array, width?: number, height?: number}} latent
   * @param {{width?: number, height?: number}} [opts]  latent dims override
   * @returns {ImageResult}
   *
   * @example
   *   const vae = new bro.diffusion.VAE();
   *   vae.loadWeights('/weights/sd15/vae/diffusion_pytorch_model.safetensors');
   *   const img = vae.decode(latentF32, { width: 64, height: 64 });   // 512x512 out
   */
  decode(latent, opts) {}

  /**
   * Encode an image to a latent at 1/8 the image dims. The image is a path
   * string (decoded by broimage) or { data, width, height } with RGBA bytes.
   * Both dimensions must be multiples of 8 or it throws.
   *
   * @param {string|{data: Uint8ClampedArray|Uint8Array, width: number, height: number}} image
   * @param {object} [opts]  accepted for symmetry; unused
   * @returns {{width: number, height: number, channels: number, data: Float32Array}}
   *          width/height are the LATENT dims (image dims / 8); channels is 4.
   */
  encode(image, opts) {}
}

// ── 4. Attention trace and steering ──────────────────────────────────────────
//
// PipelineState.stepOnce(ctrl) is the seam — its signature is documented in
// docs/diffusion-api.js. Two things it can do:
//
//   { trace: true }     capture, for every cross-attention block, the
//                       head-averaged attention map as { Lq, Lk, data }, with
//                       data a Float32Array of Lq*Lk weights. Lk is the context
//                       length (77 for CLIP); Lq is that block's spatial token
//                       count, so early and late blocks have different Lq.
//
//   { attnBias: [...] } add a per-layer (Lq, Lk) bias to the attention scores
//                       BEFORE the softmax. The array's length must equal
//                       pipe.numXAttnBlocks() exactly — a mismatch throws
//                       RangeError naming both numbers — and each entry is
//                       either null (no bias for that layer) or
//                       { data, Lq, Lk } with data.length === Lq*Lk (TypeError
//                       otherwise). Supplying attnBias forces trace mode on
//                       internally, so a biased step also costs a trace.
//
// Trace first to learn each layer's Lq, then bias with matching shapes:
//
//   const st    = pipe.prime('a red apple on a blue table', { steps: 12 });
//   const probe = st.stepOnce({ trace: true });
//   const bias  = probe.trace.map(t => ({
//     Lq: t.Lq, Lk: t.Lk, data: new Float32Array(t.Lq * t.Lk),
//   }));
//
//   // Boost token index 2 ("red") across every spatial position of block 6.
//   const b = bias[6], TOKEN = 2;
//   for (let q = 0; q < b.Lq; q++) b.data[q * b.Lk + TOKEN] += 2.0;
//   st.stepOnce({ attnBias: bias });
//
//   // Or suppress a token in one region only: index the same way, writing
//   // negative values at the q positions that fall inside the region (q is a
//   // row-major latent cell, so q = row * sqrt(Lq) + col for a square latent).
//
// Branch and score: clone before a biased step, run both, and keep the better
// one — this is why clone() is one latent copy and shares the conditioning.
//
//   const alt = st.clone();
//   st.stepOnce();                       // the plain branch
//   alt.stepOnce({ attnBias: bias });    // the steered branch
//   const keep = score(st.decode()) >= score(alt.decode()) ? st : alt;
//
// On a flow-match model, pipe.sigmas() turns a single step into a free x̂0
// preview, so a branch can be scored after ONE step instead of a full run —
// see the sigmas() entry in docs/diffusion-api.js.
//
// =============================================================================
