// What the is* predicates, getAttachedShaders/getShaderSource and the
// invalidate calls answer, and that resizing the drawing buffer keeps the
// viewport the page set (WebGL changes it only at context creation).

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '16');
canvas.setAttribute('height', '16');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2');
if (!gl) {
    missingGpuContext('webgl2');
} else {
    const noError = (what) => {
        const e = gl.getError();
        assert(e === gl.NO_ERROR, what + ': error 0x' + e.toString(16));
    };

    // --- is*: a buffer, framebuffer, renderbuffer or vertex array is one
    // once it has been bound; a texture, sampler, shader or program from
    // its creation. Deleting ends it.
    const buf = gl.createBuffer();
    assert(gl.isBuffer(buf) === false, 'a created buffer is not one until bound');
    gl.bindBuffer(gl.ARRAY_BUFFER, buf);
    assert(gl.isBuffer(buf) === true, 'a bound buffer is one');
    gl.deleteBuffer(buf);
    assert(gl.isBuffer(buf) === false, 'a deleted buffer is not');
    assert(gl.getParameter(gl.ARRAY_BUFFER_BINDING) === null, 'deleting unbinds it');

    const fb = gl.createFramebuffer();
    assert(gl.isFramebuffer(fb) === false, 'framebuffer: not until bound');
    gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
    assert(gl.isFramebuffer(fb) === true, 'framebuffer: bound');
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);

    const rb = gl.createRenderbuffer();
    assert(gl.isRenderbuffer(rb) === false, 'renderbuffer: not until bound');
    gl.bindRenderbuffer(gl.RENDERBUFFER, rb);
    assert(gl.isRenderbuffer(rb) === true, 'renderbuffer: bound');

    const vao = gl.createVertexArray();
    assert(gl.isVertexArray(vao) === false, 'vertex array: not until bound');
    gl.bindVertexArray(vao);
    assert(gl.isVertexArray(vao) === true, 'vertex array: bound');
    gl.bindVertexArray(null);

    const deleted = gl.createBuffer();
    gl.deleteBuffer(deleted);
    gl.bindBuffer(gl.ARRAY_BUFFER, deleted);
    assert(gl.getError() === gl.INVALID_OPERATION, 'binding a deleted buffer');

    // --- Shaders and programs ---
    const vsSrc = '#version 300 es\nvoid main() { gl_Position = vec4(0.0); }';
    const fsSrc = '#version 300 es\nprecision mediump float;\nout vec4 o;\nvoid main() { o = vec4(1.0); }';
    const vs = gl.createShader(gl.VERTEX_SHADER);
    gl.shaderSource(vs, vsSrc);
    gl.compileShader(vs);
    const fs = gl.createShader(gl.FRAGMENT_SHADER);
    gl.shaderSource(fs, fsSrc);
    gl.compileShader(fs);
    assert(gl.getShaderSource(vs) === vsSrc, 'getShaderSource answers the source');
    assert(gl.getShaderParameter(vs, gl.SHADER_TYPE) === gl.VERTEX_SHADER, 'SHADER_TYPE');
    assert(gl.getShaderParameter(vs, gl.DELETE_STATUS) === false, 'DELETE_STATUS before delete');
    assert(gl.isShader(vs) === true, 'a created shader is one');

    const prog = gl.createProgram();
    assert(gl.isProgram(prog) === true, 'a created program is one');
    gl.attachShader(prog, vs);
    gl.attachShader(prog, fs);
    const attached = gl.getAttachedShaders(prog);
    assert(Array.isArray(attached) && attached.length === 2, 'two attached shaders');
    assert(attached.includes(vs) && attached.includes(fs), 'the very shader objects');
    gl.linkProgram(prog);
    assert(gl.getProgramParameter(prog, gl.LINK_STATUS) === true, 'linked');
    assert(gl.getProgramParameter(prog, gl.ATTACHED_SHADERS) === 2, 'ATTACHED_SHADERS');
    gl.useProgram(prog);
    gl.deleteShader(vs);
    assert(gl.isShader(vs) === false, 'a shader flagged for deletion is no longer one');
    gl.deleteProgram(prog);
    assert(gl.isProgram(prog) === false, 'a program flagged for deletion is no longer one');
    assert(gl.getParameter(gl.CURRENT_PROGRAM) === null, 'CURRENT_PROGRAM of a deleted program is null');
    gl.useProgram(null);
    noError('shader and program lifetime');

    // --- invalidateFramebuffer / invalidateSubFramebuffer ---
    gl.invalidateFramebuffer(gl.FRAMEBUFFER, [gl.COLOR, gl.DEPTH, gl.STENCIL]);
    noError('invalidate the canvas buffers');
    gl.invalidateFramebuffer(gl.FRAMEBUFFER, [gl.COLOR_ATTACHMENT0]);
    assert(gl.getError() === gl.INVALID_ENUM, 'the canvas has no COLOR_ATTACHMENT0');
    gl.invalidateFramebuffer(gl.TEXTURE_2D, [gl.COLOR]);
    assert(gl.getError() === gl.INVALID_ENUM, 'invalidate: bad target');
    gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
    gl.invalidateFramebuffer(gl.DRAW_FRAMEBUFFER,
                             [gl.COLOR_ATTACHMENT0, gl.DEPTH_ATTACHMENT, gl.DEPTH_STENCIL_ATTACHMENT]);
    noError('invalidate framebuffer attachments');
    gl.invalidateFramebuffer(gl.FRAMEBUFFER, [gl.COLOR]);
    assert(gl.getError() === gl.INVALID_ENUM, 'a framebuffer object has no COLOR');
    gl.invalidateFramebuffer(gl.FRAMEBUFFER, [gl.COLOR_ATTACHMENT0 + 15]);
    assert(gl.getError() === gl.INVALID_OPERATION, 'a color attachment past MAX_COLOR_ATTACHMENTS');
    gl.invalidateSubFramebuffer(gl.FRAMEBUFFER, [gl.COLOR_ATTACHMENT0], 0, 0, -1, 4);
    assert(gl.getError() === gl.INVALID_VALUE, 'invalidateSubFramebuffer: negative size');
    gl.invalidateSubFramebuffer(gl.FRAMEBUFFER, [gl.COLOR_ATTACHMENT0], 0, 0, 4, 4);
    noError('invalidateSubFramebuffer');
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);

    // --- The viewport survives a resize of the drawing buffer ---
    gl.viewport(1, 2, 3, 4);
    canvas.setAttribute('width', '32');
    canvas.setAttribute('height', '24');
    flush();
    assert(gl.drawingBufferWidth === 32 && gl.drawingBufferHeight === 24,
           'resized: ' + gl.drawingBufferWidth + 'x' + gl.drawingBufferHeight);
    const vp = Array.from(gl.getParameter(gl.VIEWPORT));
    assert(vp.join() === '1,2,3,4', 'the page\'s viewport is kept across the resize: ' + vp);
    noError('resize');
}
