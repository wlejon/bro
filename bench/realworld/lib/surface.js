// Level 2a: the WebGL2 surface the libraries see, as one JSON document:
// supported extensions, getExtension of every name either browser offers,
// getParameter of every WebGL2 pname (and the extension pnames three.js and
// pixi read), shader precision formats, context attributes, renderbuffer
// sample counts and float render-target completeness, and a few DOM facts
// the libraries branch on.

import { configure, emitBlob, emitLine, done, ENV } from './harness.js';

const PNAMES = [
    'ACTIVE_TEXTURE', 'ALIASED_LINE_WIDTH_RANGE', 'ALIASED_POINT_SIZE_RANGE', 'ALPHA_BITS', 'RED_BITS', 'GREEN_BITS', 'BLUE_BITS',
    'DEPTH_BITS', 'STENCIL_BITS', 'SUBPIXEL_BITS', 'SAMPLES', 'SAMPLE_BUFFERS',
    'MAX_COMBINED_TEXTURE_IMAGE_UNITS', 'MAX_CUBE_MAP_TEXTURE_SIZE', 'MAX_FRAGMENT_UNIFORM_VECTORS', 'MAX_RENDERBUFFER_SIZE',
    'MAX_TEXTURE_IMAGE_UNITS', 'MAX_TEXTURE_SIZE', 'MAX_VARYING_VECTORS', 'MAX_VERTEX_ATTRIBS', 'MAX_VERTEX_TEXTURE_IMAGE_UNITS',
    'MAX_VERTEX_UNIFORM_VECTORS', 'MAX_VIEWPORT_DIMS', 'VERSION', 'SHADING_LANGUAGE_VERSION', 'VENDOR', 'RENDERER',
    'IMPLEMENTATION_COLOR_READ_FORMAT', 'IMPLEMENTATION_COLOR_READ_TYPE', 'COMPRESSED_TEXTURE_FORMATS',
    'VIEWPORT', 'SCISSOR_BOX', 'COLOR_CLEAR_VALUE', 'DEPTH_CLEAR_VALUE', 'STENCIL_CLEAR_VALUE', 'FRONT_FACE', 'CULL_FACE_MODE',
    'DEPTH_FUNC', 'DEPTH_RANGE', 'DEPTH_WRITEMASK', 'COLOR_WRITEMASK', 'BLEND', 'DEPTH_TEST', 'CULL_FACE', 'SCISSOR_TEST',
    'STENCIL_TEST', 'DITHER', 'POLYGON_OFFSET_FILL', 'SAMPLE_COVERAGE', 'SAMPLE_ALPHA_TO_COVERAGE', 'RASTERIZER_DISCARD',
    'UNPACK_ALIGNMENT', 'PACK_ALIGNMENT', 'UNPACK_FLIP_Y_WEBGL', 'UNPACK_PREMULTIPLY_ALPHA_WEBGL',
    'UNPACK_COLORSPACE_CONVERSION_WEBGL', 'LINE_WIDTH', 'GENERATE_MIPMAP_HINT', 'FRAGMENT_SHADER_DERIVATIVE_HINT',
    'BLEND_SRC_RGB', 'BLEND_DST_RGB', 'BLEND_SRC_ALPHA', 'BLEND_DST_ALPHA', 'BLEND_EQUATION_RGB', 'BLEND_EQUATION_ALPHA', 'BLEND_COLOR',
    'STENCIL_FUNC', 'STENCIL_REF', 'STENCIL_VALUE_MASK', 'STENCIL_WRITEMASK', 'STENCIL_FAIL', 'STENCIL_PASS_DEPTH_FAIL', 'STENCIL_PASS_DEPTH_PASS',
    'STENCIL_BACK_FUNC', 'STENCIL_BACK_REF', 'STENCIL_BACK_VALUE_MASK', 'STENCIL_BACK_WRITEMASK',
    'POLYGON_OFFSET_FACTOR', 'POLYGON_OFFSET_UNITS', 'SAMPLE_COVERAGE_VALUE', 'SAMPLE_COVERAGE_INVERT',
    'ARRAY_BUFFER_BINDING', 'ELEMENT_ARRAY_BUFFER_BINDING', 'CURRENT_PROGRAM', 'FRAMEBUFFER_BINDING', 'RENDERBUFFER_BINDING',
    'TEXTURE_BINDING_2D', 'TEXTURE_BINDING_CUBE_MAP', 'VERTEX_ARRAY_BINDING',
    // WebGL2
    'MAX_3D_TEXTURE_SIZE', 'MAX_ARRAY_TEXTURE_LAYERS', 'MAX_CLIENT_WAIT_TIMEOUT_WEBGL', 'MAX_COLOR_ATTACHMENTS',
    'MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS', 'MAX_COMBINED_UNIFORM_BLOCKS', 'MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS',
    'MAX_DRAW_BUFFERS', 'MAX_ELEMENT_INDEX', 'MAX_ELEMENTS_INDICES', 'MAX_ELEMENTS_VERTICES', 'MAX_FRAGMENT_INPUT_COMPONENTS',
    'MAX_FRAGMENT_UNIFORM_BLOCKS', 'MAX_FRAGMENT_UNIFORM_COMPONENTS', 'MAX_PROGRAM_TEXEL_OFFSET', 'MIN_PROGRAM_TEXEL_OFFSET',
    'MAX_SAMPLES', 'MAX_SERVER_WAIT_TIMEOUT', 'MAX_TEXTURE_LOD_BIAS', 'MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS',
    'MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS', 'MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS', 'MAX_UNIFORM_BLOCK_SIZE',
    'MAX_UNIFORM_BUFFER_BINDINGS', 'MAX_VARYING_COMPONENTS', 'MAX_VERTEX_OUTPUT_COMPONENTS', 'MAX_VERTEX_UNIFORM_BLOCKS',
    'MAX_VERTEX_UNIFORM_COMPONENTS', 'UNIFORM_BUFFER_OFFSET_ALIGNMENT', 'READ_BUFFER', 'DRAW_BUFFER0',
    'PACK_ROW_LENGTH', 'PACK_SKIP_PIXELS', 'PACK_SKIP_ROWS', 'UNPACK_IMAGE_HEIGHT', 'UNPACK_ROW_LENGTH', 'UNPACK_SKIP_IMAGES',
    'UNPACK_SKIP_PIXELS', 'UNPACK_SKIP_ROWS', 'TRANSFORM_FEEDBACK_PAUSED', 'TRANSFORM_FEEDBACK_ACTIVE',
    'COPY_READ_BUFFER_BINDING', 'COPY_WRITE_BUFFER_BINDING', 'DRAW_FRAMEBUFFER_BINDING', 'READ_FRAMEBUFFER_BINDING',
    'PIXEL_PACK_BUFFER_BINDING', 'PIXEL_UNPACK_BUFFER_BINDING', 'SAMPLER_BINDING', 'TEXTURE_BINDING_2D_ARRAY',
    'TEXTURE_BINDING_3D', 'UNIFORM_BUFFER_BINDING', 'TRANSFORM_FEEDBACK_BINDING', 'TRANSFORM_FEEDBACK_BUFFER_BINDING',
];

