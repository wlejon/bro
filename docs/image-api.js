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
//   const noise = node.genUniformGrid2D(0, 0, w, h, freq, seed);
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
   * @returns {{min:number,max:number} | number | Uint32Array}
   *
   * @example
   *   bro.image.reduce(arr, 'minmax');         // {min, max}
   *   bro.image.reduce(arr, 'sum');             // number
   *   bro.image.reduce(arr, 'mean');            // number
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
// bro.image.gpu — WebGL2-backed counterparts (lives in bro, not brokit)
// =============================================================================
//
// V1 surface: colormap.
//
//   bro.image.gpu.colormap(canvas, src, lut, {lo, hi, srcW, srcH})
//
// Renders a 1-channel float field through a 1D RGBA8 LUT directly to a
// canvas via a WebGL2 fragment shader. The noise is uploaded as a R32F
// texture; the LUT as a 256x1 RGBA8 texture; one fullscreen-triangle
// draw per frame.
//
// The canvas you pass MUST be backed by a webgl2 context. The first call
// creates and caches the program / VAO / textures; subsequent calls only
// reupload data (texSubImage2D) and redraw.
//
// Eliminates the per-frame ImageData allocation + putImageData CPU upload
// that bottleneck the CPU lookup pipeline at large canvas sizes.

const imageGpu = {

  /**
   * Colormap a scalar field to `canvas` via a fragment shader.
   *
   * @param {HTMLCanvasElement} canvas - must support webgl2
   * @param {Float32Array}      src    - srcW * srcH scalar field
   * @param {Uint8Array}        lut    - 4*K bytes (RGBA8); typically built
   *                                     via bro.image.gradient()
   * @param {{lo:number, hi:number, srcW:number, srcH:number}} params
   *
   * @example
   *   const canvas = document.createElement('canvas');
   *   document.body.appendChild(canvas);
   *   canvas.width = 1280; canvas.height = 800;
   *
   *   const lut = bro.image.gradient([
   *       [0.00,  10,  30,  80],
   *       [0.45, 230, 220, 150],
   *       [0.55, 100, 170,  90],
   *       [0.75, 250, 250, 250],
   *   ]);
   *
   *   function frame() {
   *       const data = node.genUniformGrid2D(ox, oy, 1280, 800, freq, seed);
   *       const {min, max} = bro.image.reduce(data, 'minmax');
   *       bro.image.gpu.colormap(canvas, data, lut, {
   *           lo: min, hi: max, srcW: 1280, srcH: 800
   *       });
   *       requestAnimationFrame(frame);
   *   }
   */
  colormap(canvas, src, lut, params) {},
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
