// A stub WebGL2 context for mode=bench: three.js r160's WebGLRenderer and
// pixi v8's WebGL renderer initialize and render every frame against it, in
// node and in a bronze-built executable alike, with nothing reaching a GPU.
//
// - Every WebGL2 method exists. Draw calls do nothing but count; buffer and
//   texture uploads count; uniform* calls count; every call counts (`calls`).
// - The surface (getParameter, getExtension, getSupportedExtensions,
//   getShaderPrecisionFormat, getContextAttributes) is Chromium's WebGL2 on a
//   desktop GPU (ANGLE): what lib/surface.js reads and what the libraries
//   report from it (MAX_TEXTURE_IMAGE_UNITS 32, highp, ...).
// - Shaders compile and programs link; the active uniforms, attributes and
//   uniform blocks come from the shader source (lib/glsl_reflect.js).
// - State is kept only where a later query reads it (bindings, viewport,
//   scissor, enables, pixel store, buffer sizes).
// No method allocates after setup except the create* calls, linkProgram and
// the rare queries that return fresh objects in a browser too.

import { GL } from './glconst.js';
import { reflectProgram } from './glsl_reflect.js';

class WebGLObject { constructor(id) { this._id = id; this._deleted = false; } }
class WebGLBuffer extends WebGLObject { constructor(id) { super(id); this._size = 0; } }
class WebGLTexture extends WebGLObject {}
class WebGLFramebuffer extends WebGLObject {}
class WebGLRenderbuffer extends WebGLObject {}
class WebGLVertexArrayObject extends WebGLObject {}
class WebGLQuery extends WebGLObject {}
class WebGLSampler extends WebGLObject {}
class WebGLSync extends WebGLObject {}
class WebGLTransformFeedback extends WebGLObject {}
class WebGLShader extends WebGLObject { constructor(id, type) { super(id); this._type = type; this._source = ''; } }
class WebGLProgram extends WebGLObject {
    constructor(id) {
        super(id);
        this._vs = null; this._fs = null; this._bound = new Map();
        this._uniforms = []; this._attribs = []; this._blocks = [];
        this._uniformLocs = new Map(); this._attribLocs = new Map(); this._linked = false;
    }
}
class WebGLUniformLocation { constructor(program, index) { this._program = program; this._index = index; } }
class WebGLActiveInfo { constructor(name, size, type) { this.name = name; this.size = size; this.type = type; } }
class WebGLShaderPrecisionFormat { constructor(rangeMin, rangeMax, precision) { this.rangeMin = rangeMin; this.rangeMax = rangeMax; this.precision = precision; } }

// Chromium's extension list on a desktop Linux GPU (ANGLE), 32 names.
export const EXTENSIONS = [
    'EXT_clip_control', 'EXT_color_buffer_float', 'EXT_color_buffer_half_float', 'EXT_conservative_depth',
    'EXT_depth_clamp', 'EXT_disjoint_timer_query_webgl2', 'EXT_float_blend', 'EXT_polygon_offset_clamp',
    'EXT_render_snorm', 'EXT_texture_compression_bptc', 'EXT_texture_compression_rgtc',
    'EXT_texture_filter_anisotropic', 'EXT_texture_mirror_clamp_to_edge', 'EXT_texture_norm16',
    'KHR_parallel_shader_compile', 'NV_shader_noperspective_interpolation', 'OES_draw_buffers_indexed',
    'OES_sample_variables', 'OES_shader_multisample_interpolation', 'OES_texture_float_linear', 'OVR_multiview2',
    'WEBGL_blend_func_extended', 'WEBGL_clip_cull_distance', 'WEBGL_compressed_texture_s3tc',
    'WEBGL_compressed_texture_s3tc_srgb', 'WEBGL_debug_renderer_info', 'WEBGL_debug_shaders', 'WEBGL_lose_context',
    'WEBGL_multi_draw', 'WEBGL_polygon_mode', 'WEBGL_provoking_vertex', 'WEBGL_stencil_texturing',
];

const VENDOR = 'WebKit', RENDERER = 'WebKit WebGL';
const UNMASKED_VENDOR = 'Google Inc. (AMD)';
const UNMASKED_RENDERER = 'ANGLE (AMD, AMD Radeon Graphics (radeonsi), OpenGL 4.6)';

