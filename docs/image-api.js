// =============================================================================
// bro.image API Reference
// =============================================================================
//
// Composable typed-array kernels. Six verbs operate on whole TypedArray
// buffers from C++; JS stays out of the per-pixel loop. Op behavior is a
// small struct (enum + numeric params), never a JS callback.
//
//   reduce    buffer → scalar(s)         (minmax / sum / mean / histogram)
//   map       dst[i] = f(src[i])         (affine / abs / log / sqrt / exp / pow)
//   combine   dst[i] = f(a[i], b[i])     (add / sub / mul / min / max / lerp / wsum)
//   lookup    scalar field → RGBA        (the colormap workhorse)
//   stencil   dst[i] = sum(K * src[N])   (3x3, 5x5, sobel, blur, ...)
//   resample  change size                (nearest / bilinear)
//
// Builders:
//   gradient(stops, n)                   build a 1D RGBA8 LUT for lookup()
//   alloc(w, h, channels, dtype)         allocate a typed buffer
//
// All ops are caller-allocated (into-style): provide dst; nothing is allocated
// inside the kernel. Buffers are reused across frames.
//
// Typical pipeline (the FastNoise2 colormap case):
//
//   const lut = bro.image.gradient([
//       [0.00,  10,  30,  80],
//       [0.45, 230, 220, 150],
//       [0.55, 100, 170,  90],
//       [0.75, 250, 250, 250],
//   ]);
//   const noise = bro.image.alloc(w, h, 1);            // once, reused
//   node.genUniformGrid2DInto(noise, 0, 0, w, h, freq, seed);
//   const {min, max} = bro.image.reduce(noise, 'minmax');
//   bro.image.lookup(imgData.data, noise, lut, {lo: min, hi: max});
//   ctx.putImageData(imgData, 0, 0);
//
// =============================================================================


// -----------------------------------------------------------------------------
// bro.image — namespace
// -----------------------------------------------------------------------------

