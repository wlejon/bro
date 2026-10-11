/**
 * bro.vision, Vision-model inference (brovisionml sibling)
 *
 * Image-understanding models that take pixels in and emit masks / maps /
 * boxes: promptable segmentation (SAM), monocular depth (Depth-Anything-V2),
 * surface normals (DSINE), the ControlNet conditioning annotators — soft edges
 * (HED), line drawing (lineart), straight lines (MLSD), body pose (OpenPose),
 * semantic segmentation (SegFormer) — plus dichotomous matting (BiRefNet),
 * open-vocabulary object detection from text queries (OWLv2), the
 * two raw ViT backbones (DINOv2 / DINOv3), and one *generative* model that runs
 * the other way, latent → image: StyleGAN3 (loadStyleGAN3). On top of OWLv2
 * and bro.lm's CLIP sits `bro.vision.wildlife`: which animal is in a photo,
 * its species zero-shot, and the keeper of a burst (section "Wildlife").
 *
 * Backed by brovisionml on top of brotensor + broimage. brovisionml ships code
 * only; you supply the weight directories (HF safetensors checkpoints).
 *
 * `bro.vision.version` — the brovisionml version string.
 *
 * ── Devices ──
 * Every loader takes `opts.device`: 'cuda', 'vulkan' (alias
 * 'vk'), 'metal', 'gpu'
 * (the best GPU present, whichever backend it is), or 'cpu' (case-insensitive).
 * Omitted, it picks brotensor's default device — the GPU (Vulkan, CUDA or Metal)
 * when there is one, else CPU — so a GPU machine gets the GPU without asking.
 * Anything else throws a TypeError, and asking for a backend that is not
 * available (not compiled in, or no device) throws an Error rather than
 * falling back to CPU, which for these models means minutes instead of
 * milliseconds. Gate a big load on `bro.gpu` and read back `model.device` to
 * see what you got.
 *
 *   if (!bro.gpu.available) return;                     // no GPU, don't bother
 *   const depth = bro.vision.loadDepth(dir);            // the GPU when present
 *   console.log(depth.device);                          // 'CUDA' | 'Vulkan' | 'Metal' | 'CPU'
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
 * @param {string} [opts.device=best available]  'cuda' | 'vulkan' | 'gpu' | 'metal' | 'cpu'
 * @returns {Sam}
 */
const sam = bro.vision.loadSam('weights/sam-vit-base', { device: 'gpu' });
sam.device;    // 'CUDA' (or 'Vulkan' / 'Metal')
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
// SAM 2.1 — segment an image, track an object through a sequence of frames
// ═════════════════════════════════════════════════════════════════════════════
//
// Meta's SAM 2.1 (Apache-2.0), all four sizes (tiny / small / base-plus /
// large) from one implementation. One model serves both halves:
//
//   Sam2         the image predictor: setImage() once, segment() per prompt.
//   Sam2Tracker  sam2.tracker(): follow ONE object through frames you add —
//                a burst of photos, a clip's frames — prompt it on any frame,
//                track or propagate it through the rest, click again on a
//                tracked frame to correct it.
//
// Coordinates are input-image pixels. An image is resized to the model's
// 1024x1024 input WITHOUT keeping its aspect ratio (as SAM 2 does), so a crop
// around the subject is the best input. Masks come back at the input's
// resolution. GPU by default (FP16 encoder + memory attention on a GPU whose
// compute dtype is FP16); RTX 4090, base-plus, 640x480 frames: ~24 ms to add
// a frame (the image encode), ~13 ms to track one.
//
// Everything that touches the GPU takes `onDone` (see "Sync vs async"), and a
// tracker shares its model's busy guard: one op at a time across a model and
// every tracker made from it.

/**
 * Load SAM 2.1. `path` is a directory holding the Hugging Face checkpoint
 * (model.safetensors + config.json, e.g. facebook/sam2.1-hiera-base-plus);
 * config.json picks the size.
 * @param {string} path
 * @param {Object} [opts]
 * @param {string} [opts.device=best available]  'cuda' | 'vulkan' | 'gpu' | 'metal' | 'cpu'
 * @returns {Sam2}
 */
const sam2 = bro.vision.loadSam2(bro.models.find('sam2.1-hiera-base-plus'));
sam2.device;    // 'CUDA'
sam2.hasImage;  // false until setImage() has run

/**
 * Sam2.setImage(image, opts?) — encode the image (the expensive half).
 * @param {{width,height,data}|string} image
 * @param {Object} [opts]
 * @param {function} [opts.onDone]  run async: onDone(undefined, info)
 * @returns {undefined|AsyncHandle}
 */