// getParameter values that never change (Chromium WebGL2 over ANGLE).
function staticParameters() {
    const P = new Map();
    const set = (k, v) => P.set(GL[k], v);
    set('MAX_TEXTURE_SIZE', 16384); set('MAX_CUBE_MAP_TEXTURE_SIZE', 16384); set('MAX_RENDERBUFFER_SIZE', 16384);
    set('MAX_TEXTURE_IMAGE_UNITS', 32); set('MAX_VERTEX_TEXTURE_IMAGE_UNITS', 32);
    set('MAX_COMBINED_TEXTURE_IMAGE_UNITS', 64); set('MAX_VERTEX_ATTRIBS', 16);
    set('MAX_VERTEX_UNIFORM_VECTORS', 4096); set('MAX_FRAGMENT_UNIFORM_VECTORS', 1024); set('MAX_VARYING_VECTORS', 31);
    set('MAX_VIEWPORT_DIMS', new Int32Array([32767, 32767]));
    set('ALIASED_LINE_WIDTH_RANGE', new Float32Array([1, 1])); set('ALIASED_POINT_SIZE_RANGE', new Float32Array([1, 1024]));
    set('VERSION', 'WebGL 2.0 (OpenGL ES 3.0 Chromium)');
    set('SHADING_LANGUAGE_VERSION', 'WebGL GLSL ES 3.00 (OpenGL ES GLSL ES 3.0 Chromium)');
    set('VENDOR', VENDOR); set('RENDERER', RENDERER);
    P.set(0x9245, UNMASKED_VENDOR); P.set(0x9246, UNMASKED_RENDERER);
    P.set(0x84FF, 16); // MAX_TEXTURE_MAX_ANISOTROPY_EXT
    P.set(0x8864, 64); // QUERY_COUNTER_BITS_EXT
    set('RED_BITS', 8); set('GREEN_BITS', 8); set('BLUE_BITS', 8); set('ALPHA_BITS', 8);
    set('DEPTH_BITS', 24); set('STENCIL_BITS', 8); set('SUBPIXEL_BITS', 4); set('SAMPLES', 0); set('SAMPLE_BUFFERS', 0);
    set('IMPLEMENTATION_COLOR_READ_FORMAT', GL.RGBA); set('IMPLEMENTATION_COLOR_READ_TYPE', GL.UNSIGNED_BYTE);
    set('COMPRESSED_TEXTURE_FORMATS', new Uint32Array([0x83F0, 0x83F1, 0x83F2, 0x83F3, 0x8C4C, 0x8C4D, 0x8C4E, 0x8C4F,
        0x8E8C, 0x8E8D, 0x8E8E, 0x8E8F, 0x8DBB, 0x8DBC, 0x8DBD, 0x8DBE]));
    set('MAX_3D_TEXTURE_SIZE', 2048); set('MAX_ARRAY_TEXTURE_LAYERS', 2048); set('MAX_CLIENT_WAIT_TIMEOUT_WEBGL', 0);
    set('MAX_COLOR_ATTACHMENTS', 8); set('MAX_DRAW_BUFFERS', 8);
    set('MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS', 200704); set('MAX_COMBINED_UNIFORM_BLOCKS', 24);
    set('MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS', 212992); set('MAX_ELEMENT_INDEX', 4294967295);
    set('MAX_ELEMENTS_INDICES', 2147483647); set('MAX_ELEMENTS_VERTICES', 2147483647);
    set('MAX_FRAGMENT_INPUT_COMPONENTS', 124); set('MAX_FRAGMENT_UNIFORM_BLOCKS', 12);
    set('MAX_FRAGMENT_UNIFORM_COMPONENTS', 4096); set('MAX_PROGRAM_TEXEL_OFFSET', 7); set('MIN_PROGRAM_TEXEL_OFFSET', -8);
    set('MAX_SAMPLES', 8); set('MAX_SERVER_WAIT_TIMEOUT', 0); set('MAX_TEXTURE_LOD_BIAS', 16);
    set('MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS', 128); set('MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS', 4);
    set('MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS', 4); set('MAX_UNIFORM_BLOCK_SIZE', 65536);
    set('MAX_UNIFORM_BUFFER_BINDINGS', 24); set('MAX_VARYING_COMPONENTS', 124); set('MAX_VERTEX_OUTPUT_COMPONENTS', 128);
    set('MAX_VERTEX_UNIFORM_BLOCKS', 12); set('MAX_VERTEX_UNIFORM_COMPONENTS', 16384);
    set('UNIFORM_BUFFER_OFFSET_ALIGNMENT', 256);
    return P;
}
const STATIC = staticParameters();

const FLOAT_PRECISION = new WebGLShaderPrecisionFormat(127, 127, 23);
const INT_PRECISION = new WebGLShaderPrecisionFormat(31, 30, 0);

