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

