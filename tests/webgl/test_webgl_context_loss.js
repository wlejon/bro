// WEBGL_lose_context: losing the context makes every call a no-op and
// getError report CONTEXT_LOST_WEBGL once; webglcontextlost fires at the
// canvas a task later, and only a cancelled one lets restoreContext bring
// the context back — fresh state, no old objects, drawing again.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '8');
canvas.setAttribute('height', '8');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2', { antialias: false });
if (!gl) {
    missingGpuContext('webgl2');
} else {
    function px() {
        const b = new Uint8Array(4);
        gl.readPixels(4, 4, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, b);
        return Array.from(b);
    }
    const ext = gl.getExtension('WEBGL_lose_context');
    assert(ext && typeof ext.loseContext === 'function', 'WEBGL_lose_context is supported');
    assert(gl.getExtension('WEBGL_lose_context') === ext, 'getExtension answers the same object');
    assert(gl.getSupportedExtensions().includes('WEBGL_lose_context'), 'listed as supported');

    let lost = 0, restored = 0, cancel = true;
    canvas.addEventListener('webglcontextlost', (e) => {
        ++lost;
        if (cancel) e.preventDefault();
    });
    canvas.addEventListener('webglcontextrestored', () => { ++restored; });

    const buf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, buf);
    const shader = gl.createShader(gl.VERTEX_SHADER);
    const prog = gl.createProgram();
    const sync = gl.fenceSync(gl.SYNC_GPU_COMMANDS_COMPLETE, 0);
    const query = gl.createQuery();
    gl.clearColor(1, 0, 0, 1);
    gl.viewport(0, 0, 4, 4);
    gl.clear(gl.COLOR_BUFFER_BIT);
    assert(px().join() === '255,0,0,255', 'drawn before the loss: ' + px());

    // --- Loss ---
    ext.loseContext();
    assert(gl.isContextLost() === true, 'isContextLost');
    assert(gl.getError() === gl.CONTEXT_LOST_WEBGL, 'getError reports the loss');
    assert(gl.getError() === gl.NO_ERROR, 'and only once');
    assert(lost === 0, 'the lost event is not fired synchronously');
    assert(gl.getParameter(gl.VIEWPORT) === null, 'getParameter answers null while lost');
    assert(gl.getContextAttributes() === null, 'getContextAttributes answers null while lost');
    assert(gl.getSupportedExtensions() === null, 'getSupportedExtensions answers null while lost');
    assert(gl.isBuffer(buf) === false, 'objects are gone');
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    assert(gl.createBuffer() === null, 'creating while lost answers null');
    // Each call answers the spec's lost value.
    assert(gl.getShaderParameter(shader, gl.COMPILE_STATUS) === null, 'getShaderParameter: null');
    assert(gl.getShaderInfoLog(shader) === null, 'getShaderInfoLog: null');
    assert(gl.getShaderSource(shader) === null, 'getShaderSource: null');
    assert(gl.getProgramParameter(prog, gl.LINK_STATUS) === null, 'getProgramParameter: null');
    assert(gl.getProgramInfoLog(prog) === null, 'getProgramInfoLog: null');
    assert(gl.getAttachedShaders(prog) === null, 'getAttachedShaders: null');
    assert(gl.getUniformLocation(prog, 'u') === null, 'getUniformLocation: null');
    assert(gl.getAttribLocation(prog, 'a') === -1, 'getAttribLocation: -1');
    assert(gl.getFragDataLocation(prog, 'o') === -1, 'getFragDataLocation: -1');
    assert(gl.isShader(shader) === false && gl.isProgram(prog) === false, 'is*: false');
    assert(gl.isSync(sync) === false && gl.isQuery(query) === false, 'isSync / isQuery: false');
    assert(gl.isEnabled(gl.BLEND) === false, 'isEnabled: false');
    assert(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_UNSUPPORTED,
           'checkFramebufferStatus: FRAMEBUFFER_UNSUPPORTED');
    assert(gl.clientWaitSync(sync, 0, 0) === gl.WAIT_FAILED, 'clientWaitSync: WAIT_FAILED');
    assert(gl.getSyncParameter(sync, gl.SYNC_STATUS) === null, 'getSyncParameter: null');
    assert(gl.getQueryParameter(query, gl.QUERY_RESULT_AVAILABLE) === null, 'getQueryParameter: null');
    assert(gl.getShaderPrecisionFormat(gl.FRAGMENT_SHADER, gl.HIGH_FLOAT) === null,
           'getShaderPrecisionFormat: null');
    assert(gl.getExtension('EXT_color_buffer_float') === null, 'getExtension: null');
    assert(gl.createShader(gl.FRAGMENT_SHADER) === null && gl.createProgram() === null &&
           gl.createTexture() === null && gl.fenceSync(gl.SYNC_GPU_COMMANDS_COMPLETE, 0) === null,
           'create*: null');
    assert(gl.getError() === gl.NO_ERROR, 'calls while lost are silent no-ops');
    flush();
    assert(lost === 1, 'webglcontextlost fired at the canvas');
    assert(restored === 0, 'nothing restores on its own');

    // --- Restore ---
    ext.restoreContext();
    assert(gl.isContextLost() === true, 'restoring takes a task');
    flush();
    assert(restored === 1, 'webglcontextrestored fired');
    assert(gl.isContextLost() === false, 'the context is back');
    assert(gl.getError() === gl.NO_ERROR, 'no error after the restore');
    assert(gl.isBuffer(buf) === false, 'an object from before the loss stays gone');
    assert(gl.getParameter(gl.ARRAY_BUFFER_BINDING) === null, 'bindings are reset');
    const cc = gl.getParameter(gl.COLOR_CLEAR_VALUE);
    assert(cc instanceof Float32Array && cc.every((v) => v === 0), 'clear colour is reset: ' + cc);
    const vp = gl.getParameter(gl.VIEWPORT);
    assert(vp[2] === 8 && vp[3] === 8, 'the viewport is the canvas again: ' + vp);
    const buf2 = gl.createBuffer();
    assert(buf2 && buf2 !== buf, 'a new object is a new wrapper');
    gl.bindBuffer(gl.ARRAY_BUFFER, buf2);
    gl.bindBuffer(gl.ARRAY_BUFFER, buf);
    assert(gl.getError() === gl.INVALID_OPERATION, 'binding an object from before the loss');
    assert(gl.getParameter(gl.ARRAY_BUFFER_BINDING) === buf2, 'and it is not bound');
    gl.clearColor(0, 1, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    assert(px().join() === '0,255,0,255', 'drawing after the restore: ' + px());
    assert(gl.getExtension('WEBGL_lose_context') === ext, 'the extension object survives');

    // --- A loss the page does not cancel cannot be restored ---
    cancel = false;
    ext.loseContext();
    flush();
    assert(lost === 2, 'the second lost event');
    ext.restoreContext();
    flush();
    assert(restored === 1 && gl.isContextLost() === true, 'an uncancelled loss stays lost');
}
