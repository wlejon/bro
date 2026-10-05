// Framebuffer objects with depth and stencil: depth renderbuffers and depth
// textures occlude, DEPTH_STENCIL renderbuffers mask with the stencil test,
// clear/clearBuffer* reach depth and stencil, DRAW and READ bindings are
// separate, completeness answers each failure, and the attachment /
// renderbuffer getters report what was attached. Every check reads pixels
// back, so an attachment that is accepted but not rendered into fails.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '32');
canvas.setAttribute('height', '32');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2');
if (!gl) {
    missingGpuContext('webgl2');
} else {
    function program(vsSrc, fsSrc) {
        const p = gl.createProgram();
        for (const [type, src] of [[gl.VERTEX_SHADER, vsSrc], [gl.FRAGMENT_SHADER, fsSrc]]) {
            const s = gl.createShader(type);
            gl.shaderSource(s, src);
            gl.compileShader(s);
            assert(gl.getShaderParameter(s, gl.COMPILE_STATUS), 'compile: ' + gl.getShaderInfoLog(s));
            gl.attachShader(p, s);
        }
        gl.linkProgram(p);
        assert(gl.getProgramParameter(p, gl.LINK_STATUS), 'link: ' + gl.getProgramInfoLog(p));
        return p;
    }
    function pixel(x, y) {
        const px = new Uint8Array(4);
        gl.readPixels(x, y, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
        return Array.from(px);
    }
    function assertPixel(x, y, want, msg) {
        const got = pixel(x, y);
        assert(got.every((v, i) => Math.abs(v - want[i]) <= 2),
               msg + ': got [' + got + '] want [' + want + ']');
    }
    function assertError(want, msg) {
        const got = gl.getError();
        assert(got === want, msg + ': error 0x' + got.toString(16) + ' want 0x' + want.toString(16));
    }

    // A quad at depth z (NDC) in a solid color; the left half of clip space
    // when `half` is set.
    const prog = program(
        '#version 300 es\nin vec2 aPos;\nuniform float uZ;\nuniform float uHalf;\n' +
        'void main(){ vec2 p = uHalf > 0.5 ? vec2(aPos.x * 0.5 - 0.5, aPos.y) : aPos; gl_Position = vec4(p, uZ, 1.0); }',
        '#version 300 es\nprecision highp float;\nuniform vec4 uColor;\nout vec4 o;\nvoid main(){ o = uColor; }');
    gl.useProgram(prog);
    const uZ = gl.getUniformLocation(prog, 'uZ');
    const uHalf = gl.getUniformLocation(prog, 'uHalf');
    const uColor = gl.getUniformLocation(prog, 'uColor');
    const buf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, buf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    const vao = gl.createVertexArray();
    gl.bindVertexArray(vao);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    function quad(z, color, half) {
        gl.uniform1f(uZ, z);
        gl.uniform1f(uHalf, half ? 1 : 0);
        gl.uniform4fv(uColor, color);
        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    }
    function colorTexture(w, h) {
        const t = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, t);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
        return t;
    }
    gl.viewport(0, 0, 32, 32);

    // --- depth renderbuffer occludes -------------------------------------
    const fbo = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, colorTexture(32, 32), 0);
    const depthRb = gl.createRenderbuffer();
    gl.bindRenderbuffer(gl.RENDERBUFFER, depthRb);
    gl.renderbufferStorage(gl.RENDERBUFFER, gl.DEPTH_COMPONENT16, 32, 32);
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_ATTACHMENT, gl.RENDERBUFFER, depthRb);
    assert(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE, 'color + depth16 complete');
    assert(gl.getParameter(gl.DEPTH_BITS) >= 16, 'DEPTH_BITS of the FBO >= 16, got ' + gl.getParameter(gl.DEPTH_BITS));
    assert(gl.getParameter(gl.STENCIL_BITS) === 0, 'depth-only FBO has no stencil bits');

    gl.clearColor(0, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    gl.enable(gl.DEPTH_TEST);
    gl.depthFunc(gl.LESS);
    quad(-0.5, [0, 1, 0, 1], true);     // near, left half
    quad(0.5, [1, 0, 0, 1], false);     // far, whole target
    assertPixel(4, 16, [0, 255, 0, 255], 'near green survives the far red draw (depth renderbuffer)');
    assertPixel(28, 16, [255, 0, 0, 255], 'far red fills where nothing was nearer');

    // clearBufferfv(DEPTH) resets to the given value: at 0.0 nothing passes LESS.
    gl.clearBufferfv(gl.DEPTH, 0, [0.0]);
    quad(-0.9, [0, 0, 1, 1], false);
    assertPixel(28, 16, [255, 0, 0, 255], 'clearBufferfv(DEPTH, 0) made every fragment fail LESS');
    gl.clearBufferfv(gl.DEPTH, 0, [1.0]);
    quad(0.9, [0, 0, 1, 1], false);
    assertPixel(28, 16, [0, 0, 255, 255], 'clearBufferfv(DEPTH, 1) lets fragments through again');

    // depthMask(false) keeps clear() from touching depth.
    gl.depthMask(false);
    gl.clearDepth(0.0);
    gl.clear(gl.DEPTH_BUFFER_BIT);
    gl.depthMask(true);
    gl.clearDepth(1.0);
    quad(0.0, [1, 1, 0, 1], false);
    assertPixel(28, 16, [255, 255, 0, 255], 'a clear under depthMask(false) left depth at 0.95: z = 0 passes');
    gl.disable(gl.DEPTH_TEST);

    // --- depth texture occludes, and samples ----------------------------
    const fboTex = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fboTex);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, colorTexture(32, 32), 0);
    const depthTex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, depthTex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.DEPTH_COMPONENT32F, 32, 32, 0, gl.DEPTH_COMPONENT, gl.FLOAT, null);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.DEPTH_ATTACHMENT, gl.TEXTURE_2D, depthTex, 0);
    assert(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE, 'color + depth texture complete');
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    gl.enable(gl.DEPTH_TEST);
    quad(-0.5, [0, 1, 0, 1], true);
    quad(0.5, [1, 0, 0, 1], false);
    assertPixel(4, 16, [0, 255, 0, 255], 'depth texture attachment occludes');
    gl.disable(gl.DEPTH_TEST);

    // Sampling a texture the draw framebuffer renders into is a feedback loop.
    const sampleProg = program(
        '#version 300 es\nin vec2 aPos;\nout vec2 vUv;\nvoid main(){ vUv = aPos * 0.5 + 0.5; gl_Position = vec4(aPos, 0.0, 1.0); }',
        '#version 300 es\nprecision highp float;\nuniform sampler2D uDepth;\nin vec2 vUv;\nout vec4 o;\n' +
        'void main(){ o = vec4(texture(uDepth, vUv).r, 0.0, 0.0, 1.0); }');
    gl.useProgram(sampleProg);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, depthTex);
    gl.getError();
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    assertError(gl.INVALID_OPERATION, 'sampling the attached depth texture is a feedback loop');

    // Sampled from the canvas, the written depth reads back: left half at
    // window depth 0.25 (z = -0.5), right half 0.75.
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    assertError(gl.NO_ERROR, 'sampling the depth texture from the canvas');
    const left = pixel(4, 16)[0], right = pixel(28, 16)[0];
    assert(Math.abs(left - 64) <= 3 && Math.abs(right - 191) <= 3,
           'depth texture holds the drawn depths: ' + left + ', ' + right);
    gl.useProgram(prog);

    // --- stencil masks (DEPTH24_STENCIL8 renderbuffer) --------------------
    const fboS = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fboS);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, colorTexture(32, 32), 0);
    const dsRb = gl.createRenderbuffer();
    gl.bindRenderbuffer(gl.RENDERBUFFER, dsRb);
    gl.renderbufferStorage(gl.RENDERBUFFER, gl.DEPTH24_STENCIL8, 32, 32);
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_STENCIL_ATTACHMENT, gl.RENDERBUFFER, dsRb);
    assert(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE, 'color + depth24stencil8 complete');
    assert(gl.getParameter(gl.STENCIL_BITS) === 8, 'STENCIL_BITS 8');

    gl.clearStencil(0);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.STENCIL_BUFFER_BIT);
    gl.enable(gl.STENCIL_TEST);
    gl.stencilFunc(gl.ALWAYS, 1, 0xFF);
    gl.stencilOp(gl.KEEP, gl.KEEP, gl.REPLACE);
    gl.colorMask(false, false, false, false);
    quad(0, [1, 1, 1, 1], true);            // stencil = 1 in the left half
    gl.colorMask(true, true, true, true);
    gl.stencilFunc(gl.EQUAL, 1, 0xFF);
    gl.stencilOp(gl.KEEP, gl.KEEP, gl.KEEP);
    quad(0, [1, 0, 1, 1], false);
    assertPixel(4, 16, [255, 0, 255, 255], 'stencil == 1 passes in the marked half');
    assertPixel(28, 16, [0, 0, 0, 255], 'stencil test rejects the unmarked half');

    // clearBufferiv(STENCIL) and clearBufferfi(DEPTH_STENCIL).
    gl.clearBufferiv(gl.STENCIL, 0, [1]);
    quad(0, [0, 1, 1, 1], false);
    assertPixel(28, 16, [0, 255, 255, 255], 'clearBufferiv(STENCIL, 0, 1) marks everything');
    gl.clearBufferfi(gl.DEPTH_STENCIL, 0, 1.0, 0);
    quad(0, [1, 1, 0, 1], false);
    assertPixel(28, 16, [0, 255, 255, 255], 'clearBufferfi reset stencil to 0: nothing passes EQUAL 1');
    gl.disable(gl.STENCIL_TEST);
    gl.clearBufferfi(gl.DEPTH_STENCIL, 1, 1.0, 0);
    assertError(gl.INVALID_VALUE, 'clearBufferfi drawbuffer must be 0');
    gl.clearBufferfi(gl.COLOR, 0, 1.0, 0);
    assertError(gl.INVALID_ENUM, 'clearBufferfi takes DEPTH_STENCIL only');

    // Getters for what is attached.
    const P = (att, pname) => gl.getFramebufferAttachmentParameter(gl.FRAMEBUFFER, att, pname);
    assert(P(gl.DEPTH_STENCIL_ATTACHMENT, gl.FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE) === gl.RENDERBUFFER,
           'DEPTH_STENCIL attachment is a renderbuffer');
    // (Binding queries answer a fresh wrapper of the object, not the page's own.)
    assert(gl.isRenderbuffer(P(gl.DEPTH_STENCIL_ATTACHMENT, gl.FRAMEBUFFER_ATTACHMENT_OBJECT_NAME)),
           'DEPTH_STENCIL attachment names the renderbuffer');
    assert(P(gl.STENCIL_ATTACHMENT, gl.FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE) === 8, 'stencil size 8');
    assert(P(gl.COLOR_ATTACHMENT0, gl.FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE) === gl.TEXTURE, 'color is a texture');
    assert(P(gl.COLOR_ATTACHMENT0, gl.FRAMEBUFFER_ATTACHMENT_RED_SIZE) === 8, 'RGBA8 red size 8');
    assert(P(gl.COLOR_ATTACHMENT1, gl.FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE) === gl.NONE, 'nothing at COLOR_ATTACHMENT1');
    assert(P(gl.COLOR_ATTACHMENT1, gl.FRAMEBUFFER_ATTACHMENT_OBJECT_NAME) === null, 'no object at COLOR_ATTACHMENT1');
    gl.bindRenderbuffer(gl.RENDERBUFFER, dsRb);
    assert(gl.getRenderbufferParameter(gl.RENDERBUFFER, gl.RENDERBUFFER_WIDTH) === 32, 'renderbuffer width');
    assert(gl.getRenderbufferParameter(gl.RENDERBUFFER, gl.RENDERBUFFER_INTERNAL_FORMAT) === gl.DEPTH24_STENCIL8,
           'renderbuffer internal format');
    assert(gl.getRenderbufferParameter(gl.RENDERBUFFER, gl.RENDERBUFFER_DEPTH_SIZE) >= 24, 'renderbuffer depth size');
    assert(gl.isRenderbuffer(gl.getParameter(gl.RENDERBUFFER_BINDING)), 'RENDERBUFFER_BINDING is a renderbuffer');

    // --- DRAW and READ bindings are separate ------------------------------
    const fboA = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fboA);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, colorTexture(32, 32), 0);
    gl.clearColor(0, 0, 1, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, null);   // draw to the canvas, still read A
    gl.clearColor(1, 1, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    assert(gl.isFramebuffer(gl.getParameter(gl.READ_FRAMEBUFFER_BINDING)), 'READ_FRAMEBUFFER_BINDING is A');
    assert(gl.getParameter(gl.DRAW_FRAMEBUFFER_BINDING) === null, 'DRAW_FRAMEBUFFER_BINDING is the canvas');
    assertPixel(5, 5, [0, 0, 255, 255], 'readPixels reads the READ framebuffer, untouched by the canvas clear');
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, null);
    assertPixel(5, 5, [255, 255, 0, 255], 'and the canvas got the clear');

    // --- completeness ------------------------------------------------------
    const fboC = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fboC);
    assert(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT,
           'nothing attached: MISSING_ATTACHMENT');
    gl.getError();
    gl.clear(gl.COLOR_BUFFER_BIT);
    assertError(gl.INVALID_FRAMEBUFFER_OPERATION, 'clear on an incomplete framebuffer');
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    assertError(gl.INVALID_FRAMEBUFFER_OPERATION, 'draw on an incomplete framebuffer');
    const empty = gl.createTexture();   // no storage
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, empty, 0);
    assert(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_INCOMPLETE_ATTACHMENT,
           'texture without storage: INCOMPLETE_ATTACHMENT');
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, colorTexture(32, 32), 0);
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_ATTACHMENT, gl.RENDERBUFFER, depthRb);
    assert(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE, 'color + depth complete');
    const colorRb = gl.createRenderbuffer();
    gl.bindRenderbuffer(gl.RENDERBUFFER, colorRb);
    gl.renderbufferStorage(gl.RENDERBUFFER, gl.RGBA8, 32, 32);
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_ATTACHMENT, gl.RENDERBUFFER, colorRb);
    assert(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_INCOMPLETE_ATTACHMENT,
           'a color format at DEPTH_ATTACHMENT: INCOMPLETE_ATTACHMENT');
    const stencilRb = gl.createRenderbuffer();
    gl.bindRenderbuffer(gl.RENDERBUFFER, stencilRb);
    gl.renderbufferStorage(gl.RENDERBUFFER, gl.DEPTH24_STENCIL8, 32, 32);
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_ATTACHMENT, gl.RENDERBUFFER, depthRb);
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.STENCIL_ATTACHMENT, gl.RENDERBUFFER, stencilRb);
    assert(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_UNSUPPORTED,
           'different depth and stencil images: UNSUPPORTED');
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.STENCIL_ATTACHMENT, gl.RENDERBUFFER, null);
    const samples = gl.getInternalformatParameter(gl.RENDERBUFFER, gl.RGBA8, gl.SAMPLES);
    if (samples.length > 0) {
        const msRb = gl.createRenderbuffer();
        gl.bindRenderbuffer(gl.RENDERBUFFER, msRb);
        gl.renderbufferStorageMultisample(gl.RENDERBUFFER, samples[0], gl.RGBA8, 32, 32);
        assert(gl.getRenderbufferParameter(gl.RENDERBUFFER, gl.RENDERBUFFER_SAMPLES) >= samples[0],
               'renderbuffer has the samples asked for');
        gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT1, gl.RENDERBUFFER, msRb);
        assert(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_INCOMPLETE_MULTISAMPLE,
               'mixed sample counts: INCOMPLETE_MULTISAMPLE');
    }
    gl.renderbufferStorageMultisample(gl.RENDERBUFFER, gl.getParameter(gl.MAX_SAMPLES) + 1, gl.RGBA8, 4, 4);
    assert(gl.getError() !== gl.NO_ERROR, 'more samples than MAX_SAMPLES is an error');

    // --- drawBuffers / readBuffer validation -------------------------------
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.drawBuffers([gl.COLOR_ATTACHMENT0]);
    assertError(gl.INVALID_OPERATION, 'the canvas takes BACK or NONE');
    gl.drawBuffers([gl.BACK]);
    assertError(gl.NO_ERROR, 'drawBuffers([BACK]) on the canvas');
    assert(gl.getParameter(gl.DRAW_BUFFER0) === gl.BACK, 'DRAW_BUFFER0 is BACK');
    gl.readBuffer(gl.COLOR_ATTACHMENT0);
    assertError(gl.INVALID_OPERATION, 'the canvas reads BACK or NONE');
    gl.bindFramebuffer(gl.FRAMEBUFFER, fboA);
    gl.drawBuffers([gl.NONE, gl.COLOR_ATTACHMENT0]);
    assertError(gl.INVALID_OPERATION, 'buffer i must be COLOR_ATTACHMENTi or NONE');
    gl.drawBuffers([gl.NONE]);
    gl.clearColor(1, 1, 1, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    assertPixel(5, 5, [0, 0, 255, 255], 'drawBuffers([NONE]) writes nothing');
    gl.drawBuffers([gl.COLOR_ATTACHMENT0]);
    gl.readBuffer(gl.NONE);
    gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array(4));
    assertError(gl.INVALID_OPERATION, 'readPixels with READ_BUFFER NONE');
    gl.readBuffer(gl.COLOR_ATTACHMENT0);
    assertError(gl.NO_ERROR, 'end of test');
}