function makeExtension(name, ctx) {
    const e = {};
    switch (name) {
        case 'EXT_texture_filter_anisotropic':
            e.TEXTURE_MAX_ANISOTROPY_EXT = 0x84FE; e.MAX_TEXTURE_MAX_ANISOTROPY_EXT = 0x84FF; break;
        case 'WEBGL_debug_renderer_info':
            e.UNMASKED_VENDOR_WEBGL = 0x9245; e.UNMASKED_RENDERER_WEBGL = 0x9246; break;
        case 'EXT_disjoint_timer_query_webgl2':
            e.QUERY_COUNTER_BITS_EXT = 0x8864; e.TIME_ELAPSED_EXT = 0x88BF; e.TIMESTAMP_EXT = 0x8E28; e.GPU_DISJOINT_EXT = 0x8FBB;
            e.queryCounterEXT = function () { ctx.calls++; };
            break;
        case 'KHR_parallel_shader_compile': e.COMPLETION_STATUS_KHR = 0x91B1; e.maxShaderCompilerThreadsKHR = function () { ctx.calls++; }; break;
        case 'WEBGL_compressed_texture_s3tc':
            e.COMPRESSED_RGB_S3TC_DXT1_EXT = 0x83F0; e.COMPRESSED_RGBA_S3TC_DXT1_EXT = 0x83F1;
            e.COMPRESSED_RGBA_S3TC_DXT3_EXT = 0x83F2; e.COMPRESSED_RGBA_S3TC_DXT5_EXT = 0x83F3; break;
        case 'WEBGL_compressed_texture_s3tc_srgb':
            e.COMPRESSED_SRGB_S3TC_DXT1_EXT = 0x8C4C; e.COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT = 0x8C4D;
            e.COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT = 0x8C4E; e.COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT = 0x8C4F; break;
        case 'EXT_texture_compression_bptc':
            e.COMPRESSED_RGBA_BPTC_UNORM_EXT = 0x8E8C; e.COMPRESSED_SRGB_ALPHA_BPTC_UNORM_EXT = 0x8E8D;
            e.COMPRESSED_RGB_BPTC_SIGNED_FLOAT_EXT = 0x8E8E; e.COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT_EXT = 0x8E8F; break;
        case 'EXT_texture_compression_rgtc':
            e.COMPRESSED_RED_RGTC1_EXT = 0x8DBB; e.COMPRESSED_SIGNED_RED_RGTC1_EXT = 0x8DBC;
            e.COMPRESSED_RED_GREEN_RGTC2_EXT = 0x8DBD; e.COMPRESSED_SIGNED_RED_GREEN_RGTC2_EXT = 0x8DBE; break;
        case 'EXT_color_buffer_half_float':
            e.RGBA16F_EXT = 0x881A; e.RGB16F_EXT = 0x881B; e.FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE_EXT = 0x8211;
            e.UNSIGNED_NORMALIZED_EXT = 0x8C17; break;
        case 'WEBGL_lose_context':
            e.loseContext = function () { ctx.calls++; }; e.restoreContext = function () { ctx.calls++; }; break;
        case 'WEBGL_provoking_vertex':
            e.FIRST_VERTEX_CONVENTION_WEBGL = 0x8E4D; e.LAST_VERTEX_CONVENTION_WEBGL = 0x8E4E; e.PROVOKING_VERTEX_WEBGL = 0x8E4F;
            e.provokingVertexWEBGL = function () { ctx.calls++; }; break;
        case 'OES_draw_buffers_indexed':
            for (const f of ['enableiOES', 'disableiOES', 'blendEquationiOES', 'blendEquationSeparateiOES', 'blendFunciOES',
                'blendFuncSeparateiOES', 'colorMaskiOES']) e[f] = function () { ctx.calls++; };
            e.isEnablediOES = function () { ctx.calls++; return false; };
            break;
        case 'WEBGL_multi_draw':
            for (const f of ['multiDrawArraysWEBGL', 'multiDrawElementsWEBGL', 'multiDrawArraysInstancedWEBGL', 'multiDrawElementsInstancedWEBGL']) {
                e[f] = function () { ctx.calls++; ctx.draws++; };
            }
            break;
        case 'WEBGL_debug_shaders': e.getTranslatedShaderSource = function () { ctx.calls++; return ''; }; break;
        case 'EXT_clip_control':
            e.LOWER_LEFT_EXT = 0x8CA1; e.UPPER_LEFT_EXT = 0x8CA2; e.NEGATIVE_ONE_TO_ONE_EXT = 0x935E; e.ZERO_TO_ONE_EXT = 0x935F;
            e.clipControlEXT = function () { ctx.calls++; }; break;
        case 'EXT_polygon_offset_clamp': e.POLYGON_OFFSET_CLAMP_EXT = 0x8E1B; e.polygonOffsetClampEXT = function () { ctx.calls++; }; break;
        case 'WEBGL_polygon_mode':
            e.POLYGON_MODE_WEBGL = 0x0B40; e.LINE_WEBGL = 0x1B01; e.FILL_WEBGL = 0x1B02;
            e.polygonModeWEBGL = function () { ctx.calls++; }; break;
        case 'WEBGL_clip_cull_distance':
            e.MAX_CLIP_DISTANCES_WEBGL = 0x0D32; e.MAX_CULL_DISTANCES_WEBGL = 0x82F9;
            e.MAX_COMBINED_CLIP_AND_CULL_DISTANCES_WEBGL = 0x82FA; e.CLIP_DISTANCE0_WEBGL = 0x3000; break;
        case 'OVR_multiview2':
            e.FRAMEBUFFER_ATTACHMENT_TEXTURE_NUM_VIEWS_OVR = 0x9630; e.FRAMEBUFFER_ATTACHMENT_TEXTURE_BASE_VIEW_INDEX_OVR = 0x9632;
            e.MAX_VIEWS_OVR = 0x9631; e.FRAMEBUFFER_INCOMPLETE_VIEW_TARGETS_OVR = 0x9633;
            e.framebufferTextureMultiviewOVR = function () { ctx.calls++; }; break;
        default: break;
    }
    return e;
}

