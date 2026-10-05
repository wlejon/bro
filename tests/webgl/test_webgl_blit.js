// blitFramebuffer: framebuffer object <-> canvas in both directions (the
// canvas is stored top-down, so a blit that ignored that would land upside
// down), scaling, mirroring, the scissor box, depth and stencil blits, and
// multisampled renderbuffers resolved by a blit — color and depth. Every
// case reads the result back, and the canvas orientation is checked on the
// composited frame too.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '32');
canvas.setAttribute('height', '32');
canvas.style.position = 'absolute';
canvas.style.left = '0px';
canvas.style.top = '0px';
document.body.style.margin = '0';
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
        assert(got.every((v, i) => Math.abs(v - want[i]) <= 2), msg + ': got [' + got + '] want [' + want + ']');
    }
    function assertError(want, msg) {
        const got = gl.getError();
        assert(got === want, msg + ': error 0x' + got.toString(16) + ' want 0x' + want.toString(16));
    }
    const RED = [255, 0, 0, 255], GREEN = [0, 255, 0, 255], BLUE = [0, 0, 255, 255], BLACK = [0, 0, 0, 255];

    const prog = program(
        '#version 300 es\nin vec2 aPos;\nuniform vec4 uRect;\nuniform float uZ;\n' +
        'void main(){ gl_Position = vec4(mix(uRect.xy, uRect.zw, aPos * 0.5 + 0.5), uZ, 1.0); }',
        '#version 300 es\nprecision highp float;\nuniform vec4 uColor;\nout vec4 o;\nvoid main(){ o = uColor; }');
    gl.useProgram(prog);
    const uRect = gl.getUniformLocation(prog, 'uRect');
    const uZ = gl.getUniformLocation(prog, 'uZ');
    const uColor = gl.getUniformLocation(prog, 'uColor');
    const buf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, buf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    const vao = gl.createVertexArray();
    gl.bindVertexArray(vao);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    // A rectangle in clip space (x0, y0, x1, y1) at depth z.
    function rect(r, color, z) {
        gl.uniform4fv(uRect, r);
        gl.uniform1f(uZ, z || 0);
        gl.uniform4fv(uColor, color.map(v => v / 255));
        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    }
    function textureFbo(w, h, internal) {
        const t = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, t);
        gl.texStorage2D(gl.TEXTURE_2D, 1, internal || gl.RGBA8, w, h);
        const f = gl.createFramebuffer();
        gl.bindFramebuffer(gl.FRAMEBUFFER, f);
        gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, t, 0);
        assert(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE, 'texture FBO complete');
        return f;
    }
    gl.viewport(0, 0, 32, 32);

    // A source FBO: bottom half (GL y < 16) green, top half red.
    const src = textureFbo(32, 32);
    rect([-1, -1, 1, 0], GREEN);
    rect([-1, 0, 1, 1], RED);
    assertPixel(4, 4, GREEN, 'source bottom');
    assertPixel(4, 28, RED, 'source top');

    // --- FBO -> canvas, 1:1 -----------------------------------------------
    gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, null);
    gl.blitFramebuffer(0, 0, 32, 32, 0, 0, 32, 32, gl.COLOR_BUFFER_BIT, gl.NEAREST);
    assertError(gl.NO_ERROR, 'blit FBO -> canvas');
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, null);
    assertPixel(4, 4, GREEN, 'canvas bottom after blit');
    assertPixel(4, 28, RED, 'canvas top after blit');
    // On screen the top of the canvas is GL's top: red.
    const top = getPixel(4, 2), bottom = getPixel(4, 29);
    assert(top.r > 200 && top.g < 50 && bottom.g > 200 && bottom.r < 50,
           'composited canvas is upright: top ' + JSON.stringify(top) + ' bottom ' + JSON.stringify(bottom));

    // --- canvas -> FBO ----------------------------------------------------
    const back = textureFbo(32, 32);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, null);
    gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, back);
    gl.blitFramebuffer(0, 0, 32, 32, 0, 0, 32, 32, gl.COLOR_BUFFER_BIT, gl.NEAREST);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, back);
    assertPixel(4, 4, GREEN, 'canvas -> FBO keeps the bottom at the bottom');
    assertPixel(4, 28, RED, 'canvas -> FBO keeps the top at the top');

    // --- scaled, mirrored and scissored blits onto the canvas --------------
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.clearColor(0, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, src);
    gl.blitFramebuffer(0, 0, 32, 32, 0, 0, 16, 16, gl.COLOR_BUFFER_BIT, gl.NEAREST);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, null);
    assertPixel(4, 3, GREEN, 'half-size blit: bottom quarter green');
    assertPixel(4, 12, RED, 'half-size blit: its top red');
    assertPixel(24, 24, BLACK, 'half-size blit leaves the rest');

    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, src);
    gl.blitFramebuffer(0, 0, 32, 32, 0, 32, 32, 0, gl.COLOR_BUFFER_BIT, gl.NEAREST);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, null);
    assertPixel(4, 4, RED, 'y-mirrored blit: red at the bottom');
    assertPixel(4, 28, GREEN, 'y-mirrored blit: green at the top');

    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.enable(gl.SCISSOR_TEST);
    gl.scissor(0, 0, 8, 32);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, src);
    gl.blitFramebuffer(0, 0, 32, 32, 0, 0, 32, 32, gl.COLOR_BUFFER_BIT, gl.NEAREST);
    gl.disable(gl.SCISSOR_TEST);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, null);
    assertPixel(4, 4, GREEN, 'scissored blit writes inside the box');
    assertPixel(20, 4, BLACK, 'scissored blit leaves outside the box');

    // A source rectangle reaching outside the source writes only what exists.
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, src);
    gl.blitFramebuffer(16, 0, 48, 32, 0, 0, 32, 32, gl.COLOR_BUFFER_BIT, gl.NEAREST);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, null);
    assertPixel(4, 4, GREEN, 'clipped blit: the part inside the source');
    assertPixel(24, 4, BLACK, 'clipped blit: nothing where the source had no pixels');

    // --- depth blit FBO -> canvas -------------------------------------------
    const dsFbo = textureFbo(32, 32);
    const ds = gl.createRenderbuffer();
    gl.bindRenderbuffer(gl.RENDERBUFFER, ds);
    gl.renderbufferStorage(gl.RENDERBUFFER, gl.DEPTH24_STENCIL8, 32, 32);
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_STENCIL_ATTACHMENT, gl.RENDERBUFFER, ds);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    gl.enable(gl.DEPTH_TEST);
    rect([-1, -1, 0, 1], GREEN, -0.5);      // left half at window depth 0.25
    gl.disable(gl.DEPTH_TEST);
    gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, null);
    gl.blitFramebuffer(0, 0, 32, 32, 0, 0, 32, 32, gl.DEPTH_BUFFER_BIT, gl.LINEAR);
    assertError(gl.INVALID_OPERATION, 'depth blit with LINEAR');
    gl.blitFramebuffer(0, 0, 32, 32, 0, 0, 32, 32, gl.DEPTH_BUFFER_BIT | gl.STENCIL_BUFFER_BIT, gl.NEAREST);
    assertError(gl.NO_ERROR, 'depth+stencil blit FBO -> canvas');
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.enable(gl.DEPTH_TEST);
    gl.depthFunc(gl.LESS);
    rect([-1, -1, 1, 1], BLUE, 0.0);         // window depth 0.5
    gl.disable(gl.DEPTH_TEST);
    assertPixel(4, 16, BLACK, 'blitted depth (0.25) occludes on the canvas');
    assertPixel(28, 16, BLUE, 'blitted depth (1.0) lets the draw through');
    // Mismatched depth formats cannot be blitted.
    const d16Fbo = textureFbo(32, 32);
    const d16 = gl.createRenderbuffer();
    gl.bindRenderbuffer(gl.RENDERBUFFER, d16);
    gl.renderbufferStorage(gl.RENDERBUFFER, gl.DEPTH_COMPONENT16, 32, 32);
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_ATTACHMENT, gl.RENDERBUFFER, d16);
    gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, null);
    gl.blitFramebuffer(0, 0, 32, 32, 0, 0, 32, 32, gl.DEPTH_BUFFER_BIT, gl.NEAREST);
    assertError(gl.INVALID_OPERATION, 'depth blit between different depth formats');

    // --- integer validation ------------------------------------------------
    function renderbufferFbo(w, h, internal) {
        const rb = gl.createRenderbuffer();
        gl.bindRenderbuffer(gl.RENDERBUFFER, rb);
        gl.renderbufferStorage(gl.RENDERBUFFER, internal, w, h);
        const f = gl.createFramebuffer();
        gl.bindFramebuffer(gl.FRAMEBUFFER, f);
        gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, rb);
        assert(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE, 'renderbuffer FBO complete');
        return f;
    }
    const intFbo = renderbufferFbo(8, 8, gl.RGBA8UI);
    gl.clearBufferuiv(gl.COLOR, 0, [7, 8, 9, 10]);
    gl.clearBufferfv(gl.COLOR, 0, [1, 1, 1, 1]);
    assertError(gl.INVALID_OPERATION, 'clearBufferfv on an unsigned integer buffer');
    gl.clear(gl.COLOR_BUFFER_BIT);
    assertError(gl.INVALID_OPERATION, 'clear() on an integer buffer');
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, intFbo);
    gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, null);
    gl.blitFramebuffer(0, 0, 8, 8, 0, 0, 8, 8, gl.COLOR_BUFFER_BIT, gl.NEAREST);
    assertError(gl.INVALID_OPERATION, 'integer -> normalized blit');
    const intDst = renderbufferFbo(8, 8, gl.RGBA8UI);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, intFbo);
    gl.blitFramebuffer(0, 0, 8, 8, 0, 0, 8, 8, gl.COLOR_BUFFER_BIT, gl.LINEAR);
    assertError(gl.INVALID_OPERATION, 'integer blit with LINEAR');
    gl.blitFramebuffer(0, 0, 8, 8, 0, 0, 8, 8, gl.COLOR_BUFFER_BIT, gl.NEAREST);
    assertError(gl.NO_ERROR, 'integer -> integer blit');
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, intDst);
    const ints = new Uint32Array(4);
    gl.readPixels(3, 3, 1, 1, gl.RGBA_INTEGER, gl.UNSIGNED_INT, ints);
    assert(Array.from(ints).join() === '7,8,9,10', 'integer blit carries the clearBufferuiv values: ' + Array.from(ints));
    gl.readPixels(3, 3, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array(4));
    assertError(gl.INVALID_OPERATION, 'an integer buffer does not read as RGBA/UNSIGNED_BYTE');
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, intFbo);
    gl.blitFramebuffer(0, 0, 8, 8, 0, 0, 8, 8, 0x8000, gl.NEAREST);
    assertError(gl.INVALID_VALUE, 'unknown mask bit');

    // --- multisampled color: resolve by blit -------------------------------
    const counts = gl.getInternalformatParameter(gl.RENDERBUFFER, gl.RGBA8, gl.SAMPLES);
    assert(counts.length > 0 && gl.getParameter(gl.MAX_SAMPLES) >= counts[0],
           'some multisample count for RGBA8, within MAX_SAMPLES');
    const n = counts[0];
    const msFbo = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, msFbo);
    const msColor = gl.createRenderbuffer();
    gl.bindRenderbuffer(gl.RENDERBUFFER, msColor);
    gl.renderbufferStorageMultisample(gl.RENDERBUFFER, n, gl.RGBA8, 32, 32);
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, msColor);
    const msDepth = gl.createRenderbuffer();
    gl.bindRenderbuffer(gl.RENDERBUFFER, msDepth);
    gl.renderbufferStorageMultisample(gl.RENDERBUFFER, n, gl.DEPTH24_STENCIL8, 32, 32);
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_STENCIL_ATTACHMENT, gl.RENDERBUFFER, msDepth);
    assert(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE, 'multisampled FBO complete');
    assert(gl.getParameter(gl.SAMPLES) === gl.getRenderbufferParameter(gl.RENDERBUFFER, gl.RENDERBUFFER_SAMPLES) &&
           gl.getParameter(gl.SAMPLE_BUFFERS) === 1, 'SAMPLES / SAMPLE_BUFFERS of the multisampled FBO');
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    // A triangle whose hypotenuse crosses pixels diagonally.
    const tri = program(
        '#version 300 es\nin vec2 aPos;\nuniform float uZ;\nvoid main(){ gl_Position = vec4(aPos, uZ, 1.0); }',
        '#version 300 es\nprecision highp float;\nout vec4 o;\nvoid main(){ o = vec4(1.0, 0.0, 0.0, 1.0); }');
    gl.useProgram(tri);
    const triBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, triBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1]), gl.STATIC_DRAW);
    const triVao = gl.createVertexArray();
    gl.bindVertexArray(triVao);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    gl.enable(gl.DEPTH_TEST);
    gl.uniform1f(gl.getUniformLocation(tri, 'uZ'), -0.5);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    gl.disable(gl.DEPTH_TEST);
    gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array(4));
    assertError(gl.INVALID_OPERATION, 'readPixels from a multisampled framebuffer');

    const resolved = textureFbo(32, 32);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, msFbo);
    gl.blitFramebuffer(0, 0, 32, 32, 0, 0, 16, 16, gl.COLOR_BUFFER_BIT, gl.NEAREST);
    assertError(gl.INVALID_OPERATION, 'a multisampled blit must not scale');
    gl.blitFramebuffer(0, 0, 32, 32, 0, 0, 32, 32, gl.COLOR_BUFFER_BIT, gl.NEAREST);
    assertError(gl.NO_ERROR, 'resolve blit');
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, resolved);
    assertPixel(2, 2, RED, 'resolved interior is the triangle');
    assertPixel(29, 29, [0, 0, 0, 255], 'resolved exterior is the clear');
    const row = new Uint8Array(32 * 4);
    let partial = 0;
    for (let y = 0; y < 32; ++y) {
        gl.readPixels(0, y, 32, 1, gl.RGBA, gl.UNSIGNED_BYTE, row);
        for (let x = 0; x < 32; ++x) if (row[x * 4] > 20 && row[x * 4] < 235) ++partial;
    }
    assert(partial >= 8, 'the resolved hypotenuse is antialiased (' + partial + ' partial pixels)');

    // Resolved straight onto the canvas, upright.
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, msFbo);
    gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, null);
    gl.blitFramebuffer(0, 0, 32, 32, 0, 0, 32, 32, gl.COLOR_BUFFER_BIT, gl.NEAREST);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, null);
    assertPixel(2, 2, RED, 'resolve onto the canvas: triangle at the bottom left');
    assertPixel(29, 29, BLACK, 'resolve onto the canvas: clear at the top right');

    // Multisampled depth resolves too (sample zero): the resolved depth
    // occludes a later draw where the triangle was.
    const dsResolved = textureFbo(32, 32);
    const dsTarget = gl.createRenderbuffer();
    gl.bindRenderbuffer(gl.RENDERBUFFER, dsTarget);
    gl.renderbufferStorage(gl.RENDERBUFFER, gl.DEPTH24_STENCIL8, 32, 32);
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_STENCIL_ATTACHMENT, gl.RENDERBUFFER, dsTarget);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, msFbo);
    gl.blitFramebuffer(0, 0, 32, 32, 0, 0, 32, 32, gl.DEPTH_BUFFER_BIT, gl.NEAREST);
    assertError(gl.NO_ERROR, 'multisampled depth resolve blit');
    gl.bindFramebuffer(gl.FRAMEBUFFER, dsResolved);
    gl.useProgram(prog);
    gl.bindVertexArray(vao);
    gl.enable(gl.DEPTH_TEST);
    rect([-1, -1, 1, 1], BLUE, 0.0);
    gl.disable(gl.DEPTH_TEST);
    assertPixel(2, 2, BLACK, 'resolved depth occludes inside the triangle');
    assertPixel(29, 29, BLUE, 'and passes outside it');

    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, resolved);
    gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, msFbo);
    gl.blitFramebuffer(0, 0, 32, 32, 0, 0, 32, 32, gl.COLOR_BUFFER_BIT, gl.NEAREST);
    assertError(gl.INVALID_OPERATION, 'blit into a multisampled framebuffer');
    assertError(gl.NO_ERROR, 'end of test');
}
