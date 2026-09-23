// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * =============================================================================
 * CanvasRenderingContext2D — 2D Canvas Graphics Context
 * =============================================================================
 *
 * High-performance 2D drawing context backed by Skia graphics engine.
 * Supports path drawing, gradients, text measurement, image rendering,
 * and pixel buffer manipulation.
 * @example
 * const ctx = canvas.getContext('2d');
 *   ctx.fillStyle = '#ff0000';
 *   ctx.fillRect(10, 10, 100, 100);
 */
class CanvasGradient {

  /**
   * @param {number} offset
   * @param {string} color
   */
  addColorStop(offset, color) {}

}

/**
 * An image tiled as a fill or stroke style, from `ctx.createPattern()`.
 * The pattern keeps a snapshot of its source taken at createPattern time,
 * lives in user space (the current transform moves and scales it), and is
 * sampled with the context's imageSmoothingEnabled / imageSmoothingQuality at
 * draw time.
 *
 * @example
 *   const p = ctx.createPattern(tileCanvas, 'repeat');
 *   p.setTransform({ a: 2, b: 0, c: 0, d: 2, e: 0, f: 0 });  // tiles at 2x
 *   ctx.fillStyle = p;
 *   ctx.fillRect(0, 0, 200, 200);
 */
class CanvasPattern {

  /**
   * Set the pattern's own transform, applied on top of the context's current
   * transform. Takes effect on later draws even when the pattern was already
   * assigned to fillStyle/strokeStyle. Omitted members take their identity
   * values; `m11/m12/m21/m22/m41/m42` are accepted as aliases of `a..f`.
   * Throws TypeError for a non-finite member.
   *
   * @param {{a?:number,b?:number,c?:number,d?:number,e?:number,f?:number}} [transform]
   */
  setTransform(transform) {}

}

/**
 * The context of a `canvas.getContext('bitmaprenderer')` canvas: it shows an
 * ImageBitmap handed to it, at that bitmap's size. See imagebitmap-api.js.
 */
class ImageBitmapRenderingContext {

  /**
   * @readonly
   * @type {HTMLCanvasElement}
   */
  canvas;

  /**
   * Replace the canvas's bitmap with `bitmap`'s pixels and detach `bitmap`
   * (its width/height become 0 and it can no longer be drawn or transferred).
   * `null` resets the canvas to transparent black at its own width/height.
   * Throws InvalidStateError for an already detached or closed bitmap and
   * TypeError for anything that is not an ImageBitmap.
   *
   * @param {ImageBitmap|null} bitmap
   */
  transferFromImageBitmap(bitmap) {}

}

class TextMetrics {

  /**
   * @readonly
   * @type {number}
   */
  width;

}

class CanvasRenderingContext2D {

  /**
   * @readonly
   * @type {number}
   */
  canvasWidth;

  /**
   * @readonly
   * @type {number}
   */
  canvasHeight;

  /**
   * A CSS color string, a CanvasGradient or a CanvasPattern. A gradient or
   * pattern reads back as the same object; a color reads back as
   * `rgba(r,g,b,a)`. An unparseable value is ignored.
   * @type {string|CanvasGradient|CanvasPattern}
   */
  fillStyle;

  /**
   * As fillStyle, for strokes.
   * @type {string|CanvasGradient|CanvasPattern}
   */
  strokeStyle;

  /**
   * A CSS filter applied to every later draw (fills, strokes, text,
   * drawImage; not clearRect or putImageData): "none" (the default) or a
   * space-separated list of blur(<length>), brightness(), contrast(),
   * grayscale(), invert(), opacity(), saturate(), sepia() (each a <number> or
   * <percentage>), hue-rotate(<angle>) and drop-shadow(<color>? <dx> <dy>
   * <blur>?). Lengths are canvas pixels and are not scaled by the current
   * transform. The filter runs before globalAlpha and
   * globalCompositeOperation, as the spec orders them. An invalid value
   * (unknown function, negative amount, unitless non-zero length, url()) is
   * ignored and the previous filter kept. Saved and restored with the state.
   * @example
   *   ctx.filter = 'blur(4px) grayscale(1)';
   *   ctx.drawImage(photo, 0, 0);
   *   ctx.filter = 'none';
   * @type {string}
   */
  filter;

  /**
   * @type {number}
   */
  lineWidth;

  /**
   * @type {string}
   */
  lineCap;

  /**
   * @type {string}
   */
  lineJoin;