// Every extension name either browser reports (bro 11, Chromium 32 on halo).
const KNOWN_EXTS = [
    'EXT_clip_control', 'EXT_color_buffer_float', 'EXT_color_buffer_half_float', 'EXT_conservative_depth', 'EXT_depth_clamp',
    'EXT_disjoint_timer_query_webgl2', 'EXT_float_blend', 'EXT_polygon_offset_clamp', 'EXT_render_snorm',
    'EXT_texture_compression_bptc', 'EXT_texture_compression_rgtc', 'EXT_texture_filter_anisotropic', 'EXT_texture_mirror_clamp_to_edge',
    'EXT_texture_norm16', 'KHR_parallel_shader_compile', 'NV_shader_noperspective_interpolation', 'OES_draw_buffers_indexed',
    'OES_sample_variables', 'OES_shader_multisample_interpolation', 'OES_texture_float_linear', 'OVR_multiview2',
    'WEBGL_blend_func_extended', 'WEBGL_clip_cull_distance', 'WEBGL_compressed_texture_astc', 'WEBGL_compressed_texture_etc',
    'WEBGL_compressed_texture_etc1', 'WEBGL_compressed_texture_pvrtc', 'WEBGL_compressed_texture_s3tc', 'WEBGL_compressed_texture_s3tc_srgb',
    'WEBGL_debug_renderer_info', 'WEBGL_debug_shaders', 'WEBGL_lose_context', 'WEBGL_multi_draw', 'WEBGL_polygon_mode',
    'WEBGL_provoking_vertex', 'WEBGL_render_shared_exponent', 'WEBGL_stencil_texturing', 'OES_texture_half_float_linear',
    'WEBGL_draw_instanced_base_vertex_base_instance', 'WEBGL_multi_draw_instanced_base_vertex_base_instance', 'EXT_texture_compression_s3tc',
    'OES_vertex_array_object', 'ANGLE_instanced_arrays', 'OES_element_index_uint', 'OES_standard_derivatives', 'EXT_shader_texture_lod',
    'EXT_frag_depth', 'WEBGL_depth_texture', 'OES_texture_float', 'OES_texture_half_float', 'EXT_sRGB', 'EXT_blend_minmax', 'WEBGL_draw_buffers',
];

