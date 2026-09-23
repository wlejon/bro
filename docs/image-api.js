/**
 * =============================================================================
 * bro.image — CPU Image Kernels, Codecs and Preprocessing
 * =============================================================================
 *
 * Plain typed-array in, plain typed-array out. Every kernel here is a C++ call
 * over whole buffers (broimage, surfaced through brokit + broimage's own
 * `_api`); JS never enters the per-pixel loop and an op is described by a small
 * struct — an enum name plus numbers — never by a JS callback.
 *
 * Everything is caller-allocated ("into"-style): you pass `dst`, nothing is
 * allocated inside the kernel, and buffers are meant to be reused across
 * frames. The two exceptions are the builders (`gradient`, `alloc`) and the
 * decoders, which necessarily hand back a fresh buffer.
 *
 * The GPU half of the namespace — `bro.image.gpu.colormap` / `fbm2D`, a
 * WebGL2 *renderer* that draws to a canvas rather than filling a typed array —
 * is documented separately in **docs/image-gpu-api.js**. It lives in bro, not
 * in broimage; it shares the namespace for ergonomics only (CPU `lookup` is
 * the GPU `colormap`, both eating a `bro.image.gradient()` LUT).
 *
 * TWO GENERATIONS OF NAMES. `bro.image` is one object assembled from several
 * installers, and the geometric / layout kernels are reachable under two name
 * sets that differ in their option contract:
 *
 *   explicitly typed   resizeU8 / cropU8 / padU8 / rotate90U8 / normalizeNchw /
 *                      u8NhwcToF32Nchw / nhwcToNchwF32 ...
 *                      uppercase N/C/H/W keys, `pad: [r,g,b,a]` arrays
 *   untyped            resize / crop / pad / rotate90 / normalize / u8ToF32 /
 *                      nhwcToNchw ...
 *                      lowercase n/c/h/w keys, padR/padG/padB/padA scalars
 *
 * Both are live and both are documented below. New code should prefer the
 * typed names: they say which element type they read, they reject a wrong
 * filter instead of silently falling back to bilinear, and they are what the
 * ML preprocessors in the stack call.
 *
 * ERRORS. A kernel trusts its dimensions (it touches w*h*channels elements
 * whatever the buffer holds), so the broimage kernels check every buffer
 * against what the kernel will read or write BEFORE running, and throw
 * instead of reading or writing out of bounds:
 *
 *   RangeError  a buffer too small for its dimensions: "<fn>: <name> is too
 *               small (needs N bytes, has M)". An image spans (h-1) rows of
 *               its stride plus one row of w*channels elements; rotate90's
 *               dst is checked at the rotated size for odd turns.
 *   RangeError  a row stride that is negative or shorter than
 *               width*channels bytes (0 = tightly packed).
 *   RangeError  a span past 2^31-1 bytes ("dimensions are too large"), and
 *               alloc() whose w*h*channels is too large.
 *   RangeError  a positional width / height / channels / count / N,C,H,W
 *               that is not a number in 1..2^31-1 (0, negative, NaN);
 *               encoders also require channels <= 4 and a quality or
 *               strideBytes in 0..2^31-1.
 *   RangeError  an integer option outside int32, or NaN ("<key> is out of
 *               range"). Integers are truncated toward zero.
 *   TypeError   a float kernel given anything but a Float32Array (another
 *               4-byte view such as Int32Array is refused, not reinterpreted);
 *               an encoder's pixels that are not a Uint8Array.
 *   TypeError   an option of the wrong type ("<key> must be a number" /
 *               "must be a string"). A missing or null key takes its default.
 *   TypeError   normalize's mean/std that are not an array or typed array,
 *               have fewer than C entries, or hold a non-number; an encoder's
 *               file path that is not a string; a non-number orient / alpha /
 *               gamma.
 *
 * Covered: the geometric kernels in both name sets (resize, crop, centerCrop,
 * flip, rotate90, pad, letterbox and the alpha-aware RGBA8 resizes), the
 * colour converters (rgbaToRgb ... hslToRgb, srgbToLinear / linearToSrgb,
 * applyGamma, whose `count` sizes both buffers), the layout shuffles
 * (normalize, u8ToF32 / f32ToU8, nhwcToNchw / nchwToNhwc, u8NhwcToF32Nchw /
 * f32NchwToU8Nhwc), applyExifOrientation and the PNG / JPEG encoders.
 *
 * @example
 *   // The colormap pipeline: build a LUT once, reduce, look up, blit.
 *   const lut = bro.image.gradient([
 *     [0.00,  10,  30,  80],
 *     [0.45, 230, 220, 150],
 *     [0.55, 100, 170,  90],
 *     [0.75, 250, 250, 250],
 *   ]);
 *   const field = bro.image.alloc(w, h, 1);          // once, reused
 *   const {min, max} = bro.image.reduce(field, 'minmax');
 *   bro.image.lookup(imgData.data, field, lut, {lo: min, hi: max});
 *   ctx.putImageData(imgData, 0, 0);
 */

// ── Classes: the DOM image element ───────────────────────────────────────────

/**
 * `Image` and `HTMLImageElement` are the same constructor under two names, as
 * on the web. `new Image()` makes a detached `<img>`; assigning `src` decodes
 * synchronously, so `complete` and the size members are true by the time the
 * assignment returns and the `load` event fires on the next turn.
 *
 * Optional width/height constructor arguments are accepted and ignored: they
 * set a layout box, and a detached image has none.
 */
class HTMLImageElement {

  constructor() {}

  /**
   * Image source: a path resolved against the app dir and the engine mounts,
   * a `data:` URL, or a `URL.createObjectURL()` blob URL. Assigning a
   * non-string throws.
   * @type {string}
   */
  src;