  /**
   * @type {number}
   */
  miterLimit;

  /**
   * @type {number}
   */
  globalAlpha;

  /**
   * @type {string}
   */
  globalCompositeOperation;

  /**
   * Shadow offset in canvas pixels. The transform does not apply to it: a
   * scaled or rotated draw still casts its shadow this far right. A
   * non-finite value is ignored.
   *
   * Every fill, stroke, text and drawImage casts a shadow while shadowColor
   * is not fully transparent and shadowBlur or an offset is non-zero
   * (putImageData and clearRect never do). The shadow is cast from the
   * shape's alpha — after ctx.filter, when one is set — in shadowColor, and
   * composited under the shape with globalAlpha, the composite operation
   * and the clip, each applied to the shadow and the shape separately.
   * @example
   *   ctx.shadowColor = 'rgba(0, 0, 0, 0.5)';
   *   ctx.shadowOffsetX = 4;
   *   ctx.shadowOffsetY = 4;
   *   ctx.shadowBlur = 8;
   *   ctx.fillRect(20, 20, 100, 60);
   * @type {number}
   */
  shadowOffsetX;

  /**
   * @type {number}
   */
  shadowOffsetY;

  /**
   * Blur of the shadow: a Gaussian of standard deviation shadowBlur / 2,
   * in canvas pixels whatever the transform. Negative or non-finite values
   * are ignored.
   * @type {number}
   */
  shadowBlur;

  /**
   * Default transparent black, which draws no shadow.
   * @type {string}
   */
  shadowColor;

  /**
   * @type {string}
   */
  font;

  /**
   * @type {string}
   */
  textAlign;

  /**
   * @type {string}
   */
  textBaseline;

  /**
   * @type {string}
   */
  direction;

  /**
   * false samples drawImage and patterns nearest-neighbour, whatever the
   * quality.
   * @type {boolean}
   */
  imageSmoothingEnabled;

  /**
   * 'low' (the default: bilinear), 'medium' (bilinear with mipmaps, which
   * only differs when an image is drawn smaller) or 'high' (a Mitchell
   * cubic). Any other value is ignored. Applies to drawImage and patterns
   * while imageSmoothingEnabled is true. Saved and restored with the state.
   * @type {'low'|'medium'|'high'}
   */
  imageSmoothingQuality;

  /**
   * @type {number}
   */
  lineDashOffset;

  save() {}

  restore() {}

  /**
   * Clear the bitmap to transparent black and put the whole drawing state
   * back to its defaults: the save() stack is emptied, the current path,
   * transform, clip and line dash are reset, and every attribute (styles,
   * line settings, font and text settings, shadows, filter, image
   * smoothing, globalAlpha, the composite operation) returns to its initial
   * value. Assigning canvas.width or canvas.height does the same, even to
   * the current value.
   */
  reset() {}

  beginPath() {}

  closePath() {}

  stroke() {}

  fill() {}

  clip() {}

  resetTransform() {}

  /**
   * @param {number} x
   * @param {number} y
   * @param {number} w
   * @param {number} h
   */
  fillRect(x, y, w, h) {}

  /**
   * @param {number} x
   * @param {number} y
   * @param {number} w
   * @param {number} h
   */
  strokeRect(x, y, w, h) {}

  /**
   * @param {number} x
   * @param {number} y
   * @param {number} w
   * @param {number} h
   */
  clearRect(x, y, w, h) {}

  /**
   * @param {string} text
   * @param {number} x
   * @param {number} y
   * @param {number} [maxWidth]
   */
  fillText(text, x, y, maxWidth) {}

  /**
   * @param {string} text
   * @param {number} x
   * @param {number} y
   * @param {number} [maxWidth]
   */
  strokeText(text, x, y, maxWidth) {}

  /**
   * @param {number} x
   * @param {number} y
   */
  translate(x, y) {}

  /**
   * @param {number} angle
   */
  rotate(angle) {}

  /**
   * @param {number} x
   * @param {number} y
   */
  scale(x, y) {}

  /**
   * @param {number} a
   * @param {number} b
   * @param {number} c
   * @param {number} d
   * @param {number} e
   * @param {number} f
   */
  setTransform(a, b, c, d, e, f) {}

  /**
   * @param {number} a
   * @param {number} b
   * @param {number} c
   * @param {number} d
   * @param {number} e
   * @param {number} f
   */
  transform(a, b, c, d, e, f) {}

  /**
   * @param {number} x
   * @param {number} y
   */
  moveTo(x, y) {}

