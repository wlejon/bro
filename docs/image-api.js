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
// bro.image, namespace
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


  // ===========================================================================
  // Decode / probe / EXIF  (broimage decode.h)
  // ===========================================================================
  //
  // Each decode accepts EITHER a path string (resolved against the app dir +
  // engine mounts, like an Image src) OR a Uint8Array/ArrayBuffer of an encoded
  // image (e.g. a fetched response body). Returns
  // {width, height, channels, pixels} where `pixels` is the noted TypedArray.
  // 8-bit RGBA decode is already available via `new Image()`; these cover the
  // high-bit-depth, HDR, and auto-oriented cases.

  /**
   * Decode a 16-bit image (most importantly 16-bit PNGs: depth maps, masks).
   * Channels forced to 4 (RGBA). Returns null on failure.
   * @param {string|Uint8Array|ArrayBuffer} src
   * @returns {{width:number,height:number,channels:number,pixels:Uint16Array}|null}
   * @example const {width, height, pixels} = bro.image.decodeU16('depth16.png');
   */
  decodeU16(src) {},

  /**
   * Decode a float / HDR image (Radiance .hdr/.pic, float PNG). RGBA.
   * Returns null on failure.
   * @param {string|Uint8Array|ArrayBuffer} src
   * @returns {{width:number,height:number,channels:number,pixels:Float32Array}|null}
   */
  decodeF32(src) {},

  /**
   * Decode 8-bit RGBA and apply the EXIF orientation tag (phone photos render
   * sideways without this). Same 1x1-white fallback as Image on hard failure.
   * @param {string|Uint8Array|ArrayBuffer} src
   * @returns {{width:number,height:number,channels:number,pixels:Uint8Array}}
   */
  decodeOriented(src) {},

  /**
   * Cheap header probe: dimensions without decoding pixels.
   * @param {Uint8Array|ArrayBuffer} bytes
   * @returns {{width:number,height:number,channels:number}|null}
   */
  probeDimensions(bytes) {},

  /**
   * Read the EXIF Orientation tag (1..8; 1 = normal / absent). The enum:
   * 1 Normal, 2 FlipH, 3 Rotate180, 4 FlipV, 5 Transpose, 6 Rotate90CW,
   * 7 Transverse, 8 Rotate90CCW.
   * @param {string|Uint8Array|ArrayBuffer} src - path or JPEG bytes
   * @returns {number}
   */
  readExifOrientation(src) {},

  /**
   * Apply an EXIF orientation transform to an RGBA8 buffer. Dimensions swap for
   * the 90/270 transforms, so the result is returned (not written in place).
   * @param {Uint8Array} pixels - RGBA8, width*height*4
   * @param {number} width
   * @param {number} height
   * @param {number} orient - 1..8 (see readExifOrientation)
   * @returns {{width:number,height:number,pixels:Uint8Array}}
   */
  applyExifOrientation(pixels, width, height, orient) {},


  // ===========================================================================
  // Encode  (broimage encode.h)
  // ===========================================================================
  //
  // `pixels` is a Uint8Array of HWC interleaved data. *File variants write the
  // file (path resolved against the app dir) and return a boolean; *memory
  // variants return a Uint8Array of the encoded bytes (or null on failure).

  /** PNG (lossless RGBA). strideBytes defaults to width*channels (tight). */
  encodePngFile(path, pixels, width, height, channels, strideBytes) {},
  /** @returns {Uint8Array|null} */
  encodePng(pixels, width, height, channels, strideBytes) {},
  /** JPEG (lossy). quality in [1,100], default 90. channels 1 (gray) or 3. */
  encodeJpegFile(path, pixels, width, height, channels, quality) {},
  /** @returns {Uint8Array|null} */
  encodeJpeg(pixels, width, height, channels, quality) {},


  // ===========================================================================
  // Geometric  (broimage geometric.h). Caller-allocated dst (into-style)
  // ===========================================================================
  //
  // Resize filters: 'nearest' | 'bilinear' | 'bicubic' | 'lanczos3' | 'area'
  // (area is the correct choice for downscales; lanczos3 for quality upscales).
  // u8 variants take optional srcStride/dstStride (byte distance between rows;
  // 0 = tightly packed) so they can work on a sub-rect of a larger buffer.

  /**
   * Resize HWC uint8 (RGBA8 etc).
   * @param {Uint8Array} dst
   * @param {Uint8Array} src
   * @param {{srcW,srcH,dstW,dstH,channels,filter?,srcStride?,dstStride?}} params
   * @example
   *   bro.image.resizeU8(dst, src, {srcW:512,srcH:512,dstW:224,dstH:224,
   *                                 channels:4, filter:'area'});
   */
  resizeU8(dst, src, params) {},

  /**
   * Resize HWC float32 with the full filter set (the `resample` verb is
   * nearest/bilinear only).
   * @param {Float32Array} dst
   * @param {Float32Array} src
   * @param {{srcW,srcH,dstW,dstH,channels,filter?}} params
   */
  resizeF32(dst, src, params) {},

  /**
   * Resize planar CHW float32 (the layout brolm / Qwen3.5-VL / brodiffusion
   * preprocessors use).
   * @param {Float32Array} dst
   * @param {Float32Array} src
   * @param {{srcW,srcH,dstW,dstH,channels,filter?}} params
   */
  resizeChwF32(dst, src, params) {},

  /**
   * Letterbox HWC uint8: scale-to-fit preserving aspect, center, fill the rest
   * with `pad`. Returns the content rect.
   * @param {Uint8Array} dst
   * @param {Uint8Array} src
   * @param {{srcW,srcH,dstW,dstH,channels,pad?:[r,g,b,a],filter?}} params
   * @returns {{x:number,y:number,w:number,h:number}}
   */
  letterboxU8(dst, src, params) {},

  /**
   * Constant-pad HWC uint8 with the source placed at (offX, offY).
   * @param {{srcW,srcH,dstW,dstH,channels,offX?,offY?,pad?:[r,g,b,a],srcStride?,dstStride?}} params
   */
  padU8(dst, src, params) {},

  /**
   * Crop an [x,y,w,h] rect (out-of-range clamps to the source edge).
   * @param {{srcW,srcH,channels,x,y,w,h,srcStride?,dstStride?}} params
   */
  cropU8(dst, src, params) {},

  /**
   * Center-crop a cropW x cropH rect from the middle of the source.
   * @param {{srcW,srcH,channels,cropW,cropH,srcStride?,dstStride?}} params
   */
  centerCropU8(dst, src, params) {},

  /** Mirror left<->right. @param {{w,h,channels,srcStride?,dstStride?}} params */
  flipHorizontalU8(dst, src, params) {},
  /** Mirror top<->bottom. @param {{w,h,channels,srcStride?,dstStride?}} params */
  flipVerticalU8(dst, src, params) {},
  /**
   * Rotate by 90-degree multiples. `turns` = number of 90-CCW turns (0..3).
   * dst dims swap for odd turns; size dst accordingly.
   * @param {{srcW,srcH,channels,turns,srcStride?,dstStride?}} params
   */
  rotate90U8(dst, src, params) {},


  // ===========================================================================
  // Alpha  (broimage alpha.h), RGBA8, fixes composite fringing
  // ===========================================================================

  /** R,G,B *= a/255. @param {Uint8Array} dst @param {Uint8Array} src */
  premultiplyAlpha(dst, src) {},
  /** Inverse of premultiplyAlpha. */
  unpremultiplyAlpha(dst, src) {},
  /**
   * Alpha-aware RGBA8 resize (premultiply -> resize -> unpremultiply). Use for
   * any RGBA composite whose transparent regions carry arbitrary RGB (decoded
   * PNGs, sprite atlases, glyph alphas).
   * @param {{srcW,srcH,dstW,dstH,filter?}} params
   */
  resizeRgba8Alpha(dst, src, params) {},
  /**
   * Alpha-aware RGBA8 letterbox. `pad` is written verbatim (straight RGBA).
   * @param {{srcW,srcH,dstW,dstH,pad?:[r,g,b,a],filter?}} params
   * @returns {{x:number,y:number,w:number,h:number}}
   */
  letterboxRgba8Alpha(dst, src, params) {},


  // ===========================================================================
  // Color  (broimage color.h)
  // ===========================================================================
  //
  // Channel-convert ops are uint8; pixel count is derived from src length and
  // its channel count. Layout / gamma / sRGB / HSV / HSL / color-matrix ops are
  // float32 and may alias (dst === src allowed).

  /** RGBA8 -> RGB8. */
  rgbaToRgb(dst, src) {},
  /** RGB8 -> RGBA8 (alpha defaults to 255). */
  rgbToRgba(dst, src, alpha) {},
  /** RGBA8 -> gray8 (Rec.601 luma). */
  rgbaToGray(dst, src) {},
  /** RGB8 -> gray8 (Rec.601 luma). */
  rgbToGray(dst, src) {},
  /** Interleaved HWC -> planar CHW float32. @param {{width,height,channels}} params */
  hwcToChw(dst, src, params) {},
  /** Planar CHW -> interleaved HWC float32. @param {{width,height,channels}} params */
  chwToHwc(dst, src, params) {},
  /** dst = src^gamma over the whole float32 buffer. */
  applyGamma(dst, src, gamma) {},
  /** sRGB -> linear (IEC 61966-2-1), float32 buffers. */
  srgbToLinear(dst, src) {},
  /** linear -> sRGB, float32 buffers. */
  linearToSrgb(dst, src) {},
  /** uint8 sRGB -> float32 linear in one pass. @param {Float32Array} dst @param {Uint8Array} src */
  srgbToLinearU8ToF32(dst, src) {},
  /** float32 linear -> uint8 sRGB in one pass. @param {Uint8Array} dst @param {Float32Array} src */
  linearF32ToSrgbU8(dst, src) {},
  /** RGB -> HSV, float32, 3 components per pixel, all in [0,1] (hue normalized). */
  rgbToHsv(dst, src) {},
  /** HSV -> RGB. */
  hsvToRgb(dst, src) {},
  /** RGB -> HSL. */
  rgbToHsl(dst, src) {},
  /** HSL -> RGB. */
  hslToRgb(dst, src) {},
  /**
   * Apply a row-major 3x3 color matrix to each RGB(A) float32 pixel
   * ([R' G' B']^T = M*[R G B]^T; alpha passes through).
   * @param {{channels:3|4, matrix:number[9]}} params
   */
  applyColorMatrix3x3(dst, src, params) {},
  /**
   * Apply a row-major 3x4 color matrix (last column is a bias):
   * [R' G' B']^T = M*[R G B 1]^T.
   * @param {{channels:3|4, matrix:number[12]}} params
   */
  applyColorMatrix3x4(dst, src, params) {},


  // ===========================================================================
  // Preproc  (broimage preproc.h), NHWC <-> NCHW + dtype scale/bias
  // ===========================================================================
  //
  // The layout shuffle every model preprocess does on the way into a Tensor.
  // Typical scalings: [0,255]->[0,1] is scale 1/255 bias 0; [0,255]->[-1,1] is
  // scale 2/255 bias -1.

  /**
   * Packed NHWC uint8 -> planar NCHW float32 with Y = src*scale + bias.
   * @param {Float32Array} dst @param {Uint8Array} src
   * @param {{N?,H,W,C,scale?,bias?}} params
   */
  u8NhwcToF32Nchw(dst, src, params) {},
  /** Float32 NHWC -> NCHW (no dtype change). @param {{N?,H,W,C}} params */
  nhwcToNchwF32(dst, src, params) {},
  /** Float32 NCHW -> NHWC. @param {{N?,C,H,W}} params */
  nchwToNhwcF32(dst, src, params) {},
  /**
   * Planar NCHW float32 -> packed NHWC uint8: clamp(round(src*scale+bias),0,255).
   * Inverse of u8NhwcToF32Nchw. @param {Uint8Array} dst @param {Float32Array} src
   * @param {{N?,C,H,W,scale?,bias?}} params
   */
  f32NchwToU8Nhwc(dst, src, params) {},


  // ===========================================================================
  // Normalize  (broimage normalize.h + presets.h)
  // ===========================================================================

  /**
   * Per-channel (x - mean[c]) / std[c] on NCHW float32 (in place allowed).
   * @param {Float32Array} dst @param {Float32Array} src
   * @param {{N?,C,H,W, mean:number[C], std:number[C]}} params
   * @example
   *   bro.image.normalizeNchw(t, t, {C:3, H:224, W:224,
   *       mean: bro.image.presets.clip.mean, std: bro.image.presets.clip.std});
   */
  normalizeNchw(dst, src, params) {},

  /**
   * Preprocessing constants as {mean:[3], std:[3]} (applied after [0,255]->[0,1]):
   *   presets.clip      CLIP ViT-L/14
   *   presets.imagenet  torchvision default
   *   presets.sam       Segment Anything (= ImageNet stats; SAM also pads to 1024)
   */
  presets: {},


  // ===========================================================================
  // Multi-channel stencil  (broimage kernels.h)
  // ===========================================================================

  /**
   * HWC float32 convolution: the same kernel applied to each channel
   * independently. The multi-channel companion to `stencil` (which is
   * single-channel): what you want for blur/sharpen/edge on an RGB(A) image
   * without deinterleaving first.
   * @param {Float32Array} dst
   * @param {Float32Array} src
   * @param {{data:Float32Array, w:number, h:number}} kernel - w/h odd
   * @param {{srcW,srcH,channels,edge?:'clamp'|'wrap'|'zero',divisor?,bias?}} params
   */
  stencilHwc(dst, src, kernel, params) {},


  // ===========================================================================
  // Tiling  (broimage tiling.h), feather window + weighted accumulate
  // ===========================================================================
  //
  // Split a large image into overlapping tiles, run a local operator per tile,
  // then glue the outputs back seamlessly. Seams are hidden by a feather window
  // (raised-cosine ramp across each overlapped edge). Each tile contributes
  // value*window into `acc` and window into `wacc`; a final divide blends them.

  /**
   * Fill a single-channel feather window for a tw x th tile. Overlap widths
   * (pixels) per edge; an edge with overlap 0 keeps full weight to the boundary
   * (image-border tiles).
   * @param {Float32Array} win - tw*th
   * @param {{tw,th,ovL?,ovR?,ovT?,ovB?}} params
   */
  featherWindow(win, params) {},

  /**
   * Scatter one tile into the full-size accumulators, weighted by `window`.
   * `acc` (full_w*full_h*channels) and `wacc` (full_w*full_h) must be
   * zero-initialized before the first tile and reused across the pass.
   * @param {Float32Array} acc
   * @param {Float32Array} wacc
   * @param {Float32Array} tile   - tw*th*channels (HWC)
   * @param {Float32Array} window - tw*th
   * @param {{fullW,fullH,channels,tw,th,dstX,dstY}} params
   */
  accumulateTile(acc, wacc, tile, window, params) {},

  /**
   * Resolve the accumulators into the final blended map, in place:
   * acc[i] /= max(wacc[i], eps). Uncovered pixels (weight 0) resolve to 0.
   * @param {Float32Array} acc
   * @param {Float32Array} wacc
   * @param {{nPixels,channels,eps?}} params
   */
  normalizeAccumulator(acc, wacc, params) {},
};


// =============================================================================
// bro.image.gpu, WebGL2-backed counterparts (lives in bro, NOT broimage)
// =============================================================================
//
// Ownership boundary: the CPU `bro.image` kernels above are the broimage C++
// library (surfaced via brokit). `bro.image.gpu.*` is a separate, bro-side
// WebGL2 *renderer*, it draws to a canvas via fragment shaders and shares the
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
// never sees the values, no `bro.image.reduce`, no per-frame range
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
   * in one call. The scalar field never materializes on the CPU side, it
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
   * yet: stay on the CPU path for those types.
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
   *   // Animating FBm with autoRange: no CPU buffer, no reduce, no upload.
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