  /**
   * Used width. Nothing in this layer scales an image, so `width` and
   * `naturalWidth` are the same number.
   * @readonly
   * @type {number}
   */
  width;

  /** @readonly @type {number} */
  height;

  /** @readonly @type {number} */
  naturalWidth;

  /** @readonly @type {number} */
  naturalHeight;

  /**
   * True once there are pixels. Zero-sized until a `src` decodes.
   * @readonly
   * @type {boolean}
   */
  complete;

  /**
   * Stored and ignored — there is no network here, so there is no origin to
   * be cross. Defaults to `null`; three.js assigns it on every load.
   * @type {string|null}
   */
  crossOrigin;

  /**
   * The promise a loader awaits before touching the pixels. Already settled
   * when returned (the decode happened at `src` assignment), so an `await`
   * continues on the next microtask; rejects with an `EncodingError` when the
   * image did not decode.
   * @returns {Promise<void>}
   */
  decode() {}

  // `onload` / `onerror` / `addEventListener` / `removeEventListener` come
  // from HTMLElement and EventTarget; see brokit-api.js.
}

/** Alias: `Image === HTMLImageElement`. */
class Image extends HTMLImageElement {}

// ── Kernel verbs ─────────────────────────────────────────────────────────────
//
//   reduce     buffer -> scalar(s)        minmax / sum / mean / histogram
//   map        dst[i] = f(src[i])         affine / abs / log / sqrt / exp / pow
//   combine    dst[i] = f(a[i], b[i])     add / sub / mul / min / max / lerp / wsum
//   lookup     scalar field -> RGBA8      the colormap workhorse
//   stencil    dst[i] = sum(K * src[N])   single-channel convolution
//   stencilHwc  ... the same, per channel of an interleaved image
//   resample   change size                nearest / bilinear only
// Builders: gradient() (a LUT), alloc() (a sized typed array).

/**
 * Collapse a buffer to a scalar, a pair, or a histogram.
 *
 * `src` may be any scalar TypedArray; Float32Array is the SIMD hot path and
 * everything else goes through a scalar widening read.
 *
 * @param {ArrayBufferView} src
 * @param {'minmax'|'sum'|'mean'|'histogram'} op
 * @param {object} [params]
 * @param {number} [params.stride=1] visit every Nth element — a cheap
 *   approximate range for huge buffers. `mean` is the mean of the *visited*
 *   elements; `histogram` counts only the visited ones.
 * @param {number} [params.bins=256] histogram only
 * @param {number} [params.lo=0] histogram only; values outside [lo,hi) are dropped
 * @param {number} [params.hi=1] histogram only
 * @returns {{min:number,max:number}|number|Uint32Array}
 *
 * @example
 *   bro.image.reduce(arr, 'minmax');                       // {min, max}
 *   bro.image.reduce(arr, 'minmax', {stride: 8});          // ~8x cheaper
 *   bro.image.reduce(arr, 'sum');                          // number
 *   bro.image.reduce(arr, 'histogram', {bins: 256, lo: 0, hi: 1});  // Uint32Array
 */
bro.image.reduce = function(src, op, params) {};

/**
 * Element-wise unary kernel, `dst[i] = f(src[i])`. Both buffers must be
 * Float32Array; `dst === src` is allowed.
 *
 * @param {Float32Array} dst
 * @param {Float32Array} src
 * @param {{op:string, a?:number, b?:number, clamp?:number[], exp?:number}} opSpec
 *
 * @example
 *   bro.image.map(dst, src, {op: 'affine', a: 2, b: 1});             // 2x + 1
 *   bro.image.map(dst, src, {op: 'affine', a: 1, b: 0, clamp: [0, 1]});
 *   bro.image.map(dst, src, {op: 'abs'});     // also 'log' 'sqrt' 'exp'
 *   bro.image.map(dst, src, {op: 'pow', exp: 2.2});
 */
bro.image.map = function(dst, src, opSpec) {};

/**
 * Element-wise binary kernel, `dst[i] = f(a[i], b[i])`. All three buffers must
 * be Float32Array; `a` and `b` must be the same length.
 *
 * @param {Float32Array} dst
 * @param {Float32Array} a
 * @param {Float32Array} b
 * @param {{op:string, t?:number, wa?:number, wb?:number}} opSpec
 *
 * @example
 *   bro.image.combine(dst, a, b, {op: 'add'});   // also sub/mul/min/max
 *   bro.image.combine(dst, a, b, {op: 'lerp', t: 0.5});       // (1-t)a + t*b
 *   bro.image.combine(dst, a, b, {op: 'wsum', wa: 2, wb: 1}); // 2a + b
 */
bro.image.combine = function(dst, a, b, opSpec) {};

/**
 * Map each scalar in `src` through a 1D RGBA8 LUT into `dst`.
 *
 * For each `src[i]`: `t = (src[i] - lo) / (hi - lo)`,
 * `idx = clamp(floor(t * (lutN - 1)))`, `dst[i*4 .. i*4+3] = lut[idx*4 ...]`.
 *
 * `dst` must be a 1-byte-per-element array (Uint8Array / Uint8ClampedArray)
 * holding at least `4 * src.length` bytes — typically `imageData.data`. `lut`
 * is RGBA8, so `4 * K` bytes with `K >= 2`, usually from `gradient()`.
 *
 * @param {Uint8Array|Uint8ClampedArray} dst
 * @param {ArrayBufferView} src any scalar TypedArray; Float32Array is fastest
 * @param {Uint8Array} lut
 * @param {{lo:number, hi:number, edge?:'clamp'|'wrap'}} params `edge` defaults
 *   to 'clamp'; only the exact string 'wrap' selects the toroidal mode.
 *
 * @example
 *   bro.image.lookup(imgData.data, field, lut, {lo: 0, hi: 1});
 */
