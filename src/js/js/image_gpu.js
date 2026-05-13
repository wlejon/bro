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
    // uViewScale / uViewOffset translate canvas vUv [0..1]² into a sub-rect
    // of the source texture, so callers can scroll a pre-rendered tile across
    // the visible canvas without re-running the gen pass. When omitted (full
    // source = full canvas), scale=(1,1) offset=(0,0) → identical to before.
    const COLORMAP_UNIFORM_FS = `#version 300 es
precision highp float;
uniform sampler2D uNoise;
uniform sampler2D uLut;
uniform float uLo;
uniform float uInvSpan;
uniform vec2 uViewScale;
uniform vec2 uViewOffset;
in vec2 vUv;
out vec4 fragColor;
void main() {
    vec2 srcUv = vUv * uViewScale + uViewOffset;
    float v = texture(uNoise, srcUv).r;
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
uniform vec2 uViewScale;
uniform vec2 uViewOffset;
in vec2 vUv;
out vec4 fragColor;
void main() {
    vec2 r = texelFetch(uRange, ivec2(0), 0).rg;
    float lo = r.r;
    float hi = r.g;
    float invSpan = (hi > lo) ? (1.0 / (hi - lo)) : 0.0;
    vec2 srcUv = vUv * uViewScale + uViewOffset;
    float v = texture(uNoise, srcUv).r;
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
            uNoise:      gl.getUniformLocation(program, "uNoise"),
            uLut:        gl.getUniformLocation(program, "uLut"),
            uLo:         gl.getUniformLocation(program, "uLo"),
            uInvSpan:    gl.getUniformLocation(program, "uInvSpan"),
            uViewScale:  gl.getUniformLocation(program, "uViewScale"),
            uViewOffset: gl.getUniformLocation(program, "uViewOffset"),
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
            uNoise:      gl.getUniformLocation(st.autoProg, "uNoise"),
            uLut:        gl.getUniformLocation(st.autoProg, "uLut"),
            uRange:      gl.getUniformLocation(st.autoProg, "uRange"),
            uViewScale:  gl.getUniformLocation(st.autoProg, "uViewScale"),
            uViewOffset: gl.getUniformLocation(st.autoProg, "uViewOffset"),
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

    // Ensure noiseTex is allocated at (srcW, srcH) as R32F and FBO-attachable.
    // Returns whether the texture was (re)allocated this call.
    function ensureNoiseTex(st, srcW, srcH) {
        const gl = st.gl;
        if (srcW === st.srcW && srcH === st.srcH && st.noiseFbo) return false;
        gl.bindTexture(gl.TEXTURE_2D, st.noiseTex);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.R32F, srcW, srcH, 0,
                      gl.RED, gl.FLOAT, null);
        if (!st.noiseFbo) st.noiseFbo = gl.createFramebuffer();
        gl.bindFramebuffer(gl.FRAMEBUFFER, st.noiseFbo);
        gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0,
                                gl.TEXTURE_2D, st.noiseTex, 0);
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        st.srcW = srcW; st.srcH = srcH;
        return true;
    }

    function uploadLut(st, lut) {
        const gl = st.gl;
        const lutN = lut.byteLength >> 2;
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
    }

    // Shared pipeline: given st.noiseTex is already populated (R32F, srcW×srcH)
    // and lut is uploaded, render to `canvas` either in uniform-range or
    // autoRange mode. sizeChanged signals whether the source dims just changed
    // (used to invalidate the reduceChain).
    // Compute the (scale, offset) that maps canvas vUv [0..1]² into a sub-rect
    // of the source noise texture. With no viewRect, this is the identity.
    function computeViewTransform(st, params) {
        if (!params.viewRect) return { sx: 1, sy: 1, ox: 0, oy: 0 };
        const v = params.viewRect;
        const sw = Math.max(1, st.srcW);
        const sh = Math.max(1, st.srcH);
        return {
            sx: (+v.w) / sw,
            sy: (+v.h) / sh,
            ox: (+v.x) / sw,
            oy: (+v.y) / sh,
        };
    }

    function applyColormapToCanvas(st, canvas, params, sizeChanged) {
        const gl = st.gl;
        const cw = canvas.width | 0, ch = canvas.height | 0;
        const autoRange = !!params.autoRange;
        const vt = computeViewTransform(st, params);

        gl.disable(gl.BLEND);
        gl.disable(gl.DEPTH_TEST);
        gl.disable(gl.CULL_FACE);

        if (!autoRange) {
            buildUniformProgram(st);
            const lo = +params.lo;
            const hi = +params.hi;
            gl.bindFramebuffer(gl.FRAMEBUFFER, null);
            gl.viewport(0, 0, cw, ch);
            gl.useProgram(st.uniProg);
            gl.activeTexture(gl.TEXTURE0);
            gl.bindTexture(gl.TEXTURE_2D, st.noiseTex);
            gl.uniform1i(st.uniLocs.uNoise, 0);
            gl.activeTexture(gl.TEXTURE1);
            gl.bindTexture(gl.TEXTURE_2D, st.lutTex);
            gl.uniform1i(st.uniLocs.uLut, 1);
            gl.uniform1f(st.uniLocs.uLo, lo);
            gl.uniform1f(st.uniLocs.uInvSpan, (hi > lo) ? 1.0 / (hi - lo) : 0.0);
            gl.uniform2f(st.uniLocs.uViewScale,  vt.sx, vt.sy);
            gl.uniform2f(st.uniLocs.uViewOffset, vt.ox, vt.oy);
            gl.bindVertexArray(st.uniVao);
            gl.drawArrays(gl.TRIANGLES, 0, 3);
            gl.bindVertexArray(null);
            return;
        }

        buildAutoPath(st);
        if (sizeChanged || st.reduceChain.length === 0) {
            buildReduceChain(st, st.srcW, st.srcH);
        }
        const ema = (params.ema != null) ? +params.ema : 0.02;
        const k = st.hasRange ? Math.max(0, Math.min(1, ema)) : 1.0;

        // (1) Reduce R32F noise → 1×1 RG32F (raw min,max).
        let srcTex = st.noiseTex;
        let srcSizeW = st.srcW, srcSizeH = st.srcH;
        let usingR = true;
        for (let i = 0; i < st.reduceChain.length; i++) {
            const lvl = st.reduceChain[i];
            gl.bindFramebuffer(gl.FRAMEBUFFER, lvl.fbo);
            gl.viewport(0, 0, lvl.w, lvl.h);
            const prog = usingR ? st.reduceR : st.reduceRG;
            const locs = usingR ? st.reduceRLocs : st.reduceRGLocs;
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

        // (2) EMA blend.
        gl.bindFramebuffer(gl.FRAMEBUFFER, st.rangePong.fbo);
        gl.viewport(0, 0, 1, 1);
        gl.useProgram(st.emaProg);
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, st.rangePing.tex);
        gl.uniform1i(st.emaLocs.uPrev, 0);
        gl.activeTexture(gl.TEXTURE1);
        gl.bindTexture(gl.TEXTURE_2D, srcTex);
        gl.uniform1i(st.emaLocs.uNew, 1);
        gl.uniform1f(st.emaLocs.uK, k);
        gl.bindVertexArray(st.autoVao);
        gl.drawArrays(gl.TRIANGLES, 0, 3);
        const tmp = st.rangePing; st.rangePing = st.rangePong; st.rangePong = tmp;
        st.hasRange = true;

        // (3) Colormap to the canvas.
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
        gl.uniform2f(st.autoLocs.uViewScale,  vt.sx, vt.sy);
        gl.uniform2f(st.autoLocs.uViewOffset, vt.ox, vt.oy);
        gl.bindVertexArray(st.autoVao);
        gl.drawArrays(gl.TRIANGLES, 0, 3);
        gl.bindVertexArray(null);
        gl.activeTexture(gl.TEXTURE0);
    }

    // ----- public colormap() ------------------------------------------------

    /**
     * Colormap a 1-channel float field through a 1D RGBA8 LUT, rendering
     * directly to `canvas` via WebGL2.
     */
    bro.image.gpu.colormap = function colormap(canvas, src, lut, params) {
        if (!canvas || canvas.nodeType !== 1)
            throw new TypeError("bro.image.gpu.colormap: canvas required");
        if (!(src instanceof Float32Array))
            throw new TypeError("bro.image.gpu.colormap: src must be Float32Array");
        if (!(lut instanceof Uint8Array) && !(lut instanceof Uint8ClampedArray))
            throw new TypeError("bro.image.gpu.colormap: lut must be Uint8Array");
        params = params || {};
        const srcW = (params.srcW != null ? params.srcW : canvas.width) | 0;
        const srcH = (params.srcH != null ? params.srcH : canvas.height) | 0;
        if (srcW <= 0 || srcH <= 0)
            throw new RangeError("bro.image.gpu.colormap: srcW/srcH must be positive");
        if (src.length < srcW * srcH)
            throw new RangeError("bro.image.gpu.colormap: src too small");
        if ((lut.byteLength & 3) !== 0)
            throw new RangeError("bro.image.gpu.colormap: lut length must be a multiple of 4");
        if ((lut.byteLength >> 2) < 2)
            throw new RangeError("bro.image.gpu.colormap: lut needs >= 2 entries");

        const st = getState(canvas);
        const gl = st.gl;
        const sizeChanged = ensureNoiseTex(st, srcW, srcH);
        // Upload the CPU-provided field into the noise texture.
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, st.noiseTex);
        gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, srcW, srcH,
                         gl.RED, gl.FLOAT, src);
        uploadLut(st, lut);
        applyColormapToCanvas(st, canvas, params, sizeChanged);
    };

    // ----- FBm-on-GPU --------------------------------------------------------

    // Stefan Gustavson / Ian McEwan (Ashima Arts) Simplex 2D — MIT/public.
    // Driven by an additive seed offset; FBm sums octaves with gain/lacunarity.
    // The summed result is *not* normalized by Σaᵢ (matches FastNoise2's
    // FractalFBm shape; autoRange compensates).
    const FBM_FS = `#version 300 es
