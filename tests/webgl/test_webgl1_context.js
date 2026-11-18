// getContext('webgl') is a WebGL 1 context, as in a browser: a
// WebGLRenderingContext (not a WebGL2RenderingContext) with WebGL 1's members
// only, WebGL 1's version strings and GLSL ES 1.00, and the WebGL 1
// extensions that put WebGL 2 functionality under WebGL 1 names — instancing,
// vertex array objects, draw buffers, float / half-float / depth / sRGB
// textures. Old three.js (r49-r75: HexGL, Astray, The Aviator) and Potree
// run on these.

const canvas = document.createElement('canvas');
canvas.width = 32;
canvas.height = 32;
document.body.appendChild(canvas);
const gl = canvas.getContext('webgl');
if (!gl) {
    missingGpuContext('webgl');
} else {
    // --- Identity and surface ---
    assert(gl instanceof WebGLRenderingContext, 'a WebGLRenderingContext');
    assert(!(gl instanceof WebGL2RenderingContext), 'not a WebGL2RenderingContext');
    assert(gl.constructor.name === 'WebGLRenderingContext', 'constructor ' + gl.constructor.name);
    assert(Object.getPrototypeOf(WebGL2RenderingContext.prototype) === Object.prototype,
           'the two interfaces are unrelated');
    for (const m of ['bindVertexArray', 'drawArraysInstanced', 'texImage3D', 'drawBuffers', 'mapBufferRange',
                     'uniform1ui', 'fenceSync', 'createSampler'])
        assert(gl[m] === undefined, 'no WebGL 2 method ' + m);
    for (const k of ['RGBA32F', 'TEXTURE_3D', 'HALF_FLOAT', 'COLOR_ATTACHMENT1', 'TEXTURE_MAX_ANISOTROPY_EXT'])
        assert(gl[k] === undefined && WebGLRenderingContext[k] === undefined, 'no WebGL 2 constant ' + k);
    assert(gl.RGBA === 0x1908 && WebGLRenderingContext.TRIANGLES === 4, 'the WebGL 1 constants');
    assert(typeof WebGL2RenderingContext.prototype.bindVertexArray === 'function', 'WebGL 2 keeps its members');
    assert(canvas.getContext('webgl2') === null, 'a webgl2 context after webgl is refused');
    assert(canvas.getContext('experimental-webgl') === gl, "'experimental-webgl' is the same context");
    assert(gl.getParameter(gl.VERSION).startsWith('WebGL 1.0'), 'VERSION ' + gl.getParameter(gl.VERSION));
    assert(gl.getParameter(gl.SHADING_LANGUAGE_VERSION).startsWith('WebGL GLSL ES 1.0'),
           'SHADING_LANGUAGE_VERSION ' + gl.getParameter(gl.SHADING_LANGUAGE_VERSION));

    const exts = gl.getSupportedExtensions();
    for (const e of ['ANGLE_instanced_arrays', 'OES_vertex_array_object', 'WEBGL_draw_buffers', 'OES_texture_float',
                     'OES_texture_half_float', 'OES_texture_half_float_linear', 'OES_element_index_uint',
                     'OES_standard_derivatives', 'WEBGL_depth_texture', 'EXT_blend_minmax', 'EXT_frag_depth',
                     'EXT_shader_texture_lod', 'EXT_sRGB', 'WEBGL_lose_context'])
        assert(exts.includes(e) && gl.getExtension(e) !== null, 'WebGL 1 extension ' + e);
    for (const e of ['EXT_color_buffer_float', 'BRO_buffer_map', 'EXT_disjoint_timer_query_webgl2'])
        assert(!exts.includes(e) && gl.getExtension(e) === null, 'no WebGL 2 extension ' + e);
    assert(gl.getExtension('EXT_blend_minmax').MAX_EXT === 0x8008, 'EXT_blend_minmax.MAX_EXT');
    assert(gl.getExtension('WEBGL_depth_texture').UNSIGNED_INT_24_8_WEBGL === 0x84FA, 'UNSIGNED_INT_24_8_WEBGL');

    // --- Shaders: GLSL ES 1.00 with WebGL 1 extensions; not 3.00 ---
    const compile = (type, src) => {
        const s = gl.createShader(type);
        gl.shaderSource(s, src);
        gl.compileShader(s);
        return s;
    };
    const es300 = compile(gl.FRAGMENT_SHADER,
        '#version 300 es\nprecision mediump float; out vec4 o; void main() { o = vec4(1.0); }');
    assert(gl.getShaderParameter(es300, gl.COMPILE_STATUS) === false, 'a #version 300 es shader does not compile');
    assert(gl.getShaderInfoLog(es300).length > 0, 'and says why');
    const deriv = compile(gl.FRAGMENT_SHADER,
        '#extension GL_OES_standard_derivatives : enable\nprecision mediump float; varying float v;\n' +
        'void main() { gl_FragColor = vec4(fwidth(v)); }');
    assert(gl.getShaderParameter(deriv, gl.COMPILE_STATUS) === true,
           'OES_standard_derivatives compiles: ' + gl.getShaderInfoLog(deriv));

    // --- Texture formats a WebGL 1 program names ---
    const half = gl.getExtension('OES_texture_half_float');
    const srgb = gl.getExtension('EXT_sRGB');
    const tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    const formats = [
        ['RGBA/FLOAT', gl.RGBA, gl.RGBA, gl.FLOAT],
        ['RGB/HALF_FLOAT_OES', gl.RGB, gl.RGB, half.HALF_FLOAT_OES],
        ['LUMINANCE/FLOAT', gl.LUMINANCE, gl.LUMINANCE, gl.FLOAT],
        ['DEPTH_COMPONENT/UNSIGNED_INT', gl.DEPTH_COMPONENT, gl.DEPTH_COMPONENT, gl.UNSIGNED_INT],
        ['DEPTH_STENCIL/UNSIGNED_INT_24_8_WEBGL', gl.DEPTH_STENCIL, gl.DEPTH_STENCIL, 0x84FA],
        ['SRGB_ALPHA_EXT', srgb.SRGB_ALPHA_EXT, srgb.SRGB_ALPHA_EXT, gl.UNSIGNED_BYTE],
    ];
    for (const [name, ifmt, fmt, type] of formats) {
        gl.texImage2D(gl.TEXTURE_2D, 0, ifmt, 4, 4, 0, fmt, type, null);
        const err = gl.getError();
        assert(err === gl.NO_ERROR, name + ': error 0x' + err.toString(16));
    }
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 2, 2, 0, gl.RGBA, half.HALF_FLOAT_OES, new Uint16Array(16));
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 1, 1, gl.RGBA, half.HALF_FLOAT_OES, new Uint16Array(4));
    assert(gl.getError() === gl.NO_ERROR, 'half-float upload and sub-upload');
    gl.deleteTexture(tex);

    // --- Instancing through a vertex array object, both by extension ---
    const inst = gl.getExtension('ANGLE_instanced_arrays');
    const vaoExt = gl.getExtension('OES_vertex_array_object');
    const vs = compile(gl.VERTEX_SHADER,
        'attribute vec2 p; attribute float off; void main() { gl_Position = vec4(p.x * 0.5 + off, p.y, 0.0, 1.0); }');
    const fs = compile(gl.FRAGMENT_SHADER, 'precision mediump float; void main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }');
    const prog = gl.createProgram();
    gl.attachShader(prog, vs);
    gl.attachShader(prog, fs);
    gl.linkProgram(prog);
    assert(gl.getProgramParameter(prog, gl.LINK_STATUS), 'link: ' + gl.getProgramInfoLog(prog));
    gl.useProgram(prog);
    const vao = vaoExt.createVertexArrayOES();
    vaoExt.bindVertexArrayOES(vao);
    assert(gl.getParameter(vaoExt.VERTEX_ARRAY_BINDING_OES) === vao, 'VERTEX_ARRAY_BINDING_OES');
    assert(vaoExt.isVertexArrayOES(vao), 'isVertexArrayOES');
    const quad = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, quad);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    const pLoc = gl.getAttribLocation(prog, 'p');
    gl.enableVertexAttribArray(pLoc);
    gl.vertexAttribPointer(pLoc, 2, gl.FLOAT, false, 0, 0);
    // Two instances: the left and the right half of the canvas.
    const offs = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, offs);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-0.5, 0.5]), gl.STATIC_DRAW);
    const oLoc = gl.getAttribLocation(prog, 'off');
    gl.enableVertexAttribArray(oLoc);
    gl.vertexAttribPointer(oLoc, 1, gl.FLOAT, false, 0, 0);
    inst.vertexAttribDivisorANGLE(oLoc, 1);
    assert(gl.getVertexAttrib(oLoc, inst.VERTEX_ATTRIB_ARRAY_DIVISOR_ANGLE) === 1, 'the divisor');
    vaoExt.bindVertexArrayOES(null);

    gl.viewport(0, 0, 32, 32);
    gl.clearColor(1, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    vaoExt.bindVertexArrayOES(vao);
    inst.drawArraysInstancedANGLE(gl.TRIANGLE_STRIP, 0, 4, 2);
    assert(gl.getError() === gl.NO_ERROR, 'the instanced draw');
    const px = new Uint8Array(4);
    for (const x of [4, 27]) {
        gl.readPixels(x, 16, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
        assert(px[0] === 0 && px[1] === 255, `instance covers x=${x}: ${Array.from(px)}`);
    }
    vaoExt.deleteVertexArrayOES(vao);
    assert(!vaoExt.isVertexArrayOES(vao), 'deleted');

    // --- WEBGL_draw_buffers ---
    const db = gl.getExtension('WEBGL_draw_buffers');
    assert(db.COLOR_ATTACHMENT1_WEBGL === 0x8CE1 && db.DRAW_BUFFER0_WEBGL === 0x8825, 'WEBGL_draw_buffers enums');
    assert(gl.getParameter(db.MAX_DRAW_BUFFERS_WEBGL) >= 4, 'MAX_DRAW_BUFFERS_WEBGL');
    const fb = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
    const targets = [0, 1].map(i => {
        const t = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, t);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 4, 4, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
        gl.framebufferTexture2D(gl.FRAMEBUFFER, db.COLOR_ATTACHMENT0_WEBGL + i, gl.TEXTURE_2D, t, 0);
        return t;
    });
    db.drawBuffersWEBGL([db.COLOR_ATTACHMENT0_WEBGL, db.COLOR_ATTACHMENT1_WEBGL]);
    assert(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE, 'two attachments complete');
    assert(gl.getError() === gl.NO_ERROR, 'drawBuffersWEBGL');
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    targets.forEach(t => gl.deleteTexture(t));
    gl.deleteFramebuffer(fb);

    console.log('test_webgl1_context: OK');
}