sam2.setImage(crop);

/**
 * Sam2.segment(opts) — decode masks for a prompt against the encoded image.
 * Throws a TypeError without points or a box, an Error before setImage().
 *
 * @param {Object} opts
 * @param {Array} [opts.points]    `[[x, y], ...]` or `[{ x, y, label? }, ...]`
 * @param {number[]} [opts.labels] 1 = foreground, 0 = background (default 1);
 *                                 an explicit array wins over per-point labels
 * @param {number[]} [opts.box]    `[x0, y0, x1, y1]` (one box)
 * @param {boolean} [opts.multimask=true]  three proposals vs one. With one,
 *                                 SAM 2 swaps in its best proposal when the
 *                                 single mask is unstable.
 * @param {Float32Array} [opts.maskInput]  256*256 low-res logits from an
 *                                 earlier result's `lowRes`, to refine it
 * @param {function} [opts.onDone] run async: onDone(result, info)
 * @returns {{ num: number, width: number, height: number, best: number,
 *             objectScore: number,
 *             masks: Array<{ iou: number, data: Uint8Array, logits: Float32Array,
 *                            lowRes: Float32Array, box: number[]|null,
 *                            image: ImageBitmap }> } | AsyncHandle}
 *   `data` is the h*w 0/1 mask, `logits` the h*w FP32 logits behind it,
 *   `lowRes` the 256*256 logits (feed back as maskInput), `box` the mask's
 *   bounds `[x0, y0, x1, y1]` (x1/y1 exclusive; null for an empty mask),
 *   `iou` the predicted IoU, `objectScore` the model's object-present logit
 *   (> 0: there is an object at the prompt).
 */
const s = sam2.segment({ points: [[412, 300]] });
ctx.drawImage(s.masks[s.best].image, 0, 0);
// Box, single mask, then refine it with a background click:
const one = sam2.segment({ box: [300, 180, 560, 420], multimask: false });
sam2.segment({ points: [[420, 300], [330, 410]], labels: [1, 0], multimask: false,
               maskInput: one.masks[0].lowRes });

/**
 * Sam2.tracker() — a new, empty tracker on this model. It keeps the model
 * alive. Frames keep their encoded features (~16 MB of GPU memory each), so
 * re-prompting and re-propagating never re-encode.
 * @returns {Sam2Tracker}
 */
const trk = sam2.tracker();

/**
 * Sam2Tracker.addFrame(image, opts?) — encode the next frame. Frames are
 * numbered in the order added, and that order is the sequence the memory's
 * temporal encodings see: add a burst in capture order.
 * @param {{width,height,data}|string} image   frames may differ in size
 * @param {Object} [opts]
 * @param {function} [opts.onDone]   run async: onDone(index, info)
 * @returns {number|AsyncHandle}     the frame's index
 */
for (const photo of burst) trk.addFrame(photo);
trk.numFrames;   // burst.length

/**
 * Sam2Tracker.prompt(frame, opts) — mark the object on `frame` with clicks
 * and/or a box (as Sam2.segment). On a frame not yet tracked it is a fresh
 * start; on a tracked frame it is a CORRECTION, conditioned on the memory and
 * on that frame's previous mask. Either way the frame becomes a conditioning
 * frame that every later track() remembers. Each call replaces that frame's
 * earlier clicks, so send all of them. RangeError for a frame not added.
 * @param {number} frame
 * @param {Object} opts   { points, labels, box, onDone }
 * @returns {Sam2Frame|AsyncHandle}
 */
const first = trk.prompt(0, { points: [[412, 300]] });

/**
 * Sam2Tracker.track(frame, opts?) — follow the object into `frame` from its
 * memories: the prompted frames, the six frames before it (after it with
 * `reverse`) and their object pointers. You choose the order; a typical
 * forward pass is track(1), track(2), ... A conditioning frame returns its
 * prompted result.
 * @param {number} frame
 * @param {Object} [opts]
 * @param {boolean} [opts.reverse=false]  remember the frames AFTER this one
 * @param {function} [opts.onDone]        run async
 * @returns {Sam2Frame|AsyncHandle}
 */
const next = trk.track(1);

/**
 * Sam2Tracker.propagate(opts?) — track from the earliest prompted frame to
 * the last, then backwards to frame 0 when the earliest prompt is past it,
 * re-tracking every frame that is not a conditioning frame. Run it again
 * after a correction.
 * @param {Object} [opts]
 * @param {function} [opts.onDone]   run async: onDone(frames, info)
 * @returns {Sam2Frame[]|AsyncHandle}  one result per frame, in frame order
 */