export class WebGL2RenderingContext {
    constructor(canvas, attrs) {
        this.canvas = canvas;
        this.drawingBufferColorSpace = 'srgb';
        this.unpackColorSpace = 'srgb';
        const a = attrs || {};
        this._attrs = {
            alpha: a.alpha !== undefined ? !!a.alpha : true,
            antialias: a.antialias !== undefined ? !!a.antialias : true,
            depth: a.depth !== undefined ? !!a.depth : true,
            desynchronized: !!a.desynchronized,
            failIfMajorPerformanceCaveat: !!a.failIfMajorPerformanceCaveat,
            powerPreference: a.powerPreference || 'default',
            premultipliedAlpha: a.premultipliedAlpha !== undefined ? !!a.premultipliedAlpha : true,
            preserveDrawingBuffer: !!a.preserveDrawingBuffer,
            stencil: a.stencil !== undefined ? !!a.stencil : false,
            xrCompatible: !!a.xrCompatible,
        };
        // counters: since construction; beginFrame() snapshots them
        this.calls = 0; this.draws = 0; this.uniforms = 0; this.uploads = 0;
        this._f0 = new Float64Array(4); this._last = new Float64Array(4);
        this._ids = 0;
        this._viewport = new Int32Array([0, 0, canvas.width, canvas.height]);
        this._scissor = new Int32Array([0, 0, canvas.width, canvas.height]);
        this._caps = Object.create(null);
        this._caps[GL.DITHER] = true;
        this._pixelStore = new Map([[GL.UNPACK_ALIGNMENT, 4], [GL.PACK_ALIGNMENT, 4], [GL.UNPACK_FLIP_Y_WEBGL, false],
            [GL.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false], [GL.UNPACK_COLORSPACE_CONVERSION_WEBGL, GL.BROWSER_DEFAULT_WEBGL]]);
        this._buffers = Object.create(null);
        this._program = null; this._vao = null; this._fb = null; this._readFb = null; this._rb = null;
        this._unit = 0;
        this._tex2d = new Array(32).fill(null); this._texCube = new Array(32).fill(null);
        this._ext = new Map();
        for (const name of EXTENSIONS) this._ext.set(name, makeExtension(name, this));
    }
    get drawingBufferWidth() { return this.canvas.width; }
    get drawingBufferHeight() { return this.canvas.height; }

    // ---- bench bookkeeping (not WebGL) ----
    beginFrame() {
        const f = this._f0, l = this._last;
        l[0] = this.calls - f[0]; l[1] = this.draws - f[1]; l[2] = this.uniforms - f[2]; l[3] = this.uploads - f[3];
        f[0] = this.calls; f[1] = this.draws; f[2] = this.uniforms; f[3] = this.uploads;
    }
    // counts of the frame since the last beginFrame()
    frameCounts() {
        const f = this._f0;
        return { calls: this.calls - f[0], draws: this.draws - f[1], uniforms: this.uniforms - f[2], uploads: this.uploads - f[3] };
    }
    totals() { return { calls: this.calls, draws: this.draws, uniforms: this.uniforms, uploads: this.uploads }; }