const image = {

  // --- reduce -------------------------------------------------------------

  /**
   * Collapse a buffer to a scalar (or scalars).
   *
   * @param {TypedArray} src
   * @param {string} op - 'minmax' | 'sum' | 'mean' | 'histogram'
   * @param {object} [params]
   * @param {number} [params.stride=1] - visit every Nth element. Cheap
   *        approximate range/sum for huge buffers driving smoothed estimators
   *        (mean is the mean of the *visited* elements; histogram counts only
   *        the visited elements).
   * @returns {{min:number,max:number} | number | Uint32Array}
   *
   * @example
   *   bro.image.reduce(arr, 'minmax');                         // {min, max}
   *   bro.image.reduce(arr, 'minmax', {stride: 8});            // ~8× cheaper
   *   bro.image.reduce(arr, 'sum');                            // number
   *   bro.image.reduce(arr, 'mean');                           // number
   *   bro.image.reduce(arr, 'histogram', {bins: 256, lo: 0, hi: 1});  // Uint32Array
   */
  reduce(src, op, params) {},

  // --- map ----------------------------------------------------------------

  /**
   * Element-wise unary kernel. dst[i] = f(src[i]).
   *
   * Both buffers must be Float32Array.
   *
   * @param {Float32Array} dst
   * @param {Float32Array} src
   * @param {object} opSpec
   *
   * @example
   *   bro.image.map(dst, src, {op: 'affine', a: 2, b: 1});            // 2x + 1
   *   bro.image.map(dst, src, {op: 'affine', a: 1, b: 0, clamp: [0, 1]});
   *   bro.image.map(dst, src, {op: 'abs'});
   *   bro.image.map(dst, src, {op: 'log'});
   *   bro.image.map(dst, src, {op: 'sqrt'});
   *   bro.image.map(dst, src, {op: 'exp'});
   *   bro.image.map(dst, src, {op: 'pow', exp: 2.2});
   */
  map(dst, src, opSpec) {},

  // --- combine ------------------------------------------------------------

  /**
   * Element-wise binary kernel. dst[i] = f(a[i], b[i]).
   *
   * All three buffers must be Float32Array of the same length.
   *
   * @param {Float32Array} dst
   * @param {Float32Array} a
   * @param {Float32Array} b
   * @param {object} opSpec
   *
   * @example
   *   bro.image.combine(dst, a, b, {op: 'add'});               // a + b
   *   bro.image.combine(dst, a, b, {op: 'sub'});
   *   bro.image.combine(dst, a, b, {op: 'mul'});
   *   bro.image.combine(dst, a, b, {op: 'min'});
   *   bro.image.combine(dst, a, b, {op: 'max'});
   *   bro.image.combine(dst, a, b, {op: 'lerp', t: 0.5});      // (1-t)*a + t*b
   *   bro.image.combine(dst, a, b, {op: 'wsum', wa: 2, wb: 1}); // 2a + b
   */
  combine(dst, a, b, opSpec) {},

  // --- lookup -------------------------------------------------------------

  /**
   * Map each scalar in `src` through a 1D RGBA8 LUT into `dst`.
   *
   * For each src[i]: t = (src[i] - lo) / (hi - lo);
   *                  idx = clamp(floor(t * (lutN - 1)));
   *                  dst[i*4..i*4+3] = lut[idx*4..idx*4+3].
   *
   * dst must be Uint8Array or Uint8ClampedArray sized to 4*N. Typically the
   * `data` of an ImageData created via canvas2d.createImageData(w, h).
   *
   * src may be any scalar TypedArray; Float32Array is the hot path.
   * lut must be a Uint8Array of RGBA8 entries (4*K bytes); usually built
   * via bro.image.gradient().
   *
   * @param {Uint8Array|Uint8ClampedArray} dst
   * @param {TypedArray} src
   * @param {Uint8Array} lut
   * @param {{lo:number, hi:number, edge?:'clamp'|'wrap'}} params
   *
   * @example
   *   bro.image.lookup(imgData.data, noiseField, lut, {lo: 0, hi: 1});
   */
  lookup(dst, src, lut, params) {},

  // --- stencil ------------------------------------------------------------

  /**
   * Convolve `src` with `kernel` into `dst`.
   *
   * dst[y*W + x] = (sum over (kx,ky) of kernel[ky*kw+kx] * src[(y+ky-hkh)*W + (x+kx-hkw)])
   *               / divisor + bias.
   *
   * Kernel w and h must be odd. Buffers are Float32Array.
   *
   * Edge modes: 'clamp' (replicate edge), 'wrap' (toroidal), 'zero' (off-grid is 0).
   *
   * @param {Float32Array} dst
   * @param {Float32Array} src
   * @param {{data:Float32Array, w:number, h:number}} kernel
   * @param {{srcW:number, srcH:number, edge?:string, divisor?:number, bias?:number}} params
   *
   * @example
   *   const sobelX = {data: new Float32Array([-1,0,1,-2,0,2,-1,0,1]), w:3, h:3};
   *   bro.image.stencil(edges, src, sobelX, {srcW:w, srcH:h, edge:'clamp'});
   *
   *   const blur = {data: new Float32Array(9).fill(1), w:3, h:3};
   *   bro.image.stencil(out, src, blur, {srcW:w, srcH:h, edge:'clamp', divisor:9});
   */
  stencil(dst, src, kernel, params) {},

  // --- resample -----------------------------------------------------------

  /**
   * Resize a Float32Array image. Supports multi-channel data (interleaved).
   *
   * @param {Float32Array} dst
   * @param {Float32Array} src
   * @param {{srcW:number, srcH:number, dstW:number, dstH:number,
   *          channels:number, filter:'nearest'|'bilinear'}} params
   *
   * @example
   *   bro.image.resample(hi, lo, {
   *       srcW: 256, srcH: 160, dstW: 1280, dstH: 800,
   *       channels: 1, filter: 'bilinear'
   *   });
   */
  resample(dst, src, params) {},

  // --- gradient (builder) -------------------------------------------------

  /**
   * Build a 1D RGBA8 LUT from color stops, linearly interpolated.
   *
   * Each stop is [t, r, g, b] or [t, r, g, b, a]. t in [0,1]; rgb/a in [0,255].
   * Stops outside the [first.t, last.t] range clamp to the endpoints.
   *
   * @param {Array<Array<number>>} stops
   * @param {number} [n=256] - LUT entry count
   * @returns {Uint8Array} - RGBA8, 4*n bytes
   *
   * @example
   *   const viridisish = bro.image.gradient([
   *       [0.00,  68,  1,  84],
   *       [0.50,  33, 145, 140],
   *       [1.00, 253, 231,  37],
   *   ]);
   *
   *   // Posterize / threshold via LUT shape:
   *   const thresh = bro.image.gradient([
   *       [0.0,   0,   0,   0],
   *       [0.5,   0,   0,   0],
   *       [0.5, 255, 255, 255],
   *       [1.0, 255, 255, 255],
   *   ]);
   */
  gradient(stops, n) {},

  // --- alloc (sugar) ------------------------------------------------------

  /**
   * Allocate a TypedArray sized for w*h*channels.
   *
   * dtype: 'float32' (default) | 'float64' | 'uint8' | 'uint8c' |
   *        'int16' | 'int32' | 'uint16' | 'uint32'.
   *
   * @param {number} w
   * @param {number} h
   * @param {number} channels
   * @param {string} [dtype='float32']
   * @returns {TypedArray}
   *
   * @example
   *   const heightmap = bro.image.alloc(1024, 1024, 1);
   *   const rgba      = bro.image.alloc(w, h, 4, 'uint8c');
   */
  alloc(w, h, channels, dtype) {},
};


