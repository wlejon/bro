/**
 * Geometry interfaces: DOMRect, DOMPoint, DOMMatrix, DOMQuad, and the legacy
 * SVG geometry types (src/bronze_host/js/geometry.js; the SVG geometry is the
 * layout's, layout/svg_geometry.h). Values match Chromium's, rounding
 * included: rotations reduce to within 45 degrees of a multiple of 90, so
 * `rotate(90)` is exact and `rotate(60)`'s cosine is sin(30).
 *
 * - `DOMRectReadOnly` / `DOMRect(x, y, width, height)`: `left`/`top`/`right`/
 *   `bottom` derived (a negative size flips them), `fromRect(dict)`, `toJSON()`.
 *   `getBoundingClientRect()` (elements, ranges) answers a DOMRect and
 *   `getClientRects()` a `DOMRectList` (`length`, `item(i)`, indexed, iterable).
 *   Members are prototype accessors: `Object.keys(rect)` is empty, as on the web.
 * - `DOMPointReadOnly` / `DOMPoint(x, y, z, w = 1)`: `matrixTransform(matrix)`,
 *   `fromPoint(dict)`, `toJSON()`.
 * - `DOMMatrixReadOnly` / `DOMMatrix(init)` (`WebKitCSSMatrix` is DOMMatrix):
 *   init is nothing (identity), six numbers (2D), sixteen (3D, column-major
 *   m11..m44) or a CSS transform list (`'translate(10px) rotate(45deg)'`,
 *   px and unitless 0 lengths, deg/rad/grad/turn angles; `'none'`).
 *   Members `a`..`f`, `m11`..`m44`, `is2D`, `isIdentity`; `fromMatrix(dict)`,
 *   `fromFloat32Array`, `fromFloat64Array`. Returning a new matrix:
 *   `multiply`, `translate`, `scale(sx, sy, sz, ox, oy, oz)`, `scaleNonUniform`,
 *   `scale3d`, `rotate(rx, ry, rz)` (one argument is about z), `rotateFromVector`,
 *   `rotateAxisAngle`, `skewX`, `skewY`, `flipX`, `flipY`, `inverse` (all NaN and
 *   3D when singular); on DOMMatrix each has a `*Self` mutator, plus
 *   `preMultiplySelf`, `invertSelf`, `setMatrixValue(css)`. `transformPoint`,
 *   `toFloat32Array` / `toFloat64Array`, `toJSON`, and `toString()`
 *   (`matrix(...)` / `matrix3d(...)`; InvalidStateError on NaN or Infinity).
 * - `DOMQuad(p1, p2, p3, p4)`: `fromRect`, `fromQuad`, `getBounds()`, `toJSON`.
 * - Canvas: `ctx.getTransform()` is a DOMMatrix; `ctx.setTransform(matrix)`,
 *   `path.addPath(other, matrix)` and `pattern.setTransform(matrix)` take a
 *   DOMMatrix or a DOMMatrix2DInit dictionary.
 * - SVG (on SVGGraphicsElement): `getBBox()` (an SVGRect in the element's user
 *   space, without its own transform, no stroke), `getCTM()` (an SVGMatrix to
 *   the outermost <svg>'s viewport, viewBox included), `getScreenCTM()` (to
 *   client coordinates), `transform.baseVal` (an SVGTransformList live over the
 *   `transform` attribute: `numberOfItems`, `getItem`, `appendItem`,
 *   `consolidate`, ...). On <svg>: `createSVGPoint`, `createSVGMatrix`,
 *   `createSVGRect`, `createSVGTransform`, `createSVGTransformFromMatrix`.
 *   SVGMatrix (`a`..`f`, `multiply`, `inverse`, `translate`, `scale`,
 *   `scaleNonUniform`, `rotate`, `rotateFromVector`, `flipX`, `flipY`, `skewX`,
 *   `skewY`), SVGPoint (`x`, `y`, `matrixTransform`) and SVGRect are their own
 *   interfaces, as in Chromium: `el.getCTM() instanceof DOMMatrix` is false.
 *   `ownerSVGElement` / `viewportElement` name the nearest <svg> ancestor.
 *
 * @example
 * const m = new DOMMatrix('translate(10px, 20px) rotate(90deg)');
 * m.toString();                                   // 'matrix(0, 1, -1, 0, 10, 20)'
 * new DOMPoint(1, 0).matrixTransform(m);          // {x: 10, y: 21, z: 0, w: 1}
 * const p = svg.createSVGPoint();
 * p.x = ev.clientX; p.y = ev.clientY;
 * const local = p.matrixTransform(shape.getScreenCTM().inverse());
 */