    // ---- context ----
    getContextAttributes() { this.calls++; return Object.assign({}, this._attrs); }
    isContextLost() { this.calls++; return false; }
    getError() { this.calls++; return 0; }
    getSupportedExtensions() { this.calls++; return EXTENSIONS.slice(); }
    getExtension(name) { this.calls++; const e = this._ext.get(name); return e === undefined ? null : e; }
    getShaderPrecisionFormat(shaderType, precisionType) {
        this.calls++;
        return precisionType >= GL.LOW_INT ? INT_PRECISION : FLOAT_PRECISION;
    }
    getParameter(p) {
        this.calls++;
        switch (p) {
            case GL.VIEWPORT: return this._viewport;
            case GL.SCISSOR_BOX: return this._scissor;
            case GL.CURRENT_PROGRAM: return this._program;
            case GL.ARRAY_BUFFER_BINDING: return this._buffers[GL.ARRAY_BUFFER] || null;
            case GL.ELEMENT_ARRAY_BUFFER_BINDING: return this._buffers[GL.ELEMENT_ARRAY_BUFFER] || null;
            case GL.UNIFORM_BUFFER_BINDING: return this._buffers[GL.UNIFORM_BUFFER] || null;
            case GL.FRAMEBUFFER_BINDING: return this._fb;
            case GL.READ_FRAMEBUFFER_BINDING: return this._readFb;
            case GL.RENDERBUFFER_BINDING: return this._rb;
            case GL.TEXTURE_BINDING_2D: return this._tex2d[this._unit];
            case GL.TEXTURE_BINDING_CUBE_MAP: return this._texCube[this._unit];
            case GL.VERTEX_ARRAY_BINDING: return this._vao;
            case GL.ACTIVE_TEXTURE: return GL.TEXTURE0 + this._unit;
            case GL.COLOR_CLEAR_VALUE: return new Float32Array(4);
            case GL.COLOR_WRITEMASK: return [true, true, true, true];
            case GL.DEPTH_WRITEMASK: return true;
            case GL.DEPTH_CLEAR_VALUE: return 1;
            case GL.DEPTH_FUNC: return GL.LESS;
            case GL.DEPTH_RANGE: return new Float32Array([0, 1]);
            case GL.FRONT_FACE: return GL.CCW;
            case GL.CULL_FACE_MODE: return GL.BACK;
            case GL.LINE_WIDTH: return 1;
            case GL.BLEND: case GL.CULL_FACE: case GL.DEPTH_TEST: case GL.SCISSOR_TEST: case GL.STENCIL_TEST:
            case GL.DITHER: case GL.POLYGON_OFFSET_FILL: case GL.SAMPLE_ALPHA_TO_COVERAGE: case GL.SAMPLE_COVERAGE:
            case GL.RASTERIZER_DISCARD:
                return this._caps[p] === true;
        }
        const ps = this._pixelStore.get(p);
        if (ps !== undefined) return ps;
        const v = STATIC.get(p);
        if (v !== undefined) return v;
        return 0;
    }
    getIndexedParameter() { this.calls++; return null; }
    getInternalformatParameter(target, ifmt, pname) { this.calls++; return pname === GL.SAMPLES ? new Int32Array([8, 4, 2]) : null; }
    enable(cap) { this.calls++; this._caps[cap] = true; }
    disable(cap) { this.calls++; this._caps[cap] = false; }
    isEnabled(cap) { this.calls++; return this._caps[cap] === true; }
    viewport(x, y, w, h) { this.calls++; const v = this._viewport; v[0] = x; v[1] = y; v[2] = w; v[3] = h; }
    scissor(x, y, w, h) { this.calls++; const v = this._scissor; v[0] = x; v[1] = y; v[2] = w; v[3] = h; }
    pixelStorei(p, v) { this.calls++; this._pixelStore.set(p, v); }