bro.image.lookup = function(dst, src, lut, params) {};

/**
 * Single-channel float32 convolution.
 *
 * `dst[y*W+x] = (sum over kernel of K[ky*kw+kx] * src[...]) / divisor + bias`.
 * Kernel `w` and `h` must be odd. `srcW`/`srcH` are required and both buffers
 * must hold `srcW * srcH` floats.
 *
 * @param {Float32Array} dst
 * @param {Float32Array} src
 * @param {{data:Float32Array, w:number, h:number}} kernel
 * @param {{srcW:number, srcH:number, edge?:'clamp'|'wrap'|'zero',
 *          divisor?:number, bias?:number}} params `edge` defaults to 'clamp';
 *   anything other than clamp/wrap/zero throws.
 *
 * @example
 *   const sobelX = {data: new Float32Array([-1,0,1,-2,0,2,-1,0,1]), w: 3, h: 3};
 *   bro.image.stencil(edges, src, sobelX, {srcW: w, srcH: h, edge: 'clamp'});
 *
 *   const blur = {data: new Float32Array(9).fill(1), w: 3, h: 3};
 *   bro.image.stencil(out, src, blur, {srcW: w, srcH: h, divisor: 9});
 */
bro.image.stencil = function(dst, src, kernel, params) {};

/**
 * The multi-channel companion to `stencil`: the same kernel applied to each
 * channel of an *interleaved* HWC float32 image, which is what a blur /
 * sharpen / edge pass on RGB(A) wants without deinterleaving first.
 *
 * @param {Float32Array} dst
 * @param {Float32Array} src
 * @param {{data:Float32Array, w:number, h:number}} kernel w/h odd
 * @param {{srcW:number, srcH:number, channels?:number,
 *          edge?:'clamp'|'wrap'|'zero', divisor?:number, bias?:number}} params
 *   `channels` defaults to 1. Both buffers need `srcW*srcH*channels` floats.
 */
bro.image.stencilHwc = function(dst, src, kernel, params) {};

/**
 * Resize an interleaved float32 image. Nearest and bilinear only — for the
 * full filter set (bicubic, lanczos3, area) use `resizeF32`.
 *
 * @param {Float32Array} dst
 * @param {Float32Array} src
 * @param {{srcW:number, srcH:number, dstW:number, dstH:number,
 *          channels?:number, filter?:'nearest'|'bilinear'}} params
 *   `channels` defaults to 1, `filter` to 'bilinear'; any other filter throws.
 *
 * @example
 *   bro.image.resample(hi, lo, {srcW: 256, srcH: 160, dstW: 1280, dstH: 800,
 *                               channels: 1, filter: 'bilinear'});
 */
bro.image.resample = function(dst, src, params) {};

/**
 * Build a 1D RGBA8 LUT from linearly interpolated color stops.
 *
 * Each stop is `[t, r, g, b]` or `[t, r, g, b, a]`, `t` in [0,1] and the
 * components in [0,255]. A stop with no alpha is fully opaque. Positions
 * outside `[first.t, last.t]` clamp to the endpoints. At least two stops.
 *
 * @param {Array<Array<number>>} stops
 * @param {number} [n=256] LUT entry count, >= 2
 * @returns {Uint8Array} `4 * n` bytes
 *
 * @example
 *   const viridisish = bro.image.gradient([
 *     [0.00,  68,   1,  84],
 *     [0.50,  33, 145, 140],
 *     [1.00, 253, 231,  37],
 *   ]);
 *   // A hard threshold is just a LUT shape — `lookup` does not change:
 *   const thresh = bro.image.gradient([
 *     [0.0, 0, 0, 0], [0.5, 0, 0, 0],
 *     [0.5, 255, 255, 255], [1.0, 255, 255, 255],
 *   ]);
 */
bro.image.gradient = function(stops, n) {};

/**
 * Allocate a TypedArray of `w * h * channels` elements.
 *
 * @param {number} w
 * @param {number} h
 * @param {number} channels
 * @param {'float32'|'float64'|'uint8'|'uint8c'|'int16'|'int32'|'uint16'|'uint32'} [dtype='float32']
 *   'uint8c' is Uint8ClampedArray. An unknown dtype throws.
 * @returns {ArrayBufferView}
 *
 * @example
 *   const heightmap = bro.image.alloc(1024, 1024, 1);
 *   const rgba      = bro.image.alloc(w, h, 4, 'uint8c');
 */
bro.image.alloc = function(w, h, channels, dtype) {};

// ── Decode / probe / EXIF ────────────────────────────────────────────────────
//
// Each decoder takes EITHER a path string (used as given — resolve it first if
// you want app-relative) OR the encoded bytes as a TypedArray, and returns
// `{width, height, channels, pixels}`. Plain 8-bit RGBA decode is already
// covered by `new Image()`; these are the high-bit-depth, HDR and
// auto-oriented cases.

/**
 * Decode a 16-bit image — most importantly 16-bit PNG depth maps and masks,
 * which an 8-bit decode would quantize away.
 * @param {string|ArrayBufferView} src path or encoded bytes
 * @returns {{width:number,height:number,channels:number,pixels:Uint16Array}|null}
 *   null on failure.
 * @example const {width, height, pixels} = bro.image.decodeU16('depth16.png');
 */
bro.image.decodeU16 = function(src) {};

