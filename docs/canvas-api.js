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
   * @type {*}
   */
  fillStyle;

  /**
   * @type {*}
   */
  strokeStyle;

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
   * @type {number}
   */
  shadowOffsetX;

  /**
   * @type {number}
   */
  shadowOffsetY;

  /**
   * @type {number}
   */
  shadowBlur;

  /**
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
   * @type {boolean}
   */
  imageSmoothingEnabled;

  /**
   * @type {string}
   */
  imageSmoothingQuality;

  /**
   * @type {number}
   */
  lineDashOffset;

  save() {}

  restore() {}

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