trk.propagate({ onDone(frames, info) {
    if (info.error) return;
    for (const f of frames) if (f.present) drawOutline(f.frame, f.box);
} });

/**
 * Sam2Tracker.result(frame, opts?) — the latest result for `frame` without
 * recomputing it (`tracked: false` before any prompt or track reached it).
 * Sam2Tracker.reset() — forget prompts and tracking, keep the frames.
 *
 * @typedef {Object} Sam2Frame
 * @property {number} frame, width, height   the frame's input resolution
 * @property {boolean} tracked       a result exists for this frame
 * @property {boolean} present       objectScore > 0: the object is visible
 *                                   (an occluded or departed object reads
 *                                   false, with an empty mask)
 * @property {number} objectScore    object-present logit
 * @property {number} iou            predicted IoU of the mask
 * @property {boolean} conditioning  a prompted frame
 * @property {number[]|null} box     [x0, y0, x1, y1] of the mask (x1/y1 exclusive)
 * @property {Uint8Array|null} data  h*w 0/1 mask
 * @property {Float32Array|null} logits  h*w logits
 * @property {ImageBitmap|null} image    drawable overlay
 */
trk.result(3).box;
trk.reset();

// Click the bird on the first photo, follow it through the burst, fix the
// one frame it slipped on:
trk.prompt(0, { points: [[x, y]] });
let frames = trk.propagate();
const bad = frames.findIndex(f => !f.present);
if (bad >= 0) {
  trk.prompt(bad, { points: [[bx, by]] });
  frames = trk.propagate();
}


// ═════════════════════════════════════════════════════════════════════════════
// Depth-Anything-V2, monocular depth
// ═════════════════════════════════════════════════════════════════════════════

/**
 * Load a Depth-Anything-V2 checkpoint (read as the V2-Small config). `path` is
 * a directory holding model.safetensors, or the file itself.
 * @param {string} path
 * @param {Object} [opts]
 * @param {string} [opts.device=best available]  'cuda' | 'vulkan' | 'gpu' | 'metal' | 'cpu'
 * @returns {DepthEstimator}
 */
const depth = bro.vision.loadDepth('weights/Depth-Anything-V2-Small',
                                   { device: 'gpu' });
depth.device;   // 'CUDA' (or 'Vulkan' / 'Metal')

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
const normals = bro.vision.loadNormal('weights/dsine', { device: 'gpu' });

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
const hed = bro.vision.loadHed('weights/hed', { device: 'gpu' });
const e = hed.detect(photo);        // === hed.estimate(photo)
ctx.drawImage(e.image, 0, 0);

/**
 * Lineart — a clean line drawing.
 * @returns {{ width, height, line: Float32Array, lines: Uint8Array,
 *             image: ImageBitmap } | AsyncHandle}
 */
const lineart = bro.vision.loadLineart('weights/lineart', { device: 'gpu' });
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
const mlsd = bro.vision.loadMlsd('weights/mlsd', { device: 'gpu' });
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
const openpose = bro.vision.loadOpenpose('weights/openpose', { device: 'gpu' });
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
                                           { device: 'gpu' });
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
  { device: 'gpu', modelSize: 1024 });

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
// OWLv2 — open-vocabulary object detection (text queries -> boxes)
// ═════════════════════════════════════════════════════════════════════════════

/**
 * Load OWLv2 (google/owlv2-base-patch16-ensemble): CLIP ViT-B/16 at 960 px with
 * class / box / objectness heads, and the CLIP text tower for the queries.
 * `dir` is the HF release as-is: model.safetensors, vocab.json, merges.txt
 * (brovisionml-data/owlv2; `bro.models.find('owlv2')`).
 * On a GPU the vision tower runs FP16 (~75 ms an image on a Radeon 8060S); the
 * CPU path is exact FP32 and takes about a minute (a parity reference, not a
 * way to run it).
 * @param {string} dir
 * @param {Object} [opts]
 * @param {string} [opts.device=best available]
 * @returns {Owlv2}   props: `device`, `imageSize` (960)
 */
const owl = bro.vision.loadOwlv2(bro.models.find('owlv2'), { device: 'gpu' });

