/**
 * bro.vision, Vision-model inference (brovisionml sibling)
 *
 * Image-understanding models that take pixels in and emit masks / maps /
 * boxes: promptable segmentation (SAM), monocular depth (Depth-Anything-V2),
 * surface normals (DSINE), the ControlNet conditioning annotators — soft edges
 * (HED), line drawing (lineart), straight lines (MLSD), body pose (OpenPose),
 * semantic segmentation (SegFormer) — plus dichotomous matting (BiRefNet), the
 * two raw ViT backbones (DINOv2 / DINOv3), and one *generative* model that runs
 * the other way, latent → image: StyleGAN3 (loadStyleGAN3).
 *
 * Backed by brovisionml on top of brotensor + broimage. brovisionml ships code
 * only; you supply the weight directories (HF safetensors checkpoints).
 *
 * `bro.vision.version` — the brovisionml version string.
 *
 * ── Devices ──
 * Every loader takes `opts.device`: 'cuda', 'metal', 'gpu' (the best GPU present,
 * CUDA then Metal), or 'cpu' (case-insensitive). Omitted, it picks the best backend present —
 * CUDA, then Metal, then CPU — so a GPU machine gets the GPU without asking.
 * Anything else throws a TypeError, and asking for a backend that is not
 * available (not compiled in, or no device) throws an Error rather than
 * falling back to CPU, which for these models means minutes instead of
 * milliseconds. Gate a big load on `bro.gpu` and read back `model.device` to
 * see what you got.
 *
 *   if (!bro.gpu.available) return;                     // no GPU, don't bother
 *   const depth = bro.vision.loadDepth(dir);            // CUDA/Metal when present
 *   console.log(depth.device);                          // 'CUDA' | 'Metal' | 'CPU'
 *   bro.vision.loadDepth(dir, { device: 'tpu' });       // TypeError
 *
 * A loader throws when its path is missing, not a directory/file it can read,
 * or holds a checkpoint that fails to load; the message names the loader
 * ("loadDepth failed: …"). The loaders are synchronous: they return the
 * model and take no onReady / onError callbacks (only the inference calls
 * have async forms).
 *
 * ── Inputs ──
 * Every model takes an image as either:
 *   - an ImageData-shaped `{ width, height, data }` where `data` is an RGBA
 *     Uint8Array / Uint8ClampedArray (e.g. `ctx.getImageData(0, 0, w, h)`), or
 *   - a filesystem path string, decoded by broimage (PNG / JPEG / …).
 *
 * Holding an `ImageBitmap`? Draw it to a canvas and pass the `getImageData()`
 * result — that object already has the `{ width, height, data }` shape.
 *
 * An `{ width, height, data }` image needs integer sides in 1..65536 and a
 * `data` of at least width*height*4 bytes; anything else throws a TypeError
 * rather than reading past the buffer.
 *
 * A model with no weights never answers with a placeholder: calling a model
 * after `dispose()` (or one whose weights are not loaded) throws.
 */


// ═════════════════════════════════════════════════════════════════════════════
// ImageBitmap results, and the sync / async duality
// ═════════════════════════════════════════════════════════════════════════════
//
// The heavy ops below return BOTH representations of their output:
//
//   (a) the raw typed-array planes brovisionml computes — `data`, `matte`,
//       `gray`, `edge`/`edges`, `line`/`lines`, `classes`/`segments`,
//       `bodies`/`poses`, `alpha`, `normals`, `depth`, always alongside
//       `width` / `height`. Which planes you get depends on the op; each
//       method below names its own. These are for your own math: thresholding,
//       compositing, feeding a WebGL shader, writing a file.
//
//   (b) an engine-minted `ImageBitmap`, ready to draw. The engine builds these
//       on bro's side (brovisionml is a standalone sibling and cannot mint a
//       DOM type), so they exist only through `bro.vision`, not in a bare
//       brovisionml build:
//
//         `image`  on SAM segment() / segmentEverything() masks
//         `image`  on every annotator detect() / estimate()
//         `image`  on depth estimate() and normals estimate()
//         `image`  on BiRefNet removeBackground()
//         `image`  on StyleGAN3 generate() / synthesize() / invert()
//         `matte`  on BiRefNet removeBackground() — an ImageBitmap here, not a
//                  typed array; the byte matte is on `mask`
//
//       An ImageBitmap is directly usable as a `drawImage` / `texImage2D`
//       source, and as a ControlNet conditioning input for bro.diffusion. See
//       docs/imagebitmap-api.js.
//
// ── Sync vs async ──
//
// Those same ops — plus `Sam.setImage()`, which mints no bitmap but is SAM's
// expensive half — accept an optional `onDone` callback in their options
// object:
//
//   WITHOUT onDone — the op runs synchronously on the JS thread and returns
//   its result directly. Simplest; blocks the frame for as long as inference
//   takes. Fine in bro-headless, fine for cheap ops (SAM's per-click
//   segment()), bad for a windowed app running a 1024² BiRefNet.
//
//   WITH onDone — the op runs on a background thread and returns an
//   `AsyncHandle` immediately: `handle.cancel()` asks it to stop, and
//   `handle.done` reads true once the worker has returned (the callback fires
//   on the next frame after that, never inside the launching call). When it
//   finishes, `onDone(result, info)` fires once on the JS thread, where
//   `result` is the same object the sync form would have returned and `info`
//   is `{ cancelled: boolean, error?: string }`. On a cancel or an error,
//   `result` is `null` — inspect `info` first.
//
//   The delivery point is the engine's per-frame pump, so a headless script
//   has to let frames run: `advanceTime()` moves virtual time and returns at
//   once, so pair it with a real `sleep()` to give the worker wall time (see
//   tests/vision/test_vision_async.js).
//
//   ONE OP AT A TIME PER MODEL. The worker uses the model's own inference
//   workspace, so a second call on a model with an op in flight throws
//   "an operation is already in flight on this model" rather than racing.
//   The model is free again the moment the callback has run — starting the
//   next step from inside onDone is fine, and is the intended way to chain.