    // ---- objects ----
    createBuffer() { this.calls++; return new WebGLBuffer(++this._ids); }
    createTexture() { this.calls++; return new WebGLTexture(++this._ids); }
    createFramebuffer() { this.calls++; return new WebGLFramebuffer(++this._ids); }
    createRenderbuffer() { this.calls++; return new WebGLRenderbuffer(++this._ids); }
    createVertexArray() { this.calls++; return new WebGLVertexArrayObject(++this._ids); }
    createQuery() { this.calls++; return new WebGLQuery(++this._ids); }
    createSampler() { this.calls++; return new WebGLSampler(++this._ids); }
    createTransformFeedback() { this.calls++; return new WebGLTransformFeedback(++this._ids); }
    createShader(type) { this.calls++; return new WebGLShader(++this._ids, type); }
    createProgram() { this.calls++; return new WebGLProgram(++this._ids); }
    fenceSync() { this.calls++; return new WebGLSync(++this._ids); }
    bindBuffer(target, b) { this.calls++; this._buffers[target] = b; }
    bindBufferBase(target, index, b) { this.calls++; this._buffers[target] = b; }
    bindBufferRange(target, index, b) { this.calls++; this._buffers[target] = b; }
    bindTexture(target, t) {
        this.calls++;
        if (target === GL.TEXTURE_2D) this._tex2d[this._unit] = t;
        else if (target === GL.TEXTURE_CUBE_MAP) this._texCube[this._unit] = t;
    }
    activeTexture(unit) { this.calls++; this._unit = unit - GL.TEXTURE0; }
    bindFramebuffer(target, fb) {
        this.calls++;
        if (target === GL.FRAMEBUFFER) { this._fb = fb; this._readFb = fb; }
        else if (target === GL.DRAW_FRAMEBUFFER) this._fb = fb;
        else this._readFb = fb;
    }
    bindRenderbuffer(target, rb) { this.calls++; this._rb = rb; }
    bindVertexArray(vao) { this.calls++; this._vao = vao; }
    useProgram(p) { this.calls++; this._program = p; }
    bufferData(target, data, usage) {
        this.calls++; this.uploads++;
        const b = this._buffers[target];
        if (b) b._size = typeof data === 'number' ? data : (data ? data.byteLength : 0);
    }
    bufferSubData() { this.calls++; this.uploads++; }
    getBufferParameter(target, pname) {
        this.calls++;
        const b = this._buffers[target];
        if (pname === GL.BUFFER_SIZE) return b ? b._size : 0;
        if (pname === GL.BUFFER_USAGE) return GL.STATIC_DRAW;
        return 0;
    }
    texImage2D() { this.calls++; this.uploads++; }
    texSubImage2D() { this.calls++; this.uploads++; }
    texImage3D() { this.calls++; this.uploads++; }
    texSubImage3D() { this.calls++; this.uploads++; }
    compressedTexImage2D() { this.calls++; this.uploads++; }
    compressedTexSubImage2D() { this.calls++; this.uploads++; }
    compressedTexImage3D() { this.calls++; this.uploads++; }
    compressedTexSubImage3D() { this.calls++; this.uploads++; }
    checkFramebufferStatus() { this.calls++; return GL.FRAMEBUFFER_COMPLETE; }
    getFramebufferAttachmentParameter() { this.calls++; return 0; }
    getRenderbufferParameter() { this.calls++; return 0; }
    getTexParameter() { this.calls++; return 0; }
    getSamplerParameter() { this.calls++; return 0; }
    getVertexAttrib() { this.calls++; return null; }
    getVertexAttribOffset() { this.calls++; return 0; }
    getQuery() { this.calls++; return null; }
    getQueryParameter(q, pname) { this.calls++; return pname === GL.QUERY_RESULT_AVAILABLE ? true : 0; }
    getSyncParameter(s, pname) { this.calls++; return pname === GL.SYNC_STATUS ? GL.SIGNALED : 0; }
    clientWaitSync() { this.calls++; return GL.ALREADY_SIGNALED; }
    getBufferSubData() { this.calls++; }
    readPixels() { this.calls++; }