/**
 * Owlv2.detect(image, opts) — find what the queries name. Synchronous.
 * The image is padded to a square (bottom/right, grey), resized to 960 and
 * every one of the 3600 patches predicts a box; a box's score is the sigmoid
 * of its best query's logit. Kept: score >= threshold, then greedy NMS across
 * queries, best first.
 * @param {{width,height,data|pixels,channels?}|string} image  8-bit RGB or
 *   RGBA rows (ImageData, a bro.image.decodeScaled result) or a file path
 * @param {Object} opts
 * @param {string|string[]} opts.queries  "a bird", "a squirrel", ... — embedded
 *   once per instance and cached by string, so repeating them costs nothing
 * @param {number} [opts.threshold=0.1]    minimum score; OWLv2's scores run
 *   low (0.1 to 0.3 is a typical cut for a real object)
 * @param {number} [opts.nms=0.3]          IoU above which a lower-scored box
 *   is dropped; 0 (or >= 1) keeps them all
 * @param {number} [opts.maxDetections=100]
 * @returns {Array<{ box: number[], score: number, label: number,
 *                   query: string, objectness: number }>}
 *   `box` is [x, y, w, h] in the image's pixels; `label` the index into
 *   `queries`, `query` the string; `objectness` the query-free "is this an
 *   object" probability. An empty array means nothing scored the threshold.
 */
const found = owl.detect(bro.image.decodeScaled(path, { width: 1280, height: 1280 }),
                         { queries: ['a bird', 'a squirrel', 'a deer'], threshold: 0.2 });
if (!found.length) console.log('no animal in frame');
else console.log(found[0].query, found[0].score, found[0].box);

/**
 * Owlv2.embedQueries(queries) — the projected, L2-normalised CLIP text
 * embeddings, (n, 512) row-major, as detect() uses them (and caches them).
 * @param {string|string[]} queries
 * @returns {Float32Array}
 */
const textEmb = owl.embedQueries(['a bird', 'a squirrel']);

/** Owlv2.dispose() — free the weights now; detect() afterwards throws. */
owl.dispose();


// ═════════════════════════════════════════════════════════════════════════════
// Wildlife — what animal is in a photo, and which shot of a burst to keep
// ═════════════════════════════════════════════════════════════════════════════
//
// bro.vision.wildlife is the per-photo pipeline of a camera-trap / feeder
// card, as plain JS in bro (src/bronze_host/js/vision_wildlife.js) over
// loadOwlv2, bro.lm.loadClip and bro.image:
//
//   1. OWLv2 asks for the animals of the lists (`QUERIES`, 28: 'a bird',
//      'a squirrel', ... 'a mink'; 'a snake', 'a turtle', 'a frog', 'a toad',
//      'a lizard', 'a salamander'; 'a dog', 'a cat'; 'an insect',
//      'a dragonfly', 'a butterfly', 'a moth', 'a bee', 'a beetle',
//      'a spider'). No box over the threshold (0.15) is the
//      answer "no animal" (an empty feeder, a branch, the lawn). Clear animals
//      score 0.3 and up; between 0.1 and 0.3 sit both real ones (half hidden,
//      distant) and look-alikes; empty frames score under 0.1.
//   2. Every box after NMS, best first up to `maxClassify` (8), padded 15% a
//      side and made square (subjectSquare), is cropped and classified
//      zero-shot by BioCLIP 2 (imageomics/bioclip-2; or BioCLIP) over a
//      species list: one prompt per species in the BioCLIP recipe ("a photo
//      of <kingdom ... species> with common name <common>."), embedded once
//      and cached on disk; a crop's label is the prompt its embedding sits
//      closest to. `score` is the softmax at the model's temperature (100)
//      over the candidates, `similarity` the cosine. A genus, family, order
//      or class scores the sum of its species' probabilities (taxaScores).
//      The candidates are every species (`candidates: 'all'`, the default),
//      or with 'byQuery' the species of the classes that fit the box's query
//      (queryClasses: 'a bird' Aves, the mammals, 'a dog' and 'a cat'
//      Mammalia, the reptiles Squamata/Testudines/Reptilia, the amphibians
//      Amphibia, the insects Insecta, 'a spider' Arachnida) so a long
//      insect list does not thin a bird's softmax; when the best species
//      there is under labelThreshold and the best over every species is at
//      least that and of another class, that one is the answer.
//      encodeImage takes one image, so crops are encoded one by one.
//   3. Sharpness (bro.image.sharpness) of the subject and of the frame, and
//      groupBursts: shots under 1.0 s apart are a burst, its keeper the frame
//      with the sharpest subject (Tenengrad); per taxon, every animal
//      frame's score (the best of its crops) and their mean (`share`).
//   4. labels(): the names to show, only at 0.85 or more (`labelThreshold`),
//      each at the narrowest rank that clears it (species, else genus,
//      family, order, class; "Darners (Aeshnidae)" via formatLabel), every
//      animal of a photo or burst, with its boxes. A crop that clears no
//      rank is "found, not identified"; the records keep every score, so
//      the threshold can change without re-analysis.
//
// The input image is the DETECTION image: decode the photo to 1280 on the
// long side (bro.image.decodeScaled, a scaled JPEG decode, ~10x cheaper than
// full size). OWLv2 pads to a square and resamples to 960 itself, so 1280 is
// the smallest size that loses nothing there, and it keeps ~1.3x the
// detector's resolution for the subject crop the classifier sees (BioCLIP
// takes 224 px). Results come back in the SOURCE photo's pixels when you
// pass `source` (the full-size width/height), so they line up with the file.
//
// Models, by bro.models name (docs/models-api.js): `owlv2`
// (google/owlv2-base-patch16-ensemble as released), `bioclip-2` / `bioclip`
// (the open_clip releases converted by brolm's scripts/convert-openclip.py:
// model.safetensors, config.json, vocab.json, merges.txt), and the species
// lists (`SPECIES_LISTS`), merged: `species/upper-midwest.json` (birds and
// 15 yard mammals, eBird names), `species/upper-midwest-herps/species.json`
// (78 reptiles and amphibians from GBIF, CC BY 4.0),
// `species/upper-midwest-arthropods/species.json` (insects and spiders) and
// `species/pets/species.json` (dogs and cats). bro.models finds
// directories, so each list is found through its folder; fetching `species`
// brings them all, and each list folder of its own alone adds that list to
// an older species folder. A list is `{ species: [{ common, scientific,
// kingdom?, phylum?, class, order, family }, ...], taxa?: { "<scientific
// name of a genus, family, order or class>": { rank: 'genus' | 'family' |
// 'order' | 'class', common?: 'Darners' } } }` or the bare array. A
// species' genus is the first word of its scientific name; a row without
// a rank adds nothing at that rank. `taxa` only names labels (readTaxa,
// first list wins): records hold scientific names, so a changed common
// name needs no re-analysis and is not in config's hash.
//
// Realms: the same module is in the main realm and every Worker. bro.models
// is main-realm only, so in a Worker every path must be given (open/locate
// throw saying so); find them in the page and post them over.
//
// broworkshop's ai/birds batch runs on this module and its record format
// is RECORDS.md there: a change to a record (any constant here, a field)
// goes together with that file.

