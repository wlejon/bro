// The geometry interfaces (W3C Geometry Interfaces Module Level 1), as
// JavaScript: DOMRectReadOnly / DOMRect / DOMRectList, DOMPointReadOnly /
// DOMPoint, DOMMatrixReadOnly / DOMMatrix (WebKitCSSMatrix is the same
// constructor) and DOMQuad; the legacy SVG types Chromium still hands out —
// SVGMatrix, SVGPoint, SVGRect, SVGTransform and SVGTransformList — and the
// SVG element methods that answer them: getBBox, getCTM, getScreenCTM,
// createSVGPoint / createSVGMatrix / createSVGRect / createSVGTransform,
// createSVGTransformFromMatrix, and `transform.baseVal`.
//
// Values follow Chromium's (rotations through exact sines at multiples of
// 90 degrees, toString with the shortest round-tripping numbers). The SVG
// geometry is the layout's, read through `__bro_geometry`
// (host_geometry_hooks.cpp), and the host makes its rect and matrix results
// (getBoundingClientRect, getClientRects, a canvas's getTransform) through
// the factories this module puts on that object.
(function () {
    'use strict';

    const g = globalThis;
    const hooks = __bro_geometry;

    function domException(message, name) {
        const D = g.DOMException;
        if (typeof D === 'function') return new D(message, name);
        const e = new Error(message);
        e.name = name;
        return e;
    }

    function illegal() {
        throw new TypeError('Illegal constructor');
    }

    function num(v, dflt) {
        return v === undefined ? dflt : +v;
    }

    function defineMethods(proto, methods) {
        for (const name of Object.keys(methods)) {
            Object.defineProperty(proto, name, { value: methods[name], writable: true, enumerable: true, configurable: true });
        }
    }

    function defineAccessors(proto, names, get, set) {
        for (const name of names) {
            const d = { get: get(name), enumerable: true, configurable: true };
            if (set) d.set = set(name);
            Object.defineProperty(proto, name, d);
        }
    }

    // --- DOMRect ------------------------------------------------------------

    const RECT = Symbol('rect');

    function DOMRectReadOnly(x, y, width, height) {
        if (!new.target) throw new TypeError("Failed to construct 'DOMRectReadOnly': Please use the 'new' operator.");
        Object.defineProperty(this, RECT, { value: [num(x, 0), num(y, 0), num(width, 0), num(height, 0)] });
    }
    function rectOf(r) {
        const s = r !== null && typeof r === 'object' ? r[RECT] : undefined;
        if (!s) throw new TypeError('Illegal invocation');
        return s;
    }
    defineAccessors(DOMRectReadOnly.prototype, ['x', 'y', 'width', 'height'], (name) => {
        const i = ['x', 'y', 'width', 'height'].indexOf(name);
        return function () { return rectOf(this)[i]; };
    });
    Object.defineProperties(DOMRectReadOnly.prototype, {
        top: { get() { const s = rectOf(this); return Math.min(s[1], s[1] + s[3]); }, enumerable: true, configurable: true },
        right: { get() { const s = rectOf(this); return Math.max(s[0], s[0] + s[2]); }, enumerable: true, configurable: true },
        bottom: { get() { const s = rectOf(this); return Math.max(s[1], s[1] + s[3]); }, enumerable: true, configurable: true },
        left: { get() { const s = rectOf(this); return Math.min(s[0], s[0] + s[2]); }, enumerable: true, configurable: true },
    });
    defineMethods(DOMRectReadOnly.prototype, {
        toJSON() {
            return { x: this.x, y: this.y, width: this.width, height: this.height,
                     top: this.top, right: this.right, bottom: this.bottom, left: this.left };
        },
    });
    function rectFrom(Ctor, other) {
        const o = other == null ? {} : other;
        return new Ctor(o.x, o.y, o.width, o.height);
    }
    DOMRectReadOnly.fromRect = function fromRect(other) { return rectFrom(DOMRectReadOnly, other); };

    function DOMRect(x, y, width, height) {
        if (!new.target) throw new TypeError("Failed to construct 'DOMRect': Please use the 'new' operator.");
        Object.defineProperty(this, RECT, { value: [num(x, 0), num(y, 0), num(width, 0), num(height, 0)] });
    }
    Object.setPrototypeOf(DOMRect, DOMRectReadOnly);
    DOMRect.prototype = Object.create(DOMRectReadOnly.prototype, {
        constructor: { value: DOMRect, writable: true, configurable: true },
    });
    defineAccessors(DOMRect.prototype, ['x', 'y', 'width', 'height'], (name) => {
        const i = ['x', 'y', 'width', 'height'].indexOf(name);
        return function () { return rectOf(this)[i]; };
    }, (name) => {
        const i = ['x', 'y', 'width', 'height'].indexOf(name);
        return function (v) { rectOf(this)[i] = +v; };
    });
    DOMRect.fromRect = function fromRect(other) { return rectFrom(DOMRect, other); };

    // getClientRects()'s list: indexed, item(), iterable.
    function DOMRectList() { illegal(); }
    defineMethods(DOMRectList.prototype, {
        item(i) {
            const v = this[i >>> 0];
            return v === undefined ? null : v;
        },
    });
    Object.defineProperty(DOMRectList.prototype, Symbol.iterator, {
        value: Array.prototype[Symbol.iterator], writable: true, configurable: true,
    });
    function makeRectList(rects) {
        const list = Object.create(DOMRectList.prototype);
        for (let i = 0; i < rects.length; i++) {
            Object.defineProperty(list, i, { value: rects[i], enumerable: true });
        }
        Object.defineProperty(list, 'length', { value: rects.length });
        return list;
    }

    // --- DOMPoint -----------------------------------------------------------

    const POINT = Symbol('point');

    function DOMPointReadOnly(x, y, z, w) {
        if (!new.target) throw new TypeError("Failed to construct 'DOMPointReadOnly': Please use the 'new' operator.");
        Object.defineProperty(this, POINT, { value: [num(x, 0), num(y, 0), num(z, 0), num(w, 1)] });
    }
    function pointOf(p) {
        const s = p !== null && typeof p === 'object' ? p[POINT] : undefined;
        if (!s) throw new TypeError('Illegal invocation');
        return s;
    }
    const XYZW = ['x', 'y', 'z', 'w'];
    defineAccessors(DOMPointReadOnly.prototype, XYZW, (name) => {
        const i = XYZW.indexOf(name);
        return function () { return pointOf(this)[i]; };
    });
    defineMethods(DOMPointReadOnly.prototype, {
        matrixTransform(matrix) {
            const m = matrixFromInit(matrix);
            const s = pointOf(this);
            return transformPoint(m, s[0], s[1], s[2], s[3]);
        },
        toJSON() { return { x: this.x, y: this.y, z: this.z, w: this.w }; },
    });
    function pointFrom(Ctor, other) {
        const o = other == null ? {} : other;
        return new Ctor(o.x, o.y, o.z, o.w);
    }
    DOMPointReadOnly.fromPoint = function fromPoint(other) { return pointFrom(DOMPointReadOnly, other); };

    function DOMPoint(x, y, z, w) {
        if (!new.target) throw new TypeError("Failed to construct 'DOMPoint': Please use the 'new' operator.");
        Object.defineProperty(this, POINT, { value: [num(x, 0), num(y, 0), num(z, 0), num(w, 1)] });
    }
    Object.setPrototypeOf(DOMPoint, DOMPointReadOnly);
    DOMPoint.prototype = Object.create(DOMPointReadOnly.prototype, {
        constructor: { value: DOMPoint, writable: true, configurable: true },
    });
    defineAccessors(DOMPoint.prototype, XYZW, (name) => {
        const i = XYZW.indexOf(name);
        return function () { return pointOf(this)[i]; };
    }, (name) => {
        const i = XYZW.indexOf(name);
        return function (v) { pointOf(this)[i] = +v; };
    });
    DOMPoint.fromPoint = function fromPoint(other) { return pointFrom(DOMPoint, other); };

    // --- Matrix math ----------------------------------------------------------
    // A matrix is 16 numbers, column-major as the interfaces name them:
    // [m11, m12, m13, m14, m21, ..., m44]; a = m11, b = m12, c = m21, d = m22,
    // e = m41, f = m42.

    function identity() {
        return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
    }

    // a × b (a applied after b to a column vector): the product the
    // interface's multiply() names, this × other.
    function mul(a, b) {
        const r = new Array(16);
        for (let col = 0; col < 4; col++) {
            for (let row = 0; row < 4; row++) {
                r[col * 4 + row] = a[row] * b[col * 4] + a[4 + row] * b[col * 4 + 1] +
                                   a[8 + row] * b[col * 4 + 2] + a[12 + row] * b[col * 4 + 3];
            }
        }
        return r;
    }

    function from2D(a, b, c, d, e, f) {
        return [a, b, 0, 0, c, d, 0, 0, 0, 0, 1, 0, e, f, 0, 1];
    }

    function mul2D(m, n) {
        // Both 2D: the six-term product, exactly as a 2D engine computes it.
        return from2D(m[0] * n[0] + m[4] * n[1], m[1] * n[0] + m[5] * n[1],
                      m[0] * n[4] + m[4] * n[5], m[1] * n[4] + m[5] * n[5],
                      m[0] * n[12] + m[4] * n[13] + m[12], m[1] * n[12] + m[5] * n[13] + m[13]);
    }

    // sin / cos of an angle in degrees as Chromium computes them: reduced to
    // within 45 degrees of a multiple of 90 (exact at the multiples, and
    // sqrt(1/2) at the odd multiples of 45), so sin(60) is cos(30).
    function sinCosDeg(deg) {
        const k = Math.round(deg / 90);
        const r = deg - 90 * k;
        let s0, c0;
        if (r === 0) { s0 = 0; c0 = 1; }
        else if (Math.abs(r) === 45) { s0 = r > 0 ? Math.SQRT1_2 : -Math.SQRT1_2; c0 = Math.SQRT1_2; }
        else { const rad = r * Math.PI / 180; s0 = Math.sin(rad); c0 = Math.cos(rad); }
        switch (((k % 4) + 4) % 4) {
            case 1: return [c0, -s0];
            case 2: return [-s0, -c0];
            case 3: return [-c0, s0];
            default: return [s0, c0];
        }
    }

    function rotation(x, y, z, deg) {
        const len = Math.sqrt(x * x + y * y + z * z);
        if (len === 0) return identity();
        if (x === 0 && y === 0) {
            const [s0, c] = sinCosDeg(z > 0 ? deg : -deg);
            return from2D(c, s0, -s0, c, 0, 0);
        }
        x /= len; y /= len; z /= len;
        const [s, c] = sinCosDeg(deg);
        const t = 1 - c;
        return [t * x * x + c, t * x * y + s * z, t * x * z - s * y, 0,
                t * x * y - s * z, t * y * y + c, t * y * z + s * x, 0,
                t * x * z + s * y, t * y * z - s * x, t * z * z + c, 0,
                0, 0, 0, 1];
    }

    function invert(m, is2D) {
        if (is2D) {
            const det = m[0] * m[5] - m[1] * m[4];
            if (det === 0 || !isFinite(det)) return null;
            return from2D(m[5] / det, -m[1] / det, -m[4] / det, m[0] / det,
                          (m[4] * m[13] - m[5] * m[12]) / det, (m[1] * m[12] - m[0] * m[13]) / det);
        }
        const inv = new Array(16);
        inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
        inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
        inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
        inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
        inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
        inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
        inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
        inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
        inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
        inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
        inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
        inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
        inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
        inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
        inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
        inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
        const det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
        if (det === 0 || !isFinite(det)) return null;
        for (let i = 0; i < 16; i++) inv[i] /= det;
        return inv;
    }

    function transformPoint(m, x, y, z, w) {
        return new DOMPoint(m[0] * x + m[4] * y + m[8] * z + m[12] * w,
                            m[1] * x + m[5] * y + m[9] * z + m[13] * w,
                            m[2] * x + m[6] * y + m[10] * z + m[14] * w,
                            m[3] * x + m[7] * y + m[11] * z + m[15] * w);
    }

    // --- CSS transform lists ----------------------------------------------------

    function cssLength(tok) {
        const m = /^([+-]?(?:\d+\.?\d*|\.\d+)(?:e[+-]?\d+)?)(px)?$/i.exec(tok);
        if (!m || (!m[2] && +m[1] !== 0)) throw domException("Failed to parse '" + tok + "'.", 'SyntaxError');
        return +m[1];
    }
    function cssNumber(tok) {
        const m = /^([+-]?(?:\d+\.?\d*|\.\d+)(?:e[+-]?\d+)?)$/i.exec(tok);
        if (!m) throw domException("Failed to parse '" + tok + "'.", 'SyntaxError');
        return +m[1];
    }
    function cssAngle(tok) {
        const m = /^([+-]?(?:\d+\.?\d*|\.\d+)(?:e[+-]?\d+)?)(deg|rad|grad|turn)?$/i.exec(tok);
        if (!m || (!m[2] && +m[1] !== 0)) throw domException("Failed to parse '" + tok + "'.", 'SyntaxError');
        const v = +m[1];
        switch ((m[2] || 'deg').toLowerCase()) {
            case 'rad': return v * 180 / Math.PI;
            case 'grad': return v * 0.9;
            case 'turn': return v * 360;
            default: return v;
        }
    }

    // [matrix16, is2D] for a CSS <transform-list> ("" and "none" are the
    // identity); a SyntaxError DOMException for anything else.
    function parseTransformList(text) {
        const s = String(text).trim();
        let m = identity();
        let is2D = true;
        if (s === '' || s.toLowerCase() === 'none') return [m, true];
        const re = /\s*([a-zA-Z0-9]+)\(([^)]*)\)\s*/y;
        let pos = 0;
        while (pos < s.length) {
            re.lastIndex = pos;
            const f = re.exec(s);
            if (!f) throw domException("Failed to parse '" + s + "'.", 'SyntaxError');
            pos = re.lastIndex;
            const name = f[1].toLowerCase();
            const args = f[2].trim() === '' ? [] : f[2].split(',').map((a) => a.trim());
            const want = (lo, hi) => {
                if (args.length < lo || args.length > hi) throw domException("Failed to parse '" + s + "'.", 'SyntaxError');
            };
            let t;
            let flat = true;
            switch (name) {
                case 'matrix':
                    want(6, 6);
                    t = from2D(...args.map(cssNumber));
                    break;
                case 'matrix3d':
                    want(16, 16);
                    t = args.map(cssNumber);
                    flat = false;
                    break;
                case 'translate': {
                    want(1, 2);
                    t = from2D(1, 0, 0, 1, cssLength(args[0]), args[1] === undefined ? 0 : cssLength(args[1]));
                    break;
                }
                case 'translatex': want(1, 1); t = from2D(1, 0, 0, 1, cssLength(args[0]), 0); break;
                case 'translatey': want(1, 1); t = from2D(1, 0, 0, 1, 0, cssLength(args[0])); break;
                case 'translatez':
                    want(1, 1); t = identity(); t[14] = cssLength(args[0]); flat = false; break;
                case 'translate3d':
                    want(3, 3); t = identity();
                    t[12] = cssLength(args[0]); t[13] = cssLength(args[1]); t[14] = cssLength(args[2]);
                    flat = false;
                    break;
                case 'scale': {
                    want(1, 2);
                    const sx = cssNumber(args[0]);
                    t = from2D(sx, 0, 0, args[1] === undefined ? sx : cssNumber(args[1]), 0, 0);
                    break;
                }
                case 'scalex': want(1, 1); t = from2D(cssNumber(args[0]), 0, 0, 1, 0, 0); break;
                case 'scaley': want(1, 1); t = from2D(1, 0, 0, cssNumber(args[0]), 0, 0); break;
                case 'scalez': want(1, 1); t = identity(); t[10] = cssNumber(args[0]); flat = false; break;
                case 'scale3d':
                    want(3, 3); t = identity();
                    t[0] = cssNumber(args[0]); t[5] = cssNumber(args[1]); t[10] = cssNumber(args[2]);
                    flat = false;
                    break;
                case 'rotate':
                case 'rotatez':
                    want(1, 1); t = rotation(0, 0, 1, cssAngle(args[0])); flat = name === 'rotate'; break;
                case 'rotatex': want(1, 1); t = rotation(1, 0, 0, cssAngle(args[0])); flat = false; break;
                case 'rotatey': want(1, 1); t = rotation(0, 1, 0, cssAngle(args[0])); flat = false; break;
                case 'rotate3d':
                    want(4, 4);
                    t = rotation(cssNumber(args[0]), cssNumber(args[1]), cssNumber(args[2]), cssAngle(args[3]));
                    flat = false;
                    break;
                case 'skew': {
                    want(1, 2);
                    const ax = Math.tan(cssAngle(args[0]) * Math.PI / 180);
                    const ay = args[1] === undefined ? 0 : Math.tan(cssAngle(args[1]) * Math.PI / 180);
                    t = from2D(1, ay, ax, 1, 0, 0);
                    break;
                }
                case 'skewx': want(1, 1); t = from2D(1, 0, Math.tan(cssAngle(args[0]) * Math.PI / 180), 1, 0, 0); break;
                case 'skewy': want(1, 1); t = from2D(1, Math.tan(cssAngle(args[0]) * Math.PI / 180), 0, 1, 0, 0); break;
                case 'perspective': {
                    want(1, 1);
                    t = identity();
                    const d = args[0] === 'none' ? 0 : cssLength(args[0]);
                    if (d !== 0) t[11] = -1 / Math.max(d, 1);
                    flat = false;
                    break;
                }
                default:
                    throw domException("Failed to parse '" + s + "'.", 'SyntaxError');
            }
            m = is2D && flat ? mul2D(m, t) : mul(m, t);
            if (!flat) is2D = false;
        }
        return [m, is2D];
    }

    // --- DOMMatrix --------------------------------------------------------------

    const MAT = Symbol('matrix');
    const NAMES = ['m11', 'm12', 'm13', 'm14', 'm21', 'm22', 'm23', 'm24',
                   'm31', 'm32', 'm33', 'm34', 'm41', 'm42', 'm43', 'm44'];
    const ALIAS = { a: 0, b: 1, c: 4, d: 5, e: 12, f: 13 };

    function matState(o) {
        const s = o !== null && typeof o === 'object' ? o[MAT] : undefined;
        if (!s) throw new TypeError('Illegal invocation');
        return s;
    }

    function initMatrix(self, init, ctorName) {
        let m;
        let is2D = true;
        if (init === undefined) {
            m = identity();
        } else if (init !== null && typeof init === 'object' && init[MAT]) {
            m = init[MAT].m.slice();
            is2D = init[MAT].is2D;
        } else if (typeof init === 'string') {
            [m, is2D] = parseTransformList(init);
        } else if (init !== null && typeof init === 'object' && typeof init[Symbol.iterator] === 'function') {
            const seq = Array.from(init, (v) => +v);
            if (seq.length === 6) {
                m = from2D(...seq);
            } else if (seq.length === 16) {
                m = seq;
                is2D = false;
            } else {
                throw new TypeError("Failed to construct '" + ctorName + "': The sequence must contain 6 elements for a 2D matrix or 16 elements for a 3D matrix.");
            }
        } else {
            [m, is2D] = parseTransformList(String(init));
        }
        Object.defineProperty(self, MAT, { value: { m, is2D } });
    }

    function makeMatrix(Ctor, m, is2D) {
        const o = Object.create(Ctor.prototype);
        Object.defineProperty(o, MAT, { value: { m, is2D } });
        return o;
    }

    // A DOMMatrixInit / DOMMatrix2DInit dictionary validated and filled in
    // (Geometry §6.1), or a matrix object as is: { m, is2D }.
    function matrixFromInit(init) {
        if (init === undefined || init === null) return identity();
        if (typeof init === 'object' && init[MAT]) return init[MAT].m;
        return dictMatrix(init).m;
    }
    function dictMatrix(d) {
        if (typeof d !== 'object') throw new TypeError("parameter is not of type 'DOMMatrixInit'.");
        const pairs = [['a', 'm11', 1], ['b', 'm12', 0], ['c', 'm21', 0], ['d', 'm22', 1], ['e', 'm41', 0], ['f', 'm42', 0]];
        const m = identity();
        const idx = [0, 1, 4, 5, 12, 13];
        for (let i = 0; i < pairs.length; i++) {
            const [short, long, dflt] = pairs[i];
            const sv = d[short];
            const lv = d[long];
            if (sv !== undefined && lv !== undefined && !Object.is(+sv, +lv) && !(+sv === +lv)) {
                throw new TypeError("The '" + short + "' property should equal the '" + long + "' property.");
            }
            m[idx[i]] = lv !== undefined ? +lv : sv !== undefined ? +sv : dflt;
        }
        const z = [['m13', 2, 0], ['m14', 3, 0], ['m23', 6, 0], ['m24', 7, 0], ['m31', 8, 0], ['m32', 9, 0],
                   ['m33', 10, 1], ['m34', 11, 0], ['m43', 14, 0], ['m44', 15, 1]];
        let flat = true;
        for (const [name, i, dflt] of z) {
            const v = d[name];
            m[i] = v === undefined ? dflt : +v;
            if (m[i] !== dflt) flat = false;
        }
        let is2D = d.is2D;
        if (is2D === true && !flat) throw new TypeError('The is2D member is set to true but the input matrix is a 3D matrix.');
        if (is2D === undefined) is2D = flat;
        return { m, is2D: !!is2D };
    }

    function fmt(v) {
        return String(v);
    }

    function DOMMatrixReadOnly(init) {
        if (!new.target) throw new TypeError("Failed to construct 'DOMMatrixReadOnly': Please use the 'new' operator.");
        initMatrix(this, init, 'DOMMatrixReadOnly');
    }

    const readOnlyMethods = {
        translate(tx, ty, tz) { return mutableCopy(this).translateSelf(tx, ty, tz); },
        scale(sx, sy, sz, ox, oy, oz) { return mutableCopy(this).scaleSelf(sx, sy, sz, ox, oy, oz); },
        scaleNonUniform(sx, sy) { return mutableCopy(this).scaleSelf(sx, sy, 1, 0, 0, 0); },
        scale3d(s, ox, oy, oz) { return mutableCopy(this).scale3dSelf(s, ox, oy, oz); },
        rotate(rx, ry, rz) { return mutableCopy(this).rotateSelf(rx, ry, rz); },
        rotateFromVector(x, y) { return mutableCopy(this).rotateFromVectorSelf(x, y); },
        rotateAxisAngle(x, y, z, angle) { return mutableCopy(this).rotateAxisAngleSelf(x, y, z, angle); },
        skewX(sx) { return mutableCopy(this).skewXSelf(sx); },
        skewY(sy) { return mutableCopy(this).skewYSelf(sy); },
        multiply(other) { return mutableCopy(this).multiplySelf(other); },
        flipX() { return mutableCopy(this).multiplySelf(new DOMMatrix([-1, 0, 0, 1, 0, 0])); },
        flipY() { return mutableCopy(this).multiplySelf(new DOMMatrix([1, 0, 0, -1, 0, 0])); },
        inverse() { return mutableCopy(this).invertSelf(); },
        transformPoint(point) {
            const p = point == null ? {} : point;
            return transformPoint(matState(this).m, num(p.x, 0), num(p.y, 0), num(p.z, 0), num(p.w, 1));
        },
        toFloat32Array() { return new Float32Array(matState(this).m); },
        toFloat64Array() { return new Float64Array(matState(this).m); },
        toJSON() {
            const s = matState(this);
            const o = {};
            for (const k of ['a', 'b', 'c', 'd', 'e', 'f']) o[k] = s.m[ALIAS[k]];
            for (let i = 0; i < 16; i++) o[NAMES[i]] = s.m[i];
            o.is2D = s.is2D;
            o.isIdentity = this.isIdentity;
            return o;
        },
        toString() {
            const s = matState(this);
            for (const v of s.m) {
                if (!isFinite(v)) {
                    throw domException('DOMMatrix cannot be serialized with NaN or Infinity values.', 'InvalidStateError');
                }
            }
            if (s.is2D) return 'matrix(' + [s.m[0], s.m[1], s.m[4], s.m[5], s.m[12], s.m[13]].map(fmt).join(', ') + ')';
            return 'matrix3d(' + s.m.map(fmt).join(', ') + ')';
        },
    };
    defineMethods(DOMMatrixReadOnly.prototype, readOnlyMethods);

    function mutableCopy(o) {
        const s = matState(o);
        return makeMatrix(DOMMatrix, s.m.slice(), s.is2D);
    }

    function defineMatrixAccessors(proto, writable) {
        for (let i = 0; i < 16; i++) {
            const name = NAMES[i];
            const dflt = i === 10 || i === 15 ? 1 : 0;
            const flatEntry = [2, 3, 6, 7, 8, 9, 10, 11, 14, 15].includes(i);
            Object.defineProperty(proto, name, {
                get() { return matState(this).m[i]; },
                set: writable ? function (v) {
                    const s = matState(this);
                    s.m[i] = +v;
                    if (flatEntry && s.m[i] !== dflt) s.is2D = false;
                } : undefined,
                enumerable: true, configurable: true,
            });
        }
        for (const k of ['a', 'b', 'c', 'd', 'e', 'f']) {
            const i = ALIAS[k];
            Object.defineProperty(proto, k, {
                get() { return matState(this).m[i]; },
                set: writable ? function (v) { matState(this).m[i] = +v; } : undefined,
                enumerable: true, configurable: true,
            });
        }
        if (!writable) {
            Object.defineProperty(proto, 'is2D', { get() { return matState(this).is2D; }, enumerable: true, configurable: true });
            Object.defineProperty(proto, 'isIdentity', {
                get() {
                    const m = matState(this).m;
                    const id = identity();
                    for (let j = 0; j < 16; j++) if (m[j] !== id[j]) return false;
                    return true;
                },
                enumerable: true, configurable: true,
            });
        }
    }
    defineMatrixAccessors(DOMMatrixReadOnly.prototype, false);

    function fromFloatArray(Ctor, arr, name) {
        if (!(arr instanceof Float32Array) && !(arr instanceof Float64Array)) {
            throw new TypeError("Failed to execute '" + name + "': parameter 1 is not of the expected array type.");
        }
        if (arr.length === 6) return makeMatrix(Ctor, from2D(...arr), true);
        if (arr.length === 16) return makeMatrix(Ctor, Array.from(arr), false);
        throw new TypeError("Failed to execute '" + name + "': The sequence must contain 6 elements for a 2D matrix or 16 elements for a 3D matrix.");
    }
    function fromMatrix(Ctor, other) {
        if (other !== null && typeof other === 'object' && other[MAT]) {
            return makeMatrix(Ctor, other[MAT].m.slice(), other[MAT].is2D);
        }
        const d = dictMatrix(other == null ? {} : other);
        return makeMatrix(Ctor, d.m, d.is2D);
    }
    DOMMatrixReadOnly.fromMatrix = function (other) { return fromMatrix(DOMMatrixReadOnly, other); };
    DOMMatrixReadOnly.fromFloat32Array = function (a) { return fromFloatArray(DOMMatrixReadOnly, a, 'fromFloat32Array'); };
    DOMMatrixReadOnly.fromFloat64Array = function (a) { return fromFloatArray(DOMMatrixReadOnly, a, 'fromFloat64Array'); };

    function DOMMatrix(init) {
        if (!new.target) throw new TypeError("Failed to construct 'DOMMatrix': Please use the 'new' operator.");
        initMatrix(this, init, 'DOMMatrix');
    }
    Object.setPrototypeOf(DOMMatrix, DOMMatrixReadOnly);
    DOMMatrix.prototype = Object.create(DOMMatrixReadOnly.prototype, {
        constructor: { value: DOMMatrix, writable: true, configurable: true },
    });
    defineMatrixAccessors(DOMMatrix.prototype, true);
    DOMMatrix.fromMatrix = function (other) { return fromMatrix(DOMMatrix, other); };
    DOMMatrix.fromFloat32Array = function (a) { return fromFloatArray(DOMMatrix, a, 'fromFloat32Array'); };
    DOMMatrix.fromFloat64Array = function (a) { return fromFloatArray(DOMMatrix, a, 'fromFloat64Array'); };

    // this = this × t
    function postMul(self, t, tIs2D) {
        const s = matState(self);
        if (s.is2D && tIs2D) {
            s.m = mul2D(s.m, t);
        } else {
            s.m = mul(s.m, t);
            s.is2D = false;
        }
        return self;
    }

    defineMethods(DOMMatrix.prototype, {
        multiplySelf(other) {
            const o = other !== null && typeof other === 'object' && other[MAT] ? other[MAT] : dictMatrix(other == null ? {} : other);
            return postMul(this, o.m, o.is2D);
        },
        preMultiplySelf(other) {
            const o = other !== null && typeof other === 'object' && other[MAT] ? other[MAT] : dictMatrix(other == null ? {} : other);
            const s = matState(this);
            if (s.is2D && o.is2D) s.m = mul2D(o.m, s.m);
            else { s.m = mul(o.m, s.m); s.is2D = false; }
            return this;
        },
        translateSelf(tx, ty, tz) {
            const x = num(tx, 0), y = num(ty, 0), z = num(tz, 0);
            const t = identity();
            t[12] = x; t[13] = y; t[14] = z;
            return postMul(this, t, z === 0);
        },
        scaleSelf(sx, sy, sz, ox, oy, oz) {
            const x = num(sx, 1);
            const y = sy === undefined ? x : +sy;
            const z = num(sz, 1);
            const px = num(ox, 0), py = num(oy, 0), pz = num(oz, 0);
            this.translateSelf(px, py, pz);
            const t = identity();
            t[0] = x; t[5] = y; t[10] = z;
            postMul(this, t, z === 1);
            return this.translateSelf(-px, -py, -pz);
        },
        scale3dSelf(scale, ox, oy, oz) {
            const k = num(scale, 1);
            return this.scaleSelf(k, k, k, ox, oy, oz);
        },
        rotateSelf(rx, ry, rz) {
            let x = num(rx, 0), y = ry === undefined ? undefined : +ry, z = rz === undefined ? undefined : +rz;
            if (y === undefined && z === undefined) { z = x; x = 0; y = 0; }
            if (y === undefined) y = 0;
            if (z === undefined) z = 0;
            if (z !== 0) postMul(this, rotation(0, 0, 1, z), true);
            if (y !== 0) postMul(this, rotation(0, 1, 0, y), false);
            if (x !== 0) postMul(this, rotation(1, 0, 0, x), false);
            return this;
        },
        rotateFromVectorSelf(x, y) {
            const vx = num(x, 0), vy = num(y, 0);
            if (vx === 0 && vy === 0) return this;
            return postMul(this, rotation(0, 0, 1, Math.atan2(vy, vx) * 180 / Math.PI), true);
        },
        rotateAxisAngleSelf(x, y, z, angle) {
            const ax = num(x, 0), ay = num(y, 0), az = num(z, 0);
            return postMul(this, rotation(ax, ay, az, num(angle, 0)), ax === 0 && ay === 0);
        },
        skewXSelf(sx) {
            return postMul(this, from2D(1, 0, Math.tan(num(sx, 0) * Math.PI / 180), 1, 0, 0), true);
        },
        skewYSelf(sy) {
            return postMul(this, from2D(1, Math.tan(num(sy, 0) * Math.PI / 180), 0, 1, 0, 0), true);
        },
        invertSelf() {
            const s = matState(this);
            const inv = invert(s.m, s.is2D);
            if (inv) {
                s.m = inv;
            } else {
                s.m = new Array(16).fill(NaN);
                s.is2D = false;
            }
            return this;
        },
        setMatrixValue(text) {
            const [m, is2D] = parseTransformList(String(text));
            const s = matState(this);
            s.m = m;
            s.is2D = is2D;
            return this;
        },
    });

    // --- DOMQuad ------------------------------------------------------------

    const QUAD = Symbol('quad');
    function DOMQuad(p1, p2, p3, p4) {
        if (!new.target) throw new TypeError("Failed to construct 'DOMQuad': Please use the 'new' operator.");
        Object.defineProperty(this, QUAD, {
            value: [DOMPoint.fromPoint(p1), DOMPoint.fromPoint(p2), DOMPoint.fromPoint(p3), DOMPoint.fromPoint(p4)],
        });
    }
    function quadOf(q) {
        const s = q !== null && typeof q === 'object' ? q[QUAD] : undefined;
        if (!s) throw new TypeError('Illegal invocation');
        return s;
    }
    defineAccessors(DOMQuad.prototype, ['p1', 'p2', 'p3', 'p4'], (name) => {
        const i = +name[1] - 1;
        return function () { return quadOf(this)[i]; };
    });
    defineMethods(DOMQuad.prototype, {
        getBounds() {
            const ps = quadOf(this);
            const xs = ps.map((p) => p.x), ys = ps.map((p) => p.y);
            const l = Math.min(...xs), t = Math.min(...ys);
            return new DOMRect(l, t, Math.max(...xs) - l, Math.max(...ys) - t);
        },
        toJSON() { return { p1: this.p1, p2: this.p2, p3: this.p3, p4: this.p4 }; },
    });
    DOMQuad.fromRect = function fromRect(other) {
        const o = other == null ? {} : other;
        const x = num(o.x, 0), y = num(o.y, 0), w = num(o.width, 0), h = num(o.height, 0);
        return new DOMQuad({ x, y }, { x: x + w, y }, { x: x + w, y: y + h }, { x, y: y + h });
    };
    DOMQuad.fromQuad = function fromQuad(other) {
        const o = other == null ? {} : other;
        return new DOMQuad(o.p1, o.p2, o.p3, o.p4);
    };

    // --- Legacy SVG types -------------------------------------------------------
    // SVGMatrix, SVGPoint and SVGRect are what Chromium's getCTM, getBBox and
    // createSVG* answer: separate interfaces with no constructor, each method
    // returning a new object of its own kind.

    const SVGM = Symbol('svgMatrix');
    function SVGMatrix() { illegal(); }
    function svgMatrix(a, b, c, d, e, f) {
        const o = Object.create(SVGMatrix.prototype);
        Object.defineProperty(o, SVGM, { value: [a, b, c, d, e, f] });
        return o;
    }
    function svgmOf(o) {
        const s = o !== null && typeof o === 'object' ? o[SVGM] : undefined;
        if (!s) throw new TypeError('Illegal invocation');
        return s;
    }
    // Either matrix kind, as six numbers.
    function sixOf(o) {
        if (o !== null && typeof o === 'object') {
            if (o[SVGM]) return o[SVGM];
            if (o[MAT]) { const m = o[MAT].m; return [m[0], m[1], m[4], m[5], m[12], m[13]]; }
        }
        throw new TypeError("parameter 1 is not of type 'SVGMatrix'.");
    }
    function svgMul(p, q) {
        return svgMatrix(p[0] * q[0] + p[2] * q[1], p[1] * q[0] + p[3] * q[1],
                         p[0] * q[2] + p[2] * q[3], p[1] * q[2] + p[3] * q[3],
                         p[0] * q[4] + p[2] * q[5] + p[4], p[1] * q[4] + p[3] * q[5] + p[5]);
    }
    ['a', 'b', 'c', 'd', 'e', 'f'].forEach((k, i) => {
        Object.defineProperty(SVGMatrix.prototype, k, {
            get() { return svgmOf(this)[i]; },
            set(v) { svgmOf(this)[i] = +v; },
            enumerable: true, configurable: true,
        });
    });
    defineMethods(SVGMatrix.prototype, {
        multiply(second) { return svgMul(svgmOf(this), sixOf(second)); },
        inverse() {
            const p = svgmOf(this);
            const inv = invert(from2D(...p), true);
            if (!inv) throw domException('The matrix is not invertible.', 'InvalidStateError');
            return svgMatrix(inv[0], inv[1], inv[4], inv[5], inv[12], inv[13]);
        },
        translate(x, y) { return svgMul(svgmOf(this), [1, 0, 0, 1, +x, +y]); },
        scale(k) { return svgMul(svgmOf(this), [+k, 0, 0, +k, 0, 0]); },
        scaleNonUniform(sx, sy) { return svgMul(svgmOf(this), [+sx, 0, 0, +sy, 0, 0]); },
        rotate(angle) {
            const [s, c] = sinCosDeg(+angle);
            return svgMul(svgmOf(this), [c, s, -s, c, 0, 0]);
        },
        rotateFromVector(x, y) {
            if (+x === 0 || +y === 0) throw domException('Arguments cannot be zero.', 'InvalidAccessError');
            const [s, c] = sinCosDeg(Math.atan2(+y, +x) * 180 / Math.PI);
            return svgMul(svgmOf(this), [c, s, -s, c, 0, 0]);
        },
        flipX() { return svgMul(svgmOf(this), [-1, 0, 0, 1, 0, 0]); },
        flipY() { return svgMul(svgmOf(this), [1, 0, 0, -1, 0, 0]); },
        skewX(angle) { return svgMul(svgmOf(this), [1, 0, Math.tan(+angle * Math.PI / 180), 1, 0, 0]); },
        skewY(angle) { return svgMul(svgmOf(this), [1, Math.tan(+angle * Math.PI / 180), 0, 1, 0, 0]); },
    });

    const SVGP = Symbol('svgPoint');
    function SVGPoint() { illegal(); }
    function svgPoint(x, y) {
        const o = Object.create(SVGPoint.prototype);
        Object.defineProperty(o, SVGP, { value: [x, y] });
        return o;
    }
    function svgpOf(o) {
        const s = o !== null && typeof o === 'object' ? o[SVGP] : undefined;
        if (!s) throw new TypeError('Illegal invocation');
        return s;
    }
    ['x', 'y'].forEach((k, i) => {
        Object.defineProperty(SVGPoint.prototype, k, {
            get() { return svgpOf(this)[i]; },
            set(v) { svgpOf(this)[i] = +v; },
            enumerable: true, configurable: true,
        });
    });
    defineMethods(SVGPoint.prototype, {
        matrixTransform(matrix) {
            const p = svgpOf(this);
            const m = sixOf(matrix);
            return svgPoint(m[0] * p[0] + m[2] * p[1] + m[4], m[1] * p[0] + m[3] * p[1] + m[5]);
        },
    });

    const SVGR = Symbol('svgRect');
    function SVGRect() { illegal(); }
    function svgRect(x, y, w, h) {
        const o = Object.create(SVGRect.prototype);
        Object.defineProperty(o, SVGR, { value: [x, y, w, h] });
        return o;
    }
    ['x', 'y', 'width', 'height'].forEach((k, i) => {
        Object.defineProperty(SVGRect.prototype, k, {
            get() {
                const s = this !== null && typeof this === 'object' ? this[SVGR] : undefined;
                if (!s) throw new TypeError('Illegal invocation');
                return s[i];
            },
            set(v) { this[SVGR][i] = +v; },
            enumerable: true, configurable: true,
        });
    });

    // SVGTransform: one entry of a transform list, its matrix and kind.
    const SVGT = Symbol('svgTransform');
    function SVGTransform() { illegal(); }
    const T_TYPES = { matrix: 1, translate: 2, scale: 3, rotate: 4, skewx: 5, skewy: 6 };
    Object.assign(SVGTransform, {
        SVG_TRANSFORM_UNKNOWN: 0, SVG_TRANSFORM_MATRIX: 1, SVG_TRANSFORM_TRANSLATE: 2, SVG_TRANSFORM_SCALE: 3,
        SVG_TRANSFORM_ROTATE: 4, SVG_TRANSFORM_SKEWX: 5, SVG_TRANSFORM_SKEWY: 6,
    });
    function svgTransform(type, six, angle) {
        const o = Object.create(SVGTransform.prototype);
        Object.defineProperty(o, SVGT, { value: { type, matrix: svgMatrix(...six), angle } });
        return o;
    }
    function svgtOf(o) {
        const s = o !== null && typeof o === 'object' ? o[SVGT] : undefined;
        if (!s) throw new TypeError('Illegal invocation');
        return s;
    }
    Object.defineProperties(SVGTransform.prototype, {
        type: { get() { return svgtOf(this).type; }, enumerable: true, configurable: true },
        matrix: { get() { return svgtOf(this).matrix; }, enumerable: true, configurable: true },
        angle: { get() { return svgtOf(this).angle; }, enumerable: true, configurable: true },
    });
    defineMethods(SVGTransform.prototype, {
        setMatrix(m) { const s = svgtOf(this); s.type = 1; s.matrix = svgMatrix(...sixOf(m)); s.angle = 0; },
        setTranslate(x, y) { const s = svgtOf(this); s.type = 2; s.matrix = svgMatrix(1, 0, 0, 1, +x, +y); s.angle = 0; },
        setScale(x, y) { const s = svgtOf(this); s.type = 3; s.matrix = svgMatrix(+x, 0, 0, +y, 0, 0); s.angle = 0; },
        setRotate(a, cx, cy) {
            const s = svgtOf(this);
            const [sn, c] = sinCosDeg(+a);
            const x = +cx, y = +cy;
            s.type = 4;
            s.matrix = svgMatrix(c, sn, -sn, c, x - c * x + sn * y, y - sn * x - c * y);
            s.angle = +a;
        },
        setSkewX(a) { const s = svgtOf(this); s.type = 5; s.matrix = svgMatrix(1, 0, Math.tan(+a * Math.PI / 180), 1, 0, 0); s.angle = +a; },
        setSkewY(a) { const s = svgtOf(this); s.type = 6; s.matrix = svgMatrix(1, Math.tan(+a * Math.PI / 180), 0, 1, 0, 0); s.angle = +a; },
    });

    // The transform attribute as SVGTransforms (SVG transform syntax: unitless
    // numbers, space or comma separated, rotate with an optional centre).
    function parseSvgTransforms(text) {
        const out = [];
        const re = /\s*,?\s*(matrix|translate|scale|rotate|skewX|skewY)\s*\(([^)]*)\)/gy;
        const s = String(text || '');
        let f;
        while ((f = re.exec(s))) {
            const n = f[2].trim() === '' ? [] : f[2].trim().split(/[\s,]+/).map(Number);
            const kind = f[1].toLowerCase();
            const t = svgTransform(0, [1, 0, 0, 1, 0, 0], 0);
            switch (kind) {
                case 'matrix': t.setMatrix(svgMatrix(n[0], n[1], n[2], n[3], n[4], n[5])); break;
                case 'translate': t.setTranslate(n[0] || 0, n[1] || 0); break;
                case 'scale': t.setScale(n[0], n.length > 1 ? n[1] : n[0]); break;
                case 'rotate': t.setRotate(n[0] || 0, n[1] || 0, n[2] || 0); break;
                case 'skewx': t.setSkewX(n[0] || 0); break;
                case 'skewy': t.setSkewY(n[0] || 0); break;
            }
            svgtOf(t).type = T_TYPES[kind];
            out.push(t);
        }
        return out;
    }
    function serializeTransforms(list) {
        return list.map((t) => {
            const s = svgtOf(t);
            const m = svgmOf(s.matrix);
            switch (s.type) {
                case 2: return 'translate(' + m[4] + ' ' + m[5] + ')';
                case 3: return 'scale(' + m[0] + ' ' + m[3] + ')';
                case 4: return 'rotate(' + s.angle + ')';
                case 5: return 'skewX(' + s.angle + ')';
                case 6: return 'skewY(' + s.angle + ')';
                default: return 'matrix(' + m.join(' ') + ')';
            }
        }).join(' ');
    }

    // SVGTransformList: a live view of the element's `transform` attribute.
    const SVGTL = Symbol('svgTransformList');
    function SVGTransformList() { illegal(); }
    function tlOf(o) {
        const el = o !== null && typeof o === 'object' ? o[SVGTL] : undefined;
        if (!el) throw new TypeError('Illegal invocation');
        return el;
    }
    function tlItems(o) { return parseSvgTransforms(tlOf(o).getAttribute('transform')); }
    function tlWrite(o, items) { tlOf(o).setAttribute('transform', serializeTransforms(items)); }
    Object.defineProperties(SVGTransformList.prototype, {
        numberOfItems: { get() { return tlItems(this).length; }, enumerable: true, configurable: true },
        length: { get() { return tlItems(this).length; }, enumerable: true, configurable: true },
    });
    defineMethods(SVGTransformList.prototype, {
        getItem(i) {
            const items = tlItems(this);
            if (i >= items.length) throw domException('The index provided (' + i + ') is greater than the maximum bound.', 'IndexSizeError');
            return items[i];
        },
        clear() { tlOf(this).removeAttribute('transform'); },
        initialize(t) { tlWrite(this, [t]); return t; },
        appendItem(t) { const items = tlItems(this); items.push(t); tlWrite(this, items); return t; },
        insertItemBefore(t, i) { const items = tlItems(this); items.splice(Math.min(i, items.length), 0, t); tlWrite(this, items); return t; },
        replaceItem(t, i) { const items = tlItems(this); items[i] = t; tlWrite(this, items); return t; },
        removeItem(i) { const items = tlItems(this); const [t] = items.splice(i, 1); tlWrite(this, items); return t; },
        createSVGTransformFromMatrix(m) { const t = svgTransform(1, [1, 0, 0, 1, 0, 0], 0); t.setMatrix(m); return t; },
        consolidate() {
            const items = tlItems(this);
            if (items.length === 0) return null;
            let six = [1, 0, 0, 1, 0, 0];
            for (const t of items) six = svgmOf(svgMul(six, svgmOf(svgtOf(t).matrix)));
            const t = svgTransform(1, six, 0);
            tlWrite(this, [t]);
            return t;
        },
    });

    function SVGAnimatedTransformList() { illegal(); }
    const SVGATL = Symbol('svgAnimatedTransformList');
    function animatedList(el) {
        const list = Object.create(SVGTransformList.prototype);
        Object.defineProperty(list, SVGTL, { value: el });
        const o = Object.create(SVGAnimatedTransformList.prototype);
        Object.defineProperty(o, SVGATL, { value: list });
        return o;
    }
    for (const k of ['baseVal', 'animVal']) {
        Object.defineProperty(SVGAnimatedTransformList.prototype, k, {
            get() { return this[SVGATL]; }, enumerable: true, configurable: true,
        });
    }

    // --- SVG element methods --------------------------------------------------

    const graphics = g.SVGGraphicsElement;
    if (typeof graphics === 'function' && graphics.prototype) {
        const ctm = (el, screen) => {
            const m = hooks.svgCTM(el, screen);
            return m ? svgMatrix(m[0], m[1], m[2], m[3], m[4], m[5]) : null;
        };
        const transformLists = new WeakMap();
        defineMethods(graphics.prototype, {
            getBBox() {
                const r = hooks.svgBBox(this);
                return r ? svgRect(r[0], r[1], r[2], r[3]) : svgRect(0, 0, 0, 0);
            },
            getCTM() { return ctm(this, false); },
            getScreenCTM() { return ctm(this, true); },
        });
        Object.defineProperty(graphics.prototype, 'transform', {
            get() {
                let l = transformLists.get(this);
                if (!l) { l = animatedList(this); transformLists.set(this, l); }
                return l;
            },
            enumerable: true, configurable: true,
        });
        const svgEl = g.SVGElement && g.SVGElement.prototype;
        if (svgEl) {
            Object.defineProperty(svgEl, 'ownerSVGElement', {
                get() {
                    for (let p = this.parentNode; p && p.nodeType === 1; p = p.parentNode) {
                        if (p.localName === 'svg' && p.namespaceURI === 'http://www.w3.org/2000/svg') return p;
                    }
                    return null;
                },
                enumerable: true, configurable: true,
            });
            Object.defineProperty(svgEl, 'viewportElement', {
                get() { return this.ownerSVGElement; }, enumerable: true, configurable: true,
            });
        }
        const svgSvg = g.SVGSVGElement && g.SVGSVGElement.prototype;
        if (svgSvg) {
            defineMethods(svgSvg, {
                createSVGPoint() { return svgPoint(0, 0); },
                createSVGMatrix() { return svgMatrix(1, 0, 0, 1, 0, 0); },
                createSVGRect() { return svgRect(0, 0, 0, 0); },
                createSVGTransform() { return svgTransform(1, [1, 0, 0, 1, 0, 0], 0); },
                createSVGTransformFromMatrix(m) { const t = svgTransform(1, [1, 0, 0, 1, 0, 0], 0); t.setMatrix(m); return t; },
                createSVGNumber() { return { value: 0 }; },
            });
        }
    }

    // --- Globals and the host's factories ---------------------------------------

    const defs = { DOMRectReadOnly, DOMRect, DOMRectList, DOMPointReadOnly, DOMPoint, DOMMatrixReadOnly, DOMMatrix,
                   DOMQuad, SVGMatrix, SVGPoint, SVGRect, SVGTransform, SVGTransformList, SVGAnimatedTransformList };
    for (const name of Object.keys(defs)) {
        Object.defineProperty(defs[name], 'name', { value: name, configurable: true });
        Object.defineProperty(defs[name].prototype, Symbol.toStringTag, { value: name, configurable: true });
        Object.defineProperty(g, name, { value: defs[name], writable: true, enumerable: false, configurable: true });
    }
    Object.defineProperty(g, 'WebKitCSSMatrix', { value: DOMMatrix, writable: true, enumerable: false, configurable: true });

    hooks.rect = function (x, y, w, h) { return new DOMRect(x, y, w, h); };
    hooks.rectList = function (rects) { return makeRectList(rects); };
    hooks.matrix2d = function (a, b, c, d, e, f) { return makeMatrix(DOMMatrix, from2D(a, b, c, d, e, f), true); };
})();
