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
 *
 * createImageBitmap(blob) decodes off the page thread: the call copies the
 * Blob's bytes and returns its promise (well under a millisecond for an
 * 8 MP JPEG); the decode, the crop and the bitmap are made on a decoder
 * thread and the promise settles on a later frame. Same decoders as an
 * `<img>` (PNG, JPEG, WebP, SVG, ...). EXIF orientation is applied unless the
 * options say `{ imageOrientation: 'none' }` (options come after the source,
 * or after the crop rectangle). In a Worker the decode runs on the worker's
 * own thread instead.
 *
 * @example
 *   const bmp = await createImageBitmap(await (await fetch(url)).blob());
 *   const raw = await createImageBitmap(blob, { imageOrientation: 'none' });
 *
 * Transfer and the GPU. A bitmap's pixels are immutable and held by
 * reference: postMessage with the bitmap in the transfer list moves that
 * reference (the sender's bitmap reads 0x0 afterwards), without it the
 * receiver shares the same pixels, and createImageBitmap(bitmap) or
 * createImageBitmap(img) without a crop shares them too. Neither side copies
 * the pixels, so receiving a 24 MP bitmap from a Worker costs the page thread
 * well under a millisecond. On the page, a bitmap of 512x512 or more starts
 * its texture upload the moment it exists — the staging copy written, the
 * GPU copy run and its mip chain (GPU blits) submitted on an uploader thread
 * — so the frame that first draws it, with drawImage or through a
 * bitmaprenderer canvas, samples a texture already made instead of uploading
 * 96 MB in the frame. A bitmap transferred out of a Worker starts that upload
 * as it is posted, so it overlaps the hop to the page. The GPU copy crosses
 * the bus on the device's copy queue where it has one (a discrete GPU; tens
 * of ms for 24 MP), beside the frames rather than in front of them. A
 * drawImage before the upload is in — the very frame the bitmap arrived, or
 * while its copy is in flight — does not stall the frame: the canvas keeps
 * what it showed and shows the new content once the texture is in (a windowed
 * frame and a headless advanceTime step alike). A read that needs the pixels
 * now (getImageData, a snapshot, a headless getPixel/screenshot) waits for the
 * rest of the upload. close() lets the pixels and
 * the texture go at once. A decoded `<img>` of 512x512 or more gets the same
 * upload when its decode lands, so the frame that first paints it does not
 * pay one either.
 *
 * @example
 *   // worker: post a decoded picture to the page without a copy
 *   const bmp = await createImageBitmap(blob);
 *   self.postMessage({ bmp }, [bmp]);   // bmp.width === 0 here from now on
 *   // page: draw it next frame; its texture is already uploading
 *   worker.onmessage = (e) => ctx.drawImage(e.data.bmp, 0, 0, w, h);
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

