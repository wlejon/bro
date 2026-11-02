// The extensions three.js branches on: KHR_parallel_shader_compile (compiles
// are synchronous, so COMPLETION_STATUS_KHR is true at once),
// EXT_color_buffer_half_float (16-bit float targets render) and
// WEBGL_debug_renderer_info (the Vulkan device's vendor and name). Each is
// advertised, its object carries its constants, and its enums are enums
// only once it is enabled.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '4');
canvas.setAttribute('height', '4');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2', { antialias: false });
if (!gl) {
    missingGpuContext('webgl2');
} else {
    const exts = gl.getSupportedExtensions();
    for (const name of ['KHR_parallel_shader_compile', 'EXT_color_buffer_half_float', 'WEBGL_debug_renderer_info'])
        assert(exts.indexOf(name) >= 0, name + ' is supported: ' + exts);

    function program() {
        const p = gl.createProgram();
        const shaders = [];
        for (const [type, src] of [[gl.VERTEX_SHADER, '#version 300 es\nvoid main() { gl_Position = vec4(0.0); }'],
            [gl.FRAGMENT_SHADER, '#version 300 es\nprecision highp float; out vec4 c; void main() { c = vec4(1.0); }']]) {
            const s = gl.createShader(type);
            gl.shaderSource(s, src);
            gl.compileShader(s);
            gl.attachShader(p, s);
            shaders.push(s);
        }
        gl.linkProgram(p);
        return [p, shaders];
    }

    // --- KHR_parallel_shader_compile ---
    const COMPLETION_STATUS_KHR = 0x91B1;
    const [p0, s0] = program();
    assert(!gl.getProgramParameter(p0, COMPLETION_STATUS_KHR) && gl.getError() === gl.INVALID_ENUM,
           'COMPLETION_STATUS_KHR is not an enum before the extension is enabled');
    const par = gl.getExtension('KHR_parallel_shader_compile');
    assert(par && par.COMPLETION_STATUS_KHR === COMPLETION_STATUS_KHR && par.MAX_SHADER_COMPILER_THREADS_KHR === 0x91B0,
           'KHR_parallel_shader_compile carries its constants');
    assert(gl.getExtension('KHR_parallel_shader_compile') === par, 'getExtension answers the same object');
    const [p1, s1] = program();
    assert(gl.getProgramParameter(p1, par.COMPLETION_STATUS_KHR) === true, 'a linked program is complete');
    assert(gl.getShaderParameter(s1[0], par.COMPLETION_STATUS_KHR) === true, 'a compiled shader is complete');
    assert(gl.getShaderParameter(s0[1], par.COMPLETION_STATUS_KHR) === true, 'one compiled before enabling too');
    par.maxShaderCompilerThreadsKHR(2);
    assert(gl.getParameter(par.MAX_SHADER_COMPILER_THREADS_KHR) === 2, 'MAX_SHADER_COMPILER_THREADS_KHR reads back');
    assert(gl.getError() === gl.NO_ERROR, 'no error with KHR_parallel_shader_compile');

    // --- EXT_color_buffer_half_float ---
    const half = gl.getExtension('EXT_color_buffer_half_float');
    assert(half && half.RGBA16F_EXT === gl.RGBA16F && half.RGB16F_EXT === gl.RGB16F &&
           half.FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE_EXT === 0x8211 && half.UNSIGNED_NORMALIZED_EXT === 0x8C17,
           'EXT_color_buffer_half_float carries its constants');
    for (const [ifmt, fmt] of [[gl.RGBA16F, gl.RGBA], [gl.RG16F, gl.RG], [gl.R16F, gl.RED]]) {
        const t = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, t);
        gl.texImage2D(gl.TEXTURE_2D, 0, ifmt, 4, 4, 0, fmt, gl.HALF_FLOAT, null);
        const fb = gl.createFramebuffer();
        gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
        gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, t, 0);
        assert(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE, 'a 16-bit float target is complete');
        gl.clearColor(0.5, 0.25, 1, 1);
        gl.clear(gl.COLOR_BUFFER_BIT);
        const px = new Float32Array(4);
        gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.FLOAT, px);
        assert(px[0] === 0.5, 'it renders: ' + px);
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    }
    assert(gl.getError() === gl.NO_ERROR, 'no error with EXT_color_buffer_half_float');

    // --- WEBGL_debug_renderer_info ---
    const UNMASKED_VENDOR_WEBGL = 0x9245, UNMASKED_RENDERER_WEBGL = 0x9246;
    assert(gl.getParameter(UNMASKED_RENDERER_WEBGL) === null && gl.getError() === gl.INVALID_ENUM,
           'UNMASKED_RENDERER_WEBGL is not an enum before the extension is enabled');
    const info = gl.getExtension('WEBGL_debug_renderer_info');
    assert(info && info.UNMASKED_VENDOR_WEBGL === UNMASKED_VENDOR_WEBGL &&
           info.UNMASKED_RENDERER_WEBGL === UNMASKED_RENDERER_WEBGL, 'WEBGL_debug_renderer_info carries its constants');
    const vendor = gl.getParameter(info.UNMASKED_VENDOR_WEBGL);
    const renderer = gl.getParameter(info.UNMASKED_RENDERER_WEBGL);
    assert(typeof vendor === 'string' && vendor.length > 0, 'a vendor: ' + vendor);
    assert(typeof renderer === 'string' && renderer.length > 0 && renderer !== 'Bro Vulkan', 'the device name: ' + renderer);
    assert(gl.getError() === gl.NO_ERROR, 'no error with WEBGL_debug_renderer_info');
}