/**
 * Decode a float / HDR image (Radiance `.hdr`/`.pic`, float PNG).
 * @param {string|ArrayBufferView} src
 * @returns {{width:number,height:number,channels:number,pixels:Float32Array}|null}
 */
bro.image.decodeF32 = function(src) {};

/**
 * Decode to 8-bit RGBA with the EXIF orientation already applied, so phone
 * photos come out upright. Shares `decode_file`'s 1x1 fallback, so this one
 * never returns null — check `width`/`height` if you care.
 * @param {string|ArrayBufferView} src
 * @returns {{width:number,height:number,channels:number,pixels:Uint8Array}}
 */
bro.image.decodeOriented = function(src) {};

/**
 * Header-only probe: dimensions without decoding any pixels. Bytes only — no
 * path form.
 * @param {ArrayBufferView} bytes
 * @returns {{width:number,height:number,channels:number}|null}
 */
bro.image.probeDimensions = function(bytes) {};

/**
 * Read the raw EXIF Orientation tag: 1 Normal, 2 FlipH, 3 Rotate180,
 * 4 FlipV, 5 Transpose, 6 Rotate90CW, 7 Transverse, 8 Rotate90CCW; 0 when
 * there is no tag.
 * @param {string|ArrayBufferView} src path or encoded bytes
 * @returns {number}
 */
bro.image.readExifOrientation = function(src) {};

/**
 * Apply an orientation transform to an RGBA8 buffer. The transposing codes
 * swap width and height, which is why this returns a fresh buffer rather than
 * writing in place.
 * @param {Uint8Array} pixels RGBA8, at least `width*height*4` bytes
 * @param {number} width
 * @param {number} height
 * @param {number} orient 1..8, see `readExifOrientation`
 * @returns {{width:number,height:number,channels:number,pixels:Uint8Array}}
 */
bro.image.applyExifOrientation = function(pixels, width, height, orient) {};

// ── Encode / transcode ───────────────────────────────────────────────────────
//
// `pixels` is interleaved HWC. The *File variants write the file (path used as
// given) and return a boolean; the memory variants return the encoded bytes,
// or null on failure.

/**
 * PNG, lossless.
 * @param {string} path
 * @param {ArrayBufferView} pixels
 * @param {number} width
 * @param {number} height
 * @param {number} channels
 * @param {number} [strideBytes=0] row pitch in bytes; 0 = tightly packed
 * @returns {boolean}
 */
bro.image.encodePngFile = function(path, pixels, width, height, channels, strideBytes) {};

/** PNG into memory. @returns {Uint8Array|null} */
bro.image.encodePng = function(pixels, width, height, channels, strideBytes) {};

/**
 * JPEG, lossy. `channels` is 1 (gray) or 3 (RGB).
 * @param {number} [quality=90] 1..100
 * @returns {boolean}
 */
bro.image.encodeJpegFile = function(path, pixels, width, height, channels, quality) {};

/** JPEG into memory. @returns {Uint8Array|null} */
bro.image.encodeJpeg = function(pixels, width, height, channels, quality) {};

/**
 * Transcode a KTX2 / Basis texture to a GPU format or to plain RGBA8, mips
 * included. Only present when broimage was built with KTX2 support; it throws
 * otherwise.
 *
 * @param {ArrayBufferView} bytes the .ktx2 file
 * @param {'rgba8'|'bc1'|'bc3'|'bc4'|'bc5'|'bc7'} [format='rgba8']
 * @returns {{width:number, height:number, hasAlpha:boolean, srgb:boolean,
 *            format:string, mips:Array<{width:number,height:number,data:Uint8Array}>}}
 */
bro.image.transcodeKTX2 = function(bytes, format) {};

// ── Geometric, explicitly typed ──────────────────────────────────────────────
//
// Filters: 'nearest' | 'bilinear' | 'bicubic' | 'lanczos3' | 'area'. Use
// 'area' for downscales (bilinear/bicubic alias badly at large reductions) and
// 'lanczos3' for quality upscales. These names *throw* on an unknown filter.
//
// The u8 ops take optional `srcStride` / `dstStride`: the byte distance between
// successive rows, 0 meaning tightly packed (`width * channels`). Non-zero
// strides let an op work on a sub-rectangle of a bigger buffer — a tile in an
// atlas, a slice of a video frame — without copying it out first.
//
// The resize sampling rule is center-pixel (`(i + 0.5) * src/dst - 0.5`),
// matching PyTorch `align_corners=False`, which is what CLIP / SAM / Qwen-VL
// preprocessors expect.

/**
 * Resize interleaved HWC uint8 (RGBA8 and friends).
 * @param {Uint8Array} dst
 * @param {Uint8Array} src
 * @param {{srcW:number, srcH:number, dstW:number, dstH:number, channels?:number,
 *          filter?:string, srcStride?:number, dstStride?:number}} params
 *   `channels` defaults to 4.
 * @example
 *   bro.image.resizeU8(dst, src, {srcW: 512, srcH: 512, dstW: 224, dstH: 224,
 *                                 channels: 4, filter: 'area'});
 */
bro.image.resizeU8 = function(dst, src, params) {};

/**
 * Resize interleaved HWC float32 with the full filter set (`resample` is
 * nearest/bilinear only). No stride arguments — float resizes are tight.
 * @param {Float32Array} dst
 * @param {Float32Array} src
 * @param {{srcW:number, srcH:number, dstW:number, dstH:number,
 *          channels?:number, filter?:string}} params `channels` defaults to 1.
 */
bro.image.resizeF32 = function(dst, src, params) {};

/**
 * Resize *planar* CHW float32 — the layout brolm / Qwen3.5-VL / brodiffusion
 * preprocessors work in.
 * @param {Float32Array} dst
 * @param {Float32Array} src
 * @param {{srcW:number, srcH:number, dstW:number, dstH:number,
 *          channels?:number, filter?:string}} params
 */
