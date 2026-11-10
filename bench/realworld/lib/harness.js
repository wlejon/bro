// Shared harness for the real-world scenes: configuration, the seeded PRNG,
// state hashing, output, and the frame loop. The same file runs in node
// (Level 1 reference), bro / bro-headless (bronze JIT or AOT) and Chromium.
//
// Configuration, later sources overriding earlier ones:
//   defaults  <  globalThis.BENCH_CONFIG (config.js, how bro gets it: its
//   location.search is always empty)  <  location.search (Chromium)  <
//   node argv `key=value`.
//
// Modes:
//   state   no renderer; K frames stepped, the renderer's CPU half emulated,
//           a state hash printed at each checkpoint frame (Level 1)
//   render  windowed frame loop through the real renderer (state hashes at
//           checkpoints too, read from the live renderer)
//   record  render, with every WebGL call of frames 0..rec-1 logged (Level 2b)
//   shot    render, readPixels of frame `shot` written out (Level 3)
//   time    render `warm` frames, then time `meas` more (lib/timing.js): the
//           rAF interval, the CPU time of the callback, the GPU time of its
//           WebGL work (EXT_disjoint_timer_query_webgl2), and in bro the
//           flight recorder's records of the same frames
//   bench   node or a bronze-built executable (ENV 'standalone'), no DOM, no
//           GPU: the library renders every frame into lib/glstub.js's stub
//           WebGL2 context; `warm` frames, then `frames` frames timed with
//           performance.now() in one synchronous loop (runBench)
//
// Time is the frame index / 60 s, never a clock.

import { installBenchGlobals, makeStubCanvas, captureGeneratedFunctions } from './bench_env.js';
import { makeRecorder } from './glrecord.js';
import { makeTimer } from './timing.js';

export const ENV = (typeof bro !== 'undefined' && bro !== null) ? 'bro'
    : (typeof window === 'undefined' && typeof process !== 'undefined' && process.versions && process.versions.node) ? 'node'
    : (typeof window === 'undefined' && typeof document === 'undefined') ? 'standalone'
    : 'browser';

const DEFAULTS = {
    scene: '', n: 0, mode: 'render', frames: 0, cp: '', seed: 12345,
    w: 1280, h: 720, aa: 0, shot: 30, rec: 3, shadows: 0, out: '', detail: 0,
    quit: 1, watchdog: 30000, tag: '', warm: 120, meas: 300, gpu: 1, mark: 0,
};

export function readConfig(sceneDefaults) {
    const cfg = Object.assign({}, DEFAULTS, sceneDefaults || {});
    const set = (k, v) => {
        if (typeof cfg[k] === 'number' || (DEFAULTS[k] === undefined && /^-?\d+(\.\d+)?$/.test(String(v)))) cfg[k] = Number(v);
        else cfg[k] = v;
    };
    const bc = globalThis.BENCH_CONFIG;
    if (bc) for (const k of Object.keys(bc)) set(k, bc[k]);
    // A bronze-built executable has no arguments (its host passes none to the
    // program): its configuration is a BENCH_CONFIG compiled in (rw.py js),
    // and without one it benches at the scene's defaults.
    if (ENV === 'standalone' && !(bc && bc.mode)) cfg.mode = 'bench';
    if (ENV === 'browser' && typeof location !== 'undefined' && location.search) {
        for (const [k, v] of new URLSearchParams(location.search)) set(k, v);
    }
    if (ENV === 'node') {
        for (const a of process.argv.slice(2)) {
            const i = a.indexOf('=');
            if (i > 0) set(a.slice(0, i), a.slice(i + 1));
        }
    }
    if (cfg.mode === 'time') cfg.frames = cfg.warm + cfg.meas;
    if (!cfg.frames) cfg.frames = cfg.mode === 'state' ? 120 : (cfg.mode === 'shot' ? cfg.shot + 1 : (cfg.mode === 'record' ? cfg.rec : (cfg.mode === 'bench' ? 300 : 600)));
    cfg.checkpoints = new Set((cfg.cp ? String(cfg.cp) : `0,1,2,30,60,${cfg.frames - 1}`).split(',').map(Number));
    return cfg;
}