    // ---- shaders and programs ----
    shaderSource(s, src) { this.calls++; s._source = String(src); }
    getShaderSource(s) { this.calls++; return s._source; }
    compileShader() { this.calls++; }
    getShaderParameter(s, pname) {
        this.calls++;
        if (pname === GL.SHADER_TYPE) return s._type;
        if (pname === GL.DELETE_STATUS) return s._deleted;
        return true; // COMPILE_STATUS, COMPLETION_STATUS_KHR
    }
    getShaderInfoLog() { this.calls++; return ''; }
    getProgramInfoLog() { this.calls++; return ''; }
    attachShader(p, s) { this.calls++; if (s._type === GL.VERTEX_SHADER) p._vs = s; else p._fs = s; }
    detachShader(p, s) { this.calls++; }
    getAttachedShaders(p) { this.calls++; return [p._vs, p._fs].filter((s) => s); }
    bindAttribLocation(p, index, name) { this.calls++; p._bound.set(String(name), index); }
    linkProgram(p) {
        this.calls++;
        const r = reflectProgram(p._vs ? p._vs._source : '', p._fs ? p._fs._source : '', p._bound);
        p._uniforms = r.uniforms.map((u) => new WebGLActiveInfo(u.name, u.size, u.type));
        p._uniformBlock = r.uniforms.map((u) => u.block);
        p._attribs = r.attribs.map((a) => new WebGLActiveInfo(a.name, a.size, a.type));
        p._blocks = r.blocks;
        p._uniformLocs = new Map();
        r.uniforms.forEach((u, i) => {
            if (u.block >= 0) return;
            const loc = new WebGLUniformLocation(p, i);
            p._uniformLocs.set(u.name, loc);
            if (u.name.endsWith('[0]')) {
                const base = u.name.slice(0, -3);
                p._uniformLocs.set(base, loc);
                for (let k = 1; k < u.size; k++) p._uniformLocs.set(base + '[' + k + ']', new WebGLUniformLocation(p, i));
            }
        });
        p._attribLocs = new Map(r.attribs.map((a) => [a.name, a.location]));
        p._linked = true;
    }
    validateProgram() { this.calls++; }
    getProgramParameter(p, pname) {
        this.calls++;
        switch (pname) {
            case GL.ACTIVE_UNIFORMS: return p._uniforms.length;
            case GL.ACTIVE_ATTRIBUTES: return p._attribs.length;
            case GL.ACTIVE_UNIFORM_BLOCKS: return p._blocks.length;
            case GL.ATTACHED_SHADERS: return (p._vs ? 1 : 0) + (p._fs ? 1 : 0);
            case GL.DELETE_STATUS: return p._deleted;
            case GL.TRANSFORM_FEEDBACK_VARYINGS: return 0;
            case GL.TRANSFORM_FEEDBACK_BUFFER_MODE: return GL.INTERLEAVED_ATTRIBS;
        }
        return true; // LINK_STATUS, VALIDATE_STATUS, COMPLETION_STATUS_KHR
    }
    getActiveUniform(p, i) { this.calls++; return p._uniforms[i] || null; }
    getActiveAttrib(p, i) { this.calls++; return p._attribs[i] || null; }
    getUniformLocation(p, name) { this.calls++; const l = p._uniformLocs.get(name); return l === undefined ? null : l; }
    getAttribLocation(p, name) { this.calls++; const l = p._attribLocs.get(name); return l === undefined ? -1 : l; }
    getFragDataLocation() { this.calls++; return 0; }
    getUniform() { this.calls++; return 0; }
    getUniformBlockIndex(p, name) {
        this.calls++;
        for (let i = 0; i < p._blocks.length; i++) if (p._blocks[i].name === name) return i;
        return GL.INVALID_INDEX;
    }
    getActiveUniformBlockName(p, i) { this.calls++; return p._blocks[i] ? p._blocks[i].name : null; }
    getActiveUniformBlockParameter(p, i, pname) {
        this.calls++;
        const b = p._blocks[i];
        if (!b) return null;
        switch (pname) {
            case GL.UNIFORM_BLOCK_DATA_SIZE: return b.size;
            case GL.UNIFORM_BLOCK_ACTIVE_UNIFORMS: return b.members.length;
            case GL.UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES: return new Uint32Array(b.members);
            case GL.UNIFORM_BLOCK_BINDING: return 0;
        }
        return true; // REFERENCED_BY_*
    }
    getUniformIndices(p, names) {
        this.calls++;
        return names.map((n) => { const i = p._uniforms.findIndex((u) => u.name === n); return i < 0 ? GL.INVALID_INDEX : i; });
    }
    getActiveUniforms(p, indices, pname) {
        this.calls++;
        return Array.from(indices, (i) => {
            const u = p._uniforms[i];
            if (pname === GL.UNIFORM_TYPE) return u.type;
            if (pname === GL.UNIFORM_SIZE) return u.size;
            if (pname === GL.UNIFORM_BLOCK_INDEX) return p._uniformBlock[i];
            if (pname === GL.UNIFORM_IS_ROW_MAJOR) return false;
            return -1;
        });
    }
    uniformBlockBinding() { this.calls++; }
    transformFeedbackVaryings() { this.calls++; }
    getTransformFeedbackVarying() { this.calls++; return null; }

    // ---- draws ----
    drawArrays() { this.calls++; this.draws++; }
    drawElements() { this.calls++; this.draws++; }
    drawArraysInstanced() { this.calls++; this.draws++; }
    drawElementsInstanced() { this.calls++; this.draws++; }
    drawRangeElements() { this.calls++; this.draws++; }
    clear() { this.calls++; }
}

const P = WebGL2RenderingContext.prototype;