bro.image.resizeChwF32 = function(dst, src, params) {};

/**
 * Letterbox HWC uint8: scale to fit preserving aspect, center, fill the rest
 * with `pad`, and report where the content landed.
 * @param {Uint8Array} dst
 * @param {Uint8Array} src
 * @param {{srcW:number, srcH:number, dstW:number, dstH:number, channels?:number,
 *          pad?:number[], filter?:string}} params `pad` is `[r,g,b,a]` and
 *   defaults to opaque black `[0,0,0,255]`; `channels` defaults to 4.
 * @returns {{x:number,y:number,w:number,h:number}} content rect inside dst
 * @example
 *   const rect = bro.image.letterboxU8(dst, src, {
 *     srcW: iw, srcH: ih, dstW: 640, dstH: 640, channels: 3,
 *     pad: [114, 114, 114, 255], filter: 'area',
 *   });   // rect maps model-space boxes back to image space
 */
bro.image.letterboxU8 = function(dst, src, params) {};

/**
 * Constant-pad HWC uint8 with the source placed at `(offX, offY)`; source area
 * falling outside dst is clipped.
 * @param {Uint8Array} dst
 * @param {Uint8Array} src
 * @param {{srcW:number, srcH:number, dstW:number, dstH:number, channels?:number,
 *          offX?:number, offY?:number, pad?:number[],
 *          srcStride?:number, dstStride?:number}} params `pad` is `[r,g,b,a]`,
 *   default opaque black.
 */
bro.image.padU8 = function(dst, src, params) {};

/**
 * Copy an `[x, y, w, h]` rect out of the source; out-of-range reads clamp to
 * the source edge rather than failing.
 * @param {Uint8Array} dst at least `w*h*channels` bytes
 * @param {Uint8Array} src
 * @param {{srcW:number, srcH:number, channels?:number, x?:number, y?:number,
 *          w:number, h:number, srcStride?:number, dstStride?:number}} params
 */
bro.image.cropU8 = function(dst, src, params) {};

/**
 * Crop a `cropW x cropH` rect from the middle of the source.
 * @param {{srcW:number, srcH:number, channels?:number, cropW:number,
 *          cropH:number, srcStride?:number, dstStride?:number}} params
 */
bro.image.centerCropU8 = function(dst, src, params) {};

/**
 * Mirror left to right.
 * @param {{w:number, h:number, channels?:number, srcStride?:number,
 *          dstStride?:number}} params
 */
bro.image.flipHorizontalU8 = function(dst, src, params) {};

/**
 * Mirror top to bottom.
 * @param {{w:number, h:number, channels?:number, srcStride?:number,
 *          dstStride?:number}} params
 */
bro.image.flipVerticalU8 = function(dst, src, params) {};

/**
 * Rotate by a multiple of 90 degrees. `turns` counts 90-degree CCW turns
 * (0..3, other values reduce mod 4) and the destination dimensions swap on an
 * odd count — size `dst` accordingly.
 * @param {{srcW:number, srcH:number, channels?:number, turns?:number,
 *          srcStride?:number, dstStride?:number}} params `turns` defaults to 1.
 */
bro.image.rotate90U8 = function(dst, src, params) {};

// ── Geometric, untyped names ─────────────────────────────────────────────────
//
// The same broimage kernels under the shorter names, with the same option keys
// except where noted. All of these are byte kernels (uint8 HWC) — the one
// exception is `resize`, which takes the float path when BOTH buffers are
// Float32Array and the byte path otherwise. They also differ from the *U8
// family in that an unrecognized `filter` silently falls back to bilinear
// instead of throwing.

/**
 * Resize. Float32Array dst+src take the float path (strides ignored);
 * anything else is treated as HWC bytes.
 * @param {ArrayBufferView} dst
 * @param {ArrayBufferView} src
 * @param {{srcW:number, srcH:number, dstW:number, dstH:number, channels?:number,
 *          filter?:string, srcStride?:number, dstStride?:number}} params
 *   `channels` defaults to 4.
 */
bro.image.resize = function(dst, src, params) {};

/** Crop an `[x,y,w,h]` rect, uint8. Same keys as `cropU8`. */
bro.image.crop = function(dst, src, params) {};

/** Center-crop, uint8. Same keys as `centerCropU8`. */
bro.image.centerCrop = function(dst, src, params) {};

/** Mirror left to right, uint8. Same keys as `flipHorizontalU8`. */
bro.image.flipHorizontal = function(dst, src, params) {};

/** Mirror top to bottom, uint8. Same keys as `flipVerticalU8`. */
bro.image.flipVertical = function(dst, src, params) {};

/** Rotate 90-degree CCW turns, uint8. Same keys as `rotate90U8`. */
bro.image.rotate90 = function(dst, src, params) {};

/**
 * Constant-pad, uint8. Differs from `padU8`: the fill color is four scalar
 * keys rather than an array.
 * @param {{srcW:number, srcH:number, dstW:number, dstH:number, channels?:number,
 *          offX?:number, offY?:number, padR?:number, padG?:number,
 *          padB?:number, padA?:number, srcStride?:number, dstStride?:number}} params
 *   `padR/G/B` default to 0 and `padA` to 255.
 */
bro.image.pad = function(dst, src, params) {};

// ── Alpha (RGBA8) ────────────────────────────────────────────────────────────
//
// Straight-alpha RGBA sampled directly bleeds the arbitrary RGB of fully
// transparent texels into the visible edge. Premultiply first and these ops do
// that for you.