// Sync: the whole call returns the finished result.
const dm = depth.estimate(frame);
ctx.drawImage(dm.image, 0, 0);                  // (b) drawable
console.log(dm.depth[0], dm.min, dm.max);       // (a) raw plane

// Async: returns a handle now, calls back later.
const job = depth.estimate(frame, {
  onDone(result, info) {
    if (info.cancelled) return;
    if (info.error) { console.error(info.error); return; }
    ctx.drawImage(result.image, 0, 0);
  }
});
// …later, if the user navigated away:
job.cancel();


// ═════════════════════════════════════════════════════════════════════════════
// Init
// ═════════════════════════════════════════════════════════════════════════════

/**
 * Initialize brotensor (probes the CUDA / Metal backends). Idempotent and
 * thread-safe. Optional — every loadXxx path needs it — but useful to warm up
 * explicitly before timing anything.
 */
bro.vision.init();


// ═════════════════════════════════════════════════════════════════════════════
// SAM (Segment Anything)
// ═════════════════════════════════════════════════════════════════════════════

/**
 * Load a SAM checkpoint. `path` is a directory holding model.safetensors, or
 * the .safetensors file itself; it must exist or the loader throws. The
 * checkpoint is read as ViT-B.
 * @param {string} path
 * @param {Object} [opts]
 * @param {string} [opts.device=best available]  'cuda' | 'gpu' | 'metal' | 'cpu'
 * @returns {Sam}
 */
const sam = bro.vision.loadSam('weights/sam-vit-base', { device: 'cuda' });
sam.device;    // 'CUDA'
sam.hasImage;  // false until setImage()

/**
 * Sam.setImage(image, opts?) — run the slow ViT encode once. The embedding is
 * cached on the model, so subsequent segment() calls are the cheap decode only.
 *
 * This is the expensive half of SAM, so it takes `onDone` like the rest: pass
 * one in a windowed app and click-to-segment stays interactive while the
 * encode runs. `hasImage` flips true as soon as the call is made (the frame is
 * recorded before the encode), so gate your first segment() on the callback,
 * not on `hasImage`.
 *
 * @param {{width,height,data}|string} image
 * @param {Object} [opts]
 * @param {function} [opts.onDone]  run async: onDone(undefined, info)
 * @returns {undefined|AsyncHandle}  an AsyncHandle when onDone is given
 */
sam.setImage(photo);

/**
 * Sam.segment(opts) — per-prompt mask decode against the cached embedding.
 * Coordinates are ORIGINAL-image pixels. Throws a TypeError when neither
 * `points` nor `boxes` is given, and an Error when setImage() has not run.
 *
 * @param {Object} opts
 * @param {Array} [opts.points]   `[[x, y], ...]` or `[{ x, y, label? }, ...]`.
 *                                The object form carries its own per-point
 *                                label; the bare-pair form is all foreground.
 * @param {number[]} [opts.labels] `[1, 0, ...]` — 1 = foreground, 0 =
 *                                background. An explicit array wins over the
 *                                per-point `label` fields; short arrays are
 *                                padded with 1.
 * @param {Array} [opts.boxes]    `[[x1, y1, x2, y2], ...]` or
 *                                `[{ x1, y1, x2, y2 }, ...]`.
 * @param {boolean} [opts.multimask=true]  3 ranked proposals vs 1.
 * @param {function} [opts.onDone] run async: onDone(result, info)
 * @returns {{ num: number, width: number, height: number, best: number,
 *             masks: Array<{ iou: number, data: Uint8Array,
 *                            logits: Float32Array, image: ImageBitmap }> }
 *          | AsyncHandle}
 *   `data` is the binarized h*w mask (1 = foreground), `logits` the raw h*w
 *   FP32 logits behind it (threshold them yourself for a softer edge),
 *   `image` the drawable overlay, `best` the index of the highest-IoU mask.
 */
const seg = sam.segment({ points: [[320, 240]], labels: [1] });
const best = seg.masks[seg.best];
ctx.drawImage(best.image, 0, 0);          // overlay the winning mask
best.iou;                                  // its predicted IoU

