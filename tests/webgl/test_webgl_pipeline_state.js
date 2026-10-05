// WebGL2 fixed-function state that reaches the pipeline: depthRange,
// blendColor, polygonOffset, RASTERIZER_DISCARD, SAMPLE_COVERAGE and
// SAMPLE_ALPHA_TO_COVERAGE, lineWidth, and clears under partial color and
// stencil write masks — each checked by the pixels it produces, so a setter
// that only stores its value fails.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '64');
canvas.setAttribute('height', '64');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2', { antialias: false, stencil: true });
if (!gl) {
    missingGpuContext('webgl2');
} else {
    function program(vs, fs) {
        const p = gl.createProgram();
        for (const [type, src] of [[gl.VERTEX_SHADER, vs], [gl.FRAGMENT_SHADER, fs]]) {
            const s = gl.createShader(type);
            gl.shaderSource(s, src);
            gl.compileShader(s);
            gl.attachShader(p, s);
        }
        gl.linkProgram(p);
        if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p));
        return p;
    }
    function px(x, y) {
        const b = new Uint8Array(4);
        gl.readPixels(x, y, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, b);
        return Array.from(b);
    }
    function near(a, b, tol) { return a.every((v, i) => Math.abs(v - b[i]) <= (tol === undefined ? 3 : tol)); }
    function expectPx(x, y, want, msg, tol) {
        const got = px(x, y);
        assert(near(got, want, tol), msg + ': got [' + got + '] want [' + want + ']');
    }
    function expectError(want, msg) {
        const e = gl.getError();
        assert(e === want, msg + ': error 0x' + e.toString(16) + ' want 0x' + want.toString(16));
    }

    // A quad at depth z in a uniform color.
    const prog = program(
        '#version 300 es\nin vec2 p;\nuniform float z;\nvoid main(){ gl_Position = vec4(p, z, 1.0); }',
        '#version 300 es\nprecision highp float;\nuniform vec4 c;\nout vec4 o;\nvoid main(){ o = c; }');
    const uZ = gl.getUniformLocation(prog, 'z');
    const uC = gl.getUniformLocation(prog, 'c');
    const vbo = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    gl.bindAttribLocation(prog, 0, 'p');
    gl.linkProgram(prog);
    gl.useProgram(prog);
    function quad(z, color) {
        gl.uniform1f(uZ, z);
        gl.uniform4fv(uC, color);
        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    }
    function reset() {
        gl.disable(gl.DEPTH_TEST);
        gl.disable(gl.BLEND);
        gl.disable(gl.POLYGON_OFFSET_FILL);
        gl.disable(gl.STENCIL_TEST);
        gl.depthRange(0, 1);
        gl.depthFunc(gl.LESS);
        gl.colorMask(true, true, true, true);
        gl.stencilMask(0xFF);
        gl.clearColor(0, 0, 0, 1);
        gl.clearDepth(1);
        gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT | gl.STENCIL_BUFFER_BIT);
    }
    gl.viewport(0, 0, 64, 64);

    // ---- depthRange: NDC z maps into [near, far] before the depth test ----
    reset();
    gl.enable(gl.DEPTH_TEST);
    gl.depthRange(0.75, 0.75);
    quad(0.0, [1, 0, 0, 1]);            // depth 0.75 everywhere
    gl.depthRange(0, 1);
    quad(0.6, [0, 0, 1, 1]);            // 0.8: behind
    expectPx(32, 32, [255, 0, 0, 255], 'a quad behind the depthRange-placed one fails the test');
    quad(0.4, [0, 1, 0, 1]);            // 0.7: in front
    expectPx(32, 32, [0, 255, 0, 255], 'a quad in front of it passes');
    const range = gl.getParameter(gl.DEPTH_RANGE);
    assert(range instanceof Float32Array && range[0] === 0 && range[1] === 1, 'DEPTH_RANGE reads back');
    gl.depthRange(0.8, 0.2);
    expectError(gl.INVALID_OPERATION, 'WebGL: depthRange with near > far');
    gl.depthRange(-1, 2);
    const clamped = gl.getParameter(gl.DEPTH_RANGE);
    assert(clamped[0] === 0 && clamped[1] === 1, 'depthRange clamps to [0, 1]: ' + Array.from(clamped));

    // ---- blendColor: CONSTANT_COLOR factors read it ----
    reset();
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.CONSTANT_COLOR, gl.ZERO);
    gl.blendColor(0.5, 0.25, 1.0, 1.0);
    quad(0, [1, 1, 1, 1]);
    expectPx(32, 32, [128, 64, 255, 255], 'CONSTANT_COLOR blends by blendColor');
    gl.blendFunc(gl.CONSTANT_ALPHA, gl.ONE_MINUS_CONSTANT_ALPHA);
    gl.blendColor(0, 0, 0, 0.25);
    quad(0, [1, 1, 1, 1]);
    expectPx(32, 32, [160, 112, 255, 255], 'CONSTANT_ALPHA blends by its alpha', 4);
    const bc = gl.getParameter(gl.BLEND_COLOR);
    assert(bc instanceof Float32Array && bc[3] === 0.25, 'BLEND_COLOR reads back');

    // ---- polygonOffset: a coplanar quad pulled forward passes LESS ----
    reset();
    gl.enable(gl.DEPTH_TEST);
    quad(0, [1, 0, 0, 1]);
    quad(0, [0, 0, 1, 1]);
    expectPx(32, 32, [255, 0, 0, 255], 'a coplanar quad fails LESS');
    gl.enable(gl.POLYGON_OFFSET_FILL);
    gl.polygonOffset(0, -8);
    quad(0, [0, 1, 0, 1]);
    expectPx(32, 32, [0, 255, 0, 255], 'polygonOffset pulls it in front');
    gl.polygonOffset(0, 8);
    quad(0, [0, 0, 1, 1]);
    expectPx(32, 32, [0, 255, 0, 255], 'a positive offset keeps a coplanar quad behind');
    assert(gl.getParameter(gl.POLYGON_OFFSET_UNITS) === 8 && gl.getParameter(gl.POLYGON_OFFSET_FACTOR) === 0,
           'POLYGON_OFFSET_* read back');

    // ---- RASTERIZER_DISCARD: no fragments, and clears are discarded too ----
    reset();
    gl.enable(gl.RASTERIZER_DISCARD);
    quad(0, [1, 0, 0, 1]);
    gl.clearColor(0, 0, 1, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.clearBufferfv(gl.COLOR, 0, [1, 1, 0, 1]);
    gl.disable(gl.RASTERIZER_DISCARD);
    expectPx(32, 32, [0, 0, 0, 255], 'RASTERIZER_DISCARD drops draws and clears');
    expectError(gl.NO_ERROR, 'discarded work raises no error');

    // ---- lineWidth ----
    gl.lineWidth(0);
    expectError(gl.INVALID_VALUE, 'lineWidth(0)');
    gl.lineWidth(1);
    assert(gl.getParameter(gl.LINE_WIDTH) === 1, 'LINE_WIDTH reads back');

    // ---- partial color mask clears: only the masked-in channels change ----
    reset();
    gl.clearColor(1, 1, 1, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.colorMask(true, false, true, false);
    gl.clearColor(0.25, 0, 0, 0);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.colorMask(true, true, true, true);
    expectPx(10, 10, [64, 255, 0, 255], 'clear under colorMask(r, -, b, -)');
    gl.colorMask(false, true, false, false);
    gl.enable(gl.SCISSOR_TEST);
    gl.scissor(0, 0, 32, 64);
    gl.clearBufferfv(gl.COLOR, 0, [1, 0.5, 1, 1]);
    gl.disable(gl.SCISSOR_TEST);
    gl.colorMask(true, true, true, true);
    expectPx(10, 10, [64, 128, 0, 255], 'clearBufferfv under a partial mask, inside the scissor box');
    expectPx(50, 10, [64, 255, 0, 255], '... and not outside it');

    // ---- partial stencil write mask clears ----
    const fbo = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
    const color = gl.createRenderbuffer();
    gl.bindRenderbuffer(gl.RENDERBUFFER, color);
    gl.renderbufferStorage(gl.RENDERBUFFER, gl.RGBA8, 16, 16);
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, color);
    const ds = gl.createRenderbuffer();
    gl.bindRenderbuffer(gl.RENDERBUFFER, ds);
    gl.renderbufferStorage(gl.RENDERBUFFER, gl.DEPTH24_STENCIL8, 16, 16);
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_STENCIL_ATTACHMENT, gl.RENDERBUFFER, ds);
    gl.viewport(0, 0, 16, 16);
    gl.clearStencil(0xFF);
    gl.clear(gl.STENCIL_BUFFER_BIT | gl.COLOR_BUFFER_BIT);
    gl.stencilMask(0x0F);
    gl.clearStencil(0);
    gl.clear(gl.STENCIL_BUFFER_BIT);            // stencil = 0xF0
    gl.stencilMask(0xFF);
    gl.enable(gl.STENCIL_TEST);
    gl.stencilFunc(gl.EQUAL, 0xF0, 0xFF);
    quad(0, [0, 1, 0, 1]);
    expectPx(8, 8, [0, 255, 0, 255], 'clear under stencilMask(0x0F) keeps the high bits');
    gl.stencilMask(0xF0);
    gl.clearBufferiv(gl.STENCIL, 0, [0x3C]);    // 0xF0 -> 0x30
    gl.stencilMask(0xFF);
    gl.stencilFunc(gl.EQUAL, 0x30, 0xFF);
    quad(0, [1, 0, 1, 1]);
    expectPx(8, 8, [255, 0, 255, 255], 'clearBufferiv(STENCIL) under stencilMask(0xF0)');
    gl.disable(gl.STENCIL_TEST);

    // Integer attachments under a partial mask.
    const ui = gl.createRenderbuffer();
    gl.bindRenderbuffer(gl.RENDERBUFFER, ui);
    gl.renderbufferStorage(gl.RENDERBUFFER, gl.RGBA8UI, 16, 16);
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, ui);
    gl.clearBufferuiv(gl.COLOR, 0, [1, 2, 3, 4]);
    gl.colorMask(false, false, true, true);
    gl.clearBufferuiv(gl.COLOR, 0, [9, 9, 200, 100]);
    gl.colorMask(true, true, true, true);
    const uiPx = new Uint32Array(4);
    gl.readPixels(4, 4, 1, 1, gl.RGBA_INTEGER, gl.UNSIGNED_INT, uiPx);
    assert(Array.from(uiPx).join() === '1,2,200,100', 'clearBufferuiv under colorMask(-, -, b, a): ' + Array.from(uiPx));
    expectError(gl.NO_ERROR, 'masked clears raise no error');

    // ---- multisampling: SAMPLE_COVERAGE and SAMPLE_ALPHA_TO_COVERAGE ----
    const samples = gl.getInternalformatParameter(gl.RENDERBUFFER, gl.RGBA8, gl.SAMPLES);
    const n = samples && samples.length ? Math.max(...samples) : 0;
    const msFbo = gl.createFramebuffer();
    const ms = gl.createRenderbuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, msFbo);
    gl.bindRenderbuffer(gl.RENDERBUFFER, ms);
    gl.renderbufferStorageMultisample(gl.RENDERBUFFER, n >= 4 ? 4 : n, gl.RGBA8, 16, 16);
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, ms);
    const resolved = gl.createFramebuffer();
    const rc = gl.createRenderbuffer();
    gl.bindRenderbuffer(gl.RENDERBUFFER, rc);
    gl.renderbufferStorage(gl.RENDERBUFFER, gl.RGBA8, 16, 16);
    gl.bindFramebuffer(gl.FRAMEBUFFER, resolved);
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, rc);
    function msDraw(setup) {
        gl.bindFramebuffer(gl.FRAMEBUFFER, msFbo);
        gl.clearColor(0, 0, 0, 1);
        gl.clear(gl.COLOR_BUFFER_BIT);
        setup();
        quad(0, [1, 1, 1, 1]);
        gl.disable(gl.SAMPLE_COVERAGE);
        gl.disable(gl.SAMPLE_ALPHA_TO_COVERAGE);
        gl.bindFramebuffer(gl.READ_FRAMEBUFFER, msFbo);
        gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, resolved);
        gl.blitFramebuffer(0, 0, 16, 16, 0, 0, 16, 16, gl.COLOR_BUFFER_BIT, gl.NEAREST);
        gl.bindFramebuffer(gl.READ_FRAMEBUFFER, resolved);
        return px(8, 8);
    }
    if (n >= 4) {
        const half = msDraw(() => { gl.enable(gl.SAMPLE_COVERAGE); gl.sampleCoverage(0.5, false); });
        assert(near(half, [128, 128, 128, 255], 2), 'sampleCoverage(0.5) covers half the samples: ' + half);
        const none = msDraw(() => { gl.enable(gl.SAMPLE_COVERAGE); gl.sampleCoverage(0.5, true); gl.sampleCoverage(1.0, true); });
        assert(near(none, [0, 0, 0, 255], 0), 'sampleCoverage(1, invert) covers none: ' + none);
        const quarter = msDraw(() => { gl.enable(gl.SAMPLE_COVERAGE); gl.sampleCoverage(0.75, true); });
        assert(near(quarter, [64, 64, 64, 255], 2), 'sampleCoverage(0.75, invert) covers a quarter: ' + quarter);
        gl.useProgram(prog);
        const a2c = msDraw(() => { gl.enable(gl.SAMPLE_ALPHA_TO_COVERAGE); gl.uniform4fv(uC, [1, 1, 1, 0]); });
        // quad() set the alpha back to 1: draw again with alpha 0 and 0.5.
        gl.bindFramebuffer(gl.FRAMEBUFFER, msFbo);
        gl.clear(gl.COLOR_BUFFER_BIT);
        gl.enable(gl.SAMPLE_ALPHA_TO_COVERAGE);
        quad(0, [1, 1, 1, 0]);
        gl.disable(gl.SAMPLE_ALPHA_TO_COVERAGE);
        gl.bindFramebuffer(gl.READ_FRAMEBUFFER, msFbo);
        gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, resolved);
        gl.blitFramebuffer(0, 0, 16, 16, 0, 0, 16, 16, gl.COLOR_BUFFER_BIT, gl.NEAREST);
        gl.bindFramebuffer(gl.READ_FRAMEBUFFER, resolved);
        const zero = px(8, 8);
        assert(zero[0] === 0, 'alpha-to-coverage with alpha 0 covers no sample: ' + zero);
        assert(a2c[0] === 255, 'alpha-to-coverage with alpha 1 covers every sample: ' + a2c);
        assert(gl.getParameter(gl.SAMPLE_COVERAGE_VALUE) === 0.75 && gl.getParameter(gl.SAMPLE_COVERAGE_INVERT) === true,
               'SAMPLE_COVERAGE_* read back');
    }
    // On a single-sampled target coverage does nothing (GL ignores it there).
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, 64, 64);
    gl.clearColor(0, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.enable(gl.SAMPLE_COVERAGE);
    gl.sampleCoverage(0, false);
    quad(0, [1, 1, 1, 1]);
    gl.disable(gl.SAMPLE_COVERAGE);
    expectPx(32, 32, [255, 255, 255, 255], 'SAMPLE_COVERAGE is ignored single-sampled');
    expectError(gl.NO_ERROR, 'no stray errors');
}