/**
 * bro.vision.wildlife.available(opts?) — can open() work: loadOwlv2 and
 * bro.lm.loadClip are compiled in and the files are there. Loads nothing;
 * the probe an app hides the feature on. Takes open()'s path options.
 * @returns {boolean}
 */
if (!bro.vision.wildlife.available()) hideIdentifyButton();

/**
 * bro.vision.wildlife.locate(opts?) — the paths open() would use, and which
 * are missing ('detector' | 'classifier' | 'species'; a model directory
 * without model.safetensors, a list file that is not there, or, when no
 * list is given, a default list whose bro.models folder is not found).
 * `fetch`: the bro.models.fetch names that would install what is missing of
 * the defaults ('owlv2', the model, 'species' when there is no species
 * folder, else each list folder of its own that is missing, e.g.
 * ['species/upper-midwest-arthropods', 'species/pets']);
 * empty when fetching would not help (a folder found without its files).
 * Throws in a Worker unless detector, classifier and species are all given.
 * @returns {{ detector: string|null, classifier: string|null,
 *             species: string|Array<string|null|Object>|null, model: string,
 *             missing: string[], fetch: string[] }}
 *   `species` with no list given: the default list files, null where not found
 */
const where = bro.vision.wildlife.locate({ model: 'bioclip' });
if (where.missing.length) console.log('missing', where.missing.join(', '));
for (const name of where.fetch) await bro.models.fetch(name);

