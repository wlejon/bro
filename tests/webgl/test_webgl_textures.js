// WebGL2 conformance subset — textures: sized internal formats, texSubImage2D,
// texStorage2D/3D, explicit + generated mipmaps, NPOT/CLAMP_TO_EDGE,
// UNPACK_ALIGNMENT, UNPACK_FLIP_Y_WEBGL / UNPACK_PREMULTIPLY_ALPHA_WEBGL,
// 2D-array textures, cube maps. Verified via FBO attachment readback and
// shader sampling. Exercises src/js/webgl2_bindings_textures.cpp +
// src/webgl/webgl2_context.cpp texture paths.

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
    function near(a, b, tol) { return Math.abs(a - b) <= (tol || 3); }
    function assertPx(p, r, g, b, msg, tol) {
        assert(near(p[0], r, tol) && near(p[1], g, tol) && near(p[2], b, tol),
               msg + ' got [' + Array.from(p).join(',') + '] want [' + [r, g, b].join(',') + ']');
    }

    const vsQuad = '#version 300 es\nin vec2 aPos;\nout vec2 vUV;\n' +
        'void main(){ gl_Position = vec4(aPos, 0.0, 1.0); vUV = aPos * 0.5 + 0.5; }';

    const quadBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, quadBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    const vao = gl.createVertexArray();
    gl.bindVertexArray(vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, quadBuf);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);

    const sampleProg = makeProgram(vsQuad,
        '#version 300 es\nprecision highp float;\nuniform sampler2D uTex;\nin vec2 vUV;\nout vec4 frag;\n' +
        'void main(){ frag = texture(uTex, vUV); }');

    const readFBO = gl.createFramebuffer();

    // Read one texel of a texture level via FBO attachment
    function texel(tex, level, x, y) {
        gl.bindFramebuffer(gl.FRAMEBUFFER, readFBO);
        gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, tex, level);
        const b = new Uint8Array(4);
        gl.readPixels(x, y, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, b);
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        return b;
    }
    // Draw quad sampling tex into the canvas at the given viewport, read (x,y)
    function drawSample(tex, vpw, vph, x, y) {
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        gl.viewport(0, 0, vpw, vph);
        gl.clearColor(0, 0, 0, 0);
        gl.clear(gl.COLOR_BUFFER_BIT);
        gl.useProgram(sampleProg);
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, tex);
        gl.uniform1i(gl.getUniformLocation(sampleProg, 'uTex'), 0);
        gl.bindVertexArray(vao);
        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
        const b = new Uint8Array(4);
        gl.readPixels(x, y, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, b);
        gl.viewport(0, 0, 64, 64);
        return b;
    }
    function nearestParams(target) {
        gl.texParameteri(target, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
        gl.texParameteri(target, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
        gl.texParameteri(target, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(target, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    }

    // =====================================================================
    // RGBA8 upload + per-texel readback, texSubImage2D update
    // =====================================================================
    const t1 = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, t1);
    nearestParams(gl.TEXTURE_2D);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 2, 2, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array([
        255, 0, 0, 255,   0, 255, 0, 255,    // row 0 (y=0)
        0, 0, 255, 255,   255, 255, 0, 255,  // row 1 (y=1)
    ]));
    assertPx(texel(t1, 0, 0, 0), 255, 0, 0, 'RGBA8 texel (0,0)');
    assertPx(texel(t1, 0, 1, 0), 0, 255, 0, 'RGBA8 texel (1,0)');
    assertPx(texel(t1, 0, 0, 1), 0, 0, 255, 'RGBA8 texel (0,1)');
    assertPx(texel(t1, 0, 1, 1), 255, 255, 0, 'RGBA8 texel (1,1)');

    gl.bindTexture(gl.TEXTURE_2D, t1);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 1, 1, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE,
                     new Uint8Array([1, 2, 3, 255]));
    assertPx(texel(t1, 0, 1, 1), 1, 2, 3, 'texSubImage2D updated one texel');
    assertPx(texel(t1, 0, 0, 0), 255, 0, 0, 'texSubImage2D left others alone');

    // =====================================================================
    // Sized formats: R8 with UNPACK_ALIGNMENT 1 (3-wide rows), RG8
    // =====================================================================
    const tR8 = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tR8);
    nearestParams(gl.TEXTURE_2D);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.R8, 3, 3, 0, gl.RED, gl.UNSIGNED_BYTE, new Uint8Array([
        10, 20, 30,
        40, 50, 60,
        70, 80, 90,
    ]));
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
    assertPx(texel(tR8, 0, 0, 0), 10, 0, 0, 'R8 (0,0)');
    assertPx(texel(tR8, 0, 2, 0), 30, 0, 0, 'R8 (2,0) — alignment-1 rows land right');
    assertPx(texel(tR8, 0, 1, 1), 50, 0, 0, 'R8 (1,1)');
    assertPx(texel(tR8, 0, 2, 2), 90, 0, 0, 'R8 (2,2)');

    const tRG8 = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tRG8);
    nearestParams(gl.TEXTURE_2D);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RG8, 1, 1, 0, gl.RG, gl.UNSIGNED_BYTE,
                  new Uint8Array([100, 200]));
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
    assertPx(texel(tRG8, 0, 0, 0), 100, 200, 0, 'RG8 texel');
    assert(gl.getError() === gl.NO_ERROR, 'no error after sized formats');

    // =====================================================================
    // RGBA16F renderability (EXT_color_buffer_float is advertised)
    // =====================================================================
    const tF = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tF);
    nearestParams(gl.TEXTURE_2D);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA16F, 4, 4, 0, gl.RGBA, gl.HALF_FLOAT, null);
    gl.bindFramebuffer(gl.FRAMEBUFFER, readFBO);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, tF, 0);
    assert(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE,
           'RGBA16F color-renderable');
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);

    // RGBA32F sampled through a shader (NEAREST — no float-linear dependency)
    const tF32 = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tF32);
    nearestParams(gl.TEXTURE_2D);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA32F, 1, 1, 0, gl.RGBA, gl.FLOAT,
                  new Float32Array([0.25, 0.5, 0.75, 1.0]));
    assertPx(drawSample(tF32, 64, 64, 32, 32), 64, 128, 191, 'RGBA32F sampled values');

    // =====================================================================
    // DEPTH_COMPONENT24 texture attaches as a depth attachment
    // =====================================================================
    const tDepth = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tDepth);
    nearestParams(gl.TEXTURE_2D);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.DEPTH_COMPONENT24, 8, 8, 0,
                  gl.DEPTH_COMPONENT, gl.UNSIGNED_INT, null);
    const tColor = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tColor);
    nearestParams(gl.TEXTURE_2D);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 8, 8, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
    const fboDepth = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fboDepth);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, tColor, 0);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.DEPTH_ATTACHMENT, gl.TEXTURE_2D, tDepth, 0);
    assert(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE,
           'DEPTH_COMPONENT24 texture + RGBA8 color complete');
    gl.clearDepth(1.0);
    gl.clear(gl.DEPTH_BUFFER_BIT | gl.COLOR_BUFFER_BIT);
    assert(gl.getError() === gl.NO_ERROR, 'no error clearing depth-textured FBO');
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);

    // =====================================================================
    // Explicit mipmap levels + minification level selection
    // =====================================================================
    const tMip = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tMip);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST_MIPMAP_NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    const red2x2 = new Uint8Array([255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255]);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 2, 2, 0, gl.RGBA, gl.UNSIGNED_BYTE, red2x2);
    gl.texImage2D(gl.TEXTURE_2D, 1, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE,
                  new Uint8Array([0, 255, 0, 255]));
    // Magnified draw uses level 0 (red); 1x1-viewport draw minifies to level 1 (green)
    assertPx(drawSample(tMip, 64, 64, 32, 32), 255, 0, 0, 'mip level 0 at magnification');
    assertPx(drawSample(tMip, 1, 1, 0, 0), 0, 255, 0, 'mip level 1 at minification');

    // generateMipmap rebuilds level 1 from the red base
    gl.bindTexture(gl.TEXTURE_2D, tMip);
    gl.generateMipmap(gl.TEXTURE_2D);
    assertPx(drawSample(tMip, 1, 1, 0, 0), 255, 0, 0, 'generateMipmap rebuilt level 1', 8);

    // =====================================================================
    // texSubImage2D from an ImageBitmap must NOT regenerate mipmaps
    // =====================================================================
    gl.bindTexture(gl.TEXTURE_2D, tMip);
    gl.texImage2D(gl.TEXTURE_2D, 1, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE,
                  new Uint8Array([0, 255, 0, 255])); // explicit green L1 again
    const blueBmp = await createImageBitmap({
        width: 2, height: 2,
        data: new Uint8Array([0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255]),
    });
    gl.bindTexture(gl.TEXTURE_2D, tMip);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, gl.RGBA, gl.UNSIGNED_BYTE, blueBmp);
    assertPx(drawSample(tMip, 64, 64, 32, 32), 0, 0, 255, 'ImageBitmap texSubImage2D hit level 0');
    assertPx(drawSample(tMip, 1, 1, 0, 0), 0, 255, 0,
             'texSubImage2D did not clobber explicit level 1 (no implicit generateMipmap)');

    // =====================================================================
    // UNPACK_FLIP_Y_WEBGL flips rows at upload
    // =====================================================================
    const tFlip = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tFlip);
    nearestParams(gl.TEXTURE_2D);
    const twoRows = new Uint8Array([255, 0, 0, 255,   0, 255, 0, 255]); // row0 red, row1 green
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, 1);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 2, 0, gl.RGBA, gl.UNSIGNED_BYTE, twoRows);
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, 0);
    assertPx(texel(tFlip, 0, 0, 0), 0, 255, 0, 'flipY: first data row became top');
    assertPx(texel(tFlip, 0, 0, 1), 255, 0, 0, 'flipY: last data row became bottom');
    gl.bindTexture(gl.TEXTURE_2D, tFlip);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 2, 0, gl.RGBA, gl.UNSIGNED_BYTE, twoRows);
    assertPx(texel(tFlip, 0, 0, 0), 255, 0, 0, 'no flip: first data row is bottom');

    // UNPACK_PREMULTIPLY_ALPHA_WEBGL premultiplies RGBA8 uploads
    const tPre = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tPre);
    nearestParams(gl.TEXTURE_2D);
    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, 1);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE,
                  new Uint8Array([255, 255, 255, 128]));
    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, 0);
    assertPx(texel(tPre, 0, 0, 0), 128, 128, 128, 'premultiplied upload');

    // =====================================================================
    // texStorage2D allocation + texSubImage2D + sampling
    // =====================================================================
    const tSt = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tSt);
    nearestParams(gl.TEXTURE_2D);
    gl.texStorage2D(gl.TEXTURE_2D, 2, gl.RGBA8, 4, 4);
    const orange = new Uint8Array(4 * 4 * 4);
    for (let i = 0; i < 16; i++) { orange.set([255, 128, 0, 255], i * 4); }
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 4, 4, gl.RGBA, gl.UNSIGNED_BYTE, orange);
    assertPx(drawSample(tSt, 64, 64, 32, 32), 255, 128, 0, 'texStorage2D + texSubImage2D sampled');
    assert(gl.getError() === gl.NO_ERROR, 'no error after texStorage2D');

    // =====================================================================
    // NPOT texture with CLAMP_TO_EDGE + LINEAR samples fine
    // =====================================================================
    const tNpot = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tNpot);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    const npot = new Uint8Array(3 * 3 * 4);
    for (let i = 0; i < 9; i++) npot.set([0, 200, 200, 255], i * 4);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 3, 3, 0, gl.RGBA, gl.UNSIGNED_BYTE, npot);
    assertPx(drawSample(tNpot, 64, 64, 32, 32), 0, 200, 200, 'NPOT CLAMP_TO_EDGE LINEAR sample');
    assert(gl.getError() === gl.NO_ERROR, 'no error after NPOT');

    // =====================================================================
    // TEXTURE_2D_ARRAY — texImage3D upload, sample a specific layer
    // =====================================================================
    const tArr = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D_ARRAY, tArr);
    gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texImage3D(gl.TEXTURE_2D_ARRAY, 0, gl.RGBA8, 1, 1, 2, 0, gl.RGBA, gl.UNSIGNED_BYTE,
                  new Uint8Array([255, 0, 0, 255,   0, 255, 0, 255])); // layer0 red, layer1 green
    const arrProg = makeProgram(vsQuad,
        '#version 300 es\nprecision highp float;\nuniform sampler2DArray uTex;\nuniform float uLayer;\n' +
        'in vec2 vUV;\nout vec4 frag;\nvoid main(){ frag = texture(uTex, vec3(vUV, uLayer)); }');
    function drawLayer(layer) {
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        gl.viewport(0, 0, 64, 64);
        gl.clear(gl.COLOR_BUFFER_BIT);
        gl.useProgram(arrProg);
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D_ARRAY, tArr);
        gl.uniform1i(gl.getUniformLocation(arrProg, 'uTex'), 0);
        gl.uniform1f(gl.getUniformLocation(arrProg, 'uLayer'), layer);
        gl.bindVertexArray(vao);
        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
        const b = new Uint8Array(4);
        gl.readPixels(32, 32, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, b);
        return b;
    }
    assertPx(drawLayer(0), 255, 0, 0, '2D-array layer 0');
    assertPx(drawLayer(1), 0, 255, 0, '2D-array layer 1');

    // texStorage3D + texSubImage3D single-layer update
    const tArr2 = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D_ARRAY, tArr2);
    gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texStorage3D(gl.TEXTURE_2D_ARRAY, 1, gl.RGBA8, 1, 1, 2);
    gl.texSubImage3D(gl.TEXTURE_2D_ARRAY, 0, 0, 0, 1, 1, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE,
                     new Uint8Array([255, 0, 255, 255]));
    gl.bindTexture(gl.TEXTURE_2D_ARRAY, tArr);   // restore for cleanup path
    assert(gl.getError() === gl.NO_ERROR, 'no error after texStorage3D + texSubImage3D');

    // =====================================================================
    // Cube map — upload 6 faces, sample +X direction
    // =====================================================================
    const tCube = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_CUBE_MAP, tCube);
    gl.texParameteri(gl.TEXTURE_CUBE_MAP, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_CUBE_MAP, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    const faceColors = [
        [255, 0, 0], [0, 255, 0], [0, 0, 255], [255, 255, 0], [255, 0, 255], [0, 255, 255],
    ];
    for (let f = 0; f < 6; f++) {
        gl.texImage2D(gl.TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, gl.RGBA, 1, 1, 0,
                      gl.RGBA, gl.UNSIGNED_BYTE,
                      new Uint8Array([faceColors[f][0], faceColors[f][1], faceColors[f][2], 255]));
    }
    const cubeProg = makeProgram(vsQuad,
        '#version 300 es\nprecision highp float;\nuniform samplerCube uCube;\nin vec2 vUV;\nout vec4 frag;\n' +
        'void main(){ frag = texture(uCube, vec3(1.0, 0.0, 0.0)); }');
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, 64, 64);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.useProgram(cubeProg);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_CUBE_MAP, tCube);
    gl.uniform1i(gl.getUniformLocation(cubeProg, 'uCube'), 0);
    gl.bindVertexArray(vao);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    const cubePx = new Uint8Array(4);
    gl.readPixels(32, 32, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, cubePx);
    assertPx(cubePx, 255, 0, 0, 'cube map +X face sampled');
    assert(gl.getError() === gl.NO_ERROR, 'no error after cube map');

    // =====================================================================
    // texParameterf + wrap R accepted
    // =====================================================================
    gl.bindTexture(gl.TEXTURE_2D, t1);
    gl.texParameterf(gl.TEXTURE_2D, gl.TEXTURE_MAX_LOD, 4.0);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAX_LEVEL, 4);
    assert(gl.getError() === gl.NO_ERROR, 'texParameterf/i accepted');

    // Cleanup
    for (const t of [t1, tR8, tRG8, tF, tF32, tDepth, tColor, tMip, tFlip, tPre, tSt, tNpot, tCube, tArr, tArr2])
        gl.deleteTexture(t);
    gl.deleteFramebuffer(readFBO);
    gl.deleteFramebuffer(fboDepth);
    gl.deleteBuffer(quadBuf);
    gl.deleteVertexArray(vao);
    gl.deleteProgram(sampleProg);
    gl.deleteProgram(arrProg);
    gl.deleteProgram(cubeProg);
    assert(gl.getError() === gl.NO_ERROR, 'no error after cleanup');

    console.log('webgl texture tests passed');
}

document.body.removeChild(canvas);
