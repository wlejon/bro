/**
 * bro.vision — Vision-model inference (brovisionml sibling)
 *
 * Image-understanding models that take pixels in and emit masks / maps /
 * boxes: promptable segmentation (SAM), monocular depth (Depth-Anything-V2),
 * surface normals (DSINE), and the ControlNet conditioning annotators — soft
 * edges (HED), line drawing (lineart), straight lines (MLSD), body pose
 * (OpenPose), semantic segmentation (SegFormer). Plus one *generative* model
 * that runs the other way, latent → image: StyleGAN3-R (loadStyleGAN3).
 *
 * Backed by brovisionml on top of brotensor + broimage. Models run on CUDA by
 * default — pass { device: 'cpu' } to force the CPU backend. brovisionml ships
 * code only; you supply the weight directories (HF safetensors checkpoints).
 *
 * ── Inputs ──
 * Every model takes an image as either:
 *   - an `ImageBitmap` (from createImageBitmap / a canvas / a Worker), or
 *   - an ImageData-shaped `{ data, width, height }` where `data` is an RGBA
 *     Uint8Array / Uint8ClampedArray (e.g. ctx.getImageData(...)).
 *
 * ── Outputs ──
 * Dense-map results come back as BOTH a drawable `ImageBitmap` (grayscale /
 * colorized, ready for drawImage, WebGL texImage2D, or a bro.diffusion
 * conditioning input) AND the raw typed-array data (Float32Array / Uint8Array).
 *
 * ── Sync vs async ──
 * Heavy work (load, SAM's image encode, every inference call, "segment
 * everything") runs on a background thread when you pass a callback —
 * loaders take opts.onReady/onError; inference takes opts.onDone — keeping the
 * JS thread responsive (the same convention bro.stt / bro.tts / bro.lm use).
 * The async call returns an AsyncHandle with `.cancel()`; onDone(result, info)
 * fires once on the JS thread with info = { cancelled, error? }. With no
 * callback the op runs inline and returns its result directly.
 *
 * Only one heavy op may be in flight per model at a time (the decoder / device
 * state is single-owner); a second concurrent call throws.
 */


// ── Init ────────────────────────────────────────────────────────────────────

/**
 * Initialize brotensor (probes the CUDA backend). Idempotent and thread-safe.
 * Optional — every loadXxx calls it — but useful to warm up explicitly.
 */
bro.vision.init();


// ── SAM (Segment Anything) ────────────────────────────────────────────────

/**
 * Load a SAM checkpoint (dir holding model.safetensors).
 * @param {string} dir
 * @param {Object} [opts]
 * @param {string} [opts.variant='vit_h']  'vit_h' | 'vit_l' | 'vit_b'
 * @param {string} [opts.device='cuda']    'cuda' | 'cpu'
 * @param {function} [opts.onReady]         async load: onReady(sam)
 * @param {function} [opts.onError]         async load: onError(message)
 * @returns {Sam|AsyncHandle}
 */
const sam = bro.vision.loadSam('weights/sam-vit-base', { variant: 'vit_b' });
sam.device;    // 'CUDA'
sam.hasImage;  // false until setImage()

/**
 * Sam.setImage(image, opts?) — run the slow ViT encode once; the embedding is
 * cached for subsequent segment() calls. Heavy — pass opts.onDone to run async.
 * @returns {undefined|AsyncHandle}
 */
sam.setImage(img);  // sync; or sam.setImage(img, { onDone: () => {...} })

/**
 * Sam.segment(opts) — cheap per-prompt decode against the cached embedding.
 * Synchronous. Coordinates are ORIGINAL-image pixels.
 * @param {Object} opts
 * @param {number[][]} [opts.points]  [[x,y], ...] click points
 * @param {number[]}   [opts.labels]  [1,0,...] 1=foreground 0=background
 *                                     (defaults to all-foreground)
 * @param {number[][]} [opts.boxes]   [[x1,y1,x2,y2], ...]
 * @param {boolean}    [opts.multimask=true]  return 3 ranked proposals vs 1
 * @returns {{ num, width, height, best,
 *             masks: Array<{ iou, data: Uint8Array, image: ImageBitmap }> }}
 *   `data` is a binary h*w mask (1=foreground); `image` is a translucent
 *   colored overlay; `best` indexes the highest-IoU mask.
 */