// Point + negative point + a box, all at once:
sam.segment({
  points: [{ x: 320, y: 240, label: 1 }, { x: 40, y: 40, label: 0 }],
  boxes: [[100, 80, 540, 460]],
  multimask: false
});

/**
 * Sam.segmentEverything(image, opts?) — the automatic mask generator: a regular
 * point grid → multimask proposals → IoU / stability filtering → box-NMS. Heavy;
 * pass onDone in a windowed app. Leaves its own image cached on the model
 * (hasImage becomes true).
 *
 * @param {{width,height,data}|string} image
 * @param {Object} [opts]
 * @param {number} [opts.pointsPerSide=32]       grid is this squared
 * @param {number} [opts.pointsPerBatch=64]      grid points per batched decode
 * @param {number} [opts.predIouThresh=0.88]     drop masks below this predicted IoU
 * @param {number} [opts.stabilityThresh=0.95]   drop masks below this stability
 * @param {number} [opts.boxNmsThresh=0.7]       box-NMS IoU threshold
 * @param {number} [opts.cropNLayers=0]          0 = whole image; N adds crop layers
 * @param {number} [opts.minMaskRegionArea=0]    0 = off; else fill holes / drop islands
 * @param {function} [opts.onDone]               run async
 * @returns {{ width: number, height: number,
 *             masks: Array<{ data: Uint8Array, width: number, height: number,
 *                            bbox: [number,number,number,number], area: number,
 *                            predictedIou: number, stabilityScore: number,
 *                            point: [number, number], image: ImageBitmap }> }
 *          | AsyncHandle}
 *   `bbox` is `[x, y, w, h]` in original-image pixels, `area` the foreground
 *   pixel count, `point` the grid point that produced the mask.
 */
sam.segmentEverything(photo, {
  pointsPerSide: 32,
  onDone(r, info) {
    if (info.error) return;
    for (const m of r.masks) ctx.drawImage(m.image, 0, 0);
  }
});


// ═════════════════════════════════════════════════════════════════════════════
// Depth-Anything-V2, monocular depth
// ═════════════════════════════════════════════════════════════════════════════

/**
 * Load a Depth-Anything-V2 checkpoint (read as the V2-Small config). `path` is
 * a directory holding model.safetensors, or the file itself.
 * @param {string} path
 * @param {Object} [opts]
 * @param {string} [opts.device=best available]  'cuda' | 'gpu' | 'metal' | 'cpu'
 * @returns {DepthEstimator}
 */
const depth = bro.vision.loadDepth('weights/Depth-Anything-V2-Small',
                                   { device: 'cuda' });
depth.device;   // 'CUDA'

/**
 * DepthEstimator.estimate(image, opts?)
 * @param {{width,height,data}|string} image
 * @param {Object} [opts]
 * @param {boolean}  [opts.invert=false]  flip the normalized `gray` plane (and
 *   the `image` built from it), so far = bright instead of near = bright
 * @param {function} [opts.onDone]        run async
 * @returns {{ width: number, height: number, depth: Float32Array,
 *             gray: Uint8Array, min: number, max: number,
 *             image: ImageBitmap } | AsyncHandle}
 *   `depth` is relative inverse depth (nearer = larger; NOT metric), h*w FP32.
 *   `min` / `max` are its extremes. `gray` is that plane min-max normalized to
 *   h*w bytes, and `image` is `gray` as a drawable grayscale bitmap.
 */
const dmap = depth.estimate(photo);
ctx.drawImage(dmap.image, 0, 0);
const nearest = dmap.max;                       // relative, unitless

// Colorize the raw plane instead of using the grayscale bitmap:
const turbo = bro.vision.colorizeDepth(dmap.depth,
  { width: dmap.width, height: dmap.height, map: 'turbo' });
ctx.putImageData(new ImageData(turbo.data, turbo.width, turbo.height), 0, 0);


// ═════════════════════════════════════════════════════════════════════════════
// DSINE, surface normals
// ═════════════════════════════════════════════════════════════════════════════

/**
 * @param {string} path  dir holding model.safetensors, or the file
 * @param {Object} [opts]
 * @param {string} [opts.device=best available]
 * @returns {NormalEstimator}
 */
const normals = bro.vision.loadNormal('weights/dsine', { device: 'cuda' });

/**
 * NormalEstimator.estimate(image, opts?)
 * @param {{width,height,data}|string} image
 * @param {Object} [opts]
 * @param {number} [opts.fx]  explicit pinhole intrinsics. Passing `fx` switches
 *   DSINE off its fov-synthesized default and takes `fy`, `cx`, `cy` with it
 *   (each defaults to 0 if omitted, so pass all four). Without `fx` the model
 *   synthesizes intrinsics from an assumed field of view.
 * @param {number} [opts.fy]
 * @param {number} [opts.cx]
 * @param {number} [opts.cy]
 * @param {function} [opts.onDone]  run async
 * @returns {{ width: number, height: number, normals: Float32Array,
 *             normal: Float32Array, image: ImageBitmap } | AsyncHandle}
 *   `normals` is 3*h*w planar (all nx, then all ny, then all nz), each pixel a
 *   unit normal in CAMERA space. `normal` is the same array under the other
 *   spelling — both keys reference one buffer, so writing through one is
 *   visible through the other. `image` maps it to RGB via (n+1)/2, the usual
 *   blue-ish normal map.
 */