// uniform setters: count
for (const n of ['uniform1f', 'uniform2f', 'uniform3f', 'uniform4f', 'uniform1i', 'uniform2i', 'uniform3i', 'uniform4i',
    'uniform1ui', 'uniform2ui', 'uniform3ui', 'uniform4ui', 'uniform1fv', 'uniform2fv', 'uniform3fv', 'uniform4fv',
    'uniform1iv', 'uniform2iv', 'uniform3iv', 'uniform4iv', 'uniform1uiv', 'uniform2uiv', 'uniform3uiv', 'uniform4uiv',
    'uniformMatrix2fv', 'uniformMatrix3fv', 'uniformMatrix4fv', 'uniformMatrix2x3fv', 'uniformMatrix3x2fv',
    'uniformMatrix2x4fv', 'uniformMatrix4x2fv', 'uniformMatrix3x4fv', 'uniformMatrix4x3fv']) {
    P[n] = function () { this.calls++; this.uniforms++; };
}

// state setters and everything else without a result: count only
for (const n of ['blendColor', 'blendEquation', 'blendEquationSeparate', 'blendFunc', 'blendFuncSeparate', 'clearColor',
    'clearDepth', 'clearStencil', 'colorMask', 'cullFace', 'depthFunc', 'depthMask', 'depthRange', 'frontFace', 'hint',
    'lineWidth', 'polygonOffset', 'sampleCoverage', 'stencilFunc', 'stencilFuncSeparate', 'stencilMask',
    'stencilMaskSeparate', 'stencilOp', 'stencilOpSeparate', 'texParameterf', 'texParameteri', 'generateMipmap',
    'copyTexImage2D', 'copyTexSubImage2D', 'copyTexSubImage3D', 'texStorage2D', 'texStorage3D', 'framebufferTexture2D',
    'framebufferTextureLayer', 'framebufferRenderbuffer', 'renderbufferStorage', 'renderbufferStorageMultisample',
    'blitFramebuffer', 'invalidateFramebuffer', 'invalidateSubFramebuffer', 'readBuffer', 'drawBuffers',
    'clearBufferfv', 'clearBufferiv', 'clearBufferuiv', 'clearBufferfi', 'enableVertexAttribArray',
    'disableVertexAttribArray', 'vertexAttribPointer', 'vertexAttribIPointer', 'vertexAttribDivisor', 'vertexAttrib1f',
    'vertexAttrib2f', 'vertexAttrib3f', 'vertexAttrib4f', 'vertexAttrib1fv', 'vertexAttrib2fv', 'vertexAttrib3fv',
    'vertexAttrib4fv', 'vertexAttribI4i', 'vertexAttribI4ui', 'vertexAttribI4iv', 'vertexAttribI4uiv',
    'copyBufferSubData', 'samplerParameteri', 'samplerParameterf', 'bindSampler', 'bindTransformFeedback',
    'beginTransformFeedback', 'endTransformFeedback', 'pauseTransformFeedback', 'resumeTransformFeedback',
    'beginQuery', 'endQuery', 'waitSync', 'flush', 'finish', 'makeXRCompatible', 'drawingBufferStorage']) {
    P[n] = function () { this.calls++; };
}

// deletes: mark only
for (const n of ['deleteBuffer', 'deleteTexture', 'deleteFramebuffer', 'deleteRenderbuffer', 'deleteVertexArray',
    'deleteQuery', 'deleteSampler', 'deleteSync', 'deleteTransformFeedback', 'deleteShader', 'deleteProgram']) {
    P[n] = function (o) { this.calls++; if (o) o._deleted = true; };
}
const IS = { isBuffer: WebGLBuffer, isTexture: WebGLTexture, isFramebuffer: WebGLFramebuffer, isRenderbuffer: WebGLRenderbuffer,
    isVertexArray: WebGLVertexArrayObject, isQuery: WebGLQuery, isSampler: WebGLSampler, isSync: WebGLSync,
    isTransformFeedback: WebGLTransformFeedback, isShader: WebGLShader, isProgram: WebGLProgram };
for (const n of Object.keys(IS)) {
    const C = IS[n];
    P[n] = function (o) { this.calls++; return o instanceof C && !o._deleted; };
}

// The constants, on the prototype and the constructor as in a browser.
for (const k of Object.keys(GL)) { P[k] = GL[k]; WebGL2RenderingContext[k] = GL[k]; }

// A WebGL 1 constructor nothing is an instance of (pixi's version check:
// `gl instanceof DOMAdapter.get().getWebGLRenderingContext()`).
export class WebGLRenderingContext {}