const seg = sam.segment({ points: [[320, 240]], labels: [1] });
const best = seg.masks[seg.best];   // best.iou, best.data, best.image

/**
 * Sam.segmentEverything(image, opts?) — the automatic mask generator
 * ("segment everything"): a regular point grid → multi-mask proposals →
 * IoU / stability filtering → box-NMS. Heavy — pass opts.onDone for async.
 * @param {Object} [opts]  pointsPerSide(32), pointsPerBatch(64),
 *   predIouThresh(0.88), stabilityThresh(0.95), boxNmsThresh(0.7),
 *   cropNLayers(0), minMaskRegionArea(0)
 * @returns {{ width, height, masks: Array<{ data, image, bbox:[x,y,w,h], area,
 *             predictedIou, stabilityScore, point:[x,y] }> }}  sorted by area.
 */
sam.segmentEverything(img, { pointsPerSide: 32, onDone: (r) => { /* r.masks */ } });


// ── Depth-Anything-V2 ─────────────────────────────────────────────────────

/**
 * @param {string} dir
 * @param {Object} [opts]  variant 'small'(default)|'base'|'large'; device; onReady/onError
 * @returns {DepthEstimator|AsyncHandle}
 */
const depth = bro.vision.loadDepth('weights/Depth-Anything-V2-Small');

/**
 * DepthEstimator.estimate(image, opts?)
 * @param {Object} [opts]  invert(false) — flip grayscale; onDone — run async
 * @returns {{ width, height, depth: Float32Array, image: ImageBitmap, min, max }}
 *   `depth` is relative inverse-depth (nearer = larger; NOT metric); `image`
 *   is the min-max-normalized grayscale map (brighter = nearer by default).
 */
const dm = depth.estimate(img);   // dm.depth, dm.image, dm.min, dm.max


// ── DSINE — surface normals ────────────────────────────────────────────────

/**
 * @param {string} dir
 * @param {Object} [opts]  fov(60) — assumed field-of-view for synthesized
 *   intrinsics; maxResolution(0=native) — cap the longer side: larger images are
 *   downscaled before inference and the normal map upscaled + re-normalized back
 *   (the OOM guard for big images; DSINE is capped, not tiled, because its
 *   geometry is conditioned on global intrinsics); device; onReady/onError
 * @returns {NormalEstimator|AsyncHandle}
 */
const normals = bro.vision.loadNormal('weights/dsine');

/**
 * NormalEstimator.estimate(image, opts?)
 * @param {Object} [opts]  fx,fy,cx,cy — explicit pinhole intrinsics (else
 *   synthesized from fov); onDone — run async
 * @returns {{ width, height, normals: Float32Array(3*h*w, planar NCHW),
 *             image: ImageBitmap }}
 *   Each pixel is a unit normal in CAMERA space (nx,ny,nz); `image` maps it to
 *   RGB via (n+1)/2 (the usual blue-ish normal map).
 */
const nm = normals.estimate(img);


// ── ControlNet annotators (HED / lineart / MLSD / OpenPose / SegFormer) ──────

/**
 * HED soft edges — bro.vision.loadHed(dir,
 *   { resolution?(0=native), tile?(0=off), overlap?(0), device, onReady })
 * SoftEdgeDetector.detect(image, opts?{onDone}) →
 *   { width, height, edge: Float32Array([0,1]), image: ImageBitmap (grayscale) }
 *
 *   For large images, set `tile` (working tile size in px, e.g. 512) + `overlap`
 *   (shared px, e.g. 64): HED runs each overlapping tile at native detail and
 *   feather-blends them into one full-res map at bounded memory. HED is a local
 *   FCN so the blend is seamless. When tiling is active each tile runs native, so
 *   `resolution` (the non-tiled longer-side cap) is ignored.
 */
const hed = bro.vision.loadHed('weights/hed');
const edges = hed.detect(img);   // edges.edge, edges.image

