// The WebGL DOM surface as a browser has it: every object a context hands out
// is an instance of its interface (WebGLBuffer ... WebGLUniformLocation), each
// interface a global that cannot be constructed; the context's methods,
// accessors and constants live on WebGL2RenderingContext.prototype, not on
// the context (whose one own property is canvas), and calling one on anything but a context is a TypeError; and
// getContext lives on HTMLCanvasElement.prototype.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '4');
canvas.setAttribute('height', '4');
document.body.appendChild(canvas);
flush();

// --- HTMLCanvasElement.prototype ---
assert(canvas instanceof HTMLCanvasElement, 'a canvas is an HTMLCanvasElement');
for (const name of ['getContext', 'toDataURL', 'toBlob', 'width', 'height']) {
    assert(!Object.prototype.hasOwnProperty.call(canvas, name), name + ' is not the canvas\'s own');
    assert(name in HTMLCanvasElement.prototype, name + ' is on HTMLCanvasElement.prototype');
}
assert(typeof HTMLCanvasElement.prototype.getContext === 'function', 'HTMLCanvasElement.prototype.getContext');
assert(canvas.getContext === document.createElement('canvas').getContext, 'one getContext for every canvas');
assert(canvas.width === 4 && canvas.height === 4, 'width/height read the element: ' + canvas.width + 'x' + canvas.height);

const gl = canvas.getContext('webgl2');
if (!gl) {
    missingGpuContext('webgl2');
} else {
    // --- WebGL2RenderingContext.prototype ---
    const proto = WebGL2RenderingContext.prototype;
    assert(gl instanceof WebGL2RenderingContext, 'gl instanceof WebGL2RenderingContext');
    assert(Object.getPrototypeOf(gl) === proto, 'born on WebGL2RenderingContext.prototype');
    // Its one own property is canvas, as CanvasRenderingContext2D's is.
    const own = Object.getOwnPropertyNames(gl);
    assert(own.length === 1 && own[0] === 'canvas', 'the context\'s own properties: ' + own);
    for (const name of ['getParameter', 'drawArrays', 'uniformMatrix4fv', 'getExtension', 'texImage2D', 'createBuffer'])
        assert(typeof proto[name] === 'function', name + ' is on the prototype');
    for (const name of ['drawingBufferWidth', 'drawingBufferHeight']) {
        const d = Object.getOwnPropertyDescriptor(proto, name);
        assert(d && typeof d.get === 'function', name + ' is a prototype accessor');
    }
    assert(proto.COLOR_BUFFER_BIT === 0x4000 && WebGL2RenderingContext.COLOR_BUFFER_BIT === 0x4000 &&
           gl.COLOR_BUFFER_BIT === 0x4000, 'constants on the interface and its prototype');
    assert(gl.canvas === canvas, 'gl.canvas is the canvas');
    assert(gl.drawingBufferWidth === 4 && gl.drawingBufferHeight === 4, 'the drawing buffer size');
    assert(canvas.getContext('webgl2') === gl, 'getContext answers the same context');

    // A method called on something else is a TypeError.
    function throwsTypeError(fn, what) {
        let error = null;
        try { fn(); } catch (e) { error = e; }
        assert(error instanceof TypeError, what + ' throws a TypeError: ' + error);
    }
    throwsTypeError(() => proto.getParameter.call({}, gl.VIEWPORT), 'getParameter on a plain object');
    throwsTypeError(() => proto.clear.call(canvas, gl.COLOR_BUFFER_BIT), 'clear on a canvas');
    throwsTypeError(() => Object.getOwnPropertyDescriptor(proto, 'drawingBufferWidth').get.call({}),
                    'drawingBufferWidth of a plain object');
    throwsTypeError(() => new WebGL2RenderingContext(), 'new WebGL2RenderingContext()');
    // Called through the prototype on the context, it works.
    assert(proto.getParameter.call(gl, gl.MAX_TEXTURE_SIZE) > 0, 'a prototype method on the context');

    // --- The object interfaces ---
    const vs = gl.createShader(gl.VERTEX_SHADER);
    gl.shaderSource(vs, '#version 300 es\nuniform mat4 m; void main() { gl_Position = m * vec4(0.0); }');
    gl.compileShader(vs);
    const fs = gl.createShader(gl.FRAGMENT_SHADER);
    gl.shaderSource(fs, '#version 300 es\nprecision highp float; out vec4 c; void main() { c = vec4(1.0); }');
    gl.compileShader(fs);
    const program = gl.createProgram();
    gl.attachShader(program, vs);
    gl.attachShader(program, fs);
    gl.linkProgram(program);
    const objects = {
        WebGLBuffer: gl.createBuffer(),
        WebGLTexture: gl.createTexture(),
        WebGLProgram: program,
        WebGLShader: vs,
        WebGLFramebuffer: gl.createFramebuffer(),
        WebGLRenderbuffer: gl.createRenderbuffer(),
        WebGLVertexArrayObject: gl.createVertexArray(),
        WebGLUniformLocation: gl.getUniformLocation(program, 'm'),
        WebGLSampler: gl.createSampler(),
        WebGLSync: gl.fenceSync(gl.SYNC_GPU_COMMANDS_COMPLETE, 0),
        WebGLTransformFeedback: gl.createTransformFeedback(),
        WebGLQuery: gl.createQuery(),
    };
    const names = Object.keys(objects);
    for (const name of names) {
        const ctor = globalThis[name];
        assert(typeof ctor === 'function' && ctor.name === name, name + ' is a global interface');
        assert(objects[name] instanceof ctor, 'the ' + name + ' is an instance of it');
        assert(Object.getPrototypeOf(objects[name]) === ctor.prototype, 'the ' + name + ' is born on its prototype');
        for (const other of names)
            if (other !== name) assert(!(objects[name] instanceof globalThis[other]), name + ' is not a ' + other);
        throwsTypeError(() => new ctor(), 'new ' + name + '()');
    }
    // A wrong-kind object is still refused, not misread.
    gl.bindTexture(gl.TEXTURE_2D, objects.WebGLBuffer);
    assert(gl.getParameter(gl.TEXTURE_BINDING_2D) === null, 'a buffer is not bound as a texture');
    gl.bindBuffer(gl.ARRAY_BUFFER, objects.WebGLBuffer);
    assert(gl.getParameter(gl.ARRAY_BUFFER_BINDING) === objects.WebGLBuffer, 'the buffer binds and reads back the same object');
    assert(gl.getError() === gl.NO_ERROR, 'no error');
}