/**
 * bro.vision.wildlife.open(opts?) — load OWLv2 and the classifier and embed
 * the species list. Synchronous: ~0.5 s on a Radeon 8060S with the list's
 * embeddings cached, plus 651 text-encoder passes the first time for the
 * default lists.
 * Throws an Error with `code: 'missing'` and `missing` (locate's list) when
 * a file is not there, or when this build has no loadOwlv2 / loadClip.
 * @param {Object} [opts]
 * @param {string} [opts.detector]    OWLv2 dir; default bro.models.find('owlv2')
 * @param {string} [opts.classifier]  BioCLIP dir; default bro.models.find(model)
 * @param {string|Array<string|Object>} [opts.species]  a list file, or an
 *   array of list files, rows and { species, taxa } documents, merged in
 *   order (a scientific name seen before, ignoring case, is left out);
 *   default SPECIES_LISTS: bro.models.find('species') +
 *   '/upper-midwest.json', then '/species.json' in
 *   species/upper-midwest-herps, species/upper-midwest-arthropods and
 *   species/pets
 * @param {number} [opts.labelThreshold=0.85]  the analyzer's label() /
 *   labels(), and byQuery's fallback threshold
 * @param {number} [opts.maxClassify=8]   boxes classified a photo, best first
 * @param {'all'|'byQuery'} [opts.candidates='all']  what a crop's softmax is
 *   over: every species, or the classes of its box's query (see above)
 * @param {string} [opts.model='bioclip-2']  'bioclip-2' | 'bioclip': which
 *   classifier to find, and its name in `config`
 * @param {number} [opts.threshold=0.15]  OWLv2 score threshold
 * @param {number} [opts.topK=5]          species per photo
 * @param {number} [opts.margin=0.15]     subject square padding, each side
 * @param {string[]} [opts.queries=QUERIES]
 * @param {string} [opts.device]          both models' device (see Devices)
 * @param {string|false} [opts.cacheDir]  where the list's text embeddings are
 *   cached (keyed by model + exact prompts); default
 *   `bro.app.cacheDir + '/wildlife'`, false for none
 * @param {Object} [opts.clipConfig]      loadClip's architecture overrides
 * @param {(done:number, total:number)=>void} [opts.onProgress]  list embedding
 * @returns {WildlifeAnalyzer}
 *   props: `config` (below), `species` (the merged rows), `taxa` (the
 *   merged taxa map), `prompts`, `cached` (the embeddings came from
 *   cacheDir), `dim`, `imageSize` (224), `device`, `paths` ({ detector,
 *   classifier, species: the list files }), `labelThreshold`, and
 *   label(record, burst) / labels(record, burst) at it with its taxa names
 */
const wild = bro.vision.wildlife.open({ model: 'bioclip-2' });

/**
 * WildlifeAnalyzer.config / bro.vision.wildlife.config(opts?) — every setting
 * a result depends on, in a fixed key order, so `JSON.stringify(config)`
 * hashes a run: { detector: { name, repo, revision, weightsBytes },
 * classifier: { ... }, threshold, queries, topK, margin, maxClassify,
 * candidates: { mode: 'all' } | { mode: 'byQuery', fallback },
 * speciesList: { files, count, hash } }. `revision` is read from the model
 * dir's ATTRIBUTION.md; `files` the list files by name ('upper-midwest.json',
 * 'upper-midwest-herps/species.json', ...), `count` the merged rows, `hash` an
 * FNV-1a of every prompt, so an edited list is another config (the taxa
 * maps' names are not in it).
 * labelThreshold is not in it (except as byQuery's fallback): it changes
 * what is shown, not a record.
 * The static form reads files only, never weights: a batch decides what to
 * re-run before it pays for a load.
 */
const runKey = JSON.stringify(wild.config);

/**
 * WildlifeAnalyzer.analyze(image, opts?) — one photo.
 * @param {{width,height,data|pixels,channels?}} image  8-bit RGB or RGBA
 *   rows: the detection image (1280 long side, see above)
 * @param {Object} [opts]
 * @param {{width:number,height:number}} [opts.source]  the photo's full size;
 *   results are scaled to it (default: the image's own size)
 * @param {number} [opts.threshold]  this call only
 * @param {number} [opts.topK]       this call only
 * @param {number} [opts.maxClassify]  this call only
 * @param {'all'|'byQuery'} [opts.candidates]  this call only
 * @returns {{ animal: boolean,
 *             detections: Array<{ label: string, score: number, box: number[] }>,
 *             subject: { x, y, w, h } | null,
 *             species: Array<{ common, scientific, family, order, class,
 *                              score: number, similarity: number }>,
 *             animals: Array<{ box: number[], score: number, label: string,
 *                              subject: { x, y, w, h },
 *                              species: (as `species`),
 *                              taxa: { genus, family, order, class:
 *                                Array<{ scientific, score, family?, order?, class? }> },
 *                              candidates: 'all'|'query'|'fallback' }>,
 *             animalsDropped: number,
 *             timing: { detect: number, classify: number|null } }}
 *   detections best first (NMS 0.3, at most 20), boxes [x, y, w, h] to 0.1 px
 *   and scores to 4 places. `animals`: every non-empty detection, best
 *   first, up to maxClassify, each with its subject square, species top K
 *   and the top 3 genera, families, orders and classes (a taxon's score the
 *   sum over its species, with its lineage above it); `candidates` what its
 *   softmax was over ('query': its query's classes, 'fallback': every
 *   species after a sure answer of another class). `animalsDropped`: the
 *   boxes over the threshold not classified (show "+3 more"). `subject` and
 *   `species` are the first animal's (the best box), as before animals;
 *   both empty when there is no animal. timing in ms (classify: every crop).
 */
