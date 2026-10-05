// WebGL2 texture copies and mip chains on Vulkan: copyTexImage2D /
// copyTexSubImage2D / copyTexSubImage3D from the canvas and from a
// framebuffer object (row order kept), the format rules for copies,
// generateMipmap (a real filtered chain, read back level by level), the
// per-level feedback-loop rule, and blits between levels of one texture.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '16');
canvas.setAttribute('height', '16');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2');
if (!gl) {
    missingGpuContext('webgl2');
} else {
    function program(fsBody, sampler) {
        const vs = '#version 300 es\nin vec2 aPos;\nout vec2 vUV;\n' +
            'void main(){ gl_Position = vec4(aPos, 0.0, 1.0); vUV = aPos * 0.5 + 0.5; }';
        const fs = '#version 300 es\nprecision highp float;\nprecision highp sampler2DArray;\n' +
            'uniform ' + sampler + ' uTex;\nuniform float uLod;\nin vec2 vUV;\nout vec4 frag;\n' +
            'void main(){ ' + fsBody + ' }';
        const p = gl.createProgram();
        for (const [type, src] of [[gl.VERTEX_SHADER, vs], [gl.FRAGMENT_SHADER, fs]]) {
            const s = gl.createShader(type);
            gl.shaderSource(s, src);
            gl.compileShader(s);
            if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s));
            gl.attachShader(p, s);
        }
        gl.bindAttribLocation(p, 0, 'aPos');
        gl.linkProgram(p);
        if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p));
        return p;
    }
    const quad = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, quad);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    const lodProg = program('frag = textureLod(uTex, vUV, uLod);', 'sampler2D');
    const layerProg = program('frag = texture(uTex, vec3(vUV, uLod));', 'sampler2DArray');

    function px(x, y) {
        const b = new Uint8Array(4);
        gl.readPixels(x, y, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, b);
        return Array.from(b);
    }
    function assertPx(got, want, msg, tol) {
        const ok = want.every((v, i) => Math.abs(got[i] - v) <= (tol === undefined ? 2 : tol));
        assert(ok, msg + ': got [' + got + '] want [' + want + ']');
    }
    function assertError(want, msg) {
        const e = gl.getError();
        assert(e === want, msg + ': got 0x' + e.toString(16) + ' want 0x' + want.toString(16));
    }
    function sample(prog, lod) {
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        gl.viewport(0, 0, 16, 16);
        gl.useProgram(prog);
        gl.uniform1f(gl.getUniformLocation(prog, 'uLod'), lod || 0);
        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    }
    // The canvas: bottom half red, top half green (GL rows, y up).
    function paintCanvas() {
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        gl.enable(gl.SCISSOR_TEST);
        gl.scissor(0, 0, 16, 8);
        gl.clearColor(1, 0, 0, 1);
        gl.clear(gl.COLOR_BUFFER_BIT);
        gl.scissor(0, 8, 16, 8);
        gl.clearColor(0, 1, 0, 1);
        gl.clear(gl.COLOR_BUFFER_BIT);
        gl.disable(gl.SCISSOR_TEST);
    }
    const fbo = gl.createFramebuffer();
    function readLevel(tex, level, x, y) {
        gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
        gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, tex, level);
        const p = px(x, y);
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        return p;
    }

    // copyTexImage2D from the canvas keeps GL row order: row 0 is red.
    paintCanvas();
    const copy = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, copy);
    gl.copyTexImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 0, 0, 16, 16, 0);
    assertError(gl.NO_ERROR, 'copyTexImage2D');
    assertPx(readLevel(copy, 0, 4, 2), [255, 0, 0, 255], 'copied bottom row is red');
    assertPx(readLevel(copy, 0, 4, 13), [0, 255, 0, 255], 'copied top row is green');

    // Unsized RGB from the canvas, partly outside it: outside reads zero.
    const rgb = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, rgb);
    gl.copyTexImage2D(gl.TEXTURE_2D, 0, gl.RGB, 8, 4, 16, 8, 0);
    assertError(gl.NO_ERROR, 'copyTexImage2D RGB, half outside');
    assertPx(readLevel(rgb, 0, 1, 1), [255, 0, 0, 255], 'inside: red, alpha one');
    assertPx(readLevel(rgb, 0, 1, 6), [0, 255, 0, 255], 'inside: green');
    assertPx(readLevel(rgb, 0, 12, 2), [0, 0, 0, 255], 'outside the canvas: zero');

    // copyTexSubImage2D from a framebuffer texture into another.
    gl.bindTexture(gl.TEXTURE_2D, rgb);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, fbo);
    gl.framebufferTexture2D(gl.READ_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, copy, 0);
    gl.copyTexSubImage2D(gl.TEXTURE_2D, 0, 12, 0, 0, 12, 4, 4);
    assertError(gl.NO_ERROR, 'copyTexSubImage2D from an FBO');
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, null);
    assertPx(readLevel(rgb, 0, 13, 1), [0, 255, 0, 255], 'sub-copy landed');
    // A copy into the very level being read is a feedback loop.
    gl.bindTexture(gl.TEXTURE_2D, copy);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, fbo);
    gl.framebufferTexture2D(gl.READ_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, copy, 0);
    gl.copyTexSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 0, 0, 2, 2);
    assertError(gl.INVALID_OPERATION, 'copy onto the level being read');
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, null);
    // An integer texture cannot take a normalized read buffer.
    const ui = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, ui);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8UI, 4, 4, 0, gl.RGBA_INTEGER, gl.UNSIGNED_BYTE, null);
    gl.copyTexSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 0, 0, 4, 4);
    assertError(gl.INVALID_OPERATION, 'normalized into integer');

    // copyTexSubImage3D into layer 1 of a 2D array.
    const arr = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D_ARRAY, arr);
    gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texStorage3D(gl.TEXTURE_2D_ARRAY, 1, gl.RGBA8, 4, 4, 2);
    paintCanvas();
    gl.copyTexSubImage3D(gl.TEXTURE_2D_ARRAY, 0, 0, 0, 1, 0, 10, 4, 4);
    assertError(gl.NO_ERROR, 'copyTexSubImage3D');
    sample(layerProg, 1);
    assertPx(px(8, 8), [0, 255, 0, 255], 'layer 1 took the green rows');
    sample(layerProg, 0);
    assertPx(px(8, 8), [0, 0, 0, 0], 'layer 0 untouched (zero)');

    // generateMipmap: a 4x4 red/blue checker averages to purple at level 2.
    const mip = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, mip);
    const checker = new Uint8Array(4 * 4 * 4);
    for (let i = 0; i < 16; ++i) checker.set(((i >> 2) + i) % 2 ? [0, 0, 255, 255] : [255, 0, 0, 255], i * 4);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 4, 4, 0, gl.RGBA, gl.UNSIGNED_BYTE, checker);
    gl.generateMipmap(gl.TEXTURE_2D);
    assertError(gl.NO_ERROR, 'generateMipmap');
    assertPx(readLevel(mip, 2, 0, 0), [128, 0, 128, 255], 'level 2 is the average', 4);
    gl.bindTexture(gl.TEXTURE_2D, mip);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST_MIPMAP_NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    sample(lodProg, 2);
    assertPx(px(8, 8), [128, 0, 128, 255], 'textureLod reads level 2', 4);
    // Integer formats cannot be filtered into a chain.
    gl.bindTexture(gl.TEXTURE_2D, ui);
    gl.generateMipmap(gl.TEXTURE_2D);
    assertError(gl.INVALID_OPERATION, 'generateMipmap on an integer texture');

    // Feedback per level: drawing into level 0 while sampling level 1 only
    // (BASE_LEVEL 1) is fine; sampling level 0 too is a feedback loop.
    gl.bindTexture(gl.TEXTURE_2D, mip);
    gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, mip, 0);
    gl.viewport(0, 0, 4, 4);
    gl.useProgram(lodProg);
    gl.uniform1f(gl.getUniformLocation(lodProg, 'uLod'), 0);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_BASE_LEVEL, 1);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    assertError(gl.NO_ERROR, 'sampling level 1 while drawing level 0');
    assertPx(px(0, 0), [128, 0, 128, 255], 'level 1 (each texel a red/blue average) drawn', 4);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_BASE_LEVEL, 0);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    assertError(gl.INVALID_OPERATION, 'sampling the level being drawn');

    // blitFramebuffer between two levels of one texture.
    const blitTex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, blitTex);
    gl.texStorage2D(gl.TEXTURE_2D, 2, gl.RGBA8, 4, 4);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 4, 4, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array(64).fill(200));
    const readFbo = gl.createFramebuffer();
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, readFbo);
    gl.framebufferTexture2D(gl.READ_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, blitTex, 0);
    gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, fbo);
    gl.framebufferTexture2D(gl.DRAW_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, blitTex, 1);
    gl.blitFramebuffer(0, 0, 4, 4, 0, 0, 2, 2, gl.COLOR_BUFFER_BIT, gl.NEAREST);
    assertError(gl.NO_ERROR, 'blit level 0 onto level 1 of the same texture');
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, null);
    gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, null);
    assertPx(readLevel(blitTex, 1, 1, 1), [200, 200, 200, 200], 'level 1 holds the blit');
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, readFbo);
    gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, fbo);
    gl.framebufferTexture2D(gl.DRAW_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, blitTex, 0);
    gl.blitFramebuffer(0, 0, 4, 4, 0, 0, 4, 4, gl.COLOR_BUFFER_BIT, gl.NEAREST);
    assertError(gl.INVALID_OPERATION, 'blit a level onto itself');

    console.log('webgl texture copy tests passed');
}

document.body.removeChild(canvas);
