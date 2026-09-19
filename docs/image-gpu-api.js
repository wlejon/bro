/**
 * =============================================================================
 * bro.image.gpu — WebGL2 Colormap and Procedural Noise Renderer
 * =============================================================================
 *
 * The GPU half of the `bro.image` namespace. The CPU half — the typed-array
 * kernels, codecs and preprocessing backed by the broimage C++ library — is
 * documented in **docs/image-api.js**.
 *
 * OWNERSHIP BOUNDARY. These two halves share a namespace for ergonomics, not
 * because they share an implementation. `bro.image.*` is broimage: plain
 * buffers in, plain buffers out. `bro.image.gpu.*` lives entirely in bro
 * (`src/bronze_host/js/image_gpu.js`) and is a *renderer*: it draws to an
 * `HTMLCanvasElement` through WebGL2 fragment shaders and writes nothing back
 * to JS. It is not part of broimage and cannot move there — broimage's only
 * GPU path is brotensor compute on tensors, which is a different thing from
 * painting a canvas. What the two do share is the LUT: a `bro.image.gradient()`
 * Uint8Array feeds CPU `lookup` and GPU `colormap` alike.
 *
 * WHY. The CPU path costs an ImageData allocation and a full-canvas
 * `putImageData` upload every frame — about 4 MB at 1280x800. The GPU path
 * uploads an R32F texture (or generates the field on the GPU and uploads
 * nothing at all) and issues one fullscreen-triangle draw.
 *
 * REQUIREMENTS. The canvas you pass must be able to give a `webgl2` context;
 * the first call takes it and caches the programs, VAO and textures against
 * that canvas in a WeakMap, so later calls only re-upload and redraw. Resizing
 * the source field rebuilds the cached chain. `autoRange` additionally needs
 * `EXT_color_buffer_float` for its float render targets, and throws a clear
 * error when it is missing; the explicit lo/hi mode does not.
 *
 * MAIN REALM ONLY. `bro.image.gpu` is mounted alongside the document's globals
 * and there is no canvas to draw to in a Worker, so a Worker gets the CPU
 * `bro.image` kernels but no `.gpu` member. Generate in the Worker, render on
 * the main thread.
 *
 * @example
 *   const lut = bro.image.gradient([
 *     [0.00,  10,  30,  80],
 *     [0.55, 100, 170,  90],
 *     [1.00, 250, 250, 250],
 *   ]);
 *   bro.image.gpu.fbm2D(canvas, lut, {frequency: 0.05, octaves: 8,
 *                                     autoRange: true});
 */

// ── Ranging ──────────────────────────────────────────────────────────────────
//
// Both entry points colormap a scalar field through the LUT, and both take the
// mapping range one of two ways:
//
//   explicit    {lo, hi}          the range is a pair of uniforms. Compute it
//                                 yourself, e.g. bro.image.reduce(f, 'minmax').
//   autoRange   {autoRange: true} the engine reduces the R32F field to a 1x1
//                                 RG32F "(lo, hi)" texture with a parallel
//                                 min/max chain, EMA-smooths it across frames
//                                 in a 1x1 ping-pong, and the colormap shader
//                                 samples that texture. lo/hi are ignored and
//                                 the CPU never sees the numbers — no readback,
//                                 no stall, no per-frame uniform.
//
// EMA smoothing keeps an animated field from strobing as its extremes wander.
// `ema` is the blend factor toward this frame's raw range: lower is steadier,
// higher is more responsive, 1.0 means "use this frame's min/max as-is". The
// first frame after a resize always uses 1.0, since there is no history.

// ── Shared params ────────────────────────────────────────────────────────────
//
// `srcW` / `srcH` describe the FIELD; they default to `canvas.width` /
// `canvas.height`, the 1:1 case. Pass them when the field is rendered at a
// different resolution than the display, or when it is deliberately wider than
// the canvas so a `viewRect` can slide across it.
//
// `viewRect` is `{x, y, w, h}` in source pixels: the sub-rect of the field to
// stretch over the canvas. Omitted, the whole field is shown. Paired with
// `regenerate: false` this is how you scroll a pre-rendered tile for free — the
// expensive pass runs every N frames, the frames in between are one quad with a
// translated rect.
//
// `regenerate` defaults to true. False skips the upload (`colormap`) or the
// noise pass (`fbm2D`) and reuses the field cached for this canvas; it throws
// if nothing has been cached yet.