const nm = normals.estimate(photo, { fx: 1200, fy: 1200, cx: 640, cy: 360 });
ctx.drawImage(nm.image, 0, 0);
const nx = nm.normals[0];                                  // plane 0
const ny = nm.normals[nm.width * nm.height + 0];           // plane 1


// ═════════════════════════════════════════════════════════════════════════════
// ControlNet annotators — HED / Lineart / MLSD / OpenPose / SegFormer
// ═════════════════════════════════════════════════════════════════════════════
//
// All five load the same way: `bro.vision.loadXxx(path, { device })`, where
// `path` is a weights directory or a single .safetensors file. All five expose
// `device` and a single inference method published under BOTH names —
// `detect()` (the original) and `estimate()` (the bronze port's name) — which
// are the same function, so pick either. Both take `(image, opts?)` and accept
// `opts.onDone` for the async form. Every result carries a drawable `image`.
//
// Result planes likewise carry both spellings, an FP32 plane under the original
// key and a byte or Int32 plane under the port's key; each section names them.

/**
 * HED soft edges.
 * @returns {{ width, height, edge: Float32Array, edges: Uint8Array,
 *             image: ImageBitmap } | AsyncHandle}
 *   `edge` is the h*w FP32 response in [0, 1]; `edges` is the same plane
 *   quantized to bytes; `image` is the drawable grayscale edge map.
 */
const hed = bro.vision.loadHed('weights/hed', { device: 'cuda' });
const e = hed.detect(photo);        // === hed.estimate(photo)
ctx.drawImage(e.image, 0, 0);

/**
 * Lineart — a clean line drawing.
 * @returns {{ width, height, line: Float32Array, lines: Uint8Array,
 *             image: ImageBitmap } | AsyncHandle}
 */
const lineart = bro.vision.loadLineart('weights/lineart', { device: 'cuda' });
const l = lineart.detect(photo);    // l.line (FP32), l.lines (bytes), l.image

/**
 * MLSD straight-line segments. Unlike HED / Lineart this one is not a dense
 * plane: it returns a segment list in ORIGINAL-image pixels.
 * @returns {{ width, height,
 *             segments: Array<{x1, y1, x2, y2, score}>,
 *             lines: Array<{x1, y1, x2, y2, score}>,
 *             image: ImageBitmap } | AsyncHandle}
 *   `segments` and `lines` are the SAME array object under both key spellings.
 */
const mlsd = bro.vision.loadMlsd('weights/mlsd', { device: 'cuda' });
const segs = mlsd.detect(photo);
ctx.beginPath();
for (const s of segs.segments) { ctx.moveTo(s.x1, s.y1); ctx.lineTo(s.x2, s.y2); }
ctx.stroke();

/**
 * OpenPose — body-only pose (no hands / face), matching the ControlNet
 * openpose control image.
 * @returns {{ width, height,
 *             bodies: Array<{ keypoints: Array<{x, y, score, present}>,
 *                             totalScore: number, totalParts: number,
 *                             score: number }>,
 *             poses: Array<...>,
 *             image: ImageBitmap } | AsyncHandle}
 *   18 keypoints per body in COCO-18 order; `x`/`y` are normalized [0, 1] over
 *   the detect-resolution canvas and are -1 when `present` is false, so always
 *   test `present` before using them. `totalScore` sums the found parts'
 *   confidences, `totalParts` counts them; `score` is an alias of `totalScore`.
 *   `bodies` and `poses` are the SAME array under both key spellings.
 */
const openpose = bro.vision.loadOpenpose('weights/openpose', { device: 'cuda' });
const pose = openpose.detect(photo);
for (const body of pose.bodies) {
  for (const kp of body.keypoints) {
    if (!kp.present) continue;
    ctx.fillRect(kp.x * canvas.width - 2, kp.y * canvas.height - 2, 4, 4);
  }
}

/**
 * SegFormer semantic segmentation. `path` holds model.safetensors + config.json
 * (e.g. segformer-b0-finetuned-ade-512-512).
 * @returns {{ width, height, classes: Uint8Array, segments: Int32Array,
 *             image: ImageBitmap } | AsyncHandle}
 *   `classes` is the h*w ADE20K class-id plane (ids 0..149) as bytes;
 *   `segments` is the same ids widened to Int32 (a separate buffer, not an
 *   alias). `image` is the palette-colorized map.
 */
const segformer = bro.vision.loadSegformer('weights/segformer-b0-ade',
                                           { device: 'cuda' });
const sem = segformer.detect(photo, {
  onDone(r, info) { if (!info.error) ctx.drawImage(r.image, 0, 0); }
});

// ── Pipe an annotator into bro.diffusion ────────────────────────────────────
// The annotator's `image` ImageBitmap is exactly the conditioning input a
// ControlNet-conditioned generate expects:
//
//   const cond = bro.vision.loadHed('weights/hed').detect(photo).image;


