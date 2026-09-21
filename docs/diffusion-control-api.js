/**
 * =============================================================================
 * bro.diffusion — conditioning control, ControlNet, attention, research hooks, VAE
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
 *   7. Qwen-Image 2.1 hooks— modulation / gate dials over a 32-block stack
 *                            driven by ONE shared modulation vector, the raw
 *                            (n, 4096) Qwen3-VL conditioning, the prefix KV
 *                            cache as a steering surface, a VAE encode/decode
 *                            seam, and a releasable text encoder.
 *
 * Like the rest of the ML namespaces this is CUDA-by-default; gate real model
 * loads on `bro.gpu`. Sections 2, 4, 5 and 7 move only small tensors across the
 * boundary (directions, attention maps, taps, modulation rows), so they are
 * cheap to drive from JS every step.
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

  // ── 7. Qwen-Image 2.1 research hooks ───────────────────────────────────────
  //
  // Every qwenImage21* method is Qwen-Image 2.1 only and throws a TypeError on
  // any other model class (the krea2* block throws a plain Error instead —
  // reaching for the wrong family's hooks is a type mistake, and this block is
  // new enough to say so properly).
  //
  // Shapes: qwenImage21HiddenSize() is the DiT hidden size (4096),
  // qwenImage21NumLayers() the depth (32), qwenImage21TextHiddenDim() the
  // Qwen3-VL-8B width a control direction must have (also 4096). Block ranges
  // are HALF-OPEN [blockLo, blockHi) — unlike the krea2* block's inclusive
  // ends, which is worth re-reading before porting a script across.
  //
  // Two facts about the model shape everything here.
  //
  // (a) ONE modulation vector drives all 32 blocks. There is no per-block
  //     modulation parameter to edit, so qwenImage21SetModDelta realises a
  //     block range by building a second copy of that vector for the blocks
  //     inside it. The corollary for gate capture: with no hooks armed, every
  //     block's row is the same pair of constants. The capture reads back what
  //     a mask or a scale actually did — it does not reveal per-block
  //     structure, of which the architecture has none.
  //
  // (b) PREFIX and TARGET rows read DIFFERENT rows of that vector. Text tokens
  //     are modulated from t = 0 and the image being generated from the sampled
  //     t, which is what lets the DiT cache the text half's attention K/V once
  //     and skip it on every later step. So:
  //
  //       * a 'target' delta (the default) is free to change mid-generation;
  //       * a 'prefix' delta, a txtScale != 1, a gate mask, or edited text rows
  //         only apply on the step that EXTRACTS the cache. The binding drops
  //         the live cache for you when one is armed — and, symmetrically, when
  //         one is cleared — so the next step re-extracts. That costs one full
  //         prefill; a pure target-side dial costs nothing.
  //
  //     qwenImage21ResetCache() is the manual version, for conditioning edited
  //     outside these methods.
  //
  // A generation with no CFG (the reference default, guidanceScale 1.0) has one
  // branch; at guidanceScale > 1 both branches have their own prompt, their own
  // prefix cache, and the prefix hooks apply to both.

  /**
   * Add `delta` to the shared modulation output for blocks [blockLo, blockHi),
   * on the row named by `target`. The vector is the pre-chunk
   * [scale1, gate1, scale2, gate2] layout qwenImage21TimeMod() reads back, and
   * the delta lands BEFORE the gates' tanh — so a gate component saturates
   * rather than running away.
   *
   * @param {?Tensor2D} delta  (1, 4*qwenImage21HiddenSize()); null/undefined clears
   * @param {number} blockLo   first block index (inclusive)
   * @param {number} blockHi   one past the last block index
   * @param {('target'|'prefix'|'both')} [target='target']  which modulation row
   * @returns {undefined}
   *
   * @example
   *   // Push the second (SwiGLU) sublayer's scale in the last third of the
   *   // stack, target rows only — no re-extraction, so this is cheap to sweep.
   *   const H = pipe.qwenImage21HiddenSize();
   *   const t = st.qwenImage21StepTimestep();
   *   const { modTarget } = pipe.qwenImage21TimeMod(t);
   *   const d = { rows: 1, cols: 4 * H, data: new Float32Array(4 * H) };
   *   for (let i = 2 * H; i < 3 * H; i++) d.data[i] = 0.1 * modTarget.data[i];
   *   pipe.qwenImage21SetModDelta(d, 20, 32);
   *   st.stepOnce();
   *   pipe.qwenImage21SetModDelta(null, 0, 0);
   */
  qwenImage21SetModDelta(delta, blockLo, blockHi, target) {}

  /**
   * The time embedding and modulation rows the DiT would use at `timestep`
   * (0..1000 scale — the same value state.qwenImage21StepTimestep() returns).
   * No image forward. The mod rows are PRE-tanh: exactly the space
   * qwenImage21SetModDelta() adds into.
   *
   * `temb` is (2, hidden) with row 0 the sampled t and row 1 t = 0;
   * `modTarget` and `modPrefix` are the matching (1, 4*hidden) rows, split out
   * so nothing has to slice a flat buffer. `modPrefix` is timestep-independent
   * — the same row at every step, which is the whole reason the prefix cache
   * works.
   *
   * @param {number} timestep
   * @returns {{temb: Tensor2D, modTarget: Tensor2D, modPrefix: Tensor2D}}
   */
  qwenImage21TimeMod(timestep) {}

  /**
   * Scalar multipliers on the POST-tanh residual gates of blocks
   * [blockLo, blockHi). `attnScale` scales the attention sublayer's gate and
   * `mlpScale` the SwiGLU's; orthogonally `txtScale` scales the gate the PREFIX
   * rows see and `imgScale` the one the TARGET rows see. All four at 1 clears.
   *
   * A txtScale other than 1 is a prefix-side change and re-extracts the cache;
   * sweeping imgScale alone does not.
   *
   * @param {number} attnScale
   * @param {number} mlpScale
   * @param {number} txtScale
   * @param {number} imgScale
   * @param {number} blockLo
   * @param {number} blockHi
   * @returns {undefined}
   */
  qwenImage21SetGateScale(attnScale, mlpScale, txtScale, imgScale, blockLo, blockHi) {}

  /**
   * Per-token gate mask over blocks [blockLo, blockHi): both sublayers' gated
   * residual for row r is multiplied by mask[r], after the tanh and after any
   * qwenImage21SetGateScale. Zeroing a row removes that token's residual
   * updates entirely for the masked blocks — zeroing every row over a block
   * range is exactly "delete these blocks".
   *
   * @param {?Tensor2D} mask  holds (textRows + imgLen) values in joint forward
   *                          order; null/undefined clears
   * @param {number} blockLo
   * @param {number} blockHi
   * @returns {undefined}
   */
  qwenImage21SetGateMask(mask, blockLo, blockHi) {}

  /**
   * Add `delta` to the final adaptive scale the target rows pass through on
   * the way to proj_out — one (1, hidden) knob on output magnitude.
   * @param {?Tensor2D} delta  (1, qwenImage21HiddenSize()); null/undefined clears
   * @returns {undefined}
   */
  qwenImage21SetNormOutScaleDelta(delta) {}

  /**
   * Turn gate capture on or off. While on, every stepOnce() overwrites the
   * single sink qwenImage21Gates() reads — so read it right after the step you
   * care about. See note (a) above for what the numbers can and cannot show.
   * @param {boolean} enable
   * @returns {undefined}
   */
  qwenImage21CaptureGates(enable) {}

  /**
   * The captured gates from the most recent step: rows = qwenImage21NumLayers(),
   * cols = textRows + imgLen. Each entry is the mean EFFECTIVE attention gate
   * that multiplied that row's residual — mean_d tanh(gate1)[d] times the row's
   * scale factor times its mask entry.
   * @returns {Tensor2D}
   */
  qwenImage21Gates() {}

  /** @returns {number} the DiT hidden size (4096). */
  qwenImage21HiddenSize() {}

  /** @returns {number} the transformer depth (32). */
  qwenImage21NumLayers() {}

  /**
   * @returns {number} the Qwen3-VL-8B hidden width (4096) — the dimension a
   * control dictionary or a setControlVector() direction must have for this
   * model.
   */
  qwenImage21TextHiddenDim() {}

  /**
   * Encode a prompt into 2.1's raw text conditioning: the (n, 4096) Qwen3-VL-8B
   * hidden-state rows the DiT's txt_in consumes, an all-ones validity mask, and
   * the FULL chat-template token ids (system prefix included, so ids.length is
   * embeds.rows + dropIdx). This is the space the control axes are minted in —
   * encodeConditioning() returns the same `embeds` — and the tensor
   * qwenImage21PrimeFromText() takes back. Requires loaded weights.
   *
   * @param {string} prompt
   * @returns {{embeds: Tensor2D, mask: Tensor2D, ids: Int32Array, dropIdx: number}}
   */
  qwenImage21EncodePrompt(prompt) {}

  /**
   * Prime a step-wise generation from caller-supplied (n, 4096) rows instead of
   * a prompt string — the entry point for edited, blended or externally-sourced
   * conditioning. Returns an ordinary PipelineState (docs/diffusion-api.js),
   * which retains this Pipeline.
   *
   * `mask` may be null (every row valid). Omit `uncondEmbeds` to fall back to
   * encoding opts.negativePrompt when guidanceScale > 1.
   *
   * Note this bypasses nothing else: control axes set with setControl() /
   * setControlVector() are still applied to the rows you supply, because they
   * are applied to the positive conditioning inside prime().
   *
   * @param {Tensor2D} embeds
   * @param {?Tensor2D} mask
   * @param {GenerateOptions} [opts]
   * @param {Tensor2D} [uncondEmbeds]
   * @param {?Tensor2D} [uncondMask]
   * @returns {PipelineState}
   *
   * @example
   *   // Interpolate two prompts of equal token length.
   *   const a = pipe.qwenImage21EncodePrompt('a lighthouse at dawn');
   *   const b = pipe.qwenImage21EncodePrompt('a lighthouse at night');
   *   if (a.embeds.rows === b.embeds.rows) {
   *     const mix = { rows: a.embeds.rows, cols: a.embeds.cols,
   *                   data: a.embeds.data.map((v, i) => 0.5 * v + 0.5 * b.embeds.data[i]) };
   *     const st = pipe.qwenImage21PrimeFromText(mix, a.mask, { steps: 8 });
   *   }
   */
  qwenImage21PrimeFromText(embeds, mask, opts, uncondEmbeds, uncondMask) {}

  /**
   * The prepared (nValid, hidden) text rows of the most recent prime() — the
   * joint sequence's text half AFTER txt_in, one projection further in than
   * qwenImage21EncodePrompt()'s output.
   * @param {boolean} [uncond=false]  read the negative branch instead
   * @returns {Tensor2D}
   */
  qwenImage21TextRows(uncond) {}

  /**
   * Replace them. The prefix KV cache is reset so the change lands on the next
   * step.
   * @param {Tensor2D} rows  (n, qwenImage21HiddenSize())
   * @param {boolean} [uncond=false]
   * @returns {undefined}
   */
  qwenImage21SetTextRows(rows, uncond) {}

  /**
   * Attenuate the LIVE prefix KV cache for layers [layerLo, layerHi): the
   * cached text keys are multiplied by `kScale` and the values by `vScale`.
   * This is the one prefix-side hook that needs NO re-extraction — after the
   * extract step the cache is what the image attends to, so scaling it steers
   * generation at zero cost, mid-denoise.
   *
   * Attenuating V alone fades the text's contribution while leaving the
   * attention pattern it induces intact; K alone flattens that pattern instead.
   * Requires at least one step to have run (there is nothing to scale before
   * the extract).
   *
   * @param {number} layerLo
   * @param {number} layerHi
   * @param {number} kScale
   * @param {number} vScale
   * @returns {undefined}
   *
   * @example
   *   // Let the prompt set the composition, then fade it out for the detail
   *   // steps — deep layers only.
   *   for (let i = 0; i < steps; i++) {
   *     if (i === Math.floor(steps / 2)) pipe.qwenImage21ScalePrefixKv(16, 32, 1.0, 0.5);
   *     st.stepOnce();
   *   }
   */
  qwenImage21ScalePrefixKv(layerLo, layerHi, kScale, vScale) {}

  /**
   * Drop the live prefix KV cache so the next step re-extracts. The hooks above
   * do this themselves; call it after editing conditioning out of band.
   * A no-op when nothing has been primed.
   * @returns {undefined}
   */
  qwenImage21ResetCache() {}

  /**
   * Encode RGB pixels into a pipeline-scale latent through the resident 16x
   * RGBA autoencoder. An opaque alpha plane is appended internally. Both
   * dimensions must be multiples of 16.
   *
   * @param {Float32Array} pixels  FP32 CHW in [0,1], length exactly 3*H*W
   * @param {number} H
   * @param {number} W
   * @returns {Tensor2D & {hLat: number, wLat: number}}  (1, 64*hLat*wLat), ready
   *          for PipelineState.setLatent() or qwenImage21Decode()
   */
  qwenImage21EncodeImage(pixels, H, W) {}

  /**
   * Decode a pipeline-scale latent, alpha dropped — the same canvas-ready shape
   * PipelineState.decode() returns.
   * @param {Tensor2D} latent  holds 64*hLat*wLat floats
   * @param {number} hLat
   * @param {number} wLat
   * @param {{fp32?: boolean}} [opts]  also return the raw NCHW floats
   * @returns {ImageResult}
   */
  qwenImage21Decode(latent, hLat, wLat, opts) {}

  /**
   * Free the Qwen3-VL-8B text backbone. It is ~8.5 GiB and completely idle from
   * prime() onwards, so releasing it is what buys a BF16 DiT or a bigger canvas
   * on a 24 GB card. An already-primed PipelineState keeps stepping — its
   * conditioning is encoded and the prefix cache is downstream of it — but
   * qwenImage21EncodePrompt(), encodeConditioning() and prime() throw until the
   * encoder is reloaded. Safe to call twice.
   * @returns {undefined}
   */
  qwenImage21ReleaseTextEncoder() {}

  /** @returns {boolean} whether the text backbone is currently loaded. */
  qwenImage21TextEncoderResident() {}

  /**
   * Reload the backbone into a pipeline it was released from, or swap in a
   * different one.
   * @param {string} modelDir           the model directory
   * @param {string} [textEncoderPath]  a .gguf or safetensors file/dir; empty
   *                                    means `<modelDir>/text_encoder`
   * @param {{quantizeWeights?: boolean}} [opts]  defaults to true (INT8)
   * @returns {undefined}
   */
  qwenImage21ReloadTextEncoder(modelDir, textEncoderPath, opts) {}
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
