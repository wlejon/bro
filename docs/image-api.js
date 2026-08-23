/**
 * =============================================================================
 * bro.image / Image / bro.image.gpu — Image Processing and CPU/GPU Kernels
 * =============================================================================
 *
 * Composable typed-array kernels, CPU/GPU image decoding, transformation, and
 * WebGL2-backed canvas rendering.
 *
 * @example
 *   // FastNoise2 colormap pipeline
 *   const lut = bro.image.gradient([
 *     [0.00,  10,  30,  80],
 *     [0.45, 230, 220, 150],
 *     [0.55, 100, 170,  90],
 *     [0.75, 250, 250, 250],
 *   ]);
 *   const noise = bro.image.alloc(w, h, 1);
 *   const {min, max} = bro.image.reduce(noise, 'minmax');
 *   bro.image.lookup(imgData.data, noise, lut, {lo: min, hi: max});
 *   ctx.putImageData(imgData, 0, 0);
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * Standard HTML Image / DOM Image helper constructor.
 */
class Image {

  constructor() {}

  /**
   *  Image source URL or path.
   * @type {string}
   */
  src;

  /**
   *  Intrinsic image width in pixels.
   * @readonly
   * @type {number}
   */
  width;

  /**
   *  Intrinsic image height in pixels.
   * @readonly
   * @type {number}
   */
  height;

  /**
   *  Natural width of image.
   * @readonly
   * @type {number}
   */
  naturalWidth;

  /**
   *  Natural height of image.
   * @readonly
   * @type {number}
   */
  naturalHeight;

  /**
   *  True if image has finished loading.
   * @readonly
   * @type {boolean}
   */
  complete;

  /**
   *  Callback invoked on successful load.
   * @type {Function|null}
   */
  onload;

  /**
   *  Callback invoked on load error.
   * @type {Function|null}
   */
  onerror;

  /**
   *  Adds an event listener for image events.
   *
   * @param {string} type
   * @param {Function} listener
   */
  addEventListener(type, listener) {}

  /**
   *  Removes an event listener for image events.
   *
   * @param {string} type
   * @param {Function} listener
   */
  removeEventListener(type, listener) {}

}

/**
 * Alias for HTMLImageElement DOM interface.
 */
class HTMLImageElement extends Image {

}

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * CPU and GPU image manipulation and kernel acceleration suite.
 */
/**
 *  Preprocessing normalization presets (clip, imagenet, sam).
 * @readonly
 * @type {Object}
 */
bro.image.presets;

/**
 *  Collapse a buffer to a scalar or histogram.
 *
 * @param {ArrayBufferView} src
 * @param {string} op
 * @param {Object} [params]
 * @returns {*}
 */
bro.image.reduce = function(src, op, params) {};

/**
 *  Element-wise unary kernel. dst[i] = f(src[i]).
 *
 * @param {Float32Array} dst
 * @param {Float32Array} src
 * @param {Object} opSpec
 */
bro.image.map = function(dst, src, opSpec) {};

/**
 *  Element-wise binary kernel. dst[i] = f(a[i], b[i]).
 *
 * @param {Float32Array} dst
 * @param {Float32Array} a
 * @param {Float32Array} b
 * @param {Object} opSpec
 */
bro.image.combine = function(dst, a, b, opSpec) {};

/**
 *  Map each scalar in src through a 1D RGBA8 LUT into dst.
 *
 * @param {ArrayBufferView} dst
 * @param {ArrayBufferView} src
 * @param {Uint8Array} lut
 * @param {Object} params
 */
bro.image.lookup = function(dst, src, lut, params) {};

/**
 *  Convolve src with kernel into dst.
 *
 * @param {Float32Array} dst
 * @param {Float32Array} src
 * @param {Object} kernel
 * @param {Object} params
 */
bro.image.stencil = function(dst, src, kernel, params) {};

/**
 *  Resize a Float32Array image.
 *
 * @param {Float32Array} dst
 * @param {Float32Array} src
 * @param {Object} params
 */
bro.image.resample = function(dst, src, params) {};

/**
 *  Build a 1D RGBA8 LUT from color stops.
 *
 * @param {Array<Array<number>>} stops
 * @param {number} [n]
 * @returns {Uint8Array}
 */