// ═════════════════════════════════════════════════════════════════════════════
// BiRefNet — background removal / dichotomous matting
// ═════════════════════════════════════════════════════════════════════════════

/**
 * Load a BiRefNet checkpoint — the Swin-L .safetensors file (the same one
 * bro.triposplat takes as its optional matting front-end), or a dir holding it.
 * @param {string} path
 * @param {Object} [opts]
 * @param {number} [opts.modelSize=1024]  square inference resolution, a
 *   multiple of 32 up to 4096 (anything else is a TypeError). Lower is faster
 *   at the cost of edge fidelity; 1024 is the reference recipe.
 * @param {string} [opts.device=best available]
 * @returns {Birefnet}   props: `device`, `modelSize`
 */
const rembg = bro.vision.loadBirefnet(
  'weights/triposplat/background_removal/birefnet.safetensors',
  { device: 'cuda', modelSize: 1024 });

/**
 * Birefnet.removeBackground(image, opts?) — also published as `estimate()`,
 * the bronze port's name for the same function.
 * @param {{width,height,data}|string} image
 * @param {Object} [opts]
 * @param {number}   [opts.modelSize]  override the load-time resolution for
 *                                     this one call
 * @param {function} [opts.onDone]     run async
 * @returns {{ width: number, height: number,
 *             alpha: Float32Array, mask: Uint8Array, data: Uint8Array,
 *             matte: ImageBitmap, image: ImageBitmap } | AsyncHandle}
 *   `alpha` is the h*w predicted matte in [0, 1]; `mask` the same matte as
 *   h*w bytes; `data` the input RGBA (h*w*4) with its alpha channel replaced by
 *   the matte — a ready-to-composite cutout buffer. `matte` and `image` are
 *   the engine-minted drawables: `matte` the grayscale matte, `image` the
 *   cutout itself.
 *
 *   NOTE `matte` here is an ImageBitmap, not a typed array. The byte plane
 *   lives on `mask`.
 */
const cut = rembg.removeBackground(photo);
ctx.drawImage(cut.image, 0, 0);        // subject only, transparent background
ctx.drawImage(cut.matte, 0, 0);        // the matte, as a grayscale bitmap
cut.alpha[y * cut.width + x];          // per-pixel coverage in [0, 1]

/**
 * Birefnet.dispose() — free the weights now. They are GPU-resident and
 * invisible to the JS heap accounting, so a caller done with the model must
 * not wait for the finalizer. Calling removeBackground() afterwards throws.
 */
rembg.dispose();


// ═════════════════════════════════════════════════════════════════════════════
// StyleGAN3 — the one generative model (latent → image)
// ═════════════════════════════════════════════════════════════════════════════

/**
 * Load an NVlabs StyleGAN3 generator. The checkpoint is a CONVERTED
 * safetensors (StyleGAN3 ships Python pickles);
 * brovisionml/scripts/download-stylegan3.sh fetches and converts a released
 * model into weights/<name>/model.safetensors.
 *
 * `resolution` and `variant` are not sniffed from the file — they select the
 * config the weights are read into, so they MUST match the checkpoint. A bad
 * value throws a TypeError; a missing or unreadable checkpoint throws an
 * Error ("loadStyleGAN3 failed: ..."). There is no weights-free placeholder.
 *
 * @param {string} path
 * @param {Object} [opts]
 * @param {number} [opts.resolution=256]  256 | 512 | 1024
 * @param {string} [opts.variant='r']     'r' (config-R, rotation-equivariant)
 *                                        | 't' (config-T, translation-equivariant)
 * @param {string} [opts.device=best available]
 * @returns {StyleGAN3}
 *   props: `device`, `resolution` (and its alias `imgResolution`),
 *   `imgChannels` (3), `variant`, `zDim` (512), `wDim` (512), `numWs`, `cDim`
 */
const gan = bro.vision.loadStyleGAN3('weights/stylegan3-r-ffhqu-256',
  { resolution: 256, variant: 'r', device: 'cuda' });

/**
 * StyleGAN3.generate(opts?) — sample z, map it to W+, render.
 * @param {Object} [opts]
 * @param {number} [opts.seed=0]              sample z ~ N(0,1) from this seed
 * @param {Float32Array} [opts.z]             use this latent (length zDim)
 *                                            instead of a seed; wins over seed,
 *                                            and a wrong length throws
 * @param {number} [opts.truncation=1.0]      psi toward w_avg; < 1 = tamer
 * @param {number} [opts.truncationCutoff=-1] rows to truncate; -1 = all
 * @param {boolean} [opts.returnLatents=false] also return the mapped W+
 * @param {function} [opts.onDone]            run async
 * @returns {{ data: Uint8Array, width: number, height: number,
 *             channels: number, image: ImageBitmap,
 *             seed?: number, w?: Float32Array, numWs?: number, wDim?: number }
 *          | AsyncHandle}
 *   `data` is the raw interleaved pixel buffer, width*height*channels (channels
 *   is 3 — RGB, not RGBA); `image` is the same picture as a drawable bitmap.
 *   `seed` echoes the seed when one was used (absent when `z` was passed).
 *   With returnLatents, `w` is the (numWs*wDim) W+ row-major.
 */