function val(v) {
    if (v === null || v === undefined) return v === null ? null : 'undefined';
    if (ArrayBuffer.isView(v)) return Array.from(v);
    if (typeof v === 'object') {
        const n = v.constructor && v.constructor.name;
        if (n && n.startsWith('WebGL')) return '<' + n + '>';
        return v;
    }
    return v;
}

function collect(cfg) {
    const canvas = document.createElement('canvas');
    canvas.width = cfg.w; canvas.height = cfg.h;
    document.body.appendChild(canvas);
    // the attributes three.js r160 asks for (WebGLRenderer defaults, antialias off)
    const gl = canvas.getContext('webgl2', { alpha: true, depth: true, stencil: false, antialias: false, premultipliedAlpha: true,
        preserveDrawingBuffer: false, powerPreference: 'high-performance', failIfMajorPerformanceCaveat: false });
    const res = { env: ENV, ua: navigator.userAgent, extensions: [], parameters: {}, precision: {}, attributes: {}, formats: {}, misc: {} };
    if (!gl) { res.misc.webgl2 = false; return res; }
    res.extensions = gl.getSupportedExtensions() || [];
    const all = Array.from(new Set(KNOWN_EXTS.concat(res.extensions))).sort();
    for (const e of all) res.misc['getExtension:' + e] = !!gl.getExtension(e);
    for (const p of PNAMES) {
        const e = gl[p];
        if (typeof e !== 'number') { res.parameters[p] = 'no-enum'; continue; }
        let v;
        try { v = val(gl.getParameter(e)); } catch (err) { v = 'throws: ' + err; }
        const err = gl.getError();
        res.parameters[p] = err ? { value: v, glError: err } : v;
    }
    const ani = gl.getExtension('EXT_texture_filter_anisotropic');
    res.parameters.MAX_TEXTURE_MAX_ANISOTROPY_EXT = ani ? gl.getParameter(ani.MAX_TEXTURE_MAX_ANISOTROPY_EXT) : 'no-ext';
    const dri = gl.getExtension('WEBGL_debug_renderer_info');
    res.parameters.UNMASKED_VENDOR_WEBGL = dri ? gl.getParameter(dri.UNMASKED_VENDOR_WEBGL) : 'no-ext';
    res.parameters.UNMASKED_RENDERER_WEBGL = dri ? gl.getParameter(dri.UNMASKED_RENDERER_WEBGL) : 'no-ext';
    const tq = gl.getExtension('EXT_disjoint_timer_query_webgl2');
    res.parameters.TIMESTAMP_EXT = tq ? 'present' : 'no-ext';
    gl.getError();
    for (const sh of ['VERTEX_SHADER', 'FRAGMENT_SHADER']) {
        for (const pr of ['LOW_FLOAT', 'MEDIUM_FLOAT', 'HIGH_FLOAT', 'LOW_INT', 'MEDIUM_INT', 'HIGH_INT']) {
            const f = gl.getShaderPrecisionFormat(gl[sh], gl[pr]);
            res.precision[sh + '/' + pr] = f ? [f.rangeMin, f.rangeMax, f.precision] : null;
        }
    }
    res.attributes = gl.getContextAttributes();
    for (const fmt of ['RGBA8', 'SRGB8_ALPHA8', 'DEPTH24_STENCIL8', 'DEPTH_COMPONENT24', 'DEPTH32F_STENCIL8', 'RGBA16F', 'RGBA32F', 'R11F_G11F_B10F', 'RGB10_A2']) {
        let v;
        try { v = val(gl.getInternalformatParameter(gl.RENDERBUFFER, gl[fmt], gl.SAMPLES)); } catch (err) { v = 'throws'; }
        res.formats['samples:' + fmt] = v;
        gl.getError();
    }
    // render-target completeness three.js relies on (HalfFloat targets, float shadow / transmission)
    const rt = [['RGBA8', 'RGBA', 'UNSIGNED_BYTE'], ['RGBA16F', 'RGBA', 'HALF_FLOAT'], ['RGBA32F', 'RGBA', 'FLOAT'],
        ['R11F_G11F_B10F', 'RGB', 'HALF_FLOAT'], ['R32F', 'RED', 'FLOAT'], ['RG16F', 'RG', 'HALF_FLOAT']];
    for (const [ifmt, f, t] of rt) {
        const tex = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, tex);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl[ifmt], 4, 4, 0, gl[f], gl[t], null);
        const e1 = gl.getError();
        const fb = gl.createFramebuffer();
        gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
        gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, tex, 0);
        const st = gl.checkFramebufferStatus(gl.FRAMEBUFFER);
        res.formats['rt:' + ifmt] = (st === gl.FRAMEBUFFER_COMPLETE ? 'complete' : 'status 0x' + st.toString(16)) + (e1 ? ' teximage-err 0x' + e1.toString(16) : '');
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        gl.deleteFramebuffer(fb);
        gl.deleteTexture(tex);
        gl.getError();
    }
    const m = res.misc;
    m.drawingBuffer = [gl.drawingBufferWidth, gl.drawingBufferHeight];
    m.drawingBufferColorSpace = String(gl.drawingBufferColorSpace);
    m.unpackColorSpace = String(gl.unpackColorSpace);
    m.devicePixelRatio = window.devicePixelRatio;
    m.webgl1Context = !!document.createElement('canvas').getContext('webgl');
    m.OffscreenCanvas = typeof OffscreenCanvas;
    m.createImageBitmap = typeof createImageBitmap;
    m.ImageBitmap = typeof ImageBitmap;
    m.HTMLImageElement = typeof HTMLImageElement;
    m.VideoFrame = typeof VideoFrame;
    m.navigatorGpu = typeof navigator.gpu;
    m.protoGetContext = typeof HTMLCanvasElement.prototype.getContext;
    m.protoGetParameter = typeof WebGL2RenderingContext.prototype.getParameter;
    m.canvasToDataURL = typeof canvas.toDataURL;
    m.requestVideoFrameCallback = typeof HTMLVideoElement !== 'undefined' && typeof HTMLVideoElement.prototype.requestVideoFrameCallback;
    m.performanceNow = typeof performance !== 'undefined' && typeof performance.now;
    m.Proxy = typeof Proxy;
    m.WeakRef = typeof WeakRef;
    m.FinalizationRegistry = typeof FinalizationRegistry;
    m.structuredClone = typeof structuredClone;
    m.queueMicrotask = typeof queueMicrotask;
    m.ResizeObserver = typeof ResizeObserver;
    m.matchMediaDpr = typeof matchMedia === 'function';
    m.fontsApi = typeof document.fonts;
    m.isContextLost = gl.isContextLost();
    return res;
}

const cfg = configure({ w: 640, h: 480 });
(async () => {
    try {
        const res = collect(cfg);
        emitLine('surface', { extensions: res.extensions.length });
        await emitBlob('surface.json', JSON.stringify(res, null, 1));
        done('ok');
    } catch (e) {
        emitLine('error', { msg: String(e), stack: String(e && e.stack) });
        done('error');
    }
})();