bro.image.gradient = function(stops, n) {};

/**
 *  Allocate a TypedArray sized for w*h*channels.
 *
 * @param {number} w
 * @param {number} h
 * @param {number} channels
 * @param {string} [dtype]
 * @returns {ArrayBufferView}
 */
bro.image.alloc = function(w, h, channels, dtype) {};

/**
 *  Decode a 16-bit image into RGBA.
 *
 * @param {*} src
 * @returns {Object|null}
 */
bro.image.decodeU16 = function(src) {};

/**
 *  Decode a float / HDR image.
 *
 * @param {*} src
 * @returns {Object|null}
 */
bro.image.decodeF32 = function(src) {};

/**
 *  Decode 8-bit RGBA and apply EXIF orientation.
 *
 * @param {*} src
 * @returns {Object}
 */
bro.image.decodeOriented = function(src) {};

/**
 *  Cheap header probe for dimensions.
 *
 * @param {ArrayBufferView} bytes
 * @returns {Object|null}
 */
bro.image.probeDimensions = function(bytes) {};

/**
 *  Transcode a KTX2 texture.
 *
 * @param {ArrayBufferView} bytes
 * @param {string} [format]
 * @returns {Object}
 */
bro.image.transcodeKTX2 = function(bytes, format) {};

/**
 *  Read EXIF orientation tag.
 *
 * @param {*} src
 * @returns {number}
 */
bro.image.readExifOrientation = function(src) {};

/**
 *  Apply EXIF orientation transform to an RGBA8 buffer.
 *
 * @param {Uint8Array} pixels
 * @param {number} width
 * @param {number} height
 * @param {number} orient
 * @returns {Object}
 */
bro.image.applyExifOrientation = function(pixels, width, height, orient) {};

/**
 *  Encode PNG to file.
 *
 * @param {string} path
 * @param {Uint8Array} pixels
 * @param {number} width
 * @param {number} height
 * @param {number} channels
 * @param {number} [strideBytes]
 * @returns {boolean}
 */
bro.image.encodePngFile = function(path, pixels, width, height, channels, strideBytes) {};

/**
 *  Encode PNG to memory.
 *
 * @param {Uint8Array} pixels
 * @param {number} width
 * @param {number} height
 * @param {number} channels
 * @param {number} [strideBytes]
 * @returns {Uint8Array|null}
 */
bro.image.encodePng = function(pixels, width, height, channels, strideBytes) {};

/**
 *  Encode JPEG to file.
 *
 * @param {string} path
 * @param {Uint8Array} pixels
 * @param {number} width
 * @param {number} height
 * @param {number} channels
 * @param {number} [quality]
 * @returns {boolean}
 */
bro.image.encodeJpegFile = function(path, pixels, width, height, channels, quality) {};

/**
 *  Encode JPEG to memory.
 *
 * @param {Uint8Array} pixels
 * @param {number} width
 * @param {number} height
 * @param {number} channels
 * @param {number} [quality]
 * @returns {Uint8Array|null}
 */
bro.image.encodeJpeg = function(pixels, width, height, channels, quality) {};

/**
 *  Resize HWC uint8.
 *
 * @param {Uint8Array} dst
 * @param {Uint8Array} src
 * @param {Object} params
 */
bro.image.resizeU8 = function(dst, src, params) {};

/**
 *  Resize HWC float32.
 *
 * @param {Float32Array} dst
 * @param {Float32Array} src
 * @param {Object} params
 */
bro.image.resizeF32 = function(dst, src, params) {};

/**
 *  Resize planar CHW float32.
 *
 * @param {Float32Array} dst
 * @param {Float32Array} src
 * @param {Object} params
 */
bro.image.resizeChwF32 = function(dst, src, params) {};

/**
 *  Letterbox HWC uint8.
 *
 * @param {Uint8Array} dst
 * @param {Uint8Array} src
 * @param {Object} params
 * @returns {Object}
 */
bro.image.letterboxU8 = function(dst, src, params) {};

/**
 *  Constant-pad HWC uint8.
 *
 * @param {Uint8Array} dst
 * @param {Uint8Array} src
 * @param {Object} params
 */
bro.image.padU8 = function(dst, src, params) {};