const r = gan.generate({ seed: 42, truncation: 0.7 });
ctx.drawImage(r.image, 0, 0);
r.seed;      // 42

/**
 * StyleGAN3.synthesize(w, opts?) — render an explicit W+, skipping the mapping.
 * @param {Float32Array} w   the full W+ (numWs*wDim) OR a single w (wDim),
 *                           broadcast across all rows. Any other length throws.
 * @param {Object} [opts]
 * @param {function} [opts.onDone]  run async
 * @returns {{ data: Uint8Array, width, height, channels, image: ImageBitmap }
 *          | AsyncHandle}
 */
// Latent-space interpolation, end to end:
const a = gan.generate({ seed: 1, returnLatents: true }).w;
const b = gan.generate({ seed: 2, returnLatents: true }).w;
const t = 0.5;
const mix = a.map((v, i) => v * (1 - t) + b[i] * t);
ctx.drawImage(gan.synthesize(mix).image, 0, 0);     // the W+ midpoint

/**
 * StyleGAN3.invert(image, opts?) — recover a W+ latent from a picture, the
 * reverse of synthesize(). Optimization-based GAN inversion: Adam on the W+
 * rows minimizing image-space MSE through the frozen synthesis network. The
 * recovered `w` drops straight back into synthesize() / interpolation, so an
 * arbitrary face can be edited in the same latent space as a sampled one.
 *
 * Slow — hundreds of synthesis passes. It takes `onDone` like the other heavy
 * ops, which is the usual way to keep a window responsive through one; you can
 * also chunk it by passing the previous call's `w` back as `initW` to resume,
 * or run it in a Worker (docs/worker-api.js).
 *
 * @param {{width,height,data}|string} image  MUST be square at the model
 *   resolution (e.g. 256×256) — resize the source first; anything else throws.
 * @param {Object} [opts]
 * @param {number} [opts.steps=350]     Adam iterations (more = closer, slower)
 * @param {number} [opts.lr=0.05]       Adam learning rate
 * @param {number} [opts.regW=0]        L2 pull of W+ toward w_avg (> 0 stays
 *                                      on-manifold / more editable)
 * @param {number} [opts.initNoise=0]   stddev of gaussian jitter on the init
 * @param {number} [opts.seed=0]        rng for initNoise
 * @param {Float32Array} [opts.initW]   start latent (numWs*wDim) to resume or
 *                                      refine from instead of w_avg
 * @param {function} [opts.onDone]      run async: onDone(result, info)
 * @returns {{ data: Uint8Array, image: ImageBitmap, width, height, channels,
 *             w: Float32Array, numWs: number, wDim: number, loss: number,
 *             lossCurve: Float32Array }}
 *   `data` is the re-rendered recovered image (RGB) and `image` the same
 *   pixels as a drawable ImageBitmap, `w` the recovered W+, `loss` the final
 *   image-space MSE, `lossCurve` the per-step MSE — plot it to watch
 *   convergence. Throws when no weights are loaded, rather than returning a
 *   placeholder.
 */
const rec = gan.invert(photo256, { steps: 300, lr: 0.05 });
const edited = gan.synthesize(rec.w);      // edit rec.w first, then re-render


// ═════════════════════════════════════════════════════════════════════════════
// DINOv2 / DINOv3 backbones — raw ViT features
// ═════════════════════════════════════════════════════════════════════════════
//
// Both expose `encode(image, opts?)`, also published as `estimate()` (the same
// function), plus `dispose()`. Both stretch the source to a square — aspect
// ratio is NOT preserved — ImageNet-normalize it, and run the backbone.
// Neither takes onDone; encode() is synchronous.

/**
 * Load the DINOv2 ViT feature-extractor backbone: the image encoder behind
 * Depth-Anything-V2, exposed standalone for raw patch features. Reads
 * model.safetensors from the dir (the `backbone.` namespace of an HF
 * DepthAnythingForDepthEstimation / DINOv2 checkpoint).
 * @param {string} path
 * @param {Object} [opts]
 * @param {string} [opts.variant='small']  'small'|'vit_s' | 'base'|'vit_b' |
 *                                         'large'|'vit_l'
 * @param {string} [opts.device=best available]
 * @returns {Dinov2}
 *   props: `device`, `patchSize`, `embedDim`, `defaultSize` (the config's
 *   img_size, 518 for ViT-S)
 */
const d2 = bro.vision.loadDinov2('weights/Depth-Anything-V2-Small',
                                 { variant: 'small', device: 'cuda' });

/**
 * Dinov2.encode(image, opts?) — run the backbone and return the DPT-stage
 * hidden states (HF Dinov2Backbone out_features). Each feature map has the
 * backbone's final LayerNorm applied; token row 0 is the cls token, the rest
 * are patch tokens in row-major (h-major) order.
 * @param {{width,height,data}|string} image
 * @param {Object} [opts]
 * @param {number} [opts.size]  square input side, a positive multiple of
 *   `patchSize` (default `defaultSize`); anything else throws
 * @returns {{ features: Float32Array[], stages: number[], tokens: number,
 *             dim: number, patchH: number, patchW: number,
 *             numPrefixTokens: 1 }}
 *   `features[i]` is the (tokens*dim) map for out-stage `stages[i]`;
 *   tokens = 1 + patchH*patchW. Throws "encode: disposed" after dispose().
 */