/**
 * Colormap a 1-channel float field through a 1D RGBA8 LUT, straight to the
 * canvas.
 *
 * The field is uploaded as an R32F texture, the LUT as a `K x 1` RGBA8
 * texture, and one fullscreen triangle is drawn. Nothing is returned; the
 * canvas is the output.
 *
 * @param {HTMLCanvasElement} canvas must support webgl2
 * @param {Float32Array} src the `srcW * srcH` scalar field. May be null (and
 *   is not read) when `regenerate` is false.
 * @param {Uint8Array|Uint8ClampedArray} lut RGBA8, `4 * K` bytes with K >= 2 —
 *   typically from `bro.image.gradient()`.
 * @param {object} [params]
 * @param {number}  [params.lo] explicit low, when not using autoRange
 * @param {number}  [params.hi] explicit high
 * @param {boolean} [params.autoRange] compute lo/hi on the GPU instead; needs
 *   `EXT_color_buffer_float`
 * @param {number}  [params.ema=0.02] autoRange smoothing factor, 0..1
 * @param {number}  [params.srcW] field width, default `canvas.width`
 * @param {number}  [params.srcH] field height, default `canvas.height`
 * @param {{x:number,y:number,w:number,h:number}} [params.viewRect] sub-rect of
 *   the field to display; default the whole field
 * @param {boolean} [params.regenerate=true] false reuses the cached field
 *
 * @example
 *   // autoRange: no reduce on the CPU, no per-frame range uniforms.
 *   function frame() {
 *     node.genUniformGrid2DInto(data, ox, oy, 1280, 800, freq, seed);
 *     bro.image.gpu.colormap(canvas, data, lut, {autoRange: true});
 *     requestAnimationFrame(frame);
 *   }
 *
 * @example
 *   // Explicit range, when you want it pinned rather than adaptive.
 *   const {min, max} = bro.image.reduce(data, 'minmax');
 *   bro.image.gpu.colormap(canvas, data, lut, {lo: min, hi: max});
 */
bro.image.gpu.colormap = function(canvas, src, lut, params) {};

/**
 * Generate a 2D Simplex FBm field on the GPU and colormap it to the canvas in
 * one call. The field never materializes on the CPU side: it lives only as an
 * intermediate R32F texture between the noise shader and the colormap shader,
 * so there is no Float32Array and no upload at all.
 *
 * Prefer this over `FastNoise.create('Simplex')` + `genUniformGrid2DInto` +
 * `colormap` whenever you do not need the samples on the CPU. Note the octave
 * sum is NOT normalized by the amplitude sum — this matches FastNoise2's
 * FractalFBm shape, and `autoRange` absorbs the difference.
 *
 * Only `type: 'Simplex'` is implemented in shader form. SuperSimplex, Perlin,
 * Value, CellularValue and CellularDistance throw; stay on the CPU
 * `genUniformGrid2DInto` + `bro.image.gpu.colormap` path for those.
 *
 * @param {HTMLCanvasElement} canvas must support webgl2
 * @param {Uint8Array|Uint8ClampedArray} lut RGBA8 LUT, `4 * K` bytes, K >= 2
 * @param {object} params
 * @param {number} params.frequency base frequency
 * @param {number} [params.octaves=1] 1..16, anything else throws
 * @param {number} [params.gain=0.5] per-octave amplitude factor
 * @param {number} [params.lacunarity=2] per-octave frequency factor
 * @param {number} [params.seed=0] offsets the lattice; any float
 * @param {number} [params.ox=0] world offset x, in pixels at frequency 1
 * @param {number} [params.oy=0] world offset y
 * @param {string} [params.type='Simplex'] only 'Simplex'
 * @param {boolean} [params.autoRange] GPU min/max + EMA, as in `colormap`
 * @param {number} [params.ema=0.02]
 * @param {number} [params.lo] explicit range instead of autoRange
 * @param {number} [params.hi]
 * @param {number} [params.srcW] field width, default `canvas.width`
 * @param {number} [params.srcH] field height, default `canvas.height`
 * @param {{x:number,y:number,w:number,h:number}} [params.viewRect]
 * @param {boolean} [params.regenerate=true] false skips the noise pass and
 *   just re-displays the cached field through the colormap pipeline
 *
 * @example
 *   // Animated FBm: no CPU buffer, no reduce, no upload.
 *   function frame() {
 *     t += dt * scrollSpeed;
 *     bro.image.gpu.fbm2D(canvas, lut, {
 *       frequency: 0.05, octaves: 8, gain: 0.5, lacunarity: 2.0,
 *       seed: 1337, ox: t, oy: 0, autoRange: true,
 *     });
 *     requestAnimationFrame(frame);
 *   }
 *
 * @example
 *   // 1 Hz tile regen with smooth per-frame scrolling: the noise pass runs
 *   // once a second over a tile wider than the canvas, and the frames in
 *   // between are pure colormap draws with a translated viewRect.
 *   const cw = canvas.width, ch = canvas.height;
 *   const TILE_W = cw + 256;              // horizontal scroll headroom
 *   let tileOx = 0, lastRegen = 0, t = 0;
 *
 *   function frame(now) {
 *     t += dt * scrollSpeed;
 *     const scrollPx = (t - tileOx) / freq;       // pixels into the tile
 *     if (scrollPx > TILE_W - cw || now - lastRegen > 1000) {
 *       tileOx = t; lastRegen = now;
 *       bro.image.gpu.fbm2D(canvas, lut, {
 *         frequency: freq, octaves, gain, lacunarity, seed,
 *         ox: tileOx, oy: 0, srcW: TILE_W, srcH: ch, autoRange: true,
 *         viewRect: {x: 0, y: 0, w: cw, h: ch},
 *       });
 *     } else {
 *       bro.image.gpu.fbm2D(canvas, lut, {
 *         regenerate: false, autoRange: true,
 *         viewRect: {x: scrollPx, y: 0, w: cw, h: ch},
 *       });
 *     }
 *     requestAnimationFrame(frame);
 *   }
 */
bro.image.gpu.fbm2D = function(canvas, lut, params) {};
