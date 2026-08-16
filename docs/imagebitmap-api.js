// =============================================================================
// ImageBitmap API Reference
// =============================================================================
//
// ImageBitmap is the engine's pixels-to-drawable primitive: an immutable image
// constructed from raw RGBA (not from a file path, the way `Image` is). It
// fills the gap between pixels a program produces, a decoded video/diffusion
// frame, a `bro.image` kernel result, a generated texture, and something that
// can actually be drawn.
//
// Why it exists / when to reach for it:
//
//   - You have RGBA bytes and want to draw them, scaled, onto a <canvas>.
//     Without ImageBitmap the only fillable drawable is another <canvas>,
//     which is heavyweight (GPU surface + raster thread each).
//   - You want "decode once, draw many." An ImageBitmap is immutable, so the
//     engine uploads it to a GPU texture exactly once; every subsequent
//     drawImage reuses that texture. Ideal for scrubbing a fixed sequence of
//     frames back and forth.
//   - A Worker produces frames. ImageBitmap is transferable: a worker builds
//     one and postMessage()s it to the main thread with no pixel copy.
//
// ImageBitmap is a CanvasImageSource, `ctx.drawImage()` and WebGL
// `gl.texImage2D()` both accept it directly.
//
// =============================================================================


// -----------------------------------------------------------------------------
// createImageBitmap(source[, sx, sy, sw, sh]) -> Promise<ImageBitmap>
// -----------------------------------------------------------------------------

/**
 * Build an ImageBitmap from a source. Returns a Promise (per the web standard);
 * the RGBA→bitmap work is synchronous, so the promise resolves on the next
 * microtask.
 *
 * Accepted sources:
 *   - An ImageData-shaped object: { width, height, data: Uint8ClampedArray }.
 *     This is the load-bearing case: it is exactly what a diffusion `decode()`
 *     and `ctx.createImageData()` / `bro.image` produce.
 *   - An `Image` (a loaded HTMLImageElement).
 *   - An `HTMLCanvasElement`.
 *   - Another `ImageBitmap` (uncropped: shares the immutable image; cropped:
 *     copies the sub-rect).
 *   - A `Blob` or `File` of encoded image bytes (PNG/JPEG/WebP/SVG). This is
 *     the fetch-shaped path — `fetch(url).then(r => r.blob())`, or a file the
 *     user dropped — and it decodes here rather than needing an <img> first.
 *     Bytes that do not decode reject the promise.
 *
 * The optional (sx, sy, sw, sh) crop rect is clamped to the source bounds.
 *
 * @param {object} source
 * @param {number} [sx] @param {number} [sy]
 * @param {number} [sw] @param {number} [sh]
 * @returns {Promise<ImageBitmap>}
 *
 * @example
 *   // From raw RGBA pixels.
 *   const rgba = new Uint8ClampedArray(w * h * 4);
 *   // ... fill rgba ...
 *   const bmp = await createImageBitmap({ width: w, height: h, data: rgba });
 *   ctx.drawImage(bmp, 0, 0, dstW, dstH);   // scaled blit, GPU-uploaded once
 *
 * @example
 *   // Crop a 64x64 tile out of a sheet.
 *   const tile = await createImageBitmap(sheetBitmap, 128, 0, 64, 64);
 */
function createImageBitmap(source, sx, sy, sw, sh) {}


// -----------------------------------------------------------------------------
// ImageBitmap
// -----------------------------------------------------------------------------
//
// `ImageBitmap` is a global interface object, so `x instanceof ImageBitmap`
// answers — that is how library code tells a decoded bitmap from an <img> or a
// raw {width, height, data} object. It has no usable constructor: calling
// `new ImageBitmap()` throws, and createImageBitmap() is the only way to make
// one.

const ImageBitmap = {

  /** @type {number} Intrinsic width in pixels (0 once closed). */
  width: 0,

  /** @type {number} Intrinsic height in pixels (0 once closed). */
  height: 0,

  /**
   * Release the backing image eagerly. After close() the bitmap draws as a
   * no-op and width/height read 0. Optional, the bitmap is also freed when
   * garbage-collected, but useful to drop GPU/CPU memory promptly when a
   * large set of frames is discarded.
   */
  close() {},
};


// -----------------------------------------------------------------------------
// ImageData
// -----------------------------------------------------------------------------
//
// Standard Web ImageData constructor.
//   new ImageData(width, height)
//   new ImageData(typedArray, width[, height])

class ImageData {
  /**
   * @param {number|Uint8ClampedArray} widthOrData
   * @param {number} widthOrHeight
   * @param {number} [height]
   */
  constructor(widthOrData, widthOrHeight, height) {}

  /** @type {number} */
  width;
  /** @type {number} */
  height;
  /** @type {Uint8ClampedArray} */
  data;
}


// =============================================================================
// Worker transfer
// =============================================================================
//
// ImageBitmap is a transferable object. List it in the postMessage transfer
// array and it moves to the receiving thread with no pixel copy; the source
// bitmap is neutered (drawing it becomes a no-op). An ImageBitmap NOT in the
// transfer list is structured-cloned, but since the image is immutable, the
// clone shares the backing pixels (observationally identical to a deep copy).
//
//   // --- in the worker ---
//   const frame = decodeSomething();                  // {width,height,data}
//   const bmp = await createImageBitmap(frame);
//   self.postMessage({ type: 'frame', bitmap: bmp }, [bmp]);   // zero-copy
//
//   // --- on the main thread ---
//   worker.onmessage = (e) => {
//       const bmp = e.data.bitmap;                    // a live ImageBitmap
//       ctx.drawImage(bmp, 0, 0, w, h);
//   };
//
// This is the clean path for a frame producer (a decoder, a generator, a
// diffusion pipeline) that runs in a worker: it owns the pipeline, builds a
// bitmap per frame, and hands each one to the main thread ready to draw.
//
// =============================================================================


// =============================================================================
// Recipes
// =============================================================================
//
// Smooth scrub over a fixed sequence
// ----------------------------------
//   // Each frame is an immutable ImageBitmap, drawing it re-uses one cached
//   // GPU texture, so scrubbing back and forth costs no re-upload.
//   const frames = [];                       // ImageBitmap[]
//   function showFrame(i) {
//       const r = containFit(frames[i], canvas);
//       ctx.clearRect(0, 0, canvas.width, canvas.height);
//       ctx.drawImage(frames[i], r.x, r.y, r.w, r.h);
//   }
//   function dispose() {
//       for (const f of frames) f.close();
//       frames.length = 0;
//   }
//
// Colour-map a scalar field to a drawable
// ---------------------------------------
//   const lut  = bro.image.gradient([[0,0,0,0],[1,255,255,255]]);
//   const rgba = new Uint8ClampedArray(w * h * 4);
//   bro.image.lookup(rgba, field, lut, { lo: 0, hi: 1 });
//   const bmp  = await createImageBitmap({ width: w, height: h, data: rgba });
//   ctx.drawImage(bmp, x, y, dw, dh);
//
// Upload pixels as a WebGL texture
// --------------------------------
//   const bmp = await createImageBitmap({ width: w, height: h, data: rgba });
//   gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, bmp);
//
// =============================================================================