const img = bro.image.decodeScaled(path, { width: 1280, height: 1280 });
const rec = wild.analyze(img, { source: { width: img.sourceWidth, height: img.sourceHeight } });
if (rec.animal) console.log(rec.species[0].common, rec.species[0].score);

/**
 * WildlifeAnalyzer.detect(image, opts?) / classify(image, opts?) — the two
 * halves, unrounded, in the image's own pixels. detect: { threshold, queries,
 * nms (0.3), maxDetections (20) } -> [{ box, score, label }]. classify: the
 * species top K ({ topK }) of the image's centre square — crop first
 * (subjectSquare + cropImage) for a small subject.
 */
const boxes = wild.detect(img, { threshold: 0.3 });
const top = wild.classify(bro.vision.wildlife.cropImage(img,
    bro.vision.wildlife.subjectSquare(boxes[0].box, img.width, img.height)));

/** WildlifeAnalyzer.dispose() — free both models; analyze() afterwards throws. */
wild.dispose();

/**
 * bro.vision.wildlife.sharpness(image, opts?) — bro.image.sharpness rounded
 * to 2 places: { laplacianVar, tenengrad, lumaMean, lumaStd, pixels }.
 * Region: `opts.rect` ({ x, y, w, h } in the image's pixels), or `opts.box`
 * ([x, y, w, h], e.g. a detection) in the pixels of `opts.source` (the photo
 * the image was scaled from). Measure on a bigger decode than the detection
 * image (ai/birds uses 3000 px): focus lives in the finest detail.
 */
const big = bro.image.decodeScaled(path, { width: 3000, height: 3000 });
const frameSharp = bro.vision.wildlife.sharpness(big);
const subjectSharp = bro.vision.wildlife.sharpness(big, { box: rec.detections[0].box,
    source: { width: big.sourceWidth, height: big.sourceHeight } });

/**
 * bro.vision.wildlife.groupBursts(records, opts?) — group photos into bursts.
 * A record is { path, time (ms, or null), animal, species (analyze's),
 * sharpness: { frame, subject }, exif?: { dateTaken }, error? }; records with
 * an `error` are left out. Sorted by time, a shot joins the one before when
 * at most `gapS` later.
 * @param {Object} [opts]
 * @param {number} [opts.gapS=1.0]
 * @param {'tenengrad'|'laplacianVar'} [opts.metric='tenengrad']  keeper measure
 * @param {(prev, cur) => ({ncc:number, split:boolean}|null)} [opts.sameScene]
 *   split a burst where the frame changes (e.g. thumbnails aligned with
 *   bro.image.alignSimilarity)
 * @returns {Array<{ id, first, start, durationS, count, animalCount,
 *                   species: { common, scientific, share, agree } | null,
 *                   taxa: Array<{ rank, scientific, common?, genus?, family?,
 *                                 order?, class?, share, scores: number[] }>,
 *                   keeper, keeperSharpness, startedBy: 'gap'|'scene',
 *                   maxGapS, minNcc, photos: string[] }>}
 *   `species`: the score-weighted vote over every animal frame's top K (of
 *   its best box); `share` its mean score, `agree` the frames naming it
 *   first. `taxa`: every species and higher taxon any animal frame scored
 *   (its top K and top 3 a rank, over all its crops), with each frame's
 *   score for it (the best of its crops; best first, frames without it
 *   left out) and `share`, their sum over animalCount; labels() applies the
 *   threshold to them, so it can change after grouping. Records from before
 *   `animals` count their top K as one crop. `keeper`: the sharpest subject
 *   among the animal frames, else the sharpest frame.
 */
const time = bro.vision.wildlife.parseExifTime(bro.image.readExif(path).dateTaken);  // ms | null
const bursts = bro.vision.wildlife.groupBursts(records, { gapS: 1.0 });

