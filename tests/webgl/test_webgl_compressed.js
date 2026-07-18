// WebGL2 conformance subset — compressed textures: driver-gated extension
// surfacing (S3TC / RGTC / BPTC — desktop GL has no ETC2),
// getParameter(COMPRESSED_TEXTURE_FORMATS), hand-encoded DXT1/RGTC1 blocks
// uploaded and verified via pixels, block-size and alignment validation.
// Exercises src/js/webgl2_bindings_textures.cpp, _queries.cpp +
// src/webgl/webgl2_context.cpp.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '64');
canvas.setAttribute('height', '64');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2');
if (!gl) {
    console.log('no webgl2; skipping');
} else {
    function makeProgram(vsSrc, fsSrc) {
        const vs = gl.createShader(gl.VERTEX_SHADER);
        gl.shaderSource(vs, vsSrc); gl.compileShader(vs);
        if (!gl.getShaderParameter(vs, gl.COMPILE_STATUS))
            throw new Error('vs: ' + gl.getShaderInfoLog(vs));
        const fs = gl.createShader(gl.FRAGMENT_SHADER);
        gl.shaderSource(fs, fsSrc); gl.compileShader(fs);
        if (!gl.getShaderParameter(fs, gl.COMPILE_STATUS))
            throw new Error('fs: ' + gl.getShaderInfoLog(fs));
        const p = gl.createProgram();
        gl.attachShader(p, vs); gl.attachShader(p, fs); gl.linkProgram(p);
        if (!gl.getProgramParameter(p, gl.LINK_STATUS))
            throw new Error('link: ' + gl.getProgramInfoLog(p));
        gl.deleteShader(vs); gl.deleteShader(fs);
        return p;
    }
    function px(x, y) {
        const b = new Uint8Array(4);
        gl.readPixels(x, y, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, b);
        return b;
    }
    function near(a, b, tol) { return Math.abs(a - b) <= (tol || 8); }
    function assertPx(p, r, g, b, msg, tol) {
        assert(near(p[0], r, tol) && near(p[1], g, tol) && near(p[2], b, tol),
               msg + ' got [' + Array.from(p).join(',') + '] want [' + [r, g, b].join(',') + ']');
    }

    const prog = makeProgram(
        '#version 300 es\nin vec2 aPos;\nout vec2 vUv;\n' +
        'void main(){ vUv = aPos * 0.5 + 0.5; gl_Position = vec4(aPos, 0.0, 1.0); }',
        '#version 300 es\nprecision highp float;\nin vec2 vUv;\nuniform sampler2D uTex;\nout vec4 frag;\n' +
        'void main(){ frag = texture(uTex, vUv); }');
    gl.useProgram(prog);
    gl.uniform1i(gl.getUniformLocation(prog, 'uTex'), 0);
    const quadBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, quadBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    const vao = gl.createVertexArray();
    gl.bindVertexArray(vao);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, 64, 64);

    const exts = gl.getSupportedExtensions();

    // =====================================================================
    // Honest surfacing: RGTC is core since GL 3.0 (always present); ETC2 is
    // NOT expressible on desktop GL 3.3 and must not be claimed.
    // =====================================================================
    assert(exts.indexOf('EXT_texture_compression_rgtc') >= 0, 'RGTC extension surfaced');
    assert(exts.indexOf('WEBGL_compressed_texture_etc') < 0, 'ETC2 not claimed (desktop GL)');
    const formats = gl.getParameter(gl.COMPRESSED_TEXTURE_FORMATS);
    assert(Array.isArray(formats) && formats.length > 0, 'COMPRESSED_TEXTURE_FORMATS non-empty');

    const rgtc = gl.getExtension('EXT_texture_compression_rgtc');
    assert(rgtc !== null, 'RGTC extension object');
    assert(rgtc.COMPRESSED_RED_RGTC1_EXT === 0x8DBB, 'RGTC constants on extension object');
    assert(formats.indexOf(rgtc.COMPRESSED_RED_RGTC1_EXT) >= 0,
           'RGTC1 listed in COMPRESSED_TEXTURE_FORMATS');

    // =====================================================================
    // RGTC1 (BC4, red-only): solid red0=255 block -> texture reads (1,0,0,1).
    // Block: red0, red1, then 16x3-bit indices (all 0 selects red0).
    // =====================================================================
    const tex = gl.createTexture();
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);

    function rgtc1Block(value) {
        return [value, 0, 0, 0, 0, 0, 0, 0]; // red0=value, red1=0, indices=red0
    }
    // 8x8 texture = 2x2 blocks, all solid 255.
    const blocks = [];
    for (let i = 0; i < 4; i++) blocks.push(...rgtc1Block(255));
    gl.compressedTexImage2D(gl.TEXTURE_2D, 0, rgtc.COMPRESSED_RED_RGTC1_EXT,
                            8, 8, 0, new Uint8Array(blocks));
    assert(gl.getError() === gl.NO_ERROR, 'RGTC1 8x8 upload');
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    assertPx(px(32, 32), 255, 0, 0, 'RGTC1 solid red renders');

    // Sub-image: zero out the upper-right block (texel x>=4, y>=4).
    gl.compressedTexSubImage2D(gl.TEXTURE_2D, 0, 4, 4, 4, 4,
                               rgtc.COMPRESSED_RED_RGTC1_EXT,
                               new Uint8Array(rgtc1Block(0)));
    assert(gl.getError() === gl.NO_ERROR, 'RGTC1 sub-image upload');
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    assertPx(px(48, 48), 0, 0, 0, 'sub-image block went black');
    assertPx(px(16, 16), 255, 0, 0, 'untouched block stays red');

    // =====================================================================
    // Validation: wrong payload size and misaligned sub-rect never reach
    // the driver (no over-read of client memory).
    // =====================================================================
    gl.compressedTexImage2D(gl.TEXTURE_2D, 0, rgtc.COMPRESSED_RED_RGTC1_EXT,
                            8, 8, 0, new Uint8Array(31)); // needs 32
    assert(gl.getError() === gl.INVALID_VALUE, 'wrong payload size -> INVALID_VALUE');
    gl.compressedTexSubImage2D(gl.TEXTURE_2D, 0, 2, 0, 4, 4,
                               rgtc.COMPRESSED_RED_RGTC1_EXT,
                               new Uint8Array(rgtc1Block(0)));
    assert(gl.getError() === gl.INVALID_OPERATION, 'misaligned sub-rect -> INVALID_OPERATION');

    // Unsupported format (ETC2 RGB8) -> INVALID_ENUM, nothing uploaded.
    gl.compressedTexImage2D(gl.TEXTURE_2D, 0, 0x9274 /* COMPRESSED_RGB8_ETC2 */,
                            4, 4, 0, new Uint8Array(8));
    assert(gl.getError() === gl.INVALID_ENUM, 'ETC2 format -> INVALID_ENUM');

    // =====================================================================
    // S3TC (driver-gated): solid-red DXT1 block; color0=0xF800 (RGB565 red),
    // color0 > color1 selects 4-color mode, indices 0 pick color0.
    // =====================================================================
    const s3tc = gl.getExtension('WEBGL_compressed_texture_s3tc');
    if (s3tc) {
        assert(exts.indexOf('WEBGL_compressed_texture_s3tc') >= 0, 's3tc listed when supported');
        assert(formats.indexOf(s3tc.COMPRESSED_RGB_S3TC_DXT1_EXT) >= 0,
               'DXT1 listed in COMPRESSED_TEXTURE_FORMATS');
        const dxt1Red = new Uint8Array([0x00, 0xF8, 0x00, 0x00, 0, 0, 0, 0]);
        const tex2 = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, tex2);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
        gl.compressedTexImage2D(gl.TEXTURE_2D, 0, s3tc.COMPRESSED_RGB_S3TC_DXT1_EXT,
                                4, 4, 0, dxt1Red);
        assert(gl.getError() === gl.NO_ERROR, 'DXT1 upload');
        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
        assertPx(px(32, 32), 255, 0, 0, 'DXT1 solid red renders');
        gl.deleteTexture(tex2);
    } else {
        assert(exts.indexOf('WEBGL_compressed_texture_s3tc') < 0,
               's3tc consistently absent from getSupportedExtensions');
        console.log('s3tc not supported by driver; skipped S3TC block');
    }

    gl.deleteTexture(tex);
    assert(gl.getError() === gl.NO_ERROR, 'no error at end');

    console.log('webgl compressed texture tests passed');
}

document.body.removeChild(canvas);