precision highp float;

uniform vec2  uOrigin;        // world offset (ox, oy) at frequency 1
uniform float uFrequency;     // base frequency
uniform int   uOctaves;       // number of octaves
uniform float uGain;
uniform float uLacunarity;
uniform float uSeed;          // additive seed offset (any float)

in  vec2 vUv;     // ignored; we use gl_FragCoord
out vec4 fragColor;

vec3 mod289v3(vec3 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec2 mod289v2(vec2 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec3 permute(vec3 x)  { return mod289v3(((x * 34.0) + 1.0) * x); }

float snoise(vec2 v) {
    const vec4 C = vec4(0.211324865405187, 0.366025403784439,
                       -0.577350269189626, 0.024390243902439);
    vec2 i  = floor(v + dot(v, C.yy));
    vec2 x0 = v -   i + dot(i, C.xx);
    vec2 i1 = (x0.x > x0.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
    vec4 x12 = x0.xyxy + C.xxzz;
    x12.xy -= i1;
    i = mod289v2(i);
    vec3 p = permute(permute(i.y + vec3(0.0, i1.y, 1.0))
                  + i.x + vec3(0.0, i1.x, 1.0));
    vec3 m = max(0.5 - vec3(dot(x0,x0),
                            dot(x12.xy, x12.xy),
                            dot(x12.zw, x12.zw)), 0.0);
    m = m * m; m = m * m;
    vec3 x  = 2.0 * fract(p * C.www) - 1.0;
    vec3 h  = abs(x) - 0.5;
    vec3 ox = floor(x + 0.5);
    vec3 a0 = x - ox;
    m *= 1.79284291400159 - 0.85373472095314 * (a0*a0 + h*h);
    vec3 g;
    g.x  = a0.x  * x0.x  + h.x  * x0.y;
    g.yz = a0.yz * x12.xz + h.yz * x12.yw;
    return 130.0 * dot(m, g);
}

void main() {
    // World position: (px + uOrigin) * uFrequency for the first octave.
    // px = gl_FragCoord.xy - 0.5 so column 0 row 0 samples (0+ox, 0+oy).
    vec2 px = gl_FragCoord.xy - vec2(0.5);
    vec2 base = (px + uOrigin) * uFrequency;

    // Seed offsets the lattice irrationally so different seeds give different
    // fields (and lacunarity scaling doesn't pull all octaves through (0,0)).
    vec2 seedOfs = vec2(uSeed * 17.0, uSeed * 31.0);

    float total = 0.0;
    float amp = 1.0;
    float freq = 1.0;
    for (int i = 0; i < 16; i++) {
        if (i >= uOctaves) break;
        total += amp * snoise(base * freq + seedOfs);
        freq *= uLacunarity;
        amp  *= uGain;
    }
    fragColor = vec4(total, 0.0, 0.0, 1.0);
}`;

    function buildFbmPath(st) {
        if (st.fbmProg) return;
        const gl = st.gl;
        if (!gl.getExtension('EXT_color_buffer_float')) {
            throw new Error(
                "bro.image.gpu.fbm2D: EXT_color_buffer_float not supported"
            );
        }
        st.fbmProg = buildProgram(gl, FULLSCREEN_VS, FBM_FS);
        const aPos = gl.getAttribLocation(st.fbmProg, "aPos");
        if (!st.fbmVao) {
            st.fbmVao = gl.createVertexArray();
            gl.bindVertexArray(st.fbmVao);
            gl.bindBuffer(gl.ARRAY_BUFFER, st.vbo);
            gl.enableVertexAttribArray(aPos);
            gl.vertexAttribPointer(aPos, 2, gl.FLOAT, false, 0, 0);
            gl.bindVertexArray(null);
        }
        st.fbmLocs = {
            uOrigin:     gl.getUniformLocation(st.fbmProg, "uOrigin"),
            uFrequency:  gl.getUniformLocation(st.fbmProg, "uFrequency"),
            uOctaves:    gl.getUniformLocation(st.fbmProg, "uOctaves"),
            uGain:       gl.getUniformLocation(st.fbmProg, "uGain"),
            uLacunarity: gl.getUniformLocation(st.fbmProg, "uLacunarity"),
            uSeed:       gl.getUniformLocation(st.fbmProg, "uSeed"),
        };
    }

    /**
     * Generate a 2D Simplex FBm field on the GPU and colormap it to `canvas`.
     * The scalar field is never materialized on the CPU side — it lives only
     * as an R32F texture, then feeds the existing autoRange / uniform-range
     * colormap pipeline.
     *
     * V1 supports type === 'Simplex' (and treats no `type` as Simplex). Other
     * FastNoise2 types (SuperSimplex, Perlin, Value, CellularValue,
     * CellularDistance) are NOT implemented in shader form yet — callers that
     * need them should keep the CPU `genUniformGrid2DInto` + colormap path.
     *
     * @param {HTMLCanvasElement} canvas      - webgl2-backed
     * @param {Uint8Array} lut                - RGBA8 LUT
     * @param {object} params
     * @param {number}  params.frequency      - base frequency
     * @param {number}  [params.octaves=1]    - FBm octave count (1..16)
     * @param {number}  [params.gain=0.5]
     * @param {number}  [params.lacunarity=2]
     * @param {number}  [params.seed=0]
     * @param {number}  [params.ox=0]         - world offset x
     * @param {number}  [params.oy=0]         - world offset y
     * @param {string}  [params.type='Simplex'] - currently only 'Simplex'
     * @param {boolean} [params.autoRange]
     * @param {number}  [params.ema=0.02]
     * @param {number}  [params.lo] / [params.hi]
     * @param {number}  [params.srcW] / [params.srcH]
     */
    bro.image.gpu.fbm2D = function fbm2D(canvas, lut, params) {
        if (!canvas || canvas.nodeType !== 1)
            throw new TypeError("bro.image.gpu.fbm2D: canvas required");
        if (!(lut instanceof Uint8Array) && !(lut instanceof Uint8ClampedArray))
            throw new TypeError("bro.image.gpu.fbm2D: lut must be Uint8Array");
        params = params || {};
        const regenerate = (params.regenerate !== false);  // default true

        const st = getState(canvas);
        const gl = st.gl;

        // regenerate:false reuses the cached noiseTex — the gen pass is
        // skipped, only the colormap (and reduce/EMA if autoRange) runs.
        // Lets callers pre-render a wider tile once per N frames and slide
        // a viewRect across it cheaply in between.
        if (!regenerate) {
            if (st.srcW <= 0 || !st.noiseTex) {
                throw new Error(
                    "bro.image.gpu.fbm2D: regenerate:false but no cached noise " +
                    "field for this canvas — call once with regenerate:true first"
                );
            }
            if ((lut.byteLength & 3) !== 0)
                throw new RangeError("bro.image.gpu.fbm2D: lut length must be a multiple of 4");
            if ((lut.byteLength >> 2) < 2)
                throw new RangeError("bro.image.gpu.fbm2D: lut needs >= 2 entries");
            uploadLut(st, lut);
            applyColormapToCanvas(st, canvas, params, /*sizeChanged*/ false);
            return;
        }

        const type = params.type || 'Simplex';
        if (type !== 'Simplex') {
            throw new Error(
                "bro.image.gpu.fbm2D: type '" + type + "' not implemented " +
                "(V1 supports 'Simplex'). Use the CPU genUniformGrid2DInto + " +
                "bro.image.gpu.colormap path for other types."
            );
        }
        const srcW = (params.srcW != null ? params.srcW : canvas.width) | 0;
        const srcH = (params.srcH != null ? params.srcH : canvas.height) | 0;
        if (srcW <= 0 || srcH <= 0)
            throw new RangeError("bro.image.gpu.fbm2D: srcW/srcH must be positive");
        if ((lut.byteLength & 3) !== 0)
            throw new RangeError("bro.image.gpu.fbm2D: lut length must be a multiple of 4");
        if ((lut.byteLength >> 2) < 2)
            throw new RangeError("bro.image.gpu.fbm2D: lut needs >= 2 entries");
        const octaves = (params.octaves != null) ? (params.octaves | 0) : 1;
        if (octaves < 1 || octaves > 16)
            throw new RangeError("bro.image.gpu.fbm2D: octaves must be in [1,16]");

        buildFbmPath(st);
        const sizeChanged = ensureNoiseTex(st, srcW, srcH);
        if (sizeChanged) {
            // Resizing invalidates the reduce chain and the smoothed range.
            freeReduceChain(st);
            st.hasRange = false;
        }
        uploadLut(st, lut);

        gl.disable(gl.BLEND);
        gl.disable(gl.DEPTH_TEST);
        gl.disable(gl.CULL_FACE);

        // (0) FBm pass → noiseTex.
        gl.bindFramebuffer(gl.FRAMEBUFFER, st.noiseFbo);
        gl.viewport(0, 0, srcW, srcH);
        gl.useProgram(st.fbmProg);
        gl.uniform2f(st.fbmLocs.uOrigin, +(params.ox || 0), +(params.oy || 0));
        gl.uniform1f(st.fbmLocs.uFrequency, +params.frequency);
        gl.uniform1i(st.fbmLocs.uOctaves, octaves);
        gl.uniform1f(st.fbmLocs.uGain, params.gain != null ? +params.gain : 0.5);
        gl.uniform1f(st.fbmLocs.uLacunarity, params.lacunarity != null ? +params.lacunarity : 2.0);
        gl.uniform1f(st.fbmLocs.uSeed, +(params.seed || 0));
        gl.bindVertexArray(st.fbmVao);
        gl.drawArrays(gl.TRIANGLES, 0, 3);

        applyColormapToCanvas(st, canvas, params, sizeChanged);
    };
})();
