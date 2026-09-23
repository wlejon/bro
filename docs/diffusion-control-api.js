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
 *                            cache as a steering surface, a conditioning axis
 *                            re-aimed per denoise step, a VAE encode/decode
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
  // Four facts about the model shape everything here.
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
  //       * a 'target' delta (the default), and the attnImg / mlpImg gate
  //         multipliers, are free to change mid-generation;
  //       * a 'prefix'/'both' delta, the attnTxt / mlpTxt multipliers, ANY gate
  //         mask, and edited text rows only apply on the step that EXTRACTS the
  //         cache. The binding drops the live cache for you when one is armed —
  //         and, symmetrically, when one is cleared — so the next step
  //         re-extracts. That costs one full prefill, about +13% on the step;
  //         a pure image-side dial costs nothing.
  //
  //     qwenImage21ScalePrefixKv() is the exception in both directions: it is
  //     free either way and it SURVIVES a re-extract, because it is a dial
  //     applied where the cache is read rather than a multiply into it. A
  //     prefix-affecting change re-extracts and then re-applies it.
  //
  //     qwenImage21ResetCache() is the manual version, for conditioning edited
  //     outside these methods.
  //
  // (c) A gate mask (and a prefix-KV rowMask) addresses the JOINT SEQUENCE, and
  //     the joint sequence is text rows first, then the image tokens row-major.
  //     One image token per 16x16 px, so at 512x512 the grid is 32x32 and the
  //     token covering pixel (px, py) is
  //
  //         textRows + Math.floor(py / 16) * 32 + Math.floor(px / 16)
  //
  //     textRows is typically 17-20 for a short prompt — read it from
  //     qwenImage21TextRows().rows rather than assuming it. Getting this wrong
  //     used to be silent: a mask written at imgLen instead of
  //     textRows + imgLen was skipped, and an all-zero one rendered the
  //     baseline to the pixel, which is how a study concluded the mask surface
  //     did nothing. A wrong-length mask now throws, naming the joint length
  //     and its split.
  //
  //     A rowMask is the prefix-only analogue: one weight per PREFIX row, so
  //     its length is the cached prefix length, not the joint length.
  //
  // (d) Anything that edits the text ROWS themselves — qwenImage21SetTextRows,
  //     a setControl() / setControlVector() axis — has to land BEFORE prime().
  //     After that the conditioning has been prepared and the prefix is what
  //     the cache holds; the hooks that still bite are the ones on this page.
  //
  //     ...with ONE exception, and it is the important one. The conditioning
  //     axes carry an order of magnitude more steering authority than anything
  //     in-network (the best minted text axis moves its readout 25σ; the best
  //     in-network dial manages 4.4σ), and every modulation-side dial has
  //     spent 97% of its authority by step 2. qwenImage21SetControlSchedule()
  //     re-applies an axis with a PER-STEP alpha during the denoise, by
  //     rebuilding the text rows from the primed conditioning each time the
  //     alpha moves. It is the one way to aim the strong surface more than
  //     once, and it costs one re-extract per change of alpha — see the
  //     schedule block below.
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
   * This spelling is RANK-1 SUGAR over the four independent multipliers
   * qwenImage21SetGateScaleRows() sets: it stores attn*txt, attn*img, mlp*txt,
   * mlp*img, and the two calls are bit-identical. Reach for the rows form
   * whenever only one side should move — see the note below it.
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
   * The four post-tanh gate multipliers of blocks [blockLo, blockHi) set
   * INDEPENDENTLY, one per (sublayer, row set) pair:
   *
   *     attnTxt   the attention gate on the t = 0 rows (text / condition images)
   *     attnImg   the attention gate on the sampled-t rows (the image)
   *     mlpTxt    the SwiGLU gate on the t = 0 rows
   *     mlpImg    the SwiGLU gate on the sampled-t rows
   *
   * All four at 1, or an empty range, clears.
   *
   * A product of an (attn, mlp) pair with a (txt, img) pair cannot say "the
   * attention gate, on the image rows only" — the most useful dial on the
   * model. Under qwenImage21SetGateScale() raising attnScale raises the
   * prefix's factor too, which invalidates the prefix KV cache (+13%/step) and
   * used to silently discard a live qwenImage21ScalePrefixKv() edit. Only
   * attnTxt and mlpTxt are prefix-side here, so an attnImg sweep never
   * re-extracts and cannot disturb the prefix at all.
   *
   * @param {number} attnTxt
   * @param {number} attnImg
   * @param {number} mlpTxt
   * @param {number} mlpImg
   * @param {number} blockLo
   * @param {number} blockHi
   * @returns {undefined}
   *
   * @example
   *   // Sweep the attention gate on the image rows across the deep blocks,
   *   // with the prefix cache untouched — so every value costs one step.
   *   for (const s of [0.8, 0.9, 1.0, 1.1, 1.2]) {
   *     pipe.qwenImage21SetGateScaleRows(1.0, s, 1.0, 1.0, 16, 32);
   *     score(pipe.generate(prompt, { steps: 8, seed: 7n }));
   *   }
   */
  qwenImage21SetGateScaleRows(attnTxt, attnImg, mlpTxt, mlpImg, blockLo, blockHi) {}

  // The gate delta, and why it is not the same knob as qwenImage21SetModDelta.
  //
  // A mod delta's gate chunks land BEFORE the tanh, so its authority over a
  // gate channel is tanh'(g) — and 74% of the `gate2` chunk's channels sit
  // where tanh'(g) < 0.05. The model's own largest modulation direction is
  // therefore exactly the one a pre-tanh dial cannot move: pushing on it
  // harder just saturates it further. A post-tanh delta has unit authority
  // over every channel instead, at the cost of leaving the (-1, 1) range the
  // tanh guarantees — which is the point, since a gate above 1 is not
  // otherwise reachable at all.
  //
  // It costs nothing per token. The delta is folded into the shared
  // (1, hidden) gate row once per forward, and the fused gated-residual
  // kernel consumes the combined row exactly as it consumes the plain one.

  /**
   * Add `delta` to the EFFECTIVE gate of blocks [blockLo, blockHi) — the value
   * that multiplies the residual, AFTER the tanh and after any
   * qwenImage21SetGateScale factor:
   *
   *     g_eff = scale * tanh(gate) + delta
   *
   * `delta` is (1, 2*qwenImage21HiddenSize()) laid out [attn, mlp]: the first
   * hidden values steer the attention sublayer's gate, the second the SwiGLU's.
   * `target` picks the row class exactly as qwenImage21SetModDelta's does; a
   * 'prefix' or 'both' delta is a prefix-side change and resets the prefix
   * cache so the next step re-extracts.
   *
   * @param {?Tensor2D} delta  (1, 2*qwenImage21HiddenSize()); null/undefined clears
   * @param {number} blockLo
   * @param {number} blockHi
   * @param {('target'|'prefix'|'both')} [target='target']
   * @returns {undefined}
   *
   * @example
   *   // Open the SwiGLU gate past what tanh can reach, deep blocks only.
   *   const H = pipe.qwenImage21HiddenSize();
   *   const d = { rows: 1, cols: 2 * H, data: new Float32Array(2 * H) };
   *   d.data.fill(0.25, H, 2 * H);          // [attn | mlp] — mlp half only
   *   pipe.qwenImage21SetGateDelta(d, 24, 32);
   *   const img = st.decode();
   *   pipe.qwenImage21ClearGateDelta();
   */
  qwenImage21SetGateDelta(delta, blockLo, blockHi, target) {}

  /**
   * Clear the gate delta — sugar for passing null over an empty range.
   * @returns {undefined}
   */
  qwenImage21ClearGateDelta() {}

  /**
   * Per-token gate mask over blocks [blockLo, blockHi): the gated residual of
   * row r in the sublayer(s) `which` names is multiplied by mask[r], after the
   * tanh and after every gate scale. Zeroing a row removes that token's
   * residual updates for the masked blocks — zeroing every row over a block
   * range is exactly "delete these blocks".
   *
   * `which` selects the sublayer: 'both' (the default), 'attn' (gate1, the
   * attention residual) or 'mlp' (gate2, the SwiGLU residual); the numbers
   * 0 / 1 / 2 are accepted for the same three. 'both' is bit-identical to
   * arming 'attn' and 'mlp' with the same vector.
   *
   * 'both' is the blunt form. The MLP half is what drags a late-step edit's
   * retention from over 100% of its step-0 effect down to about 30%, so
   * "restyle this region at step 6" wants 'attn'.
   *
   * The mask addresses the WHOLE joint sequence — text rows first, then image
   * tokens row-major; see note (c) at the top of this section for the
   * pixel-to-row arithmetic. A mask whose length is not textRows + imgLen
   * THROWS on the next forward, naming the joint length and its split. It used
   * to be skipped silently, which is a far worse failure: a mask written at
   * imgLen just rendered the baseline.
   *
   * Every gate mask is prefix-side, so arming or clearing one re-extracts.
   *
   * @param {?Tensor2D} mask  holds (textRows + imgLen) values in joint forward
   *                          order; null/undefined clears
   * @param {number} blockLo
   * @param {number} blockHi
   * @param {('both'|'attn'|'mlp')} [which='both']  which sublayer's gate
   * @returns {undefined}
   *
   * @example
   *   // Free one 128x128 region of a 512x512 canvas to restyle late, without
   *   // letting the MLP half claw the edit back.
   *   const textRows = pipe.qwenImage21TextRows().rows;
   *   const n = textRows + 32 * 32;
   *   const m = { rows: 1, cols: n, data: new Float32Array(n).fill(1) };
   *   for (let py = 128; py < 256; py += 16)
   *     for (let px = 128; px < 256; px += 16)
   *       m.data[textRows + (py / 16) * 32 + (px / 16)] = 0;
   *   pipe.qwenImage21SetGateMask(m, 0, 32, 'attn');
   */
  qwenImage21SetGateMask(mask, blockLo, blockHi, which) {}

  /**
   * Add `delta` to the final adaptive scale the target rows pass through on
   * the way to proj_out — one (1, hidden) knob on output magnitude.
   * @param {?Tensor2D} delta  (1, qwenImage21HiddenSize()); null/undefined clears
   * @returns {undefined}
   */
  qwenImage21SetNormOutScaleDelta(delta) {}

  // ── multi-slot bindings ────────────────────────────────────────────────────
  //
  // Every in-network hook above holds an ORDERED LIST of bindings, not one.
  // All of the bindings in a list apply in the same generation, so a block
  // covered by two of them gets both.
  //
  // For a given block the covering bindings compose the way their semantics
  // imply: mod deltas and gate deltas ADD, gate scales and gate masks
  // MULTIPLY (masks per sublayer, so an 'attn' mask and an 'mlp' mask over the
  // same blocks never meet), prefix-KV scales multiply per layer. Order within
  // the list therefore does not change the result.
  //
  // Composition happens at BIND time, not per token: each distinct coverage
  // pattern over the 32 blocks is reduced to one finished (1, hidden)
  // modulation / gate row, so the fused residual kernel still takes exactly
  // one pass over the activations no matter how many bindings are armed. Ten
  // overlapping bindings cost what one costs.
  //
  // The qwenImage21Set* calls are unchanged in meaning: each is now sugar for
  // "replace the whole list with this one entry" — or, with a null tensor or
  // an empty range (or all-1 scales), "clear the list". A script written
  // against the single-slot API keeps doing exactly what it did.
  //
  // What the list buys is the thing the single-slot API could not express at
  // all: two different edits, over two different block ranges, live at the
  // same time. The second qwenImage21SetGateScale() used to erase the first.
  //
  // Each qwenImage21Add* returns the new binding's index in its list, each
  // qwenImage21Clear* empties the list, and each qwenImage21*Count() reports
  // its length — enough for a UI to keep its own rack in sync.
  //
  // The prefix/target rule is unchanged and is per binding: adding or
  // clearing a prefix-side binding (a 'prefix'/'both' delta, a txtScale != 1,
  // a gate mask) resets the prefix cache; a pure target-side binding does not.

  /**
   * Append a modulation-delta binding over blocks [blockLo, blockHi).
   * Same delta layout and same `target` semantics as
   * qwenImage21SetModDelta(); this one ADDS to the list instead of replacing
   * it, and deltas covering the same block sum.
   *
   * @param {Tensor2D} delta  (1, 4*qwenImage21HiddenSize())
   * @param {number} blockLo
   * @param {number} blockHi
   * @param {('target'|'prefix'|'both')} [target='target']
   * @returns {number} the new binding's index in the list
   *
   * @example
   *   // Soften the shallow blocks' attention while pushing the deep blocks'
   *   // SwiGLU scale — two edits, two ranges, ONE generation. The single-slot
   *   // API could not say this: the second Set* call erased the first.
   *   const H = pipe.qwenImage21HiddenSize();
   *   const d = { rows: 1, cols: 4 * H, data: new Float32Array(4 * H) };
   *   d.data.fill(0.08, 2 * H, 3 * H);               // the scale2 chunk
   *
   *   pipe.qwenImage21ClearGateScales();
   *   pipe.qwenImage21ClearModDeltas();
   *   pipe.qwenImage21AddGateScale(0.7, 1.0, 1.0, 1.0, 0, 8);   // shallow
   *   pipe.qwenImage21AddModDelta(d, 24, 32);                   // deep
   *   const img = pipe.generate('a lighthouse at dawn', { steps: 8 });
   */
  qwenImage21AddModDelta(delta, blockLo, blockHi, target) {}

  /**
   * Drop every modulation-delta binding.
   * @returns {undefined}
   */
  qwenImage21ClearModDeltas() {}

  /** @returns {number} how many modulation-delta bindings are armed. */
  qwenImage21ModDeltaCount() {}

  /**
   * Append a gate-scale binding over blocks [blockLo, blockHi). Same four
   * factors as qwenImage21SetGateScale(); scales covering the same block
   * multiply, so two bindings of 0.5 give 0.25.
   *
   * @param {number} attnScale
   * @param {number} mlpScale
   * @param {number} txtScale
   * @param {number} imgScale
   * @param {number} blockLo
   * @param {number} blockHi
   * @returns {number} the new binding's index in the list
   */
  qwenImage21AddGateScale(attnScale, mlpScale, txtScale, imgScale, blockLo, blockHi) {}

  /**
   * Append a gate-scale binding with the four multipliers set independently —
   * the list form of qwenImage21SetGateScaleRows(), and the spelling to use
   * when only the image side should move. Multipliers covering the same block
   * multiply, per (sublayer, row set) pair.
   *
   * @param {number} attnTxt
   * @param {number} attnImg
   * @param {number} mlpTxt
   * @param {number} mlpImg
   * @param {number} blockLo
   * @param {number} blockHi
   * @returns {number} the new binding's index in the list
   */
  qwenImage21AddGateScaleRows(attnTxt, attnImg, mlpTxt, mlpImg, blockLo, blockHi) {}

  /**
   * Drop every gate-scale binding — both spellings share one list.
   * @returns {undefined}
   */
  qwenImage21ClearGateScales() {}

  /** @returns {number} how many gate-scale bindings are armed. */
  qwenImage21GateScaleCount() {}

  /**
   * Append a gate-delta binding over blocks [blockLo, blockHi). Same
   * (1, 2*hidden) [attn, mlp] layout and same `target` semantics as
   * qwenImage21SetGateDelta(); deltas covering the same block sum, and the
   * sum lands after the tanh and after the composed gate scale.
   *
   * @param {Tensor2D} delta  (1, 2*qwenImage21HiddenSize())
   * @param {number} blockLo
   * @param {number} blockHi
   * @param {('target'|'prefix'|'both')} [target='target']
   * @returns {number} the new binding's index in the list
   */
  qwenImage21AddGateDelta(delta, blockLo, blockHi, target) {}

  /**
   * Drop every gate-delta binding.
   * @returns {undefined}
   */
  qwenImage21ClearGateDeltas() {}

  /** @returns {number} how many gate-delta bindings are armed. */
  qwenImage21GateDeltaCount() {}

  /**
   * Append a per-token gate-mask binding over blocks [blockLo, blockHi). Same
   * row order, same `which` selector and same wrong-length throw as
   * qwenImage21SetGateMask().
   *
   * Masks over the same blocks AND the same sublayer multiply element-wise —
   * an intersection of what they keep, so a token zeroed by any one of them is
   * zeroed. An 'attn' mask and an 'mlp' mask over the same blocks are
   * independent of each other.
   *
   * Prefix-side, so arming or clearing one re-extracts the cache.
   *
   * @param {Tensor2D} mask  (textRows + imgLen) values in joint forward order
   * @param {number} blockLo
   * @param {number} blockHi
   * @param {('both'|'attn'|'mlp')} [which='both']
   * @returns {number} the new binding's index in the list
   */
  qwenImage21AddGateMask(mask, blockLo, blockHi, which) {}

  /**
   * Drop every gate-mask binding.
   * @returns {undefined}
   */
  qwenImage21ClearGateMasks() {}

  /** @returns {number} how many gate-mask bindings are armed. */
  qwenImage21GateMaskCount() {}

  /**
   * Append a prefix-KV scale binding over layers [layerLo, layerHi). Same
   * dial semantics and same optional per-row `rowMask` as
   * qwenImage21ScalePrefixKv() below — idempotent, applied where the cached
   * K/V are read — and scales covering the same layer multiply per layer.
   *
   * @param {number} layerLo
   * @param {number} layerHi
   * @param {number} kScale
   * @param {number} vScale
   * @param {?Tensor2D} [rowMask]  one weight per PREFIX row; null/omitted = all
   * @returns {number} the new binding's index in the list
   */
  qwenImage21AddPrefixKvScale(layerLo, layerHi, kScale, vScale, rowMask) {}

  /**
   * Drop every prefix-KV scale binding (i.e. back to 1/1 everywhere).
   * @returns {undefined}
   */
  qwenImage21ClearPrefixKvScales() {}

  /** @returns {number} how many prefix-KV scale bindings are armed. */
  qwenImage21PrefixKvScaleCount() {}

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

  /**
   * The SwiGLU half of the same capture, in the same layout: rows =
   * qwenImage21NumLayers(), cols = textRows + imgLen, each entry the mean
   * EFFECTIVE mlp gate that multiplied that row's SwiGLU residual — the
   * gate2 chunk folded through every covering scale and post-tanh delta,
   * times the MLP composition of the armed masks.
   *
   * It is a second reading and not a copy of qwenImage21Gates(): the two
   * sublayers carry independent gates, independent multipliers
   * (qwenImage21SetGateScaleRows' mlpTxt / mlpImg against its attnTxt /
   * attnImg) and independent mask compositions, so an mlp-only mask moves
   * this one and leaves the attention strip flat.
   * @returns {Tensor2D}
   */
  qwenImage21GatesMlp() {}

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

  // ── image-conditioned ("edit") generation ──────────────────────────────────
  //
  // 2.1 generates from condition images as well as from a prompt, and the
  // condition images enter the model TWICE, through two different doors.
  //
  //   1. Through the Qwen3-VL vision tower. The chat template reserves a run
  //      of `<|image_pad|>` rows per image and the vision tower fills them
  //      (with DeepStack features spliced into the first three decoder
  //      layers). Those rows are what make the prompt *about* the picture:
  //      "make the sky red" resolves against what the encoder saw.
  //   2. Through the 16x RGBA autoencoder. The same resized image is encoded
  //      to a 64-channel latent grid and joins the DiT's joint sequence as its
  //      own block-causal segment. Those tokens carry the pixels.
  //
  // Door 1's rows are DROPPED before txt_in; the DiT fills those positions
  // from door 2's latents through a different projection. That is exact
  // because txt_in (zero-centre RMSNorm + linear) is row-independent.
  //
  // The geometry rule is what makes the two doors line up. Each image is
  // resized once to sides that are multiples of 32; the vision tower emits one
  // token per 32 px and the autoencoder one latent per 16 px, so
  //
  //     hLat * wLat  ===  4 * slots
  //
  // and each `<|image_pad|>` slot stands for exactly four CONSECUTIVE latent
  // tokens in the row-major flatten — four consecutive, not a 2x2 spatial
  // group. The library asserts the identity, so a mismatched resize fails
  // loudly instead of silently mis-aligning.
  //
  // The joint sequence is [text | image0 | image1 | ... | target]. Text and
  // condition-image rows both modulate from t = 0, so the WHOLE prefix is
  // timestep-independent and caches exactly as the text-only prefix does.

  /**
   * The image-conditioned counterpart of qwenImage21EncodePrompt(): encode a
   * prompt together with its condition images, running the vision tower.
   * `images` takes the same entries as GenerateOptions.conditionImages — a
   * path string, {path}, or {pixels, width, height, channels} with planar CHW
   * floats in [0,1] and channels 3 or 4. At least one entry is required.
   * Requires loaded weights.
   *
   * `embeds` is (nValid, 4096) with the condition-image rows INCLUDED — this
   * is the pre-drop sequence, so it is the place to edit what the model saw.
   * `imagePadMask` is an Int32Array of length nValid holding 1 at a
   * condition-image row and 0 at a text row. `imageRuns` gives, per image in
   * template order, its first pad row, its slot count and the latent grid to
   * encode that image at, with `slots * 4 === hLat * wLat`.
   *
   * @param {string} prompt
   * @param {Array<string|{path?: string, pixels?: Float32Array, width?: number, height?: number, channels?: number}>} images
   * @param {number} [outputResolution=1024]
   * @returns {{embeds: Tensor2D, mask: Tensor2D, ids: Int32Array, dropIdx: number,
   *            imagePadMask: Int32Array,
   *            imageRuns: Array<{row: number, slots: number, hLat: number, wLat: number}>}}
   *
   * @example
   *   // Damp what the model SAW without touching what it was told.
   *   const e = pipe.qwenImage21EncodePromptImages('make the sky red', ['cat.png']);
   *   const run = e.imageRuns[0];
   *   for (let r = run.row; r < run.row + run.slots; r++)
   *     for (let c = 0; c < e.embeds.cols; c++)
   *       e.embeds.data[r * e.embeds.cols + c] *= 0.5;
   *   const st = pipe.qwenImage21PrimeFromText(e.embeds, e.mask, { steps: 8 });
   */
  qwenImage21EncodePromptImages(prompt, images, outputResolution) {}

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
   * prime() with condition images: returns an ordinary PipelineState the
   * caller steps itself (stepOnce, the x̂0 preview, every hook in this block —
   * all unchanged). `images` takes the same entries as
   * GenerateOptions.conditionImages and OVERRIDES any conditionImages inside
   * `opts`; at least one entry is required. With no explicit width/height in
   * `opts` the canvas is derived from the LAST image's aspect at
   * opts.outputResolution (default 1024).
   *
   * Everything in this research block keeps working across an edit prefix:
   * qwenImage21ScalePrefixKv() addresses the interleaved text+image prefix,
   * and qwenImage21ResetCache() re-extracts it. The prefix cache is keyed on
   * the total prefix length together with the target grid, so changing the
   * image count, the images or the canvas invalidates it for you.
   *
   * `pipe.generate(prompt, { conditionImages, outputResolution })` is the
   * one-call form of the same thing, for when no per-step control is wanted.
   *
   * @param {string} prompt
   * @param {Array<string|{path?: string, pixels?: Float32Array, width?: number, height?: number, channels?: number}>} images
   * @param {GenerateOptions} [opts]
   * @returns {PipelineState}
   *
   * @example
   *   const st = pipe.qwenImage21PrimeEdit('make the sky red', ['cat.png'],
   *                                        { steps: 8, guidanceScale: 4.0 });
   *   for (let i = 0; i < 8; i++) {
   *     if (i === 4) pipe.qwenImage21ScalePrefixKv(16, 32, 1.0, 0.6);
   *     st.stepOnce();
   *   }
   *   const img = st.decode();     // canvas derived from cat.png's aspect
   */
  qwenImage21PrimeEdit(prompt, images, opts) {}

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
   * Re-apply a conditioning control axis with a PER-STEP alpha during the
   * denoise. Replaces every armed schedule with this one; returns 0.
   *
   * At each step `s` the positive text rows fed to the DiT become
   *
   *     txt_in( primedEmbeds + Σ_k alpha_k[s] * scale_k * dir_k )
   *
   * where `primedEmbeds` is the conditioning prime() built — which already
   * carries whatever setControl() asked for. So a schedule COMPOSES with the
   * static desk, and its own contribution does not depend on the desk's
   * value: a flat schedule at alpha A, with the axis at weight 0 at prime
   * time, renders the setControl(axis, A) image to the pixel.
   *
   * Why this and not one of the in-network hooks: the conditioning axes are
   * the only surface on 2.1 with real authority (25σ against 4.4σ for the
   * best dial), and they were the only one that could not be scheduled.
   * Round 3 of qwen-image-research measured a desk fitted at 8 steps losing
   * 30-37% of its travel at 40 with nothing in-network able to recover it.
   *
   * `nameOrDir` is either an axis name from the loaded control dictionary (or
   * a runtime axis registered with setControlVector) or a Float32Array
   * direction of qwenImage21TextHiddenDim() floats — the diff-of-means axes a
   * lab mints are in no bank, and the trailing `scale` carries their natural
   * unit. `alphaPerStep` is indexed by the ABSOLUTE step index, not by the
   * offset from `loStep`, so the curve and the window can be edited
   * independently; a step past the end of the array contributes nothing.
   * `[loStep, hiStep)` is half-open like every other range here, and an
   * omitted `hiStep` runs to the end.
   *
   * COST. Rebuilding the rows re-runs txt_in and drops the prefix KV cache,
   * so a step whose alpha MOVED pays one extra prefill; a step whose alpha sat
   * still pays nothing, and neither does an unscheduled generation. A flat
   * schedule therefore costs one re-extract for the whole run and a window
   * costs two (entering and leaving). Armed prefix edits — gate scales, the
   * prefix-KV dial — survive the re-extract exactly as they survive any
   * other, because they are applied where the cache is read.
   *
   * Only the POSITIVE branch is steered, matching setControl().
   *
   * @param {string|Float32Array} nameOrDir
   * @param {Float32Array|number[]} alphaPerStep  one coefficient per step
   * @param {number} [loStep=0]
   * @param {number} [hiStep=-1]   exclusive; < 0 = to the end
   * @param {number} [scale=1]     only with a Float32Array direction
   * @returns {number} the slot index (0)
   *
   * @example
   *   // Let the prompt set the composition, then warm it up over the back
   *   // half — the axis re-aimed, not a weaker copy of the step-0 one.
   *   const a = new Float32Array(steps);
   *   for (let i = steps >> 1; i < steps; i++) a[i] = 3.0;
   *   pipe.qwenImage21SetControlSchedule('color.warm', a);
   *   const img = pipe.generate(prompt, { steps });
   */
  qwenImage21SetControlSchedule(nameOrDir, alphaPerStep, loStep, hiStep, scale) {}

  /**
   * The same, appended instead of replacing: every armed slot contributes its
   * own alpha_k[s] * scale_k * dir_k to the same step, and the sum is held to
   * setControlBudget() exactly as a prime-time stack is.
   * @param {string|Float32Array} nameOrDir
   * @param {Float32Array|number[]} alphaPerStep
   * @param {number} [loStep=0]
   * @param {number} [hiStep=-1]
   * @param {number} [scale=1]
   * @returns {number} the new slot's index
   */
  qwenImage21AddControlSchedule(nameOrDir, alphaPerStep, loStep, hiStep, scale) {}

  /**
   * Drop every schedule. When one had already moved a live generation's rows,
   * this puts the PRIMED rows back (and re-extracts), so clearing mid-denoise
   * means the schedule stops rather than sticking at its last alpha.
   * @returns {undefined}
   */
  qwenImage21ClearControlSchedules() {}

  /**
   * How many schedules are armed.
   * @returns {number}
   */
  qwenImage21ControlScheduleCount() {}

  /**
   * Apply the armed schedules for `step` to `state` by hand. stepOnce() does
   * this itself, so this is only for a caller driving the denoiser out of band
   * — or for pricing a re-extract, which is what its return value is for.
   * @param {PipelineState} state
   * @param {number} [step=state.stepIndex]
   * @returns {boolean} true when the rows were rebuilt and the cache dropped
   */
  qwenImage21ApplyControlStep(state, step) {}

  /**
   * Attenuate the prefix KV for layers [layerLo, layerHi): the cached prefix
   * keys are multiplied by `kScale` and the values by `vScale`. This is the
   * one prefix-side hook that needs NO re-extraction — after the extract step
   * the cache is what the image attends to, so scaling it steers generation at
   * zero cost, mid-denoise. Attenuating V alone fades the prefix's
   * contribution while leaving the attention pattern it induces intact; K
   * alone flattens that pattern instead. With an edit prefix it addresses the
   * interleaved text+image rows together.
   *
   * This is a DIAL, not a cumulative in-place multiply. It records a per-layer
   * scale that is applied to the cached K/V where they are READ, on every
   * forward, so:
   *
   *   * it is idempotent — calling it twice with 0.5 is the same as calling it
   *     once, not 0.25 (compose ranges with qwenImage21AddPrefixKvScale());
   *   * generate() with a scale armed behaves like every other dial, which the
   *     old in-place version could not do at all;
   *   * the scale SURVIVES a cache reset / re-extract, because it is a dial
   *     and not cache content;
   *   * it no longer needs a step to have run first — arm it before step 0.
   *
   * 1/1 over the full layer range clears it.
   *
   * `rowMask`, when given, is a WEIGHT per PREFIX row — not a second
   * multiplier — saying how much of kScale/vScale that row gets:
   *
   *     k_row[r] = 1 + rowMask[r] * (kScale - 1)        (and likewise v)
   *
   * so all ones is exactly the broadcast (which is what "omitted = every row"
   * has to mean), all zeros is the identity, and anything between fades the
   * scale in. That is per-token prompt weighting — the "(word:1.3)" every
   * image UI ships — applied to the cached K/V the whole generation actually
   * attends to, with no re-encode and no re-extract. Its length must be the
   * CACHED PREFIX length (not the joint length); a mismatch throws on the next
   * step.
   *
   * The gate mask's text half is not a substitute: scaling a text token's
   * residual updates is not scaling its contribution, because its K/V is
   * dominated by what txt_in and the early blocks wrote before any gate
   * applied.
   *
   * @param {number} layerLo
   * @param {number} layerHi
   * @param {number} kScale
   * @param {number} vScale
   * @param {?Tensor2D} [rowMask]  one weight per PREFIX row; null/omitted = all
   * @returns {undefined}
   *
   * @example
   *   // Let the prompt set the composition, then fade it out for the detail
   *   // steps — deep layers only.
   *   for (let i = 0; i < steps; i++) {
   *     if (i === Math.floor(steps / 2)) pipe.qwenImage21ScalePrefixKv(16, 32, 1.0, 0.5);
   *     st.stepOnce();
   *   }
   *
   * @example
   *   // "(volcano:1.4)" — weight two prompt tokens up, every layer, no
   *   // re-encode. Rows 0..dropIdx are the template's system prefix.
   *   const n = pipe.qwenImage21TextRows().rows;
   *   const w = { rows: 1, cols: n, data: new Float32Array(n) };
   *   w.data[11] = 1; w.data[12] = 1;          // the token rows to emphasise
   *   pipe.qwenImage21ScalePrefixKv(0, 32, 1.0, 1.4, w);
   */
  qwenImage21ScalePrefixKv(layerLo, layerHi, kScale, vScale, rowMask) {}

  /**
   * Drop the live prefix KV cache so the next step re-extracts. The hooks above
   * do this themselves; call it after editing conditioning out of band.
   * A no-op when nothing has been primed.
   * @returns {undefined}
   */
  qwenImage21ResetCache() {}

  // ── prefix-cache slots ─────────────────────────────────────────────────────
  //
  // The extracted prefix IS the conditioning as far as every step after the
  // first is concerned. Saving one and blending another towards it is a
  // prompt crossfade that happens BELOW the text encoder entirely: no
  // re-encoding, no vision tower, no txt_in — just a lerp over cached K/V that
  // takes effect on the very next step.
  //
  // Slots hold real VRAM (a full per-layer K/V pair each), so there are
  // qwenImage21PrefixSlots() of them and qwenImage21ClearPrefixSlots() frees
  // them.

  /**
   * Deep-copy the LIVE extracted prefix K/V into slot `slot`. Requires the
   * prefix to have been extracted — run at least one step after priming.
   *
   * @param {number} slot  0 .. qwenImage21PrefixSlots() - 1
   * @returns {undefined}
   */
  qwenImage21SavePrefixCache(slot) {}

  /**
   * Blend the live prefix towards a saved one:
   *
   *     k = (1 - alpha) * k + alpha * saved.k        (and likewise v)
   *
   * alpha 0 is a no-op and 1 replaces the live prefix wholesale. Both caches
   * must describe the same LAYOUT — same prefix length, same target grid, same
   * layer count — which in practice means two prompts that tokenize to the
   * same length; a mismatch throws. Takes effect on the very next step, with
   * no re-extraction.
   *
   * @param {number} slot
   * @param {number} alpha  0..1
   * @returns {undefined}
   *
   * @example
   *   // A prompt crossfade that costs no re-encoding.
   *   const a = pipe.prime('a lighthouse at dawn', { steps: 8, seed: 7n });
   *   a.stepOnce();                          // extracts A's prefix
   *   pipe.qwenImage21SavePrefixCache(0);
   *
   *   const b = pipe.prime('a lighthouse at night', { steps: 8, seed: 7n });
   *   b.stepOnce();                          // extracts B's prefix
   *   pipe.qwenImage21BlendPrefixCache(0, 0.5);   // half-way to A
   *   for (let i = 1; i < 8; i++) b.stepOnce();
   *   const img = b.decode();
   */
  qwenImage21BlendPrefixCache(slot, alpha) {}

  /**
   * Drop every saved prefix slot and free their VRAM.
   * @returns {undefined}
   */
  qwenImage21ClearPrefixSlots() {}

  /** @returns {number} the slot capacity (4). */
  qwenImage21PrefixSlots() {}

  /**
   * Whether slot `slot` holds a saved prefix — what a slot picker greys out,
   * and the check to make before blending towards one.
   * @param {number} slot
   * @returns {boolean}
   */
  qwenImage21PrefixSlotValid(slot) {}

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
   * qwenImage21EncodePrompt() and encodeConditioning() throw until the encoder
   * is reloaded, and so does prime() / generate() for any prompt the memo
   * below has not already seen. Safe to call twice.
   * @returns {undefined}
   */
  qwenImage21ReleaseTextEncoder() {}

  // ── the prompt memo ────────────────────────────────────────────────────────
  //
  // prime(prompt) and generate(prompt) used to throw after
  // qwenImage21ReleaseTextEncoder() even for a prompt whose embeddings had
  // already been produced once — the rows existed, but nothing kept them.
  //
  // The pipeline now keeps a small memo of the last 8 encoded prompts (prompt
  // string -> rows), filled by qwenImage21EncodePrompt() and by every
  // prime/generate that encoded, so a memoized prompt primes with the encoder
  // gone. A prompt that is NOT memoized throws an error naming it.
  //
  // The recipe, on a 24 GB card: encode (or generate) every prompt you will
  // need, release the encoder, then prime freely with the 8.5 GiB back.
  //
  //   for (const p of prompts) pipe.qwenImage21EncodePrompt(p);   // <= 8
  //   pipe.qwenImage21ReleaseTextEncoder();
  //   const st = pipe.prime(prompts[0], { steps: 8 });            // fine

  /**
   * The prompts currently in the memo, most recently encoded first.
   * @returns {string[]}
   */
  qwenImage21MemoizedPrompts() {}

  /**
   * Empty the prompt memo.
   * @returns {undefined}
   */
  qwenImage21ClearPromptMemo() {}

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
 * loadWeights() resolves its path through the asset-path resolver, like
 * every other loader.
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
   * when nothing supplies them, must be positive, and the buffer must hold at
   * least in_channels * height * width values.
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
