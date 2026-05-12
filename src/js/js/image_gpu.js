// bro.image.gpu — hardware-accelerated counterparts to bro.image (CPU/brokit).
//
// V1 surface: colormap(canvas, src, lut, {lo, hi, srcW?, srcH?}).
// srcW/srcH default to canvas.width/canvas.height when omitted (1:1 case).
// Same shape as bro.image.lookup, but the colormap runs as a WebGL2 fragment
// shader that samples the noise field as a R32F texture and the LUT as a
// 256x1 RGBA8 texture. No CPU-side ImageData / putImageData path; the result
// is rendered straight to the canvas.
//
// Per-canvas state (program, VAO, textures, dimensions) is cached in a
// WeakMap so repeat calls reuse the GPU resources. The src texture is
// reallocated only when srcW/srcH changes.

(function () {
    if (typeof bro !== "object" || bro === null) {
        // bro.image (brokit) installs this; if it didn't run yet, leave a
        // skeleton so order doesn't matter.
        globalThis.bro = globalThis.bro || {};
    }
    if (!bro.image) bro.image = {};
    if (bro.image.gpu) return;  // already installed
    bro.image.gpu = {};

    const VS = `#version 300 es
in vec2 aPos;
out vec2 vUv;
void main() {
    vUv = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}`;

    const FS = `#version 300 es
precision highp float;
uniform sampler2D uNoise;
uniform sampler2D uLut;
uniform float uLo;
uniform float uInvSpan;
in vec2 vUv;
out vec4 fragColor;
void main() {
    // Final pipeline: noise (unit 0) -> normalized t -> LUT (unit 1).
    float v = texture(uNoise, vUv).r;
    float t = (v - uLo) * uInvSpan;
    t = clamp(t, 0.0, 1.0);
    fragColor = texture(uLut, vec2(t, 0.5));
}`;

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

    // Per-canvas state. Keyed by canvas (WeakMap so canvas removal cleans up).
    const stateByCanvas = new WeakMap();

    function getState(canvas) {
        let st = stateByCanvas.get(canvas);
        if (st) return st;

        const gl = canvas.getContext("webgl2");
        if (!gl) throw new Error("bro.image.gpu: canvas does not support webgl2");

        const vs = compile(gl, gl.VERTEX_SHADER, VS);
        const fs = compile(gl, gl.FRAGMENT_SHADER, FS);
        const program = link(gl, vs, fs);
        gl.deleteShader(vs);
        gl.deleteShader(fs);

        const aPos = gl.getAttribLocation(program, "aPos");
        const uNoise = gl.getUniformLocation(program, "uNoise");
        const uLut = gl.getUniformLocation(program, "uLut");
        const uLo = gl.getUniformLocation(program, "uLo");
        const uInvSpan = gl.getUniformLocation(program, "uInvSpan");

        // Fullscreen triangle (covers clip space [-1,1]^2 with one tri).
        const vbo = gl.createBuffer();
        gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
        gl.bufferData(gl.ARRAY_BUFFER,
            new Float32Array([-1, -1,  3, -1,  -1, 3]),
            gl.STATIC_DRAW);

        const vao = gl.createVertexArray();
        gl.bindVertexArray(vao);
        gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
        gl.enableVertexAttribArray(aPos);
        gl.vertexAttribPointer(aPos, 2, gl.FLOAT, false, 0, 0);
        gl.bindVertexArray(null);

        // R32F linear filtering is gated behind OES_texture_float_linear.
        // Without it, LINEAR samples come back as 0. NEAREST is safe and
        // visually fine here since src and dst are 1:1 in algo-viz; if
        // upscaling is needed later, request the extension or sample with
        // manual bilinear in the shader.
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
            gl, program, vao, vbo, noiseTex, lutTex,
            uNoise, uLut, uLo, uInvSpan,
            srcW: 0, srcH: 0,
            lutN: 0,
        };
        stateByCanvas.set(canvas, st);
        return st;
    }

    /**
     * Colormap a 1-channel float field through a 1D RGBA8 LUT, rendering
     * directly to `canvas` via WebGL2.
     *
     * @param {HTMLCanvasElement} canvas - must support webgl2
     * @param {Float32Array} src         - srcW * srcH scalar field
     * @param {Uint8Array}   lut         - 4*K bytes (RGBA8)
     * @param {{lo:number, hi:number, srcW?:number, srcH?:number}} params
     *        srcW/srcH default to canvas.width/canvas.height when omitted.
     */
    bro.image.gpu.colormap = function colormap(canvas, src, lut, params) {
        if (!canvas || canvas.nodeType !== 1)
            throw new TypeError("bro.image.gpu.colormap: canvas required");
        if (!(src instanceof Float32Array))
            throw new TypeError("bro.image.gpu.colormap: src must be Float32Array");
        if (!(lut instanceof Uint8Array) && !(lut instanceof Uint8ClampedArray))
            throw new TypeError("bro.image.gpu.colormap: lut must be Uint8Array");
        const lo = +params.lo;
        const hi = +params.hi;
        const srcW = (params.srcW != null ? params.srcW : canvas.width) | 0;
        const srcH = (params.srcH != null ? params.srcH : canvas.height) | 0;
        if (srcW <= 0 || srcH <= 0)
            throw new RangeError("bro.image.gpu.colormap: srcW/srcH must be positive (canvas size or explicit)");
        if (src.length < srcW * srcH)
            throw new RangeError("bro.image.gpu.colormap: src too small");
        if ((lut.byteLength & 3) !== 0)
            throw new RangeError("bro.image.gpu.colormap: lut length must be a multiple of 4");
        const lutN = lut.byteLength >> 2;
        if (lutN < 2)
            throw new RangeError("bro.image.gpu.colormap: lut needs >= 2 entries");

        const st = getState(canvas);
        const gl = st.gl;

        // Match canvas viewport size — use the canvas backing-store size.
        const cw = canvas.width | 0, ch = canvas.height | 0;
        gl.viewport(0, 0, cw, ch);

        // Upload noise. Reallocate if size changed; otherwise texSubImage2D.
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, st.noiseTex);
        if (srcW !== st.srcW || srcH !== st.srcH) {
            gl.texImage2D(gl.TEXTURE_2D, 0, gl.R32F, srcW, srcH, 0,
                          gl.RED, gl.FLOAT, src);
            st.srcW = srcW; st.srcH = srcH;
        } else {
            gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, srcW, srcH,
                             gl.RED, gl.FLOAT, src);
        }

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
        gl.useProgram(st.program);
        gl.uniform1i(st.uNoise, 0);
        gl.uniform1i(st.uLut, 1);
        gl.uniform1f(st.uLo, lo);
        gl.uniform1f(st.uInvSpan, (hi > lo) ? 1.0 / (hi - lo) : 0.0);

        gl.bindVertexArray(st.vao);
        gl.drawArrays(gl.TRIANGLES, 0, 3);
        gl.bindVertexArray(null);
    };
})();