/**
 * `R,G,B *= a/255`, in RGBA8. Pixel count comes from `src.byteLength / 4`.
 * @param {Uint8Array} dst
 * @param {Uint8Array} src
 */
bro.image.premultiplyAlpha = function(dst, src) {};

/** The inverse of `premultiplyAlpha`. */
bro.image.unpremultiplyAlpha = function(dst, src) {};

/**
 * Alpha-aware RGBA8 resize: premultiply, resize, unpremultiply. Use it for any
 * RGBA composite whose transparent regions carry arbitrary RGB — decoded PNGs,
 * sprite atlases, glyph alphas.
 * @param {{srcW:number, srcH:number, dstW:number, dstH:number, filter?:string}} params
 *   Channels are fixed at 4.
 */
bro.image.resizeRgba8Alpha = function(dst, src, params) {};

/**
 * Alpha-aware RGBA8 letterbox. `pad` is written verbatim as straight RGBA and
 * defaults to fully transparent `[0,0,0,0]`.
 * @param {{srcW:number, srcH:number, dstW:number, dstH:number, pad?:number[],
 *          filter?:string}} params
 * @returns {{x:number,y:number,w:number,h:number}}
 */
bro.image.letterboxRgba8Alpha = function(dst, src, params) {};

// ── Color ────────────────────────────────────────────────────────────────────
//
// The uint8 channel converters take an explicit PIXEL COUNT as a third
// positional argument — it is required, not derived from the buffer length.
// The float ops take an explicit ELEMENT or PIXEL count for the same reason.
// `dst === src` is allowed on the float ops.

/**
 * RGBA8 to RGB8.
 * @param {Uint8Array} dst @param {Uint8Array} src @param {number} pixelCount
 */
bro.image.rgbaToRgb = function(dst, src, pixelCount) {};

/**
 * RGB8 to RGBA8.
 * @param {Uint8Array} dst @param {Uint8Array} src @param {number} pixelCount
 * @param {number} [alpha=255]
 */
bro.image.rgbToRgba = function(dst, src, pixelCount, alpha) {};

/** RGBA8 to gray8, Rec.601 luma. @param {number} pixelCount */
bro.image.rgbaToGray = function(dst, src, pixelCount) {};

/** RGB8 to gray8, Rec.601 luma. @param {number} pixelCount */
bro.image.rgbToGray = function(dst, src, pixelCount) {};

/**
 * sRGB to linear, IEC 61966-2-1 piecewise curve. Dispatches on the buffer
 * types: a Uint8Array src with a Float32Array dst takes the fused
 * byte-to-float path, otherwise both are float32.
 * @param {Float32Array} dst
 * @param {Float32Array|Uint8Array} src
 * @param {number} count element count (not pixels)
 */
bro.image.srgbToLinear = function(dst, src, count) {};

/**
 * Linear to sRGB. A Float32Array src with a Uint8Array dst takes the fused
 * float-to-byte path; otherwise both are float32.
 * @param {Float32Array|Uint8Array} dst
 * @param {Float32Array} src
 * @param {number} count element count
 */
bro.image.linearToSrgb = function(dst, src, count) {};

/**
 * uint8 sRGB to float32 linear in one pass — one float out per input byte.
 * Unlike `srgbToLinear`, the count is derived from the buffers.
 * @param {Float32Array} dst @param {Uint8Array} src
 */
bro.image.srgbToLinearU8ToF32 = function(dst, src) {};

/**
 * float32 linear to uint8 sRGB in one pass. Count derived from the buffers.
 * @param {Uint8Array} dst @param {Float32Array} src
 */
bro.image.linearF32ToSrgbU8 = function(dst, src) {};

/**
 * `dst = src ^ gamma` over a float32 buffer.
 * @param {Float32Array} dst @param {Float32Array} src
 * @param {number} count element count
 * @param {number} gamma
 */
bro.image.applyGamma = function(dst, src, count, gamma) {};

/**
 * RGB to HSV. Three floats per pixel, every component in [0,1] — hue is
 * normalized, multiply by 360 for degrees. `dst` may alias `src`.
 * @param {Float32Array} dst @param {Float32Array} src @param {number} pixelCount
 */
bro.image.rgbToHsv = function(dst, src, pixelCount) {};

/** HSV to RGB. @param {number} pixelCount */
bro.image.hsvToRgb = function(dst, src, pixelCount) {};

/** RGB to HSL. @param {number} pixelCount */
bro.image.rgbToHsl = function(dst, src, pixelCount) {};

/** HSL to RGB. @param {number} pixelCount */
bro.image.hslToRgb = function(dst, src, pixelCount) {};

/**
 * Apply a row-major 3x3 color matrix to each pixel of an interleaved
 * RGB-or-RGBA float32 buffer: `[R' G' B']^T = M * [R G B]^T`. Alpha passes
 * through. Good for color-space conversions, saturation / hue rotation,
 * channel mixers. Pixel count is derived from `src` and `channels`.
 *
 * @param {Float32Array} dst
 * @param {Float32Array} src may alias dst
 * @param {{channels?:3|4, matrix:number[]}} params `channels` defaults to 3 and
 *   must be 3 or 4; `matrix` is 9 numbers and is required.
 *
 * @example
 *   // Desaturate to Rec.709 luma.
 *   const L = [0.2126, 0.7152, 0.0722];
 *   bro.image.applyColorMatrix3x3(out, rgb, {channels: 3, matrix: [...L, ...L, ...L]});
 */
bro.image.applyColorMatrix3x3 = function(dst, src, params) {};

