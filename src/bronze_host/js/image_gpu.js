// bro.image.gpu — GPU counterparts to bro.image, the CPU kernels backed by
// the broimage C++ library (surfaced via brokit).
//
// This module lives in bro, not broimage: it is brokernel kernels dispatched
// with bro.kernel on the engine's Vulkan device, shown on a canvas through
// getContext('kernel'), the canvas that composites a KernelBuffer's pixels
// (docs/kernel-api.js). broimage is a CPU library. The shared
// `bro.image.gpu` namespace pairs these with the CPU kernels (CPU `lookup`
// and GPU `colormap` both take a LUT built by `bro.image.gradient`).
//
// colormap(canvas, src, lut, params) colours a float field through the LUT;
// fbm2D(canvas, lut, params) makes a Simplex FBm field on the GPU and colours
// it the same way. A field's row 0 is the canvas's top row, as with the CPU
// `lookup` + putImageData.
//
//   params.lo / params.hi      — explicit range
//   params.autoRange           — lo/hi found on the GPU: the field reduced to
//                                its (min, max) in 8×8 blocks, level by level,
//                                to one cell, blended into a range kept per
//                                canvas (params.ema, default 0.02; the first
//                                frame, and the first after a resize, takes
//                                the field's own range). The CPU never sees it.
//   params.srcW / params.srcH  — field dims; default to the canvas's
//   params.viewRect            — {x, y, w, h}: the part of the field the
//                                canvas shows, sampled bilinearly
//   params.regenerate:false    — colour the field kept from the last call
//
// Per-canvas state (the canvas's kernel context, the field, LUT, reduce
// levels, range and output buffers) lives in a WeakMap keyed by the canvas,
// re-made when the field's or the canvas's size changes.
//
// HOW IT IS SHIPPED. Compiled by bronze at build time (bro_compile_js in
// ../CMakeLists.txt, against js/module.globals) and entered from
// installImageGpuModule() (host_js_modules.cpp) while the host globals are
// being installed. The module assigns its surface — `colormap` and `fbm2D` —
// onto `globalThis.__bro_image_gpu`, and the installer mounts that object as
// `bro.image.gpu`, on the same `bro.image` every other member lives on. The
// kernels compile at the first call.

