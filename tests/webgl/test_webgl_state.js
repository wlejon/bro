// WebGL2 conformance subset — context info, getParameter, initial state,
// enable/disable/isEnabled round-trips, getError semantics, pixelStorei.
// Exercises src/webgl/webgl2_context.cpp + src/js/webgl2_bindings_queries.cpp
// + webgl2_bindings_state.cpp.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '64');
canvas.setAttribute('height', '64');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2');
if (!gl) {
    console.log('no webgl2; skipping');
} else {
    // =====================================================================
    // Context info
    // =====================================================================
    assert(gl.drawingBufferWidth === 64, 'drawingBufferWidth');
    assert(gl.drawingBufferHeight === 64, 'drawingBufferHeight');
    assert(String(gl.getParameter(gl.VERSION)).indexOf('WebGL 2.0') === 0, 'VERSION starts with WebGL 2.0');
    assert(String(gl.getParameter(gl.SHADING_LANGUAGE_VERSION)).indexOf('WebGL GLSL ES 3.0') === 0,
           'SHADING_LANGUAGE_VERSION');
    assert(String(gl.getParameter(gl.VENDOR)).length > 0, 'VENDOR non-empty');
    assert(String(gl.getParameter(gl.RENDERER)).length > 0, 'RENDERER non-empty');
    assert(gl.isContextLost() === false, 'isContextLost false');

    const attrs = gl.getContextAttributes();
    assert(attrs && typeof attrs.alpha === 'boolean' && typeof attrs.depth === 'boolean',
           'getContextAttributes shape');

    const exts = gl.getSupportedExtensions();
    assert(Array.isArray(exts) && exts.length > 0, 'getSupportedExtensions non-empty');
    assert(gl.getExtension('EXT_color_buffer_float') !== null, 'EXT_color_buffer_float advertised');
    assert(gl.getExtension('BOGUS_extension_name') === null, 'bogus extension is null');

    const prec = gl.getShaderPrecisionFormat(gl.FRAGMENT_SHADER, gl.HIGH_FLOAT);
    assert(prec && prec.precision >= 16, 'highp float precision sane');

    // =====================================================================
    // Implementation limits — WebGL2 spec minimums, not exact values
    // =====================================================================
    const limits = [
        [gl.MAX_TEXTURE_SIZE, 2048, 'MAX_TEXTURE_SIZE'],
        [gl.MAX_CUBE_MAP_TEXTURE_SIZE, 2048, 'MAX_CUBE_MAP_TEXTURE_SIZE'],
        [gl.MAX_RENDERBUFFER_SIZE, 2048, 'MAX_RENDERBUFFER_SIZE'],
        [gl.MAX_VERTEX_ATTRIBS, 16, 'MAX_VERTEX_ATTRIBS'],
        [gl.MAX_TEXTURE_IMAGE_UNITS, 16, 'MAX_TEXTURE_IMAGE_UNITS'],
        [gl.MAX_VERTEX_TEXTURE_IMAGE_UNITS, 16, 'MAX_VERTEX_TEXTURE_IMAGE_UNITS'],
        [gl.MAX_COMBINED_TEXTURE_IMAGE_UNITS, 32, 'MAX_COMBINED_TEXTURE_IMAGE_UNITS'],
        [gl.MAX_VERTEX_UNIFORM_VECTORS, 256, 'MAX_VERTEX_UNIFORM_VECTORS'],
        [gl.MAX_FRAGMENT_UNIFORM_VECTORS, 224, 'MAX_FRAGMENT_UNIFORM_VECTORS'],
        [gl.MAX_VARYING_VECTORS, 15, 'MAX_VARYING_VECTORS'],
        [gl.MAX_DRAW_BUFFERS, 4, 'MAX_DRAW_BUFFERS'],
        [gl.MAX_COLOR_ATTACHMENTS, 4, 'MAX_COLOR_ATTACHMENTS'],
        [gl.MAX_SAMPLES, 4, 'MAX_SAMPLES'],
        [gl.MAX_UNIFORM_BUFFER_BINDINGS, 24, 'MAX_UNIFORM_BUFFER_BINDINGS'],
        [gl.MAX_UNIFORM_BLOCK_SIZE, 16384, 'MAX_UNIFORM_BLOCK_SIZE'],
        [gl.MAX_3D_TEXTURE_SIZE, 256, 'MAX_3D_TEXTURE_SIZE'],
        [gl.MAX_ARRAY_TEXTURE_LAYERS, 256, 'MAX_ARRAY_TEXTURE_LAYERS'],
    ];
    for (const [pname, min, name] of limits) {
        const v = gl.getParameter(pname);
        assert(typeof v === 'number' && v >= min, name + ' >= ' + min + ' (got ' + v + ')');
    }

    // Multi-value queries — these previously smashed the stack by writing 2-4
    // values through the single-int default path.
    const mvd = gl.getParameter(gl.MAX_VIEWPORT_DIMS);
    assert(Array.isArray(mvd) && mvd.length === 2, 'MAX_VIEWPORT_DIMS is [w,h]');
    assert(mvd[0] >= 2048 && mvd[1] >= 2048, 'MAX_VIEWPORT_DIMS sane');
    const lwr = gl.getParameter(gl.ALIASED_LINE_WIDTH_RANGE);
    assert(Array.isArray(lwr) && lwr.length === 2 && lwr[0] <= 1 && lwr[1] >= 1,
           'ALIASED_LINE_WIDTH_RANGE covers 1');
    const psr = gl.getParameter(gl.ALIASED_POINT_SIZE_RANGE);
    assert(Array.isArray(psr) && psr.length === 2 && psr[0] <= 1 && psr[1] >= 1,
           'ALIASED_POINT_SIZE_RANGE covers 1');
    const bc = gl.getParameter(gl.BLEND_COLOR);
    assert(Array.isArray(bc) && bc.length === 4, 'BLEND_COLOR is vec4');
    assert(gl.getError() === gl.NO_ERROR, 'no GL error after multi-value queries');

    // =====================================================================
    // Initial state per WebGL2 spec (fresh context presents defaults even on
    // the engine's shared GL context)
    // =====================================================================
    assert(gl.isEnabled(gl.BLEND) === false, 'BLEND initially off');
    assert(gl.isEnabled(gl.DEPTH_TEST) === false, 'DEPTH_TEST initially off');
    assert(gl.isEnabled(gl.CULL_FACE) === false, 'CULL_FACE initially off');
    assert(gl.isEnabled(gl.SCISSOR_TEST) === false, 'SCISSOR_TEST initially off');
    assert(gl.isEnabled(gl.STENCIL_TEST) === false, 'STENCIL_TEST initially off');
    assert(gl.getParameter(gl.BLEND) === false, 'getParameter(BLEND) false');
    assert(gl.getParameter(gl.DEPTH_TEST) === false, 'getParameter(DEPTH_TEST) false');
    assert(gl.getParameter(gl.DEPTH_FUNC) === gl.LESS, 'depth func LESS');
    assert(gl.getParameter(gl.DEPTH_WRITEMASK) === true, 'depth writemask true');
    assert(gl.getParameter(gl.BLEND_SRC_RGB) === gl.ONE, 'blend src ONE');
    assert(gl.getParameter(gl.BLEND_DST_RGB) === gl.ZERO, 'blend dst ZERO');
    assert(gl.getParameter(gl.BLEND_EQUATION_RGB) === gl.FUNC_ADD, 'blend eq FUNC_ADD');
    assert(gl.getParameter(gl.CULL_FACE_MODE) === gl.BACK, 'cull mode BACK');
    assert(gl.getParameter(gl.FRONT_FACE) === gl.CCW, 'front face CCW');
    assert(gl.getParameter(gl.ACTIVE_TEXTURE) === gl.TEXTURE0, 'active texture 0');
    assert(gl.getParameter(gl.UNPACK_ALIGNMENT) === 4, 'unpack alignment 4');
    assert(gl.getParameter(gl.PACK_ALIGNMENT) === 4, 'pack alignment 4');

    const cc = gl.getParameter(gl.COLOR_CLEAR_VALUE);
    assert(cc.length === 4 && cc[0] === 0 && cc[1] === 0 && cc[2] === 0 && cc[3] === 0,
           'clear color transparent black');
    const cm = gl.getParameter(gl.COLOR_WRITEMASK);
    assert(cm.length === 4 && cm[0] === true && cm[3] === true, 'color writemask all true');
    const dr = gl.getParameter(gl.DEPTH_RANGE);
    assert(dr.length === 2 && dr[0] === 0 && dr[1] === 1, 'depth range [0,1]');
    const vp = gl.getParameter(gl.VIEWPORT);
    assert(vp[0] === 0 && vp[1] === 0 && vp[2] === 64 && vp[3] === 64,
           'initial viewport = canvas size (got ' + vp.join(',') + ')');
    const sb = gl.getParameter(gl.SCISSOR_BOX);
    assert(sb[0] === 0 && sb[1] === 0 && sb[2] === 64 && sb[3] === 64,
           'initial scissor box = canvas size (got ' + sb.join(',') + ')');
    const swm = gl.getParameter(gl.STENCIL_WRITEMASK);
    assert((swm & 0xFF) === 0xFF, 'stencil writemask all ones');

    // The freshly created drawing buffer is transparent black, not garbage.
    const clearPx = new Uint8Array(4);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.readPixels(32, 32, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, clearPx);
    assert(clearPx[0] === 0 && clearPx[1] === 0 && clearPx[2] === 0 && clearPx[3] === 0,
           'fresh drawing buffer is transparent black (got ' + Array.from(clearPx).join(',') + ')');

    // =====================================================================
    // enable/disable/isEnabled round-trips
    // =====================================================================
    for (const cap of [gl.BLEND, gl.DEPTH_TEST, gl.CULL_FACE, gl.SCISSOR_TEST,
                       gl.STENCIL_TEST, gl.POLYGON_OFFSET_FILL, gl.DITHER,
                       gl.RASTERIZER_DISCARD]) {
        gl.enable(cap);
        assert(gl.isEnabled(cap) === true, 'cap 0x' + cap.toString(16) + ' enabled');
        gl.disable(cap);
        assert(gl.isEnabled(cap) === false, 'cap 0x' + cap.toString(16) + ' disabled');
    }
    assert(gl.getError() === gl.NO_ERROR, 'no error after cap round-trips');

    // RASTERIZER_DISCARD is queryable as a boolean
    assert(gl.getParameter(gl.RASTERIZER_DISCARD) === false, 'RASTERIZER_DISCARD queryable');
    assert(gl.getError() === gl.NO_ERROR, 'no error after RASTERIZER_DISCARD query');

    // =====================================================================
    // getError semantics — error is pending until read, then cleared
    // =====================================================================
    assert(gl.getError() === gl.NO_ERROR, 'no initial error');
    gl.enable(0x0BAD); // not a valid capability
    assert(gl.getError() === gl.INVALID_ENUM, 'bad enable -> INVALID_ENUM');
    assert(gl.getError() === gl.NO_ERROR, 'error cleared after read');
    gl.viewport(0, 0, -1, 64); // negative size
    assert(gl.getError() === gl.INVALID_VALUE, 'negative viewport -> INVALID_VALUE');
    assert(gl.getError() === gl.NO_ERROR, 'error cleared again');
    gl.viewport(0, 0, 64, 64);

    // =====================================================================
    // pixelStorei — WebGL-specific pnames are shadow state, never GL errors
    // =====================================================================
    assert(gl.getParameter(gl.UNPACK_FLIP_Y_WEBGL) === false, 'flipY default false');
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, 1);
    assert(gl.getParameter(gl.UNPACK_FLIP_Y_WEBGL) === true, 'flipY set');
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, 0);
    assert(gl.getParameter(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL) === false, 'premultiply default false');
    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, 1);
    assert(gl.getParameter(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL) === true, 'premultiply set');
    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, 0);
    assert(gl.getParameter(gl.UNPACK_COLORSPACE_CONVERSION_WEBGL) === gl.BROWSER_DEFAULT_WEBGL,
           'colorspace conversion default');
    gl.pixelStorei(gl.UNPACK_COLORSPACE_CONVERSION_WEBGL, gl.NONE);
    assert(gl.getParameter(gl.UNPACK_COLORSPACE_CONVERSION_WEBGL) === gl.NONE, 'colorspace NONE');
    gl.pixelStorei(gl.UNPACK_COLORSPACE_CONVERSION_WEBGL, gl.BROWSER_DEFAULT_WEBGL);
    assert(gl.getError() === gl.NO_ERROR, 'WebGL pixelStorei pnames raise no GL error');

    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    assert(gl.getParameter(gl.UNPACK_ALIGNMENT) === 1, 'unpack alignment 1');
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
    gl.pixelStorei(gl.PACK_ALIGNMENT, 1);
    assert(gl.getParameter(gl.PACK_ALIGNMENT) === 1, 'pack alignment 1');
    gl.pixelStorei(gl.PACK_ALIGNMENT, 4);

    // INVALID_INDEX matches the unsigned value returned by getUniformBlockIndex
    assert(gl.INVALID_INDEX === 4294967295, 'INVALID_INDEX is unsigned 0xFFFFFFFF');

    // Compressed texture formats list is an array (possibly empty)
    assert(Array.isArray(gl.getParameter(gl.COMPRESSED_TEXTURE_FORMATS)),
           'COMPRESSED_TEXTURE_FORMATS array');

    // IMPLEMENTATION_COLOR_READ_FORMAT/TYPE are queryable ints
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    assert(gl.getParameter(gl.IMPLEMENTATION_COLOR_READ_FORMAT) > 0, 'impl read format');
    assert(gl.getParameter(gl.IMPLEMENTATION_COLOR_READ_TYPE) > 0, 'impl read type');

    assert(gl.getError() === gl.NO_ERROR, 'no error at end');
    console.log('webgl state tests passed');
}

document.body.removeChild(canvas);