/**
 *  Crop an [x,y,w,h] rect.
 *
 * @param {Uint8Array} dst
 * @param {Uint8Array} src
 * @param {Object} params
 */
bro.image.cropU8 = function(dst, src, params) {};

/**
 *  Center-crop a cropW x cropH rect.
 *
 * @param {Uint8Array} dst
 * @param {Uint8Array} src
 * @param {Object} params
 */
bro.image.centerCropU8 = function(dst, src, params) {};

/**
 *  Mirror left to right.
 *
 * @param {Uint8Array} dst
 * @param {Uint8Array} src
 * @param {Object} params
 */
bro.image.flipHorizontalU8 = function(dst, src, params) {};

/**
 *  Mirror top to bottom.
 *
 * @param {Uint8Array} dst
 * @param {Uint8Array} src
 * @param {Object} params
 */
bro.image.flipVerticalU8 = function(dst, src, params) {};

/**
 *  Rotate by 90-degree multiples.
 *
 * @param {Uint8Array} dst
 * @param {Uint8Array} src
 * @param {Object} params
 */
bro.image.rotate90U8 = function(dst, src, params) {};

/**
 *  Premultiply alpha.
 *
 * @param {Uint8Array} dst
 * @param {Uint8Array} src
 */
bro.image.premultiplyAlpha = function(dst, src) {};

/**
 *  Unpremultiply alpha.
 *
 * @param {Uint8Array} dst
 * @param {Uint8Array} src
 */
bro.image.unpremultiplyAlpha = function(dst, src) {};

/**
 *  Alpha-aware RGBA8 resize.
 *
 * @param {Uint8Array} dst
 * @param {Uint8Array} src
 * @param {Object} params
 */
bro.image.resizeRgba8Alpha = function(dst, src, params) {};

/**
 *  Alpha-aware RGBA8 letterbox.
 *
 * @param {Uint8Array} dst
 * @param {Uint8Array} src
 * @param {Object} params
 * @returns {Object}
 */
bro.image.letterboxRgba8Alpha = function(dst, src, params) {};

/**
 *  RGBA8 to RGB8.
 *
 * @param {Uint8Array} dst
 * @param {Uint8Array} src
 */
bro.image.rgbaToRgb = function(dst, src) {};

/**
 *  RGB8 to RGBA8.
 *
 * @param {Uint8Array} dst
 * @param {Uint8Array} src
 * @param {number} [alpha]
 */
bro.image.rgbToRgba = function(dst, src, alpha) {};

/**
 *  RGBA8 to gray8.
 *
 * @param {Uint8Array} dst
 * @param {Uint8Array} src
 */
bro.image.rgbaToGray = function(dst, src) {};

/**
 *  RGB8 to gray8.
 *
 * @param {Uint8Array} dst
 * @param {Uint8Array} src
 */
bro.image.rgbToGray = function(dst, src) {};

/**
 *  Interleaved HWC to planar CHW float32.
 *
 * @param {Float32Array} dst
 * @param {Float32Array} src
 * @param {Object} params
 */
bro.image.hwcToChw = function(dst, src, params) {};

/**
 *  Planar CHW to interleaved HWC float32.
 *
 * @param {Float32Array} dst
 * @param {Float32Array} src
 * @param {Object} params
 */
bro.image.chwToHwc = function(dst, src, params) {};

/**
 *  Apply gamma curve to float32 buffer.
 *
 * @param {Float32Array} dst
 * @param {Float32Array} src
 * @param {number} gamma
 */
bro.image.applyGamma = function(dst, src, gamma) {};

/**
 *  sRGB to linear conversion.
 *
 * @param {Float32Array} dst
 * @param {Float32Array} src
 */
bro.image.srgbToLinear = function(dst, src) {};

/**
 *  Linear to sRGB conversion.
 *
 * @param {Float32Array} dst
 * @param {Float32Array} src
 */
bro.image.linearToSrgb = function(dst, src) {};

/**
 *  uint8 sRGB to float32 linear in one pass.
 *
 * @param {Float32Array} dst
 * @param {Uint8Array} src
 */
bro.image.srgbToLinearU8ToF32 = function(dst, src) {};