// =============================================================================
// bro.image.gpu — WebGL2-backed counterparts (lives in bro, NOT broimage)
// =============================================================================
//
// Ownership boundary: the CPU `bro.image` kernels above are the broimage C++
// library (surfaced via brokit). `bro.image.gpu.*` is a separate, bro-side
// WebGL2 *renderer* — it draws to a canvas via fragment shaders and shares the
// namespace only for ergonomics (CPU `lookup` ↔ GPU `colormap`, both consuming
// a `bro.image.gradient` LUT). It is not part of broimage and does not dispatch
// through brotensor; broimage's own GPU path is CUDA/Metal compute on tensors,
// which is a different thing from rendering to a browser canvas.
//
// V1 surface: colormap.
//
//   bro.image.gpu.colormap(canvas, src, lut, {lo, hi, srcW, srcH})
//   bro.image.gpu.colormap(canvas, src, lut, {autoRange: true, ema, srcW, srcH})
//
// Renders a 1-channel float field through a 1D RGBA8 LUT directly to a
// canvas via a WebGL2 fragment shader. The noise is uploaded as a R32F
// texture; the LUT as a K×1 RGBA8 texture; one fullscreen-triangle draw
// per frame.
//
// The canvas you pass MUST be backed by a webgl2 context. The first call
// creates and caches the program / VAO / textures; subsequent calls only
// reupload data (texSubImage2D) and redraw.
//
// Eliminates the per-frame ImageData allocation + putImageData CPU upload
// that bottleneck the CPU lookup pipeline at large canvas sizes.
//
// autoRange mode: the engine computes (min, max) via a parallel GPU
// reduction over the noise texture, EMA-smooths it across frames in a 1×1
// RG32F ping-pong, and the colormap shader samples that range. The CPU
// never sees the values — no `bro.image.reduce`, no per-frame range
// uniforms. EXT_color_buffer_float must be supported (it is on every
// real-world WebGL2 implementation; the autoRange path throws cleanly if
// not).