/**
 * bro.vision.wildlife.labels(record, burst?, opts?) — every name to show for
 * a photo (or a burst), best first; [] for none. The ONE place the label
 * threshold applies.
 *
 * A crop (an entry of record.animals) is named at the narrowest rank whose
 * best score is at least `labelThreshold`: its top species, else its top
 * genus, family, order, class; none, and it adds nothing (found, not
 * identified). A photo's crops of one taxon are one label (the best score,
 * every box); a label with a narrower label of the answer inside it (by
 * lineage: a species' genus, family, order and class) is left out, its
 * boxes joining that one when it is the only one.
 *
 * A photo in a burst whose labels are not empty shows the BURST's labels
 * (from 'burst'): every taxon in burst.taxa that one frame scores at least
 * the threshold, or whose share is, narrowest kept as above; `score` its
 * best frame's, `share` the consensus, `frames` the frames scoring it at
 * the threshold; each with `boxes` the boxes of THIS photo's crops whose own
 * best guess at that rank is it ([] when the frame does not show it). So an
 * egret in 2 of 3 frames and a mallard in 1 are both on every frame, and a
 * frame that alone reaches only "Ardea" shows the heron its burst is sure
 * of. A burst with no labels, or no burst: the photo's own (from 'photo').
 * `labels(null, burst)` is the burst's own (no boxes).
 *
 * Records from before `animals` are one crop of their top K (genus and up
 * summed over it) on the best box; bursts from before `taxa` label their
 * `species` consensus at its share.
 * @param {Object|null} record  analyze()'s record (or a records.jsonl line)
 * @param {Object|null} [burst] groupBursts' burst holding it
 * @param {{ labelThreshold?: number, taxa?: Object }} [opts]  threshold
 *   default DEFAULTS.labelThreshold (0.85); `taxa` the merged taxa map
 *   (readTaxa / the analyzer's `taxa`) naming genera and up
 * @returns {Array<{ common: string, scientific: string,
 *                   rank: 'species'|'genus'|'family'|'order'|'class',
 *                   score: number, from: 'burst'|'photo',
 *                   boxes: number[][], frames?: number, share?: number }>}
 *   `common`: a species' common name; above species the taxa map's, else
 *   the scientific name. frames and share on burst labels only.
 */
const names = bro.vision.wildlife.labels(rec, bursts.find((b) => b.photos.includes(rec.path)), { taxa: wild.taxa });
tile.tag = names.length ? names.map(bro.vision.wildlife.formatLabel).join(', ')
    : (rec.animal ? 'Unidentified' : '');
for (const n of names) for (const box of n.boxes) drawBox(box, n.common);

/**
 * bro.vision.wildlife.label(record, burst?, opts?) — labels()[0] or null:
 * the one name of a photo, same shape.
 */
const named = bro.vision.wildlife.label(rec, burst);

/**
 * bro.vision.wildlife.formatLabel(label) — the text of a label: a species'
 * common name; above species "Darners (Aeshnidae)", or the scientific name
 * alone when the taxa map has no common name for it.
 */

/**
 * bro.vision.wildlife.classifyCrop(rows, sims, opts?) — one crop's ranking
 * from its cosine similarity to each row's prompt, as analyze() does it
 * (pure; a similarity of log(p) / 100 is a probability p). opts { topK (5),
 * top (3), query, candidates: 'all' | 'byQuery', fallback (0.85) } ->
 * { species: top K (unrounded), taxa: taxaScores, candidates: 'all' |
 * 'query' | 'fallback' }.
 *
 * bro.vision.wildlife.taxaScores(rows, probs, { top }) — per genus, family,
 * order and class the sum of probs over its rows, best first (top 3, or
 * top: 0 for all): { genus: [{ scientific, score, family?, order?, class? }],
 * family, order, class }. queryClasses(query) — byQuery's classes, or null.
 * lineage(row) — { genus, family?, order?, class? }.
 */

// Also: subjectSquare(box, w, h, { margin }) (each side clamped to the frame,
// so a subject bigger than the short side gets a clipped, non-square rect),
// cropImage(image, rect) -> RGBA,
// squareInput(image, size), speciesPrompt(row), readSpeciesList(path),
// readSpecies(lists) (files, rows and documents merged, as open() does),
// readTaxa(lists) (their taxa maps merged, the first of a name kept),
// keeperScore(record, metric), and the constants QUERIES, QUERY_CLASSES,
// RANKS, SPECIES_LISTS ([{ folder, file }]) and DEFAULTS ({ model,
// threshold, topK, margin, nms, maxDetections, gapS, labelThreshold,
// maxClassify, candidates, taxaTop }).


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
  { resolution: 256, variant: 'r', device: 'gpu' });

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
                                 { variant: 'small', device: 'gpu' });

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
  { device: 'gpu' });

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