/**
 * The 3x4 form, whose last column is a per-channel bias:
 * `[R' G' B']^T = M * [R G B 1]^T` — matrix-plus-offset pipelines such as
 * white balance or a rescale with a DC shift.
 * @param {{channels?:3|4, matrix:number[]}} params `matrix` is 12 numbers.
 */
bro.image.applyColorMatrix3x4 = function(dst, src, params) {};

// ── Layout: HWC <-> CHW, NHWC <-> NCHW ───────────────────────────────────────
//
// The shuffle every model preprocess does on the way into a Tensor. Typical
// scalings: [0,255] -> [0,1] is scale 1/255 bias 0; [0,255] -> [-1,1] is
// scale 2/255 bias -1. Going back out: [0,1] -> [0,255] is scale 255 bias 0;
// [-1,1] -> [0,255] is scale 127.5 bias 127.5.
//
// NOTE the two generations disagree on defaults. `u8NhwcToF32Nchw` and
// `f32NchwToU8Nhwc` both default `scale` to 1 — the old contract made the
// caller say what it wanted. The untyped `u8ToF32` defaults to 1/255 and
// `f32ToU8` to 255.

/**
 * Interleaved HWC to planar CHW, float32, single image.
 * @param {Float32Array} dst @param {Float32Array} src
 * @param {{width:number, height:number, channels:number}} params all required
 */
bro.image.hwcToChw = function(dst, src, params) {};

/** Planar CHW to interleaved HWC, float32. Same keys as `hwcToChw`. */
bro.image.chwToHwc = function(dst, src, params) {};

/**
 * Packed NHWC uint8 to planar NCHW float32, with `Y = src * scale + bias`.
 * @param {Float32Array} dst at least `N*C*H*W` floats
 * @param {Uint8Array} src
 * @param {{N?:number, H:number, W:number, C:number, scale?:number,
 *          bias?:number}} params uppercase keys; `N` defaults to 1, `scale` to
 *   1 and `bias` to 0.
 * @example
 *   bro.image.u8NhwcToF32Nchw(t, rgb, {H: 224, W: 224, C: 3, scale: 1 / 255});
 */
bro.image.u8NhwcToF32Nchw = function(dst, src, params) {};

/**
 * The inverse: planar NCHW float32 to packed NHWC uint8, with
 * `clamp(round(src*scale + bias), 0, 255)`. Turns a preprocessor or model
 * tensor back into something drawable.
 * @param {Uint8Array} dst at least `N*H*W*C` bytes
 * @param {Float32Array} src
 * @param {{N?:number, C:number, H:number, W:number, scale?:number,
 *          bias?:number}} params `scale` defaults to 1, `bias` to 0.
 */
bro.image.f32NchwToU8Nhwc = function(dst, src, params) {};

/**
 * Float32 NHWC to NCHW, no dtype change.
 * @param {{N?:number, H:number, W:number, C:number}} params uppercase keys
 */
bro.image.nhwcToNchwF32 = function(dst, src, params) {};

/**
 * Float32 NCHW to NHWC.
 * @param {{N?:number, C:number, H:number, W:number}} params
 */
bro.image.nchwToNhwcF32 = function(dst, src, params) {};

/**
 * Untyped spelling of `u8NhwcToF32Nchw`, with lowercase keys and a `scale`
 * that defaults to `1/255`.
 * @param {Float32Array} dst @param {Uint8Array} src
 * @param {{n?:number, h:number, w:number, c?:number, scale?:number,
 *          bias?:number}} params `n` defaults to 1, `c` to 3.
 */
bro.image.u8ToF32 = function(dst, src, params) {};

/**
 * Untyped spelling of `f32NchwToU8Nhwc`; `scale` defaults to `255`.
 * @param {Uint8Array} dst @param {Float32Array} src
 * @param {{n?:number, c?:number, h:number, w:number, scale?:number,
 *          bias?:number}} params
 */
bro.image.f32ToU8 = function(dst, src, params) {};

/**
 * Untyped spelling of `nhwcToNchwF32`, lowercase keys.
 * @param {{n?:number, h:number, w:number, c?:number}} params `c` defaults to 3.
 */
bro.image.nhwcToNchw = function(dst, src, params) {};

/**
 * Untyped spelling of `nchwToNhwcF32`, lowercase keys.
 * @param {{n?:number, c?:number, h:number, w:number}} params
 */
bro.image.nchwToNhwc = function(dst, src, params) {};

// ── Normalize ────────────────────────────────────────────────────────────────

/**
 * Per-channel `(x - mean[c]) / std[c]` over planar NCHW float32. `dst === src`
 * is fine, and `bro.image.presets` supplies the usual triples.
 *
 * @param {Float32Array} dst
 * @param {Float32Array} src
 * @param {{N?:number, C:number, H:number, W:number, mean:number[],
 *          std:number[]}} params `mean` and `std` are required and must each
 *   have `C` entries (a JS array or a Float32Array).
 *
 * @example
 *   bro.image.normalizeNchw(t, t, {C: 3, H: 224, W: 224,
 *     mean: bro.image.presets.clip.mean, std: bro.image.presets.clip.std});
 */
bro.image.normalizeNchw = function(dst, src, params) {};

/**
 * The same kernel with everything positional — the untyped spelling.
 * @param {Float32Array} Y
 * @param {Float32Array} X
 * @param {number[]|Float32Array} mean length C
 * @param {number[]|Float32Array} std length C
 * @param {number} N @param {number} C @param {number} H @param {number} W
 * @example bro.image.normalize(t, t, mean, std, 1, 3, 224, 224);
 */
bro.image.normalize = function(Y, X, mean, std, N, C, H, W) {};

