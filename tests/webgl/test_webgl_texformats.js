// WebGL2 texture formats on Vulkan, each verified by sampling it into the
// canvas and reading the pixel back: RGB (alpha reads one), the packed
// 16-bit formats, integer textures through isampler/usampler, sRGB decode,
// LUMINANCE_ALPHA, depth uploads, real 3D and 2D-array textures, the
// UNPACK_ row-length/skip/image-height and PACK_ row-length/skip state, and
// the upload validation (too few bytes, immutable storage, sampler type).

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
        const fs = '#version 300 es\nprecision highp float;\nprecision highp int;\n' +
            'precision highp sampler3D;\nprecision highp sampler2DArray;\nprecision highp isampler2D;\n' +
            'precision highp usampler2D;\nuniform ' + sampler + ' uTex;\nuniform float uZ;\n' +
            'in vec2 vUV;\nout vec4 frag;\nvoid main(){ ' + fsBody + ' }';
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

    const sample2D = program('frag = texture(uTex, vUV);', 'sampler2D');
    const sample3D = program('frag = texture(uTex, vec3(vUV, uZ));', 'sampler3D');
    const sampleArray = program('frag = texture(uTex, vec3(vUV, uZ));', 'sampler2DArray');
    const sampleInt = program('ivec4 v = texelFetch(uTex, ivec2(0), 0); ' +
                              'frag = vec4(v.r == -5 ? 1.0 : 0.0, float(v.g) / 255.0, 0.0, 1.0);', 'isampler2D');
    const sampleUint = program('frag = vec4(texelFetch(uTex, ivec2(0), 0)) / 255.0;', 'usampler2D');

    function draw(prog, z) {
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        gl.viewport(0, 0, 16, 16);
        gl.clearColor(0, 0, 0, 0);
        gl.clear(gl.COLOR_BUFFER_BIT);
        gl.useProgram(prog);
        const loc = gl.getUniformLocation(prog, 'uZ');
        if (loc) gl.uniform1f(loc, z || 0);
        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    }
    function px(x, y) {
        const b = new Uint8Array(4);
        gl.readPixels(x === undefined ? 8 : x, y === undefined ? 8 : y, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, b);
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
    function texture(target) {
        const t = gl.createTexture();
        gl.bindTexture(target, t);
        gl.texParameteri(target, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
        gl.texParameteri(target, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
        return t;
    }

    // RGB8: three bytes a texel (UNPACK_ALIGNMENT 1 for the odd row), alpha one.
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    texture(gl.TEXTURE_2D);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGB8, 1, 1, 0, gl.RGB, gl.UNSIGNED_BYTE, new Uint8Array([10, 20, 30]));
    assertError(gl.NO_ERROR, 'RGB8 upload');
    draw(sample2D);
    assertPx(px(), [10, 20, 30, 255], 'RGB8 samples with alpha one');
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);

    // Packed 16-bit formats.
    texture(gl.TEXTURE_2D);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGB565, 1, 1, 0, gl.RGB, gl.UNSIGNED_SHORT_5_6_5, new Uint16Array([0x07E0]));
    draw(sample2D);
    assertPx(px(), [0, 255, 0, 255], 'RGB565 green');
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA4, 1, 1, 0, gl.RGBA, gl.UNSIGNED_SHORT_4_4_4_4, new Uint16Array([0xF008]));
    draw(sample2D);
    assertPx(px(), [255, 0, 0, 136], 'RGBA4 red, alpha 8/15');
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGB5_A1, 1, 1, 0, gl.RGBA, gl.UNSIGNED_SHORT_5_5_5_1, new Uint16Array([0x003F]));
    draw(sample2D);
    assertPx(px(), [0, 0, 255, 255], 'RGB5_A1 blue, alpha one');

    // Integer textures: their own storage, read through int samplers.
    texture(gl.TEXTURE_2D);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8UI, 1, 1, 0, gl.RGBA_INTEGER, gl.UNSIGNED_BYTE,
                  new Uint8Array([200, 7, 0, 255]));
    assertError(gl.NO_ERROR, 'RGBA8UI upload');
    draw(sampleUint);
    assertPx(px(), [200, 7, 0, 255], 'RGBA8UI texels arrive unnormalized');
    draw(sample2D);
    assertError(gl.INVALID_OPERATION, 'an integer texture through a float sampler');
    texture(gl.TEXTURE_2D);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RG32I, 1, 1, 0, gl.RG_INTEGER, gl.INT, new Int32Array([-5, 128]));
    draw(sampleInt);
    assertPx(px(), [255, 128, 0, 255], 'RG32I keeps its sign');

    // sRGB decodes on sampling: 188 is about 0.5 linear.
    texture(gl.TEXTURE_2D);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.SRGB8_ALPHA8, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE,
                  new Uint8Array([188, 188, 188, 255]));
    draw(sample2D);
    assertPx(px(), [128, 128, 128, 255], 'SRGB8_ALPHA8 decodes', 3);

    // LUMINANCE_ALPHA reads (L, L, L, A).
    texture(gl.TEXTURE_2D);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.LUMINANCE_ALPHA, 1, 1, 0, gl.LUMINANCE_ALPHA, gl.UNSIGNED_BYTE,
                  new Uint8Array([100, 200, 0, 0]));
    draw(sample2D);
    assertPx(px(), [100, 100, 100, 200], 'LUMINANCE_ALPHA swizzles');

    // A depth upload samples as (D, 0, 0, 1).
    texture(gl.TEXTURE_2D);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.DEPTH_COMPONENT16, 1, 1, 0, gl.DEPTH_COMPONENT, gl.UNSIGNED_SHORT,
                  new Uint16Array([0x8000]));
    assertError(gl.NO_ERROR, 'DEPTH_COMPONENT16 upload');
    draw(sample2D);
    assertPx(px(), [128, 0, 0, 255], 'depth texels sample');

    // 3D: four 1x1 slices, each its own color, read by r coordinate.
    const colors = [[255, 0, 0, 255], [0, 255, 0, 255], [0, 0, 255, 255], [255, 255, 0, 255]];
    texture(gl.TEXTURE_3D);
    gl.texImage3D(gl.TEXTURE_3D, 0, gl.RGBA8, 1, 1, 4, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array(colors.flat()));
    assertError(gl.NO_ERROR, '3D upload');
    for (let z = 0; z < 4; ++z) {
        draw(sample3D, (z + 0.5) / 4);
        assertPx(px(), colors[z], '3D slice ' + z);
    }
    // texSubImage3D into slice 2.
    gl.texSubImage3D(gl.TEXTURE_3D, 0, 0, 0, 2, 1, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array([9, 99, 199, 255]));
    draw(sample3D, 2.5 / 4);
    assertPx(px(), [9, 99, 199, 255], '3D sub-image slice');

    // 2D array with UNPACK_IMAGE_HEIGHT / SKIP_IMAGES: images of 2 rows, the
    // first image skipped, only row 0 of each used.
    texture(gl.TEXTURE_2D_ARRAY);
    gl.pixelStorei(gl.UNPACK_IMAGE_HEIGHT, 2);
    gl.pixelStorei(gl.UNPACK_SKIP_IMAGES, 1);
    const layered = new Uint8Array(4 * 2 * 4);
    for (let i = 0; i < 4; ++i) layered.set(colors[i], i * 8);  // row 0 of image i
    gl.texImage3D(gl.TEXTURE_2D_ARRAY, 0, gl.RGBA8, 1, 1, 3, 0, gl.RGBA, gl.UNSIGNED_BYTE, layered);
    assertError(gl.NO_ERROR, '2D array upload');
    gl.pixelStorei(gl.UNPACK_IMAGE_HEIGHT, 0);
    gl.pixelStorei(gl.UNPACK_SKIP_IMAGES, 0);
    for (let layer = 0; layer < 3; ++layer) {
        draw(sampleArray, layer);
        assertPx(px(), colors[layer + 1], '2D array layer ' + layer);
    }

    // UNPACK_ROW_LENGTH / SKIP_PIXELS / SKIP_ROWS select a 2x2 window of a
    // 4-wide source.
    const src = new Uint8Array(4 * 3 * 4);
    for (let y = 0; y < 3; ++y) for (let x = 0; x < 4; ++x) src.set([x * 60, y * 100, 7, 255], (y * 4 + x) * 4);
    const win = texture(gl.TEXTURE_2D);
    gl.pixelStorei(gl.UNPACK_ROW_LENGTH, 4);
    gl.pixelStorei(gl.UNPACK_SKIP_PIXELS, 1);
    gl.pixelStorei(gl.UNPACK_SKIP_ROWS, 1);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 2, 2, 0, gl.RGBA, gl.UNSIGNED_BYTE, src);
    assertError(gl.NO_ERROR, 'windowed upload');
    gl.pixelStorei(gl.UNPACK_ROW_LENGTH, 0);
    gl.pixelStorei(gl.UNPACK_SKIP_PIXELS, 0);
    gl.pixelStorei(gl.UNPACK_SKIP_ROWS, 0);
    const fbo = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, win, 0);
    const texels = new Uint8Array(16);
    gl.readPixels(0, 0, 2, 2, gl.RGBA, gl.UNSIGNED_BYTE, texels);
    assertPx(Array.from(texels.subarray(0, 4)), [60, 100, 7, 255], 'window texel (0,0) is source (1,1)');
    assertPx(Array.from(texels.subarray(12, 16)), [120, 200, 7, 255], 'window texel (1,1) is source (2,2)');

    // PACK_ROW_LENGTH / SKIP_PIXELS / SKIP_ROWS place readPixels' output.
    gl.pixelStorei(gl.PACK_ROW_LENGTH, 3);
    gl.pixelStorei(gl.PACK_SKIP_PIXELS, 1);
    gl.pixelStorei(gl.PACK_SKIP_ROWS, 1);
    const packed = new Uint8Array(3 * 3 * 4).fill(1);
    gl.readPixels(0, 0, 2, 2, gl.RGBA, gl.UNSIGNED_BYTE, packed);
    assertError(gl.NO_ERROR, 'packed readPixels');
    assertPx(Array.from(packed.subarray(16, 20)), [60, 100, 7, 255], 'row 1, pixel 1 holds (0,0)');
    assertPx(Array.from(packed.subarray(28, 32)), [60, 200, 7, 255], 'row 2, pixel 1 holds (0,1)');
    assertPx(Array.from(packed.subarray(32, 36)), [120, 200, 7, 255], 'row 2, pixel 2 holds (1,1)');
    assertPx(Array.from(packed.subarray(0, 4)), [1, 1, 1, 1], 'skipped bytes untouched', 0);
    gl.pixelStorei(gl.PACK_ROW_LENGTH, 0);
    gl.pixelStorei(gl.PACK_SKIP_PIXELS, 0);
    gl.pixelStorei(gl.PACK_SKIP_ROWS, 0);
    const rgb = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, rgb);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGB8, 1, 1, 0, gl.RGB, gl.UNSIGNED_BYTE, null);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, rgb, 0);
    assert(gl.getFramebufferAttachmentParameter(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0,
                                                gl.FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE) === 0, 'RGB8 has no alpha bits');
    gl.clearColor(1, 0, 0, 0);
    gl.clear(gl.COLOR_BUFFER_BIT);
    assertPx(px(0, 0), [255, 0, 0, 255], 'an RGB8 attachment reads alpha one');
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);

    // Validation.
    texture(gl.TEXTURE_2D);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 2, 2, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array(15));
    assertError(gl.INVALID_OPERATION, 'too few bytes');
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 1, 1, 0, gl.RGB, gl.UNSIGNED_BYTE, new Uint8Array(4));
    assertError(gl.INVALID_OPERATION, 'format does not match the internal format');
    gl.texStorage2D(gl.TEXTURE_2D, 2, gl.RGBA8, 4, 4);
    assertError(gl.NO_ERROR, 'texStorage2D');
    assert(gl.getTexParameter(gl.TEXTURE_2D, gl.TEXTURE_IMMUTABLE_FORMAT) === true, 'immutable format');
    assert(gl.getTexParameter(gl.TEXTURE_2D, gl.TEXTURE_IMMUTABLE_LEVELS) === 2, 'immutable levels');
    assert(gl.getTexParameter(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER) === gl.NEAREST, 'min filter reads back');
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 4, 4, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
    assertError(gl.INVALID_OPERATION, 'texImage2D on immutable storage');
    gl.texSubImage2D(gl.TEXTURE_2D, 1, 0, 0, 2, 2, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array(16).fill(255));
    assertError(gl.NO_ERROR, 'texSubImage2D into an immutable level');
    gl.texSubImage2D(gl.TEXTURE_2D, 2, 0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array(4));
    assertError(gl.INVALID_OPERATION, 'texSubImage2D past the immutable levels');

    console.log('webgl texture format tests passed');
}

document.body.removeChild(canvas);
