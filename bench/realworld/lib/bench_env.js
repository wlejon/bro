// The environment of mode=bench (lib/harness.js runBench): the scene and its
// library run as a plain program, in node or as a bronze-built executable,
// with no DOM and no GPU.
//
// At import (both runners import this first, so it runs before the library
// evaluates): `navigator` and `Intl` where the engine has none (pixi reads
// navigator at module level; a bronze executable has neither). Browsers and
// node define both, so nothing changes for the other modes.
//
// At bench start, installBenchGlobals(): a requestAnimationFrame that never
// calls back (pixi's system ticker asks for one; nothing ticks it, so pixi's
// scheduler and GC never run, the same in both engines), setTimeout where the
// engine has none, and the WebGL2RenderingContext global three.js checks.
// makeStubCanvas() is the canvas the library renders into: its 'webgl2' (and
// 'webgl') context is lib/glstub.js, its '2d' context measures text with a
// fixed advance and draws nothing (bronze/bench/pixi_env.mjs's stub).

import { WebGL2RenderingContext, WebGLRenderingContext } from './glstub.js';

if (typeof navigator === 'undefined') globalThis.navigator = { userAgent: 'bench', platform: 'bench', maxTouchPoints: 0, language: 'en-US' };
if (typeof Intl === 'undefined') globalThis.Intl = {};

export function installBenchGlobals() {
    if (typeof requestAnimationFrame === 'undefined') {
        let id = 0;
        globalThis.requestAnimationFrame = () => ++id;
        globalThis.cancelAnimationFrame = () => {};
    }
    if (typeof setTimeout === 'undefined') {
        let id = 0;
        globalThis.setTimeout = () => ++id;
        globalThis.clearTimeout = () => {};
    }
    globalThis.WebGL2RenderingContext = WebGL2RenderingContext;
    // pixi's EventSystem listens on the window and the document at init
    if (typeof globalThis.addEventListener !== 'function') {
        globalThis.addEventListener = () => {};
        globalThis.removeEventListener = () => {};
    }
    // pixi's DOMPipe creates a <div> at renderer init (the overlay for DOM
    // containers; these scenes have none)
    if (typeof document === 'undefined') {
        const element = () => ({
            style: {}, children: [], parentNode: null,
            appendChild(c) { this.children.push(c); c.parentNode = this; return c; },
            removeChild(c) { const i = this.children.indexOf(c); if (i >= 0) this.children.splice(i, 1); c.parentNode = null; return c; },
            remove() { if (this.parentNode) this.parentNode.removeChild(this); },
            contains(c) { return this.children.indexOf(c) >= 0; },
            addEventListener() {}, removeEventListener() {}, setAttribute() {}, getAttribute() { return null; },
        });
        globalThis.document = {
            body: element(), documentElement: element(),
            createElement: (tag) => (tag === 'canvas' ? makeStubCanvas(300, 150) : element()),
            addEventListener() {}, removeEventListener() {},
        };
    }
}

// capture=<file> (node only, rw.py js): record every `new Function(...)` /
// `Function(...)` the library makes (pixi's uniform, UBO and shader sync
// generators). A bronze-built executable has no `eval` (the AOT contract), so
// rw.py compiles the captured bodies into its build as ordinary functions,
// looked up by their arguments (gen.js); the code that runs is the same.
// Returns a function giving [[key, args], ...], key = JSON of the arguments.
export function captureGeneratedFunctions() {
    const Real = globalThis.Function;
    const seen = new Map();
    const record = (args) => {
        const a = Array.from(args, String);
        const k = JSON.stringify(a);
        if (!seen.has(k)) seen.set(k, a);
    };
    globalThis.Function = new Proxy(Real, {
        construct(t, args) { record(args); return Reflect.construct(t, args); },
        apply(t, self, args) { record(args); return Reflect.apply(t, self, args); },
    });
    return () => Array.from(seen.entries());
}