const f2 = d2.encode(photo);
f2.features[f2.features.length - 1];   // last-stage (tokens*dim) map
d2.dispose();

/**
 * Load the DINOv3 ViT-H backbone: the image encoder behind TripoSplat, exposed
 * standalone. `path` is the dino_v3_vit_h.safetensors file, or a dir holding
 * the checkpoint. The variant is fixed at ViT-H.
 * @param {string} path
 * @param {Object} [opts]
 * @param {string} [opts.device=best available]
 * @returns {Dinov3}
 *   props: `device`, `patchSize`, `embedDim`, `numRegisterTokens`,
 *   `defaultSize`
 */
const d3 = bro.vision.loadDinov3(
  'weights/triposplat/clip_vision/dino_v3_vit_h.safetensors',
  { device: 'cuda' });

/**
 * Dinov3.encode(image, opts?) — the single final hidden state (final LayerNorm
 * applied). The token sequence is [cls, register×N, patch tokens]: rows
 * [0, numPrefixTokens) are the cls + register tokens, the rest patch tokens in
 * row-major order.
 * @param {{width,height,data}|string} image
 * @param {Object} [opts]
 * @param {number} [opts.size]  square side, a positive multiple of `patchSize`
 * @returns {{ features: Float32Array, tokens: number, dim: number,
 *             patchH: number, patchW: number, numPrefixTokens: number }}
 *   `features` is the (tokens*dim) map; tokens = numPrefixTokens + patchH*patchW.
 */
const f3 = d3.encode(photo, { size: 224 });
const firstPatch = f3.features.subarray(f3.numPrefixTokens * f3.dim,
                                        (f3.numPrefixTokens + 1) * f3.dim);

/**
 * Dinov2.dispose() / Dinov3.dispose() — deterministically free the weights
 * (~1.3 GB for ViT-H). GPU memory is invisible to the JS GC, so a caller done
 * with a backbone must not wait for the finalizer: dispose, then
 * `bro.gpu.trim()` to hand the cached blocks back to the driver. encode()
 * throws afterwards; load again for a fresh handle.
 */
d3.dispose();


// ═════════════════════════════════════════════════════════════════════════════
// Post-processing ops — bro.vision.* and the identical bro.vision.ops.*
// ═════════════════════════════════════════════════════════════════════════════
//
// Pure CPU helpers, no model and no weights. Every one is mounted twice, on
// `bro.vision` and on `bro.vision.ops`, so `bro.vision.nms === bro.vision.ops.nms`
// in behaviour; use whichever reads better.

/**
 * bro.vision.decodeBoxes(predictions, opts?) — turn a raw YOLO-style detection
 * tensor into boxes, with NMS.
 * @param {Float32Array} predictions  the flat head output; must be a
 *   Float32Array or it throws
 * @param {Object} [opts]
 * @param {number} [opts.numClasses=80]      channel layout is 4 + numClasses
 * @param {number} [opts.confThreshold=0.25]
 * @param {number} [opts.iouThreshold=0.45]
 * @param {boolean} [opts.nms=true]          run greedy NMS (cap 100 boxes)
 * @param {boolean} [opts.transposed=false]  `[4+C, N]` (YOLOv8) vs `[N, 4+C]`
 * @returns {Array<{x1, y1, x2, y2, score: number, classId: number}>}
 *   Input boxes are center-form (cx, cy, w, h); output is corner-form.
 */
const boxes = bro.vision.decodeBoxes(head, { numClasses: 80, transposed: true });

/**
 * bro.vision.nms(boxes, opts?) — greedy non-maximum suppression over boxes
 * you already have.
 * @param {Array<{x1,y1,x2,y2,score?,classId?}>|Array<number[]>|Array<Float32Array>|Float32Array|ArrayBuffer|{data|boxes|buffer}} boxes
 *   box objects; per-box arrays or Float32Array views `[x1,y1,x2,y2,score?,classId?]`;
 *   or one flat Float32Array / ArrayBuffer of `stride` floats per box (bare or
 *   under `.data` / `.boxes` / `.buffer`)
 * @param {Object} [opts]
 * @param {number} [opts.iouThreshold=0.45]
 * @param {number} [opts.maxDetections=100]
 * @param {boolean} [opts.perClass=false]     suppress only within a class
 * @param {number} [opts.scoreThreshold=0]    drop below this before suppressing
 * @param {number} [opts.stride]              floats per box in a flat input
 *   (4 = no score, 5 = no class, 6); below 4 it is inferred from the length
 * @param {boolean} [opts.returnIndices=false] return the kept input indices
 * @param {boolean} [opts.asTypedArray=false]  kept boxes as a flat Float32Array
 *   (6 per box), or with returnIndices an Int32Array
 * @returns {Array<{x1, y1, x2, y2, score, classId, index}>|number[]|Float32Array|Int32Array}
 *   sorted by score; `index` is the box's position in the input
 */