/**
 *  float32 linear to uint8 sRGB in one pass.
 *
 * @param {Uint8Array} dst
 * @param {Float32Array} src
 */
bro.image.linearF32ToSrgbU8 = function(dst, src) {};

/**
 *  RGB to HSV conversion.
 *
 * @param {Float32Array} dst
 * @param {Float32Array} src
 */
bro.image.rgbToHsv = function(dst, src) {};

/**
 *  HSV to RGB conversion.
 *
 * @param {Float32Array} dst
 * @param {Float32Array} src
 */
bro.image.hsvToRgb = function(dst, src) {};

/**
 *  RGB to HSL conversion.
 *
 * @param {Float32Array} dst
 * @param {Float32Array} src
 */
bro.image.rgbToHsl = function(dst, src) {};

/**
 *  HSL to RGB conversion.
 *
 * @param {Float32Array} dst
 * @param {Float32Array} src
 */
bro.image.hslToRgb = function(dst, src) {};

/**
 *  Apply row-major 3x3 color matrix.
 *
 * @param {Float32Array} dst
 * @param {Float32Array} src
 * @param {Object} params
 */
bro.image.applyColorMatrix3x3 = function(dst, src, params) {};

/**
 *  Apply row-major 3x4 color matrix.
 *
 * @param {Float32Array} dst
 * @param {Float32Array} src
 * @param {Object} params
 */
bro.image.applyColorMatrix3x4 = function(dst, src, params) {};

/**
 *  Packed NHWC uint8 to planar NCHW float32.
 *
 * @param {Float32Array} dst
 * @param {Uint8Array} src
 * @param {Object} params
 */
bro.image.u8NhwcToF32Nchw = function(dst, src, params) {};

/**
 *  Float32 NHWC to NCHW.
 *
 * @param {Float32Array} dst
 * @param {Float32Array} src
 * @param {Object} params
 */
bro.image.nhwcToNchwF32 = function(dst, src, params) {};

/**
 *  Float32 NCHW to NHWC.
 *
 * @param {Float32Array} dst
 * @param {Float32Array} src
 * @param {Object} params
 */
bro.image.nchwToNhwcF32 = function(dst, src, params) {};

/**
 *  Planar NCHW float32 to packed NHWC uint8.
 *
 * @param {Uint8Array} dst
 * @param {Float32Array} src
 * @param {Object} params
 */
bro.image.f32NchwToU8Nhwc = function(dst, src, params) {};

/**
 *  Normalize NCHW float32 buffer with mean/std.
 *
 * @param {Float32Array} dst
 * @param {Float32Array} src
 * @param {Object} params
 */
bro.image.normalizeNchw = function(dst, src, params) {};

/**
 *  Multi-channel stencil convolution.
 *
 * @param {Float32Array} dst
 * @param {Float32Array} src
 * @param {Object} kernel
 * @param {Object} params
 */
bro.image.stencilHwc = function(dst, src, kernel, params) {};

/**
 *  Fill single-channel feather window.
 *
 * @param {Float32Array} win
 * @param {Object} params
 */
bro.image.featherWindow = function(win, params) {};

/**
 *  Accumulate tile into global accumulator.
 *
 * @param {Float32Array} acc
 * @param {Float32Array} wacc
 * @param {Float32Array} tile
 * @param {Float32Array} window
 * @param {Object} params
 */
bro.image.accumulateTile = function(acc, wacc, tile, window, params) {};

/**
 *  Normalize tiled accumulator in place.
 *
 * @param {Float32Array} acc
 * @param {Float32Array} wacc
 * @param {Object} params
 */
bro.image.normalizeAccumulator = function(acc, wacc, params) {};

/**
 * WebGL2-backed GPU image rendering and procedural generation.
 */
/**
 *  Colormap a scalar field to canvas via WebGL2 fragment shader.
 *
 * @param {Object} canvas
 * @param {Float32Array} src
 * @param {Uint8Array} lut
 * @param {Object} [params]
 */
bro.image_gpu.colormap = function(canvas, src, lut, params) {};

/**
 *  Generate 2D Simplex FBm noise field directly on GPU.
 *
 * @param {Object} canvas
 * @param {Uint8Array} lut
 * @param {Object} params
 */
bro.image_gpu.fbm2D = function(canvas, lut, params) {};