const imageGpu = {

  /**
   * Colormap a scalar field to `canvas` via a fragment shader.
   *
   * @param {HTMLCanvasElement} canvas - must support webgl2
   * @param {Float32Array}      src    - srcW * srcH scalar field
   * @param {Uint8Array}        lut    - 4*K bytes (RGBA8); typically built
   *                                     via bro.image.gradient()
   * srcW/srcH default to canvas.width/canvas.height when omitted (the 1:1
   * case); pass them explicitly when the field is rendered at a different
   * resolution than the canvas (e.g. lo-res field → hi-res display).
   *
   * @param {object} params
   * @param {number}  [params.lo]        - explicit low (uniform-range mode)
   * @param {number}  [params.hi]        - explicit high (uniform-range mode)
   * @param {boolean} [params.autoRange] - if true, lo/hi are computed on the
   *                                       GPU via parallel min/max reduction
   *                                       and EMA-smoothed across frames.
   *                                       lo/hi are ignored.
   * @param {number}  [params.ema=0.02]  - blend factor for autoRange smoothing.
   *                                       Higher = more responsive, lower = more
   *                                       stable. 1.0 means "use this frame's
   *                                       raw min/max directly."
   * @param {number}  [params.srcW]      - field width  (default canvas.width)
   * @param {number}  [params.srcH]      - field height (default canvas.height)
   * @param {{x,y,w,h}} [params.viewRect] - sub-rect of the source texture to
   *        display on the canvas. Lets you upload a wider field once and
   *        slide a window across it on subsequent calls. Default: full source.
   * @param {boolean} [params.regenerate=true] - if false, skip the src upload
   *        and reuse the cached noiseTex. `src` may be null. Throws if no
   *        field has been uploaded for this canvas yet.
   *
   * @example
   *   // autoRange: no reduce on CPU, no per-frame range uniforms.
   *   function frame() {
   *       node.genUniformGrid2DInto(data, ox, oy, 1280, 800, freq, seed);
   *       bro.image.gpu.colormap(canvas, data, lut, { autoRange: true });
   *       requestAnimationFrame(frame);
   *   }
   *
   * @example
   *   // Explicit range (the original mode):
   *   const {min, max} = bro.image.reduce(data, 'minmax');
   *   bro.image.gpu.colormap(canvas, data, lut, { lo: min, hi: max });
   */
  colormap(canvas, src, lut, params) {},

  /**
   * Generate a 2D Simplex FBm field on the GPU and colormap it to `canvas`
   * in one call. The scalar field never materializes on the CPU side — it
   * lives only as an intermediate R32F texture between the FBm shader and
   * the colormap shader.
   *
   * Use this instead of `FastNoise.create('Simplex')` +
   * `genUniformGrid2DInto` + `colormap` when you don't need the field on
   * the CPU. At large canvas sizes this eliminates the per-frame
   * CPU→GPU upload (5 MB at 1280×800) and the Float32Array buffer
   * entirely.
   *
   * V1 supports type === 'Simplex' only. SuperSimplex, Perlin, Value,
   * CellularValue and CellularDistance are not implemented in shader form
   * yet — stay on the CPU path for those types.
   *
   * @param {HTMLCanvasElement} canvas - webgl2-backed
   * @param {Uint8Array} lut           - RGBA8 LUT (see gradient())
   * @param {object} params
   * @param {number} params.frequency
   * @param {number} [params.octaves=1]      - 1..16
   * @param {number} [params.gain=0.5]
   * @param {number} [params.lacunarity=2]
   * @param {number} [params.seed=0]
   * @param {number} [params.ox=0]           - world offset x
   * @param {number} [params.oy=0]           - world offset y
   * @param {string} [params.type='Simplex'] - V1: only 'Simplex'
   * @param {boolean} [params.autoRange]     - GPU min/max + EMA (see colormap)
   * @param {number}  [params.ema=0.02]
   * @param {number}  [params.lo] / [params.hi] - uniform-range mode
   * @param {number}  [params.srcW] / [params.srcH]
   * @param {{x,y,w,h}} [params.viewRect] - sub-rect of the noiseTex to display.
   *        Combines naturally with regenerate:false for smooth scrolling
   *        across a pre-rendered tile (see example).
   * @param {boolean} [params.regenerate=true] - if false, skip the FBm gen
   *        pass and just re-display the cached noiseTex via the colormap
   *        pipeline. Throws if no field has been generated yet.
   *
   * @example
   *   // Animating FBm with autoRange — no CPU buffer, no reduce, no upload.
   *   function frame() {
   *       t += dt * scrollSpeed;
   *       bro.image.gpu.fbm2D(canvas, lut, {
   *           frequency: 0.05, octaves: 8, gain: 0.5, lacunarity: 2.0,
   *           seed: 1337, ox: t, oy: 0,
   *           autoRange: true,
   *       });
   *       requestAnimationFrame(frame);
   *   }
   *
   * @example
   *   // 1Hz tile regen with per-frame smooth scrolling. The FBm pass runs
   *   // once per second; intervening frames are pure 1-quad colormaps with
   *   // a translated viewRect.
   *   const cw = canvas.width, ch = canvas.height;
   *   const TILE_W = cw + 256;          // extra horizontal scroll buffer
   *   let tileOx = 0, lastRegen = 0, t = 0;
   *
   *   function frame(now) {
   *       t += dt * scrollSpeed;
   *       const scrollPx = (t - tileOx) / freq;  // pixels into the tile
   *       if (scrollPx > TILE_W - cw || now - lastRegen > 1000) {
   *           tileOx = t;
   *           lastRegen = now;
   *           bro.image.gpu.fbm2D(canvas, lut, {
   *               frequency: freq, octaves, gain, lacunarity, seed,
   *               ox: tileOx, oy: 0,
   *               srcW: TILE_W, srcH: ch,
   *               autoRange: true,
   *               viewRect: { x: 0, y: 0, w: cw, h: ch },
   *           });
   *       } else {
   *           bro.image.gpu.fbm2D(canvas, lut, {
   *               regenerate: false,
   *               autoRange: true,
   *               viewRect: { x: scrollPx, y: 0, w: cw, h: ch },
   *           });
   *       }
   *       requestAnimationFrame(frame);
   *   }
   */
  fbm2D(canvas, lut, params) {},
};


