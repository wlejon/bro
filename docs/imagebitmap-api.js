/**
 * =============================================================================
 * ImageBitmap & createImageBitmap API
 * =============================================================================
 *
 * ImageBitmap is an immutable, pixel-constructed, drawable image backed by an SkImage.
 * Supports asynchronous decoding from Blobs, typed arrays, Images, and ImageData.
 *
 * @example
 *   const bmp = await createImageBitmap({ width: 4, height: 4, data: rgbaData });
 *   ctx.drawImage(bmp, 0, 0);
 *   bmp.close();
 *
 * @example
 *   const imgData = new ImageData(new Uint8ClampedArray(64), 4, 4);
 *   const bmpFromData = await createImageBitmap(imgData);
 *
 * @example
 *   // Show a bitmap on a canvas without a 2D context: the bitmaprenderer
 *   // context takes the bitmap over (the bitmap is detached afterwards).
 *   const view = document.createElement('canvas');
 *   const brc = view.getContext('bitmaprenderer');
 *   brc.transferFromImageBitmap(await createImageBitmap(imgData));
 *
 * createImageBitmap(canvas) copies what the canvas displays now, at the size
 * of its bitmap: a 2D canvas's current bitmap, a bitmaprenderer canvas's last
 * transferred bitmap at that bitmap's own size (not the width/height
 * attributes), a WebGL canvas's drawing buffer. A canvas with no context yet
 * gives transparent black at its width/height (300x150 by default) and is
 * not given a context by the call. A zero-sized canvas rejects with an
 * InvalidStateError.
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * W3C ImageBitmap interface representing a bitmap image that can be drawn to a canvas.
 */
class ImageBitmap {

  /**
   * Intrinsic width of the image bitmap in pixels.
   * @readonly
   * @type {number}
   */
  width;

  /**
   * Intrinsic height of the image bitmap in pixels.
   * @readonly
   * @type {number}
   */
  height;

  /**
   * Releases the underlying graphics memory and closes the bitmap.
   */
  close() {}

}

/**
 * Represents underlying pixel data of an area of a canvas or image.
 */
class ImageData {

  /**
   * Creates an ImageData object with given dimensions.
   *
   * @param {number} width
   * @param {number} height
   */
  constructor(width, height) {}

  /**
   * Creates an ImageData object with given pixel data and dimensions.
   *
   * @param {Uint8ClampedArray} data
   * @param {number} width
   * @param {number} [height]
   */
  constructor(data, width, height) {}

  /**
   *  Width in pixels.
   * @readonly
   * @type {number}
   */
  width;

  /**
   *  Height in pixels.
   * @readonly
   * @type {number}
   */
  height;

  /**
   *  RGBA one-dimensional array of pixel data.
   * @readonly
   * @type {Uint8ClampedArray}
   */
  data;

}

/**
 * The context `canvas.getContext('bitmaprenderer')` answers. A canvas has one
 * context mode for its life, so a bitmaprenderer canvas answers null to
 * getContext('2d') and the WebGL types. Its width/height attributes do not
 * resize or clear the displayed bitmap. The canvas can itself be a drawImage /
 * createImageBitmap / createPattern source.
 */
class ImageBitmapRenderingContext {

  /**
   * @readonly
   * @type {HTMLCanvasElement}
   */
  canvas;

  /**
   * Make the canvas show `bitmap`, at the bitmap's own size, and detach
   * `bitmap`: its width/height become 0 and it can no longer be drawn or
   * transferred (the ownership moves to the canvas, as the name says).
   * `null` resets the canvas to transparent black at its width/height.
   * Throws InvalidStateError for a detached or closed bitmap and TypeError for
   * anything that is not an ImageBitmap.
   *
   * @param {ImageBitmap|null} bitmap
   */
  transferFromImageBitmap(bitmap) {}

}

