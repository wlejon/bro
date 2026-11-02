// getParameter's initial state, value for value against the WebGL 2 / ES 3.0
// state tables (ES 3.0 6.2), each in the type the IDL gives it: the stencil
// masks are GLuint (all ones is 4294967295, not -1), DITHER starts enabled,
// and the back-face stencil state is the front's. Then the back-face setters
// land in the back-face pnames (STENCIL_BACK_REF and STENCIL_BACK_VALUE_MASK
// are distinct enums).

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '32');
canvas.setAttribute('height', '16');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2', { antialias: false, stencil: true });
if (!gl) {
    missingGpuContext('webgl2');
} else {
    function same(got, want) {
        if (want === null || typeof want !== 'object') return got === want;
        if (got === null || typeof got !== 'object' || got.length !== want.length) return false;
        return Array.from(got).every((v, i) => v === want[i]);
    }
    const initial = {
        ACTIVE_TEXTURE: gl.TEXTURE0,
        ARRAY_BUFFER_BINDING: null, ELEMENT_ARRAY_BUFFER_BINDING: null, CURRENT_PROGRAM: null,
        FRAMEBUFFER_BINDING: null, READ_FRAMEBUFFER_BINDING: null, RENDERBUFFER_BINDING: null,
        TEXTURE_BINDING_2D: null, TEXTURE_BINDING_CUBE_MAP: null, TEXTURE_BINDING_3D: null,
        TEXTURE_BINDING_2D_ARRAY: null, VERTEX_ARRAY_BINDING: null, SAMPLER_BINDING: null,
        TRANSFORM_FEEDBACK_BINDING: null, COPY_READ_BUFFER_BINDING: null, COPY_WRITE_BUFFER_BINDING: null,
        PIXEL_PACK_BUFFER_BINDING: null, PIXEL_UNPACK_BUFFER_BINDING: null, UNIFORM_BUFFER_BINDING: null,
        TRANSFORM_FEEDBACK_BUFFER_BINDING: null,
        VIEWPORT: [0, 0, 32, 16], SCISSOR_BOX: [0, 0, 32, 16],
        COLOR_CLEAR_VALUE: [0, 0, 0, 0], DEPTH_CLEAR_VALUE: 1, STENCIL_CLEAR_VALUE: 0,
        COLOR_WRITEMASK: [true, true, true, true], DEPTH_WRITEMASK: true,
        DEPTH_FUNC: gl.LESS, DEPTH_RANGE: [0, 1], FRONT_FACE: gl.CCW, CULL_FACE_MODE: gl.BACK,
        BLEND: false, CULL_FACE: false, DEPTH_TEST: false, DITHER: true, POLYGON_OFFSET_FILL: false,
        RASTERIZER_DISCARD: false, SAMPLE_ALPHA_TO_COVERAGE: false, SAMPLE_COVERAGE: false,
        SCISSOR_TEST: false, STENCIL_TEST: false,
        BLEND_SRC_RGB: gl.ONE, BLEND_SRC_ALPHA: gl.ONE, BLEND_DST_RGB: gl.ZERO, BLEND_DST_ALPHA: gl.ZERO,
        BLEND_EQUATION_RGB: gl.FUNC_ADD, BLEND_EQUATION_ALPHA: gl.FUNC_ADD, BLEND_COLOR: [0, 0, 0, 0],
        STENCIL_FUNC: gl.ALWAYS, STENCIL_REF: 0, STENCIL_VALUE_MASK: 0xFFFFFFFF, STENCIL_WRITEMASK: 0xFFFFFFFF,
        STENCIL_FAIL: gl.KEEP, STENCIL_PASS_DEPTH_FAIL: gl.KEEP, STENCIL_PASS_DEPTH_PASS: gl.KEEP,
        STENCIL_BACK_FUNC: gl.ALWAYS, STENCIL_BACK_REF: 0, STENCIL_BACK_VALUE_MASK: 0xFFFFFFFF,
        STENCIL_BACK_WRITEMASK: 0xFFFFFFFF, STENCIL_BACK_FAIL: gl.KEEP, STENCIL_BACK_PASS_DEPTH_FAIL: gl.KEEP,
        STENCIL_BACK_PASS_DEPTH_PASS: gl.KEEP,
        POLYGON_OFFSET_FACTOR: 0, POLYGON_OFFSET_UNITS: 0, SAMPLE_COVERAGE_VALUE: 1, SAMPLE_COVERAGE_INVERT: false,
        LINE_WIDTH: 1, GENERATE_MIPMAP_HINT: gl.DONT_CARE, FRAGMENT_SHADER_DERIVATIVE_HINT: gl.DONT_CARE,
        PACK_ALIGNMENT: 4, UNPACK_ALIGNMENT: 4, PACK_ROW_LENGTH: 0, PACK_SKIP_PIXELS: 0, PACK_SKIP_ROWS: 0,
        UNPACK_ROW_LENGTH: 0, UNPACK_IMAGE_HEIGHT: 0, UNPACK_SKIP_PIXELS: 0, UNPACK_SKIP_ROWS: 0,
        UNPACK_SKIP_IMAGES: 0, UNPACK_FLIP_Y_WEBGL: false, UNPACK_PREMULTIPLY_ALPHA_WEBGL: false,
        UNPACK_COLORSPACE_CONVERSION_WEBGL: gl.BROWSER_DEFAULT_WEBGL,
        READ_BUFFER: gl.BACK, DRAW_BUFFER0: gl.BACK,
        TRANSFORM_FEEDBACK_ACTIVE: false, TRANSFORM_FEEDBACK_PAUSED: false,
    };
    for (const [name, want] of Object.entries(initial)) {
        assert(typeof gl[name] === 'number', name + ' is a constant');
        const got = gl.getParameter(gl[name]);
        assert(same(got, want), name + ': got ' + JSON.stringify(got && got.length !== undefined ? Array.from(got) : got) +
               ' want ' + JSON.stringify(want));
    }
    assert(gl.getError() === gl.NO_ERROR, 'no error from the initial-state reads');

    // The typed arrays the IDL names.
    assert(gl.getParameter(gl.VIEWPORT) instanceof Int32Array, 'VIEWPORT is an Int32Array');
    assert(gl.getParameter(gl.DEPTH_RANGE) instanceof Float32Array, 'DEPTH_RANGE is a Float32Array');
    assert(gl.getParameter(gl.BLEND_COLOR) instanceof Float32Array, 'BLEND_COLOR is a Float32Array');
    assert(Array.isArray(gl.getParameter(gl.COLOR_WRITEMASK)), 'COLOR_WRITEMASK is an Array');

    // The back-face setters land in the back-face pnames, each in its own.
    gl.stencilFuncSeparate(gl.BACK, gl.EQUAL, 5, 0x0F);
    gl.stencilMaskSeparate(gl.BACK, 0x3C);
    gl.stencilOpSeparate(gl.BACK, gl.INCR, gl.DECR, gl.INVERT);
    assert(gl.getParameter(gl.STENCIL_BACK_FUNC) === gl.EQUAL, 'STENCIL_BACK_FUNC');
    assert(gl.getParameter(gl.STENCIL_BACK_REF) === 5, 'STENCIL_BACK_REF: ' + gl.getParameter(gl.STENCIL_BACK_REF));
    assert(gl.getParameter(gl.STENCIL_BACK_VALUE_MASK) === 0x0F,
           'STENCIL_BACK_VALUE_MASK: ' + gl.getParameter(gl.STENCIL_BACK_VALUE_MASK));
    assert(gl.getParameter(gl.STENCIL_BACK_WRITEMASK) === 0x3C, 'STENCIL_BACK_WRITEMASK');
    assert(gl.getParameter(gl.STENCIL_BACK_FAIL) === gl.INCR, 'STENCIL_BACK_FAIL');
    assert(gl.getParameter(gl.STENCIL_BACK_PASS_DEPTH_FAIL) === gl.DECR, 'STENCIL_BACK_PASS_DEPTH_FAIL');
    assert(gl.getParameter(gl.STENCIL_BACK_PASS_DEPTH_PASS) === gl.INVERT, 'STENCIL_BACK_PASS_DEPTH_PASS');
    // ... and leave the front face's alone.
    assert(gl.getParameter(gl.STENCIL_FUNC) === gl.ALWAYS, 'STENCIL_FUNC untouched');
    assert(gl.getParameter(gl.STENCIL_REF) === 0, 'STENCIL_REF untouched');
    assert(gl.getParameter(gl.STENCIL_VALUE_MASK) === 0xFFFFFFFF, 'STENCIL_VALUE_MASK untouched');
    assert(gl.getParameter(gl.STENCIL_WRITEMASK) === 0xFFFFFFFF, 'STENCIL_WRITEMASK untouched');
    // A mask with the top bit set stays unsigned.
    gl.stencilMask(0x80000001);
    assert(gl.getParameter(gl.STENCIL_WRITEMASK) === 0x80000001, 'STENCIL_WRITEMASK 0x80000001 unsigned');
    gl.disable(gl.DITHER);
    assert(gl.getParameter(gl.DITHER) === false && gl.isEnabled(gl.DITHER) === false, 'DITHER disables');
    assert(gl.getError() === gl.NO_ERROR, 'no error');
}