(function () {
    const gpu = {};

    // ----- Kernels -----------------------------------------------------------

    const KERNELS = `
// Stefan Gustavson / Ian McEwan (Ashima Arts) Simplex 2D, MIT.
fn mod289_3(x: vec3<f32>) -> vec3<f32> { return x - floor(x * (1.0 / 289.0)) * 289.0; }
fn mod289_2(x: vec2<f32>) -> vec2<f32> { return x - floor(x * (1.0 / 289.0)) * 289.0; }
fn permute(x: vec3<f32>) -> vec3<f32> { return mod289_3(((x * 34.0) + 1.0) * x); }

fn snoise(v: vec2<f32>) -> f32 {
    let cx = 0.211324865405187;
    let cy = 0.366025403784439;
    let cz = -0.577350269189626;
    let cw = 0.024390243902439;
    let i0 = floor(v + (v.x + v.y) * cy);
    let x0 = v - i0 + (i0.x + i0.y) * cx;
    let i1 = select(x0.x > x0.y, vec2<f32>(1.0, 0.0), vec2<f32>(0.0, 1.0));
    let x1 = x0 + cx - i1;
    let x2 = x0 + cz;
    let i = mod289_2(i0);
    let p = permute(permute(i.y + vec3<f32>(0.0, i1.y, 1.0)) + i.x + vec3<f32>(0.0, i1.x, 1.0));
    var m = max(vec3<f32>(0.5, 0.5, 0.5) - vec3<f32>(dot(x0, x0), dot(x1, x1), dot(x2, x2)),
                vec3<f32>(0.0, 0.0, 0.0));
    m = m * m;
    m = m * m;
    let x = fract(p * cw) * 2.0 - 1.0;
    let h = abs(x) - 0.5;
    let a0 = x - floor(x + 0.5);
    m = m * (vec3<f32>(1.79284291400159, 1.79284291400159, 1.79284291400159) - (a0 * a0 + h * h) * 0.85373472095314);
    let gv = vec3<f32>(a0.x * x0.x + h.x * x0.y, a0.y * x1.x + h.y * x1.y, a0.z * x2.x + h.z * x2.y);
    return 130.0 * dot(m, gv);
}

// FBm, not normalised by the octaves' amplitude sum (FastNoise2's FractalFBm
// shape; autoRange compensates). The seed offsets the lattice so seeds differ
// and lacunarity does not pull every octave through the origin.
@kernel @grid(2)
fn k_fbm(g: grid<2>, out: write_store<f32, 2>, origin: vec2<f32>, frequency: f32,
         octaves: i32, gain: f32, lacunarity: f32, seed: f32) {
    let base = (vec2<f32>(g.x() as f32, g.y() as f32) + origin) * frequency;
    let ofs = vec2<f32>(seed * 17.0, seed * 31.0);
    var total: f32 = 0.0;
    var amp: f32 = 1.0;
    var freq: f32 = 1.0;
    for o in 0..octaves {
        total += amp * snoise(base * freq + ofs);
        freq *= lacunarity;
        amp *= gain;
    }
    out[g.x(), g.y()] = total;
}

// A field's 8×8 block of cells to its (min, max); blocks past the edge
// repeat the edge cells.
@kernel @grid(2)
fn k_reduce_field(g: grid<2>, src: read_store<f32, 1>, out: write_store<vec2<f32>, 2>, w: i32, h: i32) {
    let bx = g.x() * 8;
    let by = g.y() * 8;
    let first = src[by * w + bx];
    var mn: f32 = first;
    var mx: f32 = first;
    for j in 0..8 {
        let row = min(by + j, h - 1) * w;
        for i in 0..8 {
            let v = src[row + min(bx + i, w - 1)];
            mn = min(mn, v);
            mx = max(mx, v);
        }
    }
    out[g.x(), g.y()] = vec2<f32>(mn, mx);
}

// An 8×8 block of (min, max) pairs to one.
@kernel @grid(2)
fn k_reduce_pairs(g: grid<2>, src: read_store<vec2<f32>, 1>, out: write_store<vec2<f32>, 2>, w: i32, h: i32) {
    let bx = g.x() * 8;
    let by = g.y() * 8;
    var r = src[by * w + bx];
    for j in 0..8 {
        let row = min(by + j, h - 1) * w;
        for i in 0..8 {
            let v = src[row + min(bx + i, w - 1)];
            r = vec2<f32>(min(r.x, v.x), max(r.y, v.y));
        }
    }
    out[g.x(), g.y()] = r;
}

// The kept range moved k of the way to the field's.
@kernel @grid(1)
fn k_ema(g: grid<1>, src: read_store<vec2<f32>, 1>, state: rw_store<vec2<f32>, 1>, k: f32) {
    let p = state[0];
    state[0] = p + (src[0] - p) * k;
}

// The canvas's cell (x, y) samples the field bilinearly (edges clamped) at
// view.xy * uv + view.zw, maps it through lo / inv_span (or the kept range)
// to the LUT, sampled linearly, and packs RGBA8.
@kernel @grid(2)
fn k_colormap(g: grid<2>, field: read_store<f32, 1>, lut: read_store<u32, 1>,
              range_state: read_store<vec2<f32>, 1>, out: write_store<u32, 2>,
              src_size: vec2<i32>, lut_n: i32, canvas_size: vec2<f32>, view: vec4<f32>,
              lo_in: f32, inv_span_in: f32, auto_range: i32) {
    let uv = vec2<f32>((g.x() as f32 + 0.5) / canvas_size.x, (g.y() as f32 + 0.5) / canvas_size.y);
    let fx = (uv.x * view.x + view.z) * (src_size.x as f32) - 0.5;
    let fy = (uv.y * view.y + view.w) * (src_size.y as f32) - 0.5;
    let x0 = floor(fx);
    let y0 = floor(fy);
    let tx = fx - x0;
    let ty = fy - y0;
    let xa = clamp(x0 as i32, 0, src_size.x - 1);
    let xb = clamp(x0 as i32 + 1, 0, src_size.x - 1);
    let ya = clamp(y0 as i32, 0, src_size.y - 1) * src_size.x;
    let yb = clamp(y0 as i32 + 1, 0, src_size.y - 1) * src_size.x;
    let top = mix(field[ya + xa], field[ya + xb], tx);
    let bottom = mix(field[yb + xa], field[yb + xb], tx);
    let v = mix(top, bottom, ty);

    var lo = lo_in;
    var inv_span = inv_span_in;
    if auto_range != 0 {
        let r = range_state[0];
        lo = r.x;
        inv_span = select(r.y > r.x, 1.0 / (r.y - r.x), 0.0);
    }
    let t = clamp((v - lo) * inv_span, 0.0, 1.0);
    let u = clamp(t * (lut_n as f32) - 0.5, 0.0, (lut_n - 1) as f32);
    let ua = floor(u);
    let ia = ua as i32;
    let ib = min(ia + 1, lut_n - 1);
    let c = mix(unpack_unorm4x8(lut[ia]), unpack_unorm4x8(lut[ib]), u - ua);
    let q = clamp(c, vec4<f32>(0.0, 0.0, 0.0, 0.0), vec4<f32>(1.0, 1.0, 1.0, 1.0)) * 255.0 + 0.5;
    out[g.x(), g.y()] = (q.x as u32) | ((q.y as u32) << 8u) | ((q.z as u32) << 16u) | ((q.w as u32) << 24u);
}
`;

    let K = null;
    function kernels(fn) {
        const k = bro.kernel;
        if (!k || k.available === false)
            throw new Error("bro.image.gpu." + fn + ": this build has no bro.kernel");
        if (!k.gpu)
            throw new Error("bro.image.gpu." + fn + ": no GPU (bro.kernel.gpu is false)");
        if (K) return K;
        const mod = k.compile(KERNELS, { file: "image_gpu.bk" });
        // A kernel's handle builds its GPU program: each is taken when first
        // used, so a page that only colours never builds the FBm.
        const lazy = (name) => {
            let handle = null;
            return () => handle || (handle = mod.kernel(name));
        };
        K = {
            fbm: lazy("k_fbm"),
            reduceField: lazy("k_reduce_field"),
            reducePairs: lazy("k_reduce_pairs"),
            ema: lazy("k_ema"),
            colormap: lazy("k_colormap"),
        };
        return K;
    }

    // ----- Per-canvas state -------------------------------------------------

    const stateByCanvas = new WeakMap();

    function getState(canvas, fn) {
        let st = stateByCanvas.get(canvas);
        if (st) return st;
        const ctx = canvas.getContext("kernel");
        if (!ctx)
            throw new Error("bro.image.gpu." + fn + ": the canvas has no 'kernel' context " +
                            "(it has another context, or is in an iframe)");
        st = {
            ctx,
            field: null, srcW: 0, srcH: 0,
            lut: null, lutBytes: 0,
            levels: [],             // [{buf, w, h}] 8×8-block reductions down to 1×1
            range: bro.kernel.createBuffer(8),
            hasRange: false,
            out: null, outW: 0, outH: 0,
        };
        stateByCanvas.set(canvas, st);
        return st;
    }

    // The field buffer at srcW × srcH; a new size drops the reduce levels and
    // the kept range. Returns whether it was re-made.
    function ensureField(st, srcW, srcH) {
        if (st.field && srcW === st.srcW && srcH === st.srcH) return false;
        st.field = bro.kernel.createBuffer(srcW * srcH * 4);
        st.srcW = srcW; st.srcH = srcH;
        st.levels.length = 0;
        st.hasRange = false;
        return true;
    }

    function checkLut(fn, lut) {
        if (!(lut instanceof Uint8Array) && !(lut instanceof Uint8ClampedArray))
            throw new TypeError("bro.image.gpu." + fn + ": lut must be Uint8Array");
        if ((lut.byteLength & 3) !== 0)
            throw new RangeError("bro.image.gpu." + fn + ": lut length must be a multiple of 4");
        if ((lut.byteLength >> 2) < 2)
            throw new RangeError("bro.image.gpu." + fn + ": lut needs >= 2 entries");
    }

    function uploadLut(st, lut) {
        if (lut.byteLength !== st.lutBytes) {
            st.lut = bro.kernel.createBuffer(lut.byteLength);
            st.lutBytes = lut.byteLength;
        }
        st.lut.write(lut);
    }

    // Reduce the field to (min, max) and blend it into the kept range.
    function updateRange(st, k, ema) {
        if (st.levels.length === 0) {
            let w = st.srcW, h = st.srcH;
            do {
                w = Math.ceil(w / 8); h = Math.ceil(h / 8);
                st.levels.push({ buf: bro.kernel.createBuffer(w * h * 8), w, h });
            } while (w > 1 || h > 1);
        }
        let src = st.field, w = st.srcW, h = st.srcH;
        for (let i = 0; i < st.levels.length; i++) {
            const lvl = st.levels[i];
            (i === 0 ? k.reduceField() : k.reducePairs()).dispatch([lvl.w, lvl.h], src, lvl.buf, w, h);
            src = lvl.buf; w = lvl.w; h = lvl.h;
        }
        const blend = st.hasRange ? Math.max(0, Math.min(1, ema)) : 1.0;
        k.ema().dispatch(1, src, st.range, blend);
        st.hasRange = true;
    }

    function drawColormap(st, k, canvas, params) {
        const cw = canvas.width | 0, ch = canvas.height | 0;
        if (cw <= 0 || ch <= 0) return;
        if (!st.out || cw !== st.outW || ch !== st.outH) {
            st.out = bro.kernel.createBuffer(cw * ch * 4);
            st.outW = cw; st.outH = ch;
        }
        const autoRange = !!params.autoRange;
        if (autoRange) updateRange(st, k, params.ema != null ? +params.ema : 0.02);
        let view = [1, 1, 0, 0];
        if (params.viewRect) {
            const v = params.viewRect;
            const sw = Math.max(1, st.srcW), sh = Math.max(1, st.srcH);
            view = [(+v.w) / sw, (+v.h) / sh, (+v.x) / sw, (+v.y) / sh];
        }
        const lo = autoRange ? 0 : +params.lo;
        const hi = autoRange ? 0 : +params.hi;
        k.colormap().dispatch([cw, ch], st.field, st.lut, st.range, st.out,
            [st.srcW, st.srcH], st.lutBytes >> 2, [cw, ch], view,
            lo, (hi > lo) ? 1.0 / (hi - lo) : 0.0, autoRange ? 1 : 0);
        st.ctx.present(st.out);
    }

    function fieldSize(fn, canvas, params) {
        const srcW = (params.srcW != null ? params.srcW : canvas.width) | 0;
        const srcH = (params.srcH != null ? params.srcH : canvas.height) | 0;
        if (srcW <= 0 || srcH <= 0)
            throw new RangeError("bro.image.gpu." + fn + ": srcW/srcH must be positive");
        return [srcW, srcH];
    }

    // ----- public colormap() ------------------------------------------------

    /**
     * Colormap a 1-channel float field through a 1D RGBA8 LUT onto `canvas`.
     */
    gpu.colormap = function colormap(canvas, src, lut, params) {
        if (!canvas || canvas.nodeType !== 1)
            throw new TypeError("bro.image.gpu.colormap: canvas required");
        checkLut("colormap", lut);
        params = params || {};
        const regenerate = (params.regenerate !== false);
        if (regenerate) {
            if (!(src instanceof Float32Array))
                throw new TypeError("bro.image.gpu.colormap: src must be Float32Array");
            const [srcW, srcH] = fieldSize("colormap", canvas, params);
            if (src.length < srcW * srcH)
                throw new RangeError("bro.image.gpu.colormap: src too small");
            const k = kernels("colormap");
            const st = getState(canvas, "colormap");
            ensureField(st, srcW, srcH);
            st.field.write(src.length === srcW * srcH ? src : src.subarray(0, srcW * srcH));
            uploadLut(st, lut);
            drawColormap(st, k, canvas, params);
            return;
        }
        const k = kernels("colormap");
        const st = getState(canvas, "colormap");
        if (!st.field)
            throw new Error("bro.image.gpu.colormap: regenerate:false but no cached " +
                            "field for this canvas — call once with src first");
        uploadLut(st, lut);
        drawColormap(st, k, canvas, params);
    };

    // ----- FBm-on-GPU --------------------------------------------------------

    /**
     * Generate a 2D Simplex FBm field on the GPU and colormap it to `canvas`.
     * The field never reaches the CPU: it is made in a KernelBuffer and
     * coloured by the same pass as colormap().
     *
     * type 'Simplex' (the default) only: the other FastNoise2 types are the
     * CPU genUniformGrid2DInto + colormap path.
     */
    gpu.fbm2D = function fbm2D(canvas, lut, params) {
        if (!canvas || canvas.nodeType !== 1)
            throw new TypeError("bro.image.gpu.fbm2D: canvas required");
        if (!(lut instanceof Uint8Array) && !(lut instanceof Uint8ClampedArray))
            throw new TypeError("bro.image.gpu.fbm2D: lut must be Uint8Array");
        params = params || {};
        const regenerate = (params.regenerate !== false);

        // regenerate:false colours the field kept from the last call — a wide
        // tile made once every N frames with a viewRect slid across it.
        if (!regenerate) {
            const k = kernels("fbm2D");
            const st = getState(canvas, "fbm2D");
            if (!st.field)
                throw new Error("bro.image.gpu.fbm2D: regenerate:false but no cached noise " +
                                "field for this canvas — call once with regenerate:true first");
            checkLut("fbm2D", lut);
            uploadLut(st, lut);
            drawColormap(st, k, canvas, params);
            return;
        }

        const type = params.type || 'Simplex';
        if (type !== 'Simplex') {
            throw new Error(
                "bro.image.gpu.fbm2D: type '" + type + "' not implemented " +
                "(only 'Simplex'). Use the CPU genUniformGrid2DInto + " +
                "bro.image.gpu.colormap path for other types."
            );
        }
        const [srcW, srcH] = fieldSize("fbm2D", canvas, params);
        checkLut("fbm2D", lut);
        const octaves = (params.octaves != null) ? (params.octaves | 0) : 1;
        if (octaves < 1 || octaves > 16)
            throw new RangeError("bro.image.gpu.fbm2D: octaves must be in [1,16]");

        const k = kernels("fbm2D");
        const st = getState(canvas, "fbm2D");
        ensureField(st, srcW, srcH);
        uploadLut(st, lut);
        k.fbm().dispatch([srcW, srcH], st.field,
            [+(params.ox || 0), +(params.oy || 0)], +params.frequency, octaves,
            params.gain != null ? +params.gain : 0.5,
            params.lacunarity != null ? +params.lacunarity : 2.0,
            +(params.seed || 0));
        drawColormap(st, k, canvas, params);
    };

    globalThis.__bro_image_gpu = gpu;
})();