// mulberry32: integer-only, so every engine produces the same sequence.
export function makeRng(seed) {
    let a = seed >>> 0;
    const rng = () => {
        a = (a + 0x6D2B79F5) | 0;
        let t = Math.imul(a ^ (a >>> 15), 1 | a);
        t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
    rng.range = (lo, hi) => lo + (hi - lo) * rng();
    rng.int = (n) => Math.floor(rng() * n);
    rng.pick = (arr) => arr[Math.floor(rng() * arr.length)];
    return rng;
}

// Two FNV-1a lanes: `exact` over the float64 bits (NaN canonicalised), `f32`
// over Math.fround of each value with -0 folded to 0. An exact mismatch with
// an f32 match is a last-ulp difference (libm); an f32 mismatch is real.
const F64 = new Float64Array(1), F64U = new Uint32Array(F64.buffer);
const F32 = new Float32Array(1), F32U = new Uint32Array(F32.buffer);
function fnv(h, x) {
    h = Math.imul(h ^ (x & 0xff), 16777619);
    h = Math.imul(h ^ ((x >>> 8) & 0xff), 16777619);
    h = Math.imul(h ^ ((x >>> 16) & 0xff), 16777619);
    return Math.imul(h ^ (x >>> 24), 16777619);
}
export class Hasher {
    constructor() { this.a = 0x811c9dc5 | 0; this.f = 0x811c9dc5 | 0; this.count = 0; }
    num(v) {
        if (v !== v) v = NaN;
        F64[0] = v;
        this.a = fnv(fnv(this.a, F64U[0]), F64U[1]);
        F32[0] = v === 0 ? 0 : v;
        this.f = fnv(this.f, F32U[0]);
        this.count++;
    }
    arr(a, start = 0, end = a.length) { for (let i = start; i < end; i++) this.num(a[i]); }
    str(s) { const t = String(s); for (let i = 0; i < t.length; i++) this.num(t.charCodeAt(i)); this.num(-1); }
    get exact() { return (this.a >>> 0).toString(16).padStart(8, '0'); }
    get f32() { return (this.f >>> 0).toString(16).padStart(8, '0'); }
    get both() { return this.exact + ':' + this.f32 + ':' + this.count; }
}

// ---- output ---------------------------------------------------------------

const lines = [];
let CFG = null;

export function emitLine(tag, obj) {
    const s = 'RW ' + tag + ' ' + JSON.stringify(obj);
    lines.push(s);
    console.log(s);
}

function nodeFs() { return process.getBuiltinModule('fs'); }

export async function emitBlob(name, data) {
    if (ENV === 'browser') {
        // Chromium runs the page from file:// (it cannot reach a localhost
        // server from this sandbox), so blobs leave as base64 console lines:
        //   RWBLOB <name> <index> <count> <base64>
        const u8 = typeof data === 'string' ? new TextEncoder().encode(data) : new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
        const CH = 24576;
        const count = Math.max(1, Math.ceil(u8.length / CH));
        for (let i = 0; i < count; i++) {
            const part = u8.subarray(i * CH, (i + 1) * CH);
            let s = '';
            for (let k = 0; k < part.length; k += 4096) s += String.fromCharCode.apply(null, part.subarray(k, k + 4096));
            console.log('RWBLOB ' + name + ' ' + i + ' ' + count + ' ' + btoa(s));
        }
        return;
    }
    if (!CFG || !CFG.out) return;
    const fs = ENV === 'bro' ? require('fs') : nodeFs();
    fs.writeFileSync(CFG.out + '/' + name, data);
}

let finished = false;
export async function done(status) {
    if (finished) return;
    finished = true;
    if (CFG && (CFG.out || ENV === 'browser')) {
        try { await emitBlob('lines.txt', lines.join('\n') + '\n'); } catch (e) { console.log('RW post-failed ' + e); }
    }
    console.log('RW_DONE ' + status);
    if (ENV === 'bro' && CFG && CFG.quit && typeof bro.quit === 'function') bro.quit();
}

// For pages that are not scenes (surface.html): read the config and route output.
export function configure(defaults) {
    CFG = readConfig(defaults);
    return CFG;
}

function fail(e) {
    emitLine('error', { msg: String(e && e.message || e), stack: String(e && e.stack || '') });
    done('error');
}

// ---- canvas + recorder ------------------------------------------------------

// The recorder patches the canvas instance's getContext (an own property
// shadowing HTMLCanvasElement.prototype's, so only this canvas is recorded)
// before the library creates its context.
export function makeCanvas(cfg, recorder) {
    const canvas = document.createElement('canvas');
    canvas.width = cfg.w;
    canvas.height = cfg.h;
    canvas.style.width = cfg.w + 'px';
    canvas.style.height = cfg.h + 'px';
    canvas.style.display = 'block';
    document.body.appendChild(canvas);
    if (recorder) {
        const orig = canvas.getContext;
        canvas.getContext = function (type, attrs) {
            recorder.log('getContext', [type, attrs], null);
            const ctx = orig.call(this, type, attrs);
            if (ctx && (type === 'webgl2' || type === 'webgl' || type === 'experimental-webgl')) return recorder.wrap(ctx);
            return ctx;
        };
    }
    return canvas;
}

// ---- the loop ---------------------------------------------------------------

// def: { name, lib, defaults, start(cfg, h) -> { frame(f), state(), gl(), readback? } }
export function run(def) {
    let cfg;
    try {
        cfg = readConfig(def.defaults);
        cfg.scene = def.name;
        CFG = cfg;
    } catch (e) { console.log('RW_DONE config-error ' + e); return; }
    if (ENV === 'browser' || ENV === 'bro') {
        if (typeof document !== 'undefined' && document.body) {
            document.body.style.margin = '0';
            document.body.style.background = '#000';
        }
    }
    try {
        if (cfg.mode === 'state') runState(def, cfg);
        else if (cfg.mode === 'bench') runBench(def, cfg);
        else runLoop(def, cfg).catch(fail);
    } catch (e) { fail(e); }
}

function header(cfg, extra) {
    emitLine('config', Object.assign({ env: ENV, scene: cfg.scene, n: cfg.n, mode: cfg.mode, frames: cfg.frames, seed: cfg.seed, w: cfg.w, h: cfg.h, aa: cfg.aa, shadows: cfg.shadows, tag: cfg.tag }, extra || {}));
}

function runState(def, cfg) {
    const res = def.start(cfg, null);
    const go = (inst) => {
        header(cfg);
        for (let f = 0; f < cfg.frames; f++) {
            inst.frame(f);
            if (cfg.checkpoints.has(f)) emitLine('state', Object.assign({ f }, inst.state(cfg.detail)));
        }
        done('ok');
    };
    if (res && typeof res.then === 'function') res.then(go, fail);
    else go(res);
}

// mode=bench: the browser page's frame (scene step + library render) into the
// stub context, `warm` frames, then `frames` timed in one synchronous loop.
// The checksum folds the scene's state (as `state` reports it, with each
// float64 hash reduced to its float32 lane: libm may differ in the last ulp
// between engines, rw.py l1's `ulp`) and the last frame's GL call counts.
function runBench(def, cfg) {
    installBenchGlobals();
    const captured = cfg.capture && ENV === 'node' ? captureGeneratedFunctions() : null;
    const canvas = makeStubCanvas(cfg.w, cfg.h);
    const go = (inst) => {
        const gl = canvas._gl;
        if (!gl) throw new Error('the library created no WebGL context');
        header(cfg, inst.info ? inst.info() : null);
        // mark=1 (rw.py js): a line before and after the timed loop, each
        // padded past the pipe's capacity so the write blocks until rw.py has
        // read the marker and switched `perf stat` counting on or off; the
        // counts then cover the timed frames and nothing else.
        const pad = cfg.mark ? ' '.repeat(65536) : '';
        const markOn = 'RW mark on' + pad, markOff = 'RW mark off' + pad;
        let f = 0;
        for (; f < cfg.warm; f++) { gl.beginFrame(); inst.frame(f); }
        const before = gl.totals();
        if (cfg.mark) console.log(markOn);
        const t0 = performance.now();
        for (let k = 0; k < cfg.frames; k++, f++) { gl.beginFrame(); inst.frame(f); }
        const ms = performance.now() - t0;
        if (cfg.mark) console.log(markOff);
        const after = gl.totals();
        const last = gl.frameCounts();
        const state = inst.state(0);
        emitLine('state', Object.assign({ f: f - 1 }, state));
        const per = (k) => Math.round((after[k] - before[k]) / cfg.frames * 100) / 100;
        const h = new Hasher();
        h.str(checksumText(state));
        h.num(last.calls); h.num(last.draws); h.num(last.uniforms); h.num(last.uploads);
        emitLine('bench', {
            scene: cfg.scene, n: cfg.n, warm: cfg.warm, frames: cfg.frames, env: ENV,
            ms_per_frame: Math.round(ms / cfg.frames * 10000) / 10000,
            draw_calls_per_frame: per('draws'), gl_calls_per_frame: per('calls'),
            uniform_calls_per_frame: per('uniforms'), uploads_per_frame: per('uploads'),
            last: last, checksum: h.exact,
        });
        if (captured) nodeFs().writeFileSync(cfg.capture, JSON.stringify(captured()));
        done('ok');
    };
    const res = def.start(cfg, { canvas, bench: true });
    if (res && typeof res.then === 'function') res.then((inst) => { try { go(inst); } catch (e) { fail(e); } }, fail);
    else go(res);
}

function checksumText(state) {
    const norm = (v) => {
        if (typeof v === 'string' && /^[0-9a-f]{8}:[0-9a-f]{8}:\d+$/.test(v)) return v.slice(9);
        if (typeof v === 'number') return Number.isInteger(v) ? v : Math.fround(v).toPrecision(6);
        if (Array.isArray(v)) return v.map(norm);
        return v;
    };
    const keys = Object.keys(state).sort();
    return keys.map((k) => k + '=' + JSON.stringify(norm(state[k]))).join(';');
}

async function runLoop(def, cfg) {
    const recorder = cfg.mode === 'record' ? makeRecorder() : null;
    const canvas = makeCanvas(cfg, recorder);
    setTimeout(() => { emitLine('error', { msg: 'watchdog' }); done('watchdog'); }, cfg.watchdog);
    const inst = await def.start(cfg, { canvas, recorder });
    header(cfg, inst.info ? inst.info() : null);
    if (cfg.mode === 'time') return timeLoop(inst, cfg);
    let f = 0;
    const tick = async () => {
        try {
            if (recorder) recorder.frame = f;
            inst.frame(f);
            if (cfg.checkpoints.has(f)) emitLine('state', Object.assign({ f }, inst.state(cfg.detail)));
            if (cfg.mode === 'shot' && f === cfg.shot) {
                const gl = inst.gl();
                const px = new Uint8Array(cfg.w * cfg.h * 4);
                gl.bindFramebuffer(gl.FRAMEBUFFER, null);
                gl.readPixels(0, 0, cfg.w, cfg.h, gl.RGBA, gl.UNSIGNED_BYTE, px);
                emitLine('shot', { f, w: cfg.w, h: cfg.h, err: gl.getError() });
                await emitBlob('shot.rgba', px);
                done('ok');
                return;
            }
            if (recorder && f === cfg.rec - 1) {
                recorder.enabled = false;
                await emitBlob('calls.txt', recorder.text());
                done('ok');
                return;
            }
            f++;
            if (f < cfg.frames) requestAnimationFrame(tick);
            else done('ok');
        } catch (e) { fail(e); }
    };
    requestAnimationFrame(tick);
}

// mode=time: no state hashing inside the loop (only frame 0's, before any
// timing), nothing but the scene's own frame inside the timed region.
function timeLoop(inst, cfg) {
    const timer = makeTimer(cfg, inst.gl());
    let f = 0;
    const body = () => inst.frame(f);
    const tick = async (ts) => {
        try {
            const more = timer.frame(f, ts, body);
            if (f === 0) emitLine('state', Object.assign({ f }, inst.state(0)));
            f++;
            if (more) { requestAnimationFrame(tick); return; }
            emitLine('timing', timer.result());
            const tr = timer.trace();
            if (tr) {
                if (tr.ok) await emitBlob('trace.json', tr.payload);
                else emitLine('error', { msg: 'trace: ' + tr.payload });
            }
            done('ok');
        } catch (e) { fail(e); }
    };
    requestAnimationFrame(tick);
}
