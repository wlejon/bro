// bro.image.gpu — hardware-accelerated counterparts to bro.image (CPU/brokit).
//
// V1 surface: colormap(canvas, src, lut, params).
//
//   params.lo / params.hi      — explicit range (the original mode)
//   params.autoRange           — when true, lo/hi are computed on the GPU
//                                via a parallel min/max reduction over the
//                                R32F field, with optional EMA smoothing.
//   params.ema                 — EMA blend factor (0..1). Default 0.02.
//                                Only used when autoRange is true.
//   params.srcW / params.srcH  — source field dims; default to canvas dims.
//
// In autoRange mode the caller does NOT supply lo/hi. The CPU never sees the
// range: the reduce chain leaves a 1×1 RG32F "(lo, hi)" texture that the
// colormap fragment shader samples directly. EMA smoothing is implemented as
// a tiny ping-pong pass between two persistent 1×1 textures, so successive
// frames track a slow-moving range without ever stalling the GPU.
//
// All per-canvas resources (programs, VAO, textures, framebuffers, reduction
// chain) live in a WeakMap keyed by the canvas. The reduction chain is
// (re)built when srcW/srcH changes; the EMA state is reset along with it.

(function () {
    if (typeof bro !== "object" || bro === null) {
        globalThis.bro = globalThis.bro || {};
    }
    if (!bro.image) bro.image = {};
    if (bro.image.gpu) return;  // already installed
    bro.image.gpu = {};

    // ----- Shader sources ----------------------------------------------------

    const FULLSCREEN_VS = `#version 300 es
in vec2 aPos;
out vec2 vUv;
void main() {
    vUv = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}`;

    // Original mode: lo/hi as uniforms.
    const COLORMAP_UNIFORM_FS = `#version 300 es
precision highp float;
uniform sampler2D uNoise;
uniform sampler2D uLut;
uniform float uLo;
uniform float uInvSpan;
in vec2 vUv;
out vec4 fragColor;
void main() {
    float v = texture(uNoise, vUv).r;
    float t = (v - uLo) * uInvSpan;
    t = clamp(t, 0.0, 1.0);
    fragColor = texture(uLut, vec2(t, 0.5));
}`;

    // autoRange mode: range sampled from a 1×1 RG32F texture (R=lo, G=hi).
    const COLORMAP_AUTO_FS = `#version 300 es
precision highp float;
uniform sampler2D uNoise;
uniform sampler2D uLut;
uniform sampler2D uRange;
in vec2 vUv;
out vec4 fragColor;
void main() {
    vec2 r = texelFetch(uRange, ivec2(0), 0).rg;
    float lo = r.r;
    float hi = r.g;
    float invSpan = (hi > lo) ? (1.0 / (hi - lo)) : 0.0;
    float v = texture(uNoise, vUv).r;
    float t = clamp((v - lo) * invSpan, 0.0, 1.0);
    fragColor = texture(uLut, vec2(t, 0.5));
}`;

    // Reduction from an R32F source: scalar field → (min,max). 2×2 fetch per
    // output texel, clamped at the source edges so odd source dims still
    // contribute (instead of reading garbage outside the bound region).
    const REDUCE_FROM_R_FS = `#version 300 es
precision highp float;
uniform sampler2D uSrc;
uniform ivec2 uSrcSize;
out vec4 fragColor;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 base = p * 2;
    int x1 = min(base.x + 1, uSrcSize.x - 1);
    int y1 = min(base.y + 1, uSrcSize.y - 1);
    float a = texelFetch(uSrc, ivec2(base.x, base.y), 0).r;
    float b = texelFetch(uSrc, ivec2(x1,     base.y), 0).r;
    float c = texelFetch(uSrc, ivec2(base.x, y1    ), 0).r;
    float d = texelFetch(uSrc, ivec2(x1,     y1    ), 0).r;
    float mn = min(min(a, b), min(c, d));
    float mx = max(max(a, b), max(c, d));
    fragColor = vec4(mn, mx, 0.0, 0.0);
}`;

    // Reduction from an RG32F source: (min,max) pair → smaller (min,max).
    const REDUCE_FROM_RG_FS = `#version 300 es
precision highp float;
uniform sampler2D uSrc;
uniform ivec2 uSrcSize;
out vec4 fragColor;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 base = p * 2;
    int x1 = min(base.x + 1, uSrcSize.x - 1);
    int y1 = min(base.y + 1, uSrcSize.y - 1);
    vec2 a = texelFetch(uSrc, ivec2(base.x, base.y), 0).rg;
    vec2 b = texelFetch(uSrc, ivec2(x1,     base.y), 0).rg;
    vec2 c = texelFetch(uSrc, ivec2(base.x, y1    ), 0).rg;
    vec2 d = texelFetch(uSrc, ivec2(x1,     y1    ), 0).rg;
    float mn = min(min(a.r, b.r), min(c.r, d.r));
    float mx = max(max(a.g, b.g), max(c.g, d.g));
    fragColor = vec4(mn, mx, 0.0, 0.0);
}`;

    // EMA blend: out = mix(prev, new, k). Operates on 1×1 RG32F textures.
    const EMA_FS = `#version 300 es
precision highp float;
uniform sampler2D uPrev;
uniform sampler2D uNew;
uniform float uK;
out vec4 fragColor;
void main() {
    vec2 prev = texelFetch(uPrev, ivec2(0), 0).rg;
    vec2 nw   = texelFetch(uNew,  ivec2(0), 0).rg;
    vec2 r = mix(prev, nw, uK);
    fragColor = vec4(r, 0.0, 0.0);
}`;

    // ----- Compile / link helpers -------------------------------------------

    function compile(gl, type, src) {
        const sh = gl.createShader(type);
        gl.shaderSource(sh, src);
        gl.compileShader(sh);
        if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) {
            const log = gl.getShaderInfoLog(sh);
            gl.deleteShader(sh);
            throw new Error("bro.image.gpu shader compile failed: " + log);
        }
        return sh;
    }

    function link(gl, vs, fs) {
        const p = gl.createProgram();
        gl.attachShader(p, vs);
        gl.attachShader(p, fs);
        gl.linkProgram(p);
        if (!gl.getProgramParameter(p, gl.LINK_STATUS)) {
            const log = gl.getProgramInfoLog(p);
            gl.deleteProgram(p);
            throw new Error("bro.image.gpu shader link failed: " + log);
        }
        return p;
    }

    function buildProgram(gl, vsSrc, fsSrc) {
        const vs = compile(gl, gl.VERTEX_SHADER, vsSrc);
        const fs = compile(gl, gl.FRAGMENT_SHADER, fsSrc);
        const p = link(gl, vs, fs);
        gl.deleteShader(vs);
        gl.deleteShader(fs);
        return p;
    }

    // 1×1 RG32F texture + FBO, used for both the final reduce level and the
    // EMA ping-pong. Created lazily.
    function make1x1RG32F(gl) {
        const tex = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, tex);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RG32F, 1, 1, 0, gl.RG, gl.FLOAT, null);
        const fbo = gl.createFramebuffer();
        gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
        gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0,
                                gl.TEXTURE_2D, tex, 0);
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        return { tex, fbo };
    }

    // ----- Per-canvas state -------------------------------------------------

    const stateByCanvas = new WeakMap();

    function getState(canvas) {
        let st = stateByCanvas.get(canvas);
        if (st) return st;

        const gl = canvas.getContext("webgl2");
        if (!gl) throw new Error("bro.image.gpu: canvas does not support webgl2");

        // Fullscreen triangle (covers clip space [-1,1]^2 with one tri).
        const vbo = gl.createBuffer();
        gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
        gl.bufferData(gl.ARRAY_BUFFER,
            new Float32Array([-1, -1,  3, -1,  -1, 3]),
            gl.STATIC_DRAW);

        // R32F linear filtering is gated behind OES_texture_float_linear.
        // The colormap's texture() of the noise field works either way:
        // NEAREST is fine since rendering is typically 1:1; if upscaling is
        // desired and the extension is missing, the shader would need manual
        // bilinear, but that's not the algo-viz case.
        const hasFloatLinear = !!gl.getExtension('OES_texture_float_linear');
        const noiseFilter = hasFloatLinear ? gl.LINEAR : gl.NEAREST;
        const noiseTex = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, noiseTex);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, noiseFilter);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, noiseFilter);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);

        const lutTex = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, lutTex);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);

        st = {
            gl, vbo, noiseTex, lutTex,
            srcW: 0, srcH: 0, lutN: 0,
            // Uniform-mode colormap (compiled lazily by buildUniformProgram).
            uniProg: null, uniVao: null,
            uniLocs: null,
            // autoRange resources (built lazily by buildAutoPath).
            autoBuilt: false,
            autoProg: null, autoVao: null, autoLocs: null,
            reduceR: null, reduceRLocs: null,
            reduceRG: null, reduceRGLocs: null,
            emaProg: null, emaLocs: null,
            reduceChain: [],        // [{tex, fbo, w, h}] level 1 down to 1×1
            rangePing: null,        // {tex, fbo}  1×1 RG32F
            rangePong: null,        // {tex, fbo}  1×1 RG32F
            hasRange: false,        // first-frame seed flag
        };
        stateByCanvas.set(canvas, st);
        return st;
    }

    function buildUniformProgram(st) {
        if (st.uniProg) return;
        const gl = st.gl;
        const program = buildProgram(gl, FULLSCREEN_VS, COLORMAP_UNIFORM_FS);
        const aPos = gl.getAttribLocation(program, "aPos");
        const vao = gl.createVertexArray();
        gl.bindVertexArray(vao);
        gl.bindBuffer(gl.ARRAY_BUFFER, st.vbo);
        gl.enableVertexAttribArray(aPos);
        gl.vertexAttribPointer(aPos, 2, gl.FLOAT, false, 0, 0);
        gl.bindVertexArray(null);
        st.uniProg = program;
        st.uniVao = vao;
        st.uniLocs = {
            uNoise:   gl.getUniformLocation(program, "uNoise"),
            uLut:     gl.getUniformLocation(program, "uLut"),
            uLo:      gl.getUniformLocation(program, "uLo"),
            uInvSpan: gl.getUniformLocation(program, "uInvSpan"),
        };
    }

    function buildAutoPath(st) {
        if (st.autoBuilt) return;
        const gl = st.gl;

        // Float render targets require EXT_color_buffer_float in WebGL2.
        // Without it, our RG32F FBOs would be INCOMPLETE_ATTACHMENT.
        if (!gl.getExtension('EXT_color_buffer_float')) {
            throw new Error(
                "bro.image.gpu.colormap autoRange: EXT_color_buffer_float not supported"
            );
        }

        // Colormap-from-range program.
        st.autoProg = buildProgram(gl, FULLSCREEN_VS, COLORMAP_AUTO_FS);
        const aPosA = gl.getAttribLocation(st.autoProg, "aPos");
        st.autoVao = gl.createVertexArray();
        gl.bindVertexArray(st.autoVao);
        gl.bindBuffer(gl.ARRAY_BUFFER, st.vbo);
        gl.enableVertexAttribArray(aPosA);
        gl.vertexAttribPointer(aPosA, 2, gl.FLOAT, false, 0, 0);
        gl.bindVertexArray(null);
        st.autoLocs = {
            uNoise: gl.getUniformLocation(st.autoProg, "uNoise"),
            uLut:   gl.getUniformLocation(st.autoProg, "uLut"),
            uRange: gl.getUniformLocation(st.autoProg, "uRange"),
        };

        // Reduce programs. They use gl_FragCoord, not aPos, but the VAO with
        // the fullscreen triangle is still needed to drive vertex output —
        // we reuse autoVao (no per-program VAOs needed since attribs match).
        st.reduceR = buildProgram(gl, FULLSCREEN_VS, REDUCE_FROM_R_FS);
        st.reduceRLocs = {
            uSrc:     gl.getUniformLocation(st.reduceR, "uSrc"),
            uSrcSize: gl.getUniformLocation(st.reduceR, "uSrcSize"),
        };
        st.reduceRG = buildProgram(gl, FULLSCREEN_VS, REDUCE_FROM_RG_FS);
        st.reduceRGLocs = {
            uSrc:     gl.getUniformLocation(st.reduceRG, "uSrc"),
            uSrcSize: gl.getUniformLocation(st.reduceRG, "uSrcSize"),
        };
        st.emaProg = buildProgram(gl, FULLSCREEN_VS, EMA_FS);
        st.emaLocs = {
            uPrev: gl.getUniformLocation(st.emaProg, "uPrev"),
            uNew:  gl.getUniformLocation(st.emaProg, "uNew"),
            uK:    gl.getUniformLocation(st.emaProg, "uK"),
        };

        // 1×1 ping-pong textures for the EMA-smoothed range.
        st.rangePing = make1x1RG32F(gl);
        st.rangePong = make1x1RG32F(gl);

        st.autoBuilt = true;
    }

    // Free the reduction chain. Called when srcW/srcH changes.
    function freeReduceChain(st) {
        const gl = st.gl;
        for (const level of st.reduceChain) {
            gl.deleteTexture(level.tex);
            gl.deleteFramebuffer(level.fbo);
        }
        st.reduceChain.length = 0;
    }

    // Build a chain of RG32F mip-like targets from (srcW>>1, srcH>>1) down to
    // (1,1). Each level halves the previous, clamping to 1. The chain length
    // is ceil(log2(max(srcW,srcH))).
    function buildReduceChain(st, srcW, srcH) {
        freeReduceChain(st);
        const gl = st.gl;
        let w = srcW, h = srcH;
        while (w > 1 || h > 1) {
            w = Math.max(1, w >> 1);
            h = Math.max(1, h >> 1);
            const tex = gl.createTexture();
            gl.bindTexture(gl.TEXTURE_2D, tex);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
            gl.texImage2D(gl.TEXTURE_2D, 0, gl.RG32F, w, h, 0,
                          gl.RG, gl.FLOAT, null);
            const fbo = gl.createFramebuffer();
            gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
            gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0,
                                    gl.TEXTURE_2D, tex, 0);
            st.reduceChain.push({ tex, fbo, w, h });
        }
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        // Resizing the source field invalidates the historical range.
        st.hasRange = false;
    }

    // ----- public colormap() ------------------------------------------------

    /**
     * Colormap a 1-channel float field through a 1D RGBA8 LUT, rendering
     * directly to `canvas` via WebGL2.
     *
     * @param {HTMLCanvasElement} canvas - must support webgl2
     * @param {Float32Array} src         - srcW * srcH scalar field
     * @param {Uint8Array}   lut         - 4*K bytes (RGBA8)
     * @param {object} params
     * @param {number}  [params.lo]        - explicit low (uniform mode)
     * @param {number}  [params.hi]        - explicit high (uniform mode)
     * @param {boolean} [params.autoRange] - GPU min/max + EMA smoothing
     * @param {number}  [params.ema=0.02]  - blend factor for autoRange
     * @param {number}  [params.srcW]      - field width  (default canvas.width)
     * @param {number}  [params.srcH]      - field height (default canvas.height)
     */
    bro.image.gpu.colormap = function colormap(canvas, src, lut, params) {
        if (!canvas || canvas.nodeType !== 1)
            throw new TypeError("bro.image.gpu.colormap: canvas required");
        if (!(src instanceof Float32Array))
            throw new TypeError("bro.image.gpu.colormap: src must be Float32Array");
        if (!(lut instanceof Uint8Array) && !(lut instanceof Uint8ClampedArray))
            throw new TypeError("bro.image.gpu.colormap: lut must be Uint8Array");
        params = params || {};
        const autoRange = !!params.autoRange;
        const srcW = (params.srcW != null ? params.srcW : canvas.width) | 0;
        const srcH = (params.srcH != null ? params.srcH : canvas.height) | 0;
        if (srcW <= 0 || srcH <= 0)
            throw new RangeError("bro.image.gpu.colormap: srcW/srcH must be positive");
        if (src.length < srcW * srcH)
            throw new RangeError("bro.image.gpu.colormap: src too small");
        if ((lut.byteLength & 3) !== 0)
            throw new RangeError("bro.image.gpu.colormap: lut length must be a multiple of 4");
        const lutN = lut.byteLength >> 2;
        if (lutN < 2)
            throw new RangeError("bro.image.gpu.colormap: lut needs >= 2 entries");

        const st = getState(canvas);
        const gl = st.gl;
        const cw = canvas.width | 0, ch = canvas.height | 0;

        // Upload noise (R32F). Reallocate if size changed; otherwise subImage.
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, st.noiseTex);
        const sizeChanged = (srcW !== st.srcW || srcH !== st.srcH);
        if (sizeChanged) {
            gl.texImage2D(gl.TEXTURE_2D, 0, gl.R32F, srcW, srcH, 0,
                          gl.RED, gl.FLOAT, src);
            st.srcW = srcW; st.srcH = srcH;
        } else {
            gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, srcW, srcH,
                             gl.RED, gl.FLOAT, src);
        }

        // Upload LUT (RGBA8).
        gl.activeTexture(gl.TEXTURE1);
        gl.bindTexture(gl.TEXTURE_2D, st.lutTex);
        if (lutN !== st.lutN) {
            gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, lutN, 1, 0,
                          gl.RGBA, gl.UNSIGNED_BYTE, lut);
            st.lutN = lutN;
        } else {
            gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, lutN, 1,
                             gl.RGBA, gl.UNSIGNED_BYTE, lut);
        }

        gl.disable(gl.BLEND);
        gl.disable(gl.DEPTH_TEST);
        gl.disable(gl.CULL_FACE);

        if (!autoRange) {
            // ----- uniform-range path (original) -----
            buildUniformProgram(st);
            const lo = +params.lo;
            const hi = +params.hi;
            gl.viewport(0, 0, cw, ch);
            gl.useProgram(st.uniProg);
            gl.uniform1i(st.uniLocs.uNoise, 0);
            gl.uniform1i(st.uniLocs.uLut,   1);
            gl.uniform1f(st.uniLocs.uLo,    lo);
            gl.uniform1f(st.uniLocs.uInvSpan, (hi > lo) ? 1.0 / (hi - lo) : 0.0);
            gl.bindVertexArray(st.uniVao);
            gl.drawArrays(gl.TRIANGLES, 0, 3);
            gl.bindVertexArray(null);
            return;
        }

        // ----- autoRange path -----
        buildAutoPath(st);
        if (sizeChanged || st.reduceChain.length === 0) {
            buildReduceChain(st, srcW, srcH);
        }
        const ema = (params.ema != null) ? +params.ema : 0.02;
        // First frame after a (re)build: seed the smoothed range directly
        // from this frame's raw reduce instead of blending against stale data.
        const k = st.hasRange ? Math.max(0, Math.min(1, ema)) : 1.0;

        // (1) Reduce: noise (R32F) → chain[0] → chain[1] → ... → chain[last] (1×1).
        let srcTex = st.noiseTex;
        let srcSizeW = srcW, srcSizeH = srcH;
        let usingR = true;  // first hop reads from R32F; subsequent from RG32F.
        for (let i = 0; i < st.reduceChain.length; i++) {
            const lvl = st.reduceChain[i];
            gl.bindFramebuffer(gl.FRAMEBUFFER, lvl.fbo);
            gl.viewport(0, 0, lvl.w, lvl.h);
            const prog  = usingR ? st.reduceR    : st.reduceRG;
            const locs  = usingR ? st.reduceRLocs : st.reduceRGLocs;
            gl.useProgram(prog);
            gl.activeTexture(gl.TEXTURE0);
            gl.bindTexture(gl.TEXTURE_2D, srcTex);
            gl.uniform1i(locs.uSrc, 0);
            gl.uniform2i(locs.uSrcSize, srcSizeW, srcSizeH);
            gl.bindVertexArray(st.autoVao);
            gl.drawArrays(gl.TRIANGLES, 0, 3);
            srcTex = lvl.tex;
            srcSizeW = lvl.w;
            srcSizeH = lvl.h;
            usingR = false;
        }
        // After the loop, srcTex == reduceChain[last].tex (the raw 1×1 range).

        // (2) EMA blend: rangePing (prev) + raw → rangePong, then swap. On the
        //     first frame we use k=1 so rangePong := raw directly, with rangePing
        //     ignored. After the swap, rangePing holds the smoothed range used
        //     by the colormap pass.
        gl.bindFramebuffer(gl.FRAMEBUFFER, st.rangePong.fbo);
        gl.viewport(0, 0, 1, 1);
        gl.useProgram(st.emaProg);
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, st.rangePing.tex);
        gl.uniform1i(st.emaLocs.uPrev, 0);
        gl.activeTexture(gl.TEXTURE1);
        gl.bindTexture(gl.TEXTURE_2D, srcTex);  // raw this-frame (min,max)
        gl.uniform1i(st.emaLocs.uNew, 1);
        gl.uniform1f(st.emaLocs.uK, k);
        gl.bindVertexArray(st.autoVao);
        gl.drawArrays(gl.TRIANGLES, 0, 3);
        // Swap so rangePing := smoothed result.
        const tmp = st.rangePing; st.rangePing = st.rangePong; st.rangePong = tmp;
        st.hasRange = true;

        // (3) Colormap to the default framebuffer (the canvas).
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        gl.viewport(0, 0, cw, ch);
        gl.useProgram(st.autoProg);
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, st.noiseTex);
        gl.uniform1i(st.autoLocs.uNoise, 0);
        gl.activeTexture(gl.TEXTURE1);
        gl.bindTexture(gl.TEXTURE_2D, st.lutTex);
        gl.uniform1i(st.autoLocs.uLut, 1);
        gl.activeTexture(gl.TEXTURE2);
        gl.bindTexture(gl.TEXTURE_2D, st.rangePing.tex);
        gl.uniform1i(st.autoLocs.uRange, 2);
        gl.bindVertexArray(st.autoVao);
        gl.drawArrays(gl.TRIANGLES, 0, 3);
        gl.bindVertexArray(null);
        gl.activeTexture(gl.TEXTURE0);
    };
})();