const kept = bro.vision.nms(boxes, { iouThreshold: 0.5, perClass: true });

/**
 * bro.vision.rasterizeMask(source, opts?) — resample a mask, or fill a polygon,
 * into a binary 0/255 byte plane.
 * @param {Float32Array|Uint8Array|Array<{x,y}>} source
 *   A Float32Array is thresholded at `opts.threshold` (SAM logits go straight
 *   in); a Uint8Array is treated as nonzero = foreground; an Array of `{x, y}`
 *   points (3 or more) is filled by even-odd ray casting.
 * @param {Object} [opts]
 * @param {number} [opts.width]         source width  (defaults to targetWidth)
 * @param {number} [opts.height]        source height (defaults to targetHeight)
 * @param {number} [opts.targetWidth]   output width  (defaults to width, else 512)
 * @param {number} [opts.targetHeight]  output height (defaults to height, else 512)
 * @param {number} [opts.threshold=0]   Float32Array cutoff
 * @returns {{ width: number, height: number, data: Uint8Array }}  0 or 255
 *   Sides are capped at 16384, and a typed source must hold width*height
 *   values; either violation is a RangeError.
 */
const up = bro.vision.rasterizeMask(best.logits, {
  width: seg.width, height: seg.height,
  targetWidth: canvas.width, targetHeight: canvas.height
});

/**
 * bro.vision.colorMap(data, opts?) — colorize a scalar or class-id plane into
 * an RGBA buffer. `colorizeDepth` and `colorizeSegmentation` are the same
 * function under friendlier names.
 * @param {Float32Array|Uint8Array|Int32Array} data
 *   Float32Array — continuous, auto-ranged unless you pass `min`/`max`.
 *   Uint8Array   — normalized /255, or palette-looked-up with map:'palette'.
 *   Int32Array   — always class ids through the discrete palette.
 * @param {Object} [opts]
 * @param {number} [opts.width=512]   1..16384, else a RangeError
 * @param {number} [opts.height=512]  1..16384, else a RangeError
 * @param {string} [opts.map='turbo']  'turbo' | 'viridis' | 'grayscale' |
 *                                     'palette' (Uint8Array class ids)
 * @param {number} [opts.min]  pin the low end (disables auto-ranging)
 * @param {number} [opts.max]  pin the high end
 * @returns {{ width: number, height: number, data: Uint8ClampedArray }}
 *   `data` is width*height*4 RGBA with alpha 255 — an ImageData `data` buffer,
 *   ready for `new ImageData(...)` / `putImageData`.
 */
const semRgba = bro.vision.colorizeSegmentation(sem.segments,
  { width: sem.width, height: sem.height });
ctx.putImageData(new ImageData(semRgba.data, semRgba.width, semRgba.height), 0, 0);


// ═════════════════════════════════════════════════════════════════════════════
// bro.vision.loadModel — the generic handle
// ═════════════════════════════════════════════════════════════════════════════

/**
 * bro.vision.loadModel(path, opts?) — one loader for every task: `opts.type`
 * picks the network, loads its weights exactly as the specific loader would,
 * and the handle dispatches to it. `predict()` runs the loaded task;
 * `detect()` (hed / lineart / mlsd / openpose / segformer), `segment()` (sam /
 * segformer / birefnet), `depth()` and `pose()` run it when it is one of
 * theirs and throw otherwise. `ocr()` always throws — there is no OCR network.
 * A type it does not know throws "unrecognized task type", and so does the
 * default: pass one.
 * @param {string} path
 * @param {Object} [opts]
 * @param {string} opts.type  'depth' | 'sam' (alias 'segment') | 'normal' |
 *   'hed' (alias 'edge') | 'lineart' | 'mlsd' | 'openpose' (alias 'pose') |
 *   'segformer' | 'birefnet'; read back as `.type`
 * @param {number} [opts.confThreshold=0.25]
 * @param {number} [opts.iouThreshold=0.45]
 * @param {string} [opts.device=best available]
 * @returns {VisionModel}  props: `device`, `type`, `isLoaded`; also `dispose()`
 */
const generic = bro.vision.loadModel('weights/depth-anything-v2-small', { type: 'depth' });
generic.type;       // 'depth'
generic.isLoaded;   // true
generic.depth(photo);   // the DepthEstimator result


// ═════════════════════════════════════════════════════════════════════════════
// Direct constructors
// ═════════════════════════════════════════════════════════════════════════════
//
// Each class is also exported on the namespace for `instanceof` checks and for
// prototype inspection: `bro.vision.DepthEstimator`, `.Sam`,
// `.NormalEstimator`, `.Hed`, `.Lineart`, `.Mlsd`, `.Openpose`, `.Segformer`,
// `.Birefnet`, `.StyleGAN3`, `.Dinov2`, `.Dinov3`, `.VisionModel`.
//
//   sam instanceof bro.vision.Sam;   // true
//
// Construct instances through the loaders, not with `new` — a bare instance
// carries no wrapper and its methods will reject it.