/**
 * Lineart — bro.vision.loadLineart(dir,
 *   { resolution?, invert?(true), tile?(0=off), overlap?(0), device, onReady })
 * LineartDetector.detect(image, opts?{onDone}) →
 *   { width, height, line: Float32Array([0,1]), image: ImageBitmap }
 *   invert (default) gives bright lines on a dark field — the ControlNet convention.
 *   `tile`/`overlap` work as in loadHed: tile large images and feather-blend the
 *   per-tile line maps (the generator is a local FCN; invert commutes with the
 *   blend). When tiling is active each tile runs native, so `resolution` is ignored.
 */
const lineart = bro.vision.loadLineart('weights/lineart');
const lines = lineart.detect(img);

/**
 * MLSD straight lines — bro.vision.loadMlsd(dir, { scoreThr?(0.1), distThr?(0.1), device, onReady })
 * MLSDdetector.detect(image, opts?{onDone}) →
 *   { width, height, segments: [{x1,y1,x2,y2,score}], image: ImageBitmap (white lines) }
 *   segment coords are ORIGINAL-image pixels.
 */
const mlsd = bro.vision.loadMlsd('weights/mlsd');
const segs = mlsd.detect(img);   // segs.segments, segs.image

/**
 * OpenPose body pose — bro.vision.loadOpenpose(dir, { resolution?(512), device, onReady })
 * OpenposeDetector.detect(image, opts?{onDone}) →
 *   { width, height,
 *     bodies: [{ keypoints: [{x,y,score,present}×18], totalScore, totalParts }],
 *     image: ImageBitmap (canonical colored pose sticks) }
 *   keypoints are normalized [0,1] over the detect-res canvas (COCO-18 order).
 *   Body-only (no hands/face), matching the ControlNet openpose control image.
 */
const openpose = bro.vision.loadOpenpose('weights/openpose');
const pose = openpose.detect(img);   // pose.bodies, pose.image

/**
 * SegFormer semantic segmentation — bro.vision.loadSegformer(dir, { device, onReady })
 *   dir holds model.safetensors + config.json (e.g. segformer-b0-finetuned-ade-512-512).
 * SegformerDetector.detect(image, opts?{onDone}) →
 *   { width, height, classes: Uint8Array(ADE20K ids 0..149),
 *     image: ImageBitmap (ADE20K-palette colorized) }
 */
const segformer = bro.vision.loadSegformer('weights/segformer-b0-ade');
const sem = segformer.detect(img);   // sem.classes, sem.image


// ── StyleGAN3-R — image generation (the one generative model) ────────────────

/**
 * Load an NVlabs StyleGAN3-R generator. Unlike the image→X models above, this
 * one runs latent → RGB. The checkpoint is a CONVERTED safetensors (StyleGAN3
 * ships Python pickles): brovisionml/scripts/download-stylegan3.sh fetches +
 * converts a released config-R model into weights/<name>/model.safetensors.
 * @param {string} dir   holds model.safetensors
 * @param {Object} [opts]
 * @param {number} [opts.resolution=256]  256 | 512 | 1024 — must match the checkpoint
 * @param {string} [opts.device='cuda']   'cuda' | 'cpu'
 * @param {function} [opts.onReady]        async load: onReady(gen)
 * @param {function} [opts.onError]
 * @returns {StyleGAN3|AsyncHandle}
 *   props: device, resolution, zDim (512), numWs (16), wDim (512)
 */
const gan = bro.vision.loadStyleGAN3('weights/stylegan3-r-ffhqu-256', { resolution: 256 });

/**
 * StyleGAN3.generate(opts?) — sample/render an image.
 * @param {Object} [opts]
 * @param {number}       [opts.seed=0]            sample z ~ N(0,1) from this seed
 * @param {Float32Array} [opts.z]                 use this latent (length zDim) instead of a seed
 * @param {number}       [opts.truncation=1.0]    truncation psi toward w_avg; <1 = tamer/more typical
 * @param {number}       [opts.truncationCutoff=-1] rows to truncate; -1 = all
 * @param {boolean}      [opts.returnLatents=false] also return the mapped w+
 * @param {function}     [opts.onDone]            run async
 * @returns {{ width, height, image: ImageBitmap, seed?, w?: Float32Array, numWs?, wDim? }}
 *   `image` is the RGB result (drawImage / WebGL texImage2D ready). `seed` echoes
 *   the seed when one was used. With returnLatents, `w` is the (numWs*wDim) W+
 *   row-major — edit/interpolate it and feed it back through synthesize().
 */