/**
 * Preprocessing constants as `{mean: [3], std: [3]}` plain arrays, in RGB
 * order, meant to be applied *after* the `[0,255] -> [0,1]` scaling:
 *
 *   presets.clip       CLIP ViT-L/14 (openai/clip-vit-large-patch14)
 *   presets.imagenet   torchvision default
 *   presets.sam        Segment Anything — the same ImageNet stats, but the
 *                      upstream model also wants a pad to 1024x1024 afterwards
 *
 * @readonly
 * @type {{clip:{mean:number[],std:number[]},
 *         imagenet:{mean:number[],std:number[]},
 *         sam:{mean:number[],std:number[]}}}
 */
bro.image.presets;

// ── Tiling: feather window + weighted accumulate ─────────────────────────────
//
// Split a large image into overlapping tiles, run a local operator (a model, a
// filter) per tile, then glue the outputs back without seams. The seam is
// hidden by a feather window — a raised-cosine ramp across each overlapped
// edge. Each tile contributes `value * window` into `acc` and `window` into
// `wacc`; a final divide resolves the blend.

/**
 * Fill a single-channel feather window for a `tw x th` tile. Overlaps are in
 * pixels, per edge; an edge with overlap 0 keeps full weight right to the
 * boundary, which is what the tiles on the image border want.
 * @param {Float32Array} win `tw * th` floats
 * @param {{tw:number, th:number, ovL?:number, ovR?:number, ovT?:number,
 *          ovB?:number}} params
 */
bro.image.featherWindow = function(win, params) {};

/**
 * Scatter one tile into the full-size accumulators, weighted by `window`.
 * `acc` (`fullW*fullH*channels`) and `wacc` (`fullW*fullH`) must be zeroed
 * before the first tile and then reused across the whole pass.
 * @param {Float32Array} acc
 * @param {Float32Array} wacc
 * @param {Float32Array} tile `tw*th*channels`, interleaved HWC
 * @param {Float32Array} window `tw*th`
 * @param {{fullW:number, fullH:number, channels?:number, tw:number, th:number,
 *          dstX?:number, dstY?:number}} params
 */
bro.image.accumulateTile = function(acc, wacc, tile, window, params) {};

/**
 * Resolve the accumulators in place: `acc[i] /= max(wacc[i], eps)`. Pixels no
 * tile covered (weight 0) resolve to 0.
 * @param {Float32Array} acc
 * @param {Float32Array} wacc
 * @param {{nPixels:number, channels?:number, eps?:number}} params `eps`
 *   defaults to 1e-6.
 *
 * @example
 *   const win = bro.image.alloc(tw, th, 1);
 *   bro.image.featherWindow(win, {tw, th, ovL: 32, ovR: 32, ovT: 32, ovB: 32});
 *   for (const t of tiles) {
 *     runModelInto(tileOut, t);
 *     bro.image.accumulateTile(acc, wacc, tileOut, win,
 *       {fullW: W, fullH: H, channels: 3, tw, th, dstX: t.x, dstY: t.y});
 *   }
 *   bro.image.normalizeAccumulator(acc, wacc, {nPixels: W * H, channels: 3});
 */
bro.image.normalizeAccumulator = function(acc, wacc, params) {};

// ── Recipes ──────────────────────────────────────────────────────────────────
//
// Colormap a scalar field
// -----------------------
//   const lut = bro.image.gradient([[0,0,0,0],[1,255,255,255]]);
//   const {min, max} = bro.image.reduce(field, 'minmax');
//   bro.image.lookup(img.data, field, lut, {lo: min, hi: max});
//   ctx.putImageData(img, 0, 0);
//   // ...or skip the CPU entirely: see docs/image-gpu-api.js.
//
// Edge-detected colormap
// ----------------------
//   const sobelX = {data: new Float32Array([-1,0,1,-2,0,2,-1,0,1]), w: 3, h: 3};
//   bro.image.stencil(edges, src, sobelX, {srcW: w, srcH: h, edge: 'clamp'});
//   const {min, max} = bro.image.reduce(edges, 'minmax');
//   bro.image.lookup(img.data, edges, viridis, {lo: min, hi: max});
//
// Histogram equalization
// ----------------------
//   const hist  = bro.image.reduce(src, 'histogram', {bins: 256, lo, hi});
//   const eqLut = buildEqLut(hist);   // small JS routine, once per histogram
//   bro.image.lookup(img.data, src, eqLut, {lo, hi});
//
// Render low-res, upsample for display
// ------------------------------------
//   bro.image.resample(hi, lo, {srcW: 256, srcH: 160, dstW: 1280, dstH: 800,
//                               channels: 1, filter: 'bilinear'});
//
// CLIP-style model preprocess, decode to tensor
// ---------------------------------------------
//   const img = bro.image.decodeOriented('photo.jpg');
//   const rgba = bro.image.alloc(224, 224, 4, 'uint8');
//   bro.image.resizeU8(rgba, img.pixels, {srcW: img.width, srcH: img.height,
//     dstW: 224, dstH: 224, channels: 4, filter: 'area'});
//   const rgb = bro.image.alloc(224, 224, 3, 'uint8');
//   bro.image.rgbaToRgb(rgb, rgba, 224 * 224);
//   const t = bro.image.alloc(224 * 224, 3, 1);            // NCHW float32
//   bro.image.u8NhwcToF32Nchw(t, rgb, {H: 224, W: 224, C: 3, scale: 1 / 255});
//   bro.image.normalizeNchw(t, t, {C: 3, H: 224, W: 224,
//     mean: bro.image.presets.clip.mean, std: bro.image.presets.clip.std});
//
// Save a canvas-shaped buffer to disk
// -----------------------------------
//   bro.image.encodePngFile(bro.resolvePath('out.png'), imgData.data, w, h, 4);