// =============================================================================
// Recipes
// =============================================================================
//
// Colormap a noise field
// ----------------------
//   const lut = bro.image.gradient([[0,0,0,0],[1,255,255,255]]);
//   const {min, max} = bro.image.reduce(field, 'minmax');
//   bro.image.lookup(img.data, field, lut, {lo: min, hi: max});
//   ctx.putImageData(img, 0, 0);
//
// Threshold (LUT does the work; lookup is the same call)
// ------------------------------------------------------
//   const thresh = bro.image.gradient([
//       [0.0,   0,0,0], [0.5,   0,0,0],
//       [0.5, 255,255,255], [1.0, 255,255,255]]);
//   bro.image.lookup(img.data, field, thresh, {lo: 0, hi: 1});
//
// Edge-detected colormap
// ----------------------
//   const sobelX = {data: new Float32Array([-1,0,1,-2,0,2,-1,0,1]), w:3, h:3};
//   bro.image.stencil(edges, src, sobelX, {srcW:w, srcH:h, edge:'clamp'});
//   const {min, max} = bro.image.reduce(edges, 'minmax');
//   bro.image.lookup(img.data, edges, viridis, {lo: min, hi: max});
//
// Histogram equalization
// ----------------------
//   const hist  = bro.image.reduce(src, 'histogram', {bins: 256, lo, hi});
//   const eqLut = buildEqLut(hist);   // small JS routine, one-time per histogram
//   bro.image.lookup(img.data, src, eqLut, {lo, hi});
//
// Render at low res then upsample
// -------------------------------
//   const lo = node.genUniformGrid2D(ox, oy, 256, 160, freq, seed);
//   bro.image.resample(hi, lo, {srcW:256, srcH:160, dstW:1280, dstH:800,
//                                channels:1, filter:'bilinear'});
//   const {min, max} = bro.image.reduce(hi, 'minmax');
//   bro.image.lookup(img.data, hi, lut, {lo: min, hi: max});
//
// =============================================================================