const r = gan.generate({ seed: 42, truncation: 0.7 });   // r.image, r.seed

/**
 * StyleGAN3.synthesize(w, opts?) — render an explicit W+ (skip the mapping).
 * @param {Float32Array} w   the full w+ (numWs*wDim) OR a single w (wDim),
 *                           broadcast across all rows.
 * @param {Object} [opts]    onDone — run async
 * @returns {{ width, height, image: ImageBitmap }}
 *
 *   Latent-space interpolation, end to end:
 *     const a = gan.generate({ seed: 1, returnLatents: true }).w;
 *     const b = gan.generate({ seed: 2, returnLatents: true }).w;
 *     const t = 0.5, mix = a.map((v, i) => v * (1 - t) + b[i] * t);
 *     const img = gan.synthesize(mix).image;   // the W+ midpoint face
 */
const mid = gan.synthesize(someWPlus);   // mid.image

/**
 * StyleGAN3.invert(image, opts?) — recover a W+ latent from an image (the
 * reverse of synthesize). Optimization-based GAN inversion: Adam on the W+ rows
 * minimizing image-space MSE through the frozen synthesis network. The recovered
 * `w` drops straight back into synthesize() / interpolation / style-mixing, so an
 * arbitrary face can be edited in the same latent space as a sampled one.
 * @param {ImageBitmap|{data,width,height}} image  RGBA source; MUST be the model
 *   resolution (e.g. 256×256) — resize the source first (drawImage onto a
 *   resolution-sized canvas) if it isn't.
 * @param {Object} [opts]
 * @param {number}   [opts.steps=350]      Adam iterations (more = closer fit, slower)
 * @param {number}   [opts.lr=0.05]        Adam learning rate (peak; warmup+cosine schedule)
 * @param {number}   [opts.regW=0]         L2 pull of w+ toward w_avg (>0 stays on-manifold / more editable)
 * @param {number}   [opts.initNoise=0]    stddev of gaussian jitter on the w_avg init
 * @param {number}   [opts.seed=0]         rng for initNoise
 * @param {Float32Array} [opts.initW]      start latent (num_ws*w_dim) to resume/refine
 *   from instead of w_avg — run invert in chunks (feed back the previous `w`) for
 *   live progressive refinement, or seed it from a known latent. regW still pulls toward w_avg.
 * @param {function} [opts.onDone]         run async — RECOMMENDED, inversion is slow (hundreds of synthesis passes)
 * @returns {{ width, height, image: ImageBitmap, w: Float32Array, numWs, wDim,
 *             loss: number, lossCurve: Float32Array }}
 *   `image` is the re-rendered recovered face, `w` the (numWs*wDim) W+, `loss`
 *   the final image-space MSE, `lossCurve` the per-step MSE (plot to watch convergence).
 *
 *   Invert then edit, end to end (async — the windowed event loop drives onDone):
 *     gan.invert(photo256, { steps: 300, onDone(res) {
 *       const w = res.w;                       // recovered latent
 *       // …interpolate / style-mix `w` like any sampled latent, then:
 *       const edited = gan.synthesize(w).image;
 *     }});
 */
const rec = gan.invert(photo256, { steps: 300 });   // rec.image, rec.w, rec.loss


// ── Pipe an annotator into bro.diffusion ─────────────────────────────────────
//
// The five ControlNet annotators produce conditioning images that feed
// bro.diffusion directly — the annotator's `image` ImageBitmap is exactly the
// control input a ControlNet-conditioned generate expects.
//
//   const cond = bro.vision.loadHed('weights/hed').detect(photo).image;
//   // → use `cond` as the ControlNet conditioning image in bro.diffusion