  /**
   * @param {number} x
   * @param {number} y
   */
  lineTo(x, y) {}

  /**
   * @param {number} x1
   * @param {number} y1
   * @param {number} x2
   * @param {number} y2
   * @param {number} radius
   */
  arcTo(x1, y1, x2, y2, radius) {}

  /**
   * @param {number} cp1x
   * @param {number} cp1y
   * @param {number} cp2x
   * @param {number} cp2y
   * @param {number} x
   * @param {number} y
   */
  bezierCurveTo(cp1x, cp1y, cp2x, cp2y, x, y) {}

  /**
   * @param {number} cpx
   * @param {number} cpy
   * @param {number} x
   * @param {number} y
   */
  quadraticCurveTo(cpx, cpy, x, y) {}

  /**
   * @param {number} x
   * @param {number} y
   * @param {number} radius
   * @param {number} startAngle
   * @param {number} endAngle
   * @param {boolean} [counterclockwise=false]
   */
  arc(x, y, radius, startAngle, endAngle, counterclockwise) {}

  /**
   * @param {number} x
   * @param {number} y
   * @param {number} radiusX
   * @param {number} radiusY
   * @param {number} rotation
   * @param {number} startAngle
   * @param {number} endAngle
   * @param {boolean} [counterclockwise=false]
   */
  ellipse(x, y, radiusX, radiusY, rotation, startAngle, endAngle, counterclockwise) {}

  /**
   * @param {number} x
   * @param {number} y
   * @param {number} w
   * @param {number} h
   */
  rect(x, y, w, h) {}

  /**
   * @param {number} x
   * @param {number} y
   * @returns {boolean}
   */
  isPointInPath(x, y) {}

  /**
   * @param {*} image
   * @param {number} sx
   * @param {number} sy
   * @param {number} [sw]
   * @param {number} [sh]
   * @param {number} [dx]
   * @param {number} [dy]
   * @param {number} [dw]
   * @param {number} [dh]
   */
  drawImage(image, sx, sy, sw, sh, dx, dy, dw, dh) {}

  /**
   * @param {number} sx
   * @param {number} sy
   * @param {number} sw
   * @param {number} sh
   * @returns {ImageData}
   */
  getImageData(sx, sy, sw, sh) {}

  /**
   * @param {ImageData} imageData
   * @param {number} dx
   * @param {number} dy
   * @param {number} [dirtyX]
   * @param {number} [dirtyY]
   * @param {number} [dirtyWidth]
   * @param {number} [dirtyHeight]
   */
  putImageData(imageData, dx, dy, dirtyX, dirtyY, dirtyWidth, dirtyHeight) {}

  /**
   * @param {(number|ImageData)} swOrImagedata
   * @param {number} [sh]
   * @returns {ImageData}
   */
  createImageData(swOrImagedata, sh) {}

  /**
   * @param {number} x0
   * @param {number} y0
   * @param {number} x1
   * @param {number} y1
   * @returns {CanvasGradient}
   */
  createLinearGradient(x0, y0, x1, y1) {}

  /**
   * @param {number} x0
   * @param {number} y0
   * @param {number} r0
   * @param {number} x1
   * @param {number} y1
   * @param {number} r1
   * @returns {CanvasGradient}
   */
  createRadialGradient(x0, y0, r0, x1, y1, r1) {}

  /**
   * Make a pattern from an image: an HTMLImageElement / Image, an
   * HTMLCanvasElement, an HTMLVideoElement (its current frame) or an
   * ImageBitmap. `repetition` is 'repeat' (also for null or ''), 'repeat-x',
   * 'repeat-y' or 'no-repeat'; anything else throws SyntaxError. Answers null
   * for an image that has not finished loading (or a video with no frame);
   * throws InvalidStateError for a broken image, a closed ImageBitmap or a
   * canvas with zero width or height, and TypeError for anything else
   * (ImageData included, as in browsers — wrap it with createImageBitmap).
   *
   * @param {HTMLImageElement|HTMLCanvasElement|HTMLVideoElement|ImageBitmap} image
   * @param {string|null} repetition
   * @returns {CanvasPattern|null}
   */
  createPattern(image, repetition) {}

  /**
   * @param {string} text
   * @returns {TextMetrics}
   */
  measureText(text) {}

  /**
   * @param {Array<number>} segments
   */
  setLineDash(segments) {}

  /**
   * @returns {Array<number>}
   */
  getLineDash() {}

}

