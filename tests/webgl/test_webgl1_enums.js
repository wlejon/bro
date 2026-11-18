// A WebGL 1 context refuses WebGL 2's enums as Chromium does (INVALID_ENUM,
// INVALID_VALUE for a sized internal format), admitting the ones an enabled
// extension brings; and the rasterizer / sampler extensions bro offers on
// both versions (EXT_depth_clamp, EXT_polygon_offset_clamp,
// WEBGL_polygon_mode, EXT_texture_mirror_clamp_to_edge, WEBGL_debug_shaders),
// each extension object tagged with its interface.
const T = (v) => Object.prototype.toString.call(v).slice(8, -1);
const eq = (got, want, what) => assert(got === want, `${what}: got ${got}, want ${want}`);
const canvas = document.createElement('canvas');
canvas.width = canvas.height = 8;
const gl = canvas.getContext('webgl');
const err = () => { const s = []; for (let e = gl.getError(); e; e = gl.getError()) s.push(e); return s.join('/') || '0'; };
eq(`${gl.getParameter(0x85B5)} ${err()}`, `null ${gl.INVALID_ENUM}`, 'VERTEX_ARRAY_BINDING without the extension');
eq(`${gl.getParameter(0x8824)} ${err()}`, `null ${gl.INVALID_ENUM}`, 'MAX_DRAW_BUFFERS without the extension');
gl.enable(0x8C89); eq(err(), String(gl.INVALID_ENUM), 'RASTERIZER_DISCARD');
gl.bindBuffer(0x8A11, gl.createBuffer()); eq(err(), String(gl.INVALID_ENUM), 'UNIFORM_BUFFER');
gl.bindTexture(0x806F, gl.createTexture()); eq(err(), String(gl.INVALID_ENUM), 'TEXTURE_3D');
gl.bindFramebuffer(0x8CA8, null); eq(err(), String(gl.INVALID_ENUM), 'READ_FRAMEBUFFER');
gl.pixelStorei(0x0D02, 0); eq(err(), String(gl.INVALID_ENUM), 'PACK_ROW_LENGTH');
gl.blendEquation(0x8008); eq(err(), String(gl.INVALID_ENUM), 'MAX without EXT_blend_minmax');
gl.getExtension('EXT_blend_minmax');
gl.blendEquation(0x8008); eq(err(), '0', 'MAX with EXT_blend_minmax');
gl.blendEquation(gl.FUNC_ADD);
gl.bindTexture(gl.TEXTURE_2D, gl.createTexture());
gl.texImage2D(gl.TEXTURE_2D, 0, 0x8058, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
eq(err(), String(gl.INVALID_VALUE), 'a sized internal format');
gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.FLOAT, null);
eq(err(), String(gl.INVALID_ENUM), 'FLOAT without OES_texture_float');
gl.getExtension('OES_texture_float');
gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.FLOAT, null);
eq(err(), '0', 'FLOAT with OES_texture_float');
gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array(4));
eq(err(), '0', 'the plain upload');
const rb = gl.createRenderbuffer();
gl.bindRenderbuffer(gl.RENDERBUFFER, rb);
gl.renderbufferStorage(gl.RENDERBUFFER, 0x8058, 1, 1); eq(err(), String(gl.INVALID_ENUM), 'RGBA8 renderbuffer');
gl.renderbufferStorage(gl.RENDERBUFFER, gl.DEPTH_STENCIL, 1, 1); eq(err(), '0', 'DEPTH_STENCIL renderbuffer');
const vaoExt = gl.getExtension('OES_vertex_array_object');
eq(`${gl.getParameter(vaoExt.VERTEX_ARRAY_BINDING_OES)} ${err()} ${T(vaoExt.createVertexArrayOES())} ${T(vaoExt)}`,
   'null 0 WebGLVertexArrayObjectOES OESVertexArrayObject', 'OES_vertex_array_object');

for (const g of [gl, document.createElement('canvas').getContext('webgl2')]) {
    const v = g instanceof WebGL2RenderingContext ? 'webgl2' : 'webgl';
    const e2 = () => { const s = []; for (let e = g.getError(); e; e = g.getError()) s.push(e); return s.join('/') || '0'; };
    const have = g.getSupportedExtensions();
    if (have.includes('EXT_depth_clamp')) {
        g.enable(0x864F);
        const before = e2();
        const dc = g.getExtension('EXT_depth_clamp');
        g.enable(dc.DEPTH_CLAMP_EXT);
        eq(`${before} ${g.isEnabled(dc.DEPTH_CLAMP_EXT)} ${g.getParameter(dc.DEPTH_CLAMP_EXT)} ${e2()} ${T(dc)}`,
           `${g.INVALID_ENUM} true true 0 EXTDepthClamp`, v + ' EXT_depth_clamp');
        g.disable(dc.DEPTH_CLAMP_EXT);
    }
    if (have.includes('EXT_polygon_offset_clamp')) {
        const pc = g.getExtension('EXT_polygon_offset_clamp');
        pc.polygonOffsetClampEXT(1, 2, 0.5);
        eq(`${g.getParameter(pc.POLYGON_OFFSET_CLAMP_EXT)} ${g.getParameter(g.POLYGON_OFFSET_UNITS)} ${e2()} ${T(pc)}`,
           '0.5 2 0 EXTPolygonOffsetClamp', v + ' EXT_polygon_offset_clamp');
    }
    if (have.includes('WEBGL_polygon_mode')) {
        const pm = g.getExtension('WEBGL_polygon_mode');
        const a = g.getParameter(pm.POLYGON_MODE_WEBGL);
        pm.polygonModeWEBGL(g.FRONT_AND_BACK, pm.LINE_WEBGL);
        const b = g.getParameter(pm.POLYGON_MODE_WEBGL);
        pm.polygonModeWEBGL(g.FRONT, pm.FILL_WEBGL);
        eq(`${a} ${b} ${e2()} ${T(pm)}`, `${pm.FILL_WEBGL} ${pm.LINE_WEBGL} ${g.INVALID_ENUM} WebGLPolygonMode`, v + ' WEBGL_polygon_mode');
        pm.polygonModeWEBGL(g.FRONT_AND_BACK, pm.FILL_WEBGL);
    }
    if (have.includes('EXT_texture_mirror_clamp_to_edge')) {
        const mc = g.getExtension('EXT_texture_mirror_clamp_to_edge');
        g.bindTexture(g.TEXTURE_2D, g.createTexture());
        g.texParameteri(g.TEXTURE_2D, g.TEXTURE_WRAP_S, mc.MIRROR_CLAMP_TO_EDGE_EXT);
        eq(`${g.getTexParameter(g.TEXTURE_2D, g.TEXTURE_WRAP_S)} ${e2()} ${T(mc)}`, '34627 0 EXTTextureMirrorClampToEdge',
           v + ' EXT_texture_mirror_clamp_to_edge');
    }
    const ds = g.getExtension('WEBGL_debug_shaders');
    const s = g.createShader(g.VERTEX_SHADER);
    const blank = ds.getTranslatedShaderSource(s);
    g.shaderSource(s, 'void main() { gl_Position = vec4(0.0); }');
    g.compileShader(s);
    eq(`${JSON.stringify(blank)} ${ds.getTranslatedShaderSource(s).length > 0} ${T(ds)}`, '"" true WebGLDebugShaders', v + ' WEBGL_debug_shaders');
}
console.log('test_webgl1_enums: OK');