// The 2D context: glyphs 0.55 em wide, ascent 0.8 em, descent 0.2 em (the
// realworld state-mode measuring stub); every drawing call is a no-op.
function make2d(canvas) {
    const noop = () => {};
    return {
        canvas,
        font: '10px sans-serif', letterSpacing: '0px', textLetterSpacing: '0px', wordSpacing: '0px',
        textBaseline: 'alphabetic', textAlign: 'start', direction: 'inherit', lineWidth: 1, miterLimit: 10,
        lineJoin: 'miter', lineCap: 'butt', fillStyle: '#000', strokeStyle: '#000', shadowColor: 'rgba(0,0,0,0)',
        shadowBlur: 0, shadowOffsetX: 0, shadowOffsetY: 0, globalAlpha: 1, globalCompositeOperation: 'source-over',
        imageSmoothingEnabled: true, filter: 'none',
        measureText(s) {
            const m = /(\d+(?:\.\d+)?)px/.exec(this.font);
            const px = m ? Number(m[1]) : 10;
            const w = String(s).length * px * 0.55;
            return { width: w, actualBoundingBoxLeft: 0, actualBoundingBoxRight: w, actualBoundingBoxAscent: px * 0.8, actualBoundingBoxDescent: px * 0.2, fontBoundingBoxAscent: px * 0.8, fontBoundingBoxDescent: px * 0.2 };
        },
        resetTransform: noop, setTransform: noop, transform: noop, scale: noop, rotate: noop, translate: noop,
        clearRect: noop, fillRect: noop, strokeRect: noop, fillText: noop, strokeText: noop, save: noop, restore: noop,
        beginPath: noop, closePath: noop, moveTo: noop, lineTo: noop, arc: noop, rect: noop, fill: noop, stroke: noop,
        clip: noop, drawImage: noop, putImageData: noop, setLineDash: noop,
        getImageData(x, y, w, h) { return { width: w, height: h, data: new Uint8ClampedArray(w * h * 4) }; },
        createImageData(w, h) { return { width: w, height: h, data: new Uint8ClampedArray(w * h * 4) }; },
        createLinearGradient() { return { addColorStop: noop }; },
        createRadialGradient() { return { addColorStop: noop }; },
        createPattern() { return {}; },
    };
}

export function makeStubCanvas(width, height) {
    const canvas = {
        width: width || 0, height: height || 0, style: {}, _ctx2d: null, _gl: null,
        clientWidth: width || 0, clientHeight: height || 0,
        addEventListener() {}, removeEventListener() {}, setAttribute() {}, getAttribute() { return null; },
        getBoundingClientRect() { return { x: 0, y: 0, left: 0, top: 0, width: this.width, height: this.height, right: this.width, bottom: this.height }; },
        getContext(kind, attrs) {
            if (kind === '2d') {
                if (!this._ctx2d) this._ctx2d = make2d(this);
                return this._ctx2d;
            }
            if (kind === 'webgl2' || kind === 'webgl' || kind === 'experimental-webgl') {
                if (!this._gl) this._gl = new WebGL2RenderingContext(this, attrs);
                return this._gl;
            }
            return null;
        },
    };
    return canvas;
}

// pixi's DOMAdapter for bench mode: stub canvases, no fetch, no fonts; and
// text measured by the stub 2D context with pixi's own grapheme fallback
// (node has Intl.Segmenter and a bronze executable does not, so both are
// given the fallback and run the same code).
export function installPixiBench(PIXI) {
    PIXI.DOMAdapter.set({
        createCanvas: (w, h) => makeStubCanvas(w, h),
        createImage: () => ({}),
        getCanvasRenderingContext2D: () => Object,
        getWebGLRenderingContext: () => WebGLRenderingContext,
        getNavigator: () => globalThis.navigator,
        getBaseUrl: () => 'file:///',
        getFontFaceSet: () => null,
        fetch: () => { throw new Error('no fetch in mode=bench'); },
        parseXML: () => { throw new Error('no XML in mode=bench'); },
    });
    PIXI.CanvasTextMetrics.__canvas = makeStubCanvas(10, 10);
    PIXI.CanvasTextMetrics.__context = null;
    PIXI.CanvasTextMetrics._experimentalLetterSpacingSupported = false;
    PIXI.CanvasTextMetrics.graphemeSegmenter = (s) => [...s];
}
