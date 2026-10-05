// WebGL2 state getters read back what was set: getBufferParameter,
// getVertexAttrib / getVertexAttribOffset, getUniform (each type answered as
// the IDL types it), validateProgram, and the object getters answering the
// very wrapper objects the create* calls returned.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '16');
canvas.setAttribute('height', '16');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2', { antialias: false });
if (!gl) {
    missingGpuContext('webgl2');
} else {
    function program(vs, fs) {
        const p = gl.createProgram();
        for (const [type, src] of [[gl.VERTEX_SHADER, vs], [gl.FRAGMENT_SHADER, fs]]) {
            const s = gl.createShader(type);
            gl.shaderSource(s, src);
            gl.compileShader(s);
            gl.attachShader(p, s);
        }
        gl.bindAttribLocation(p, 0, 'p');
        gl.linkProgram(p);
        if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p));
        return p;
    }
    function expectError(want, msg) {
        const got = gl.getError();
        assert(got === want, msg + ': error 0x' + got.toString(16) + ' want 0x' + want.toString(16));
    }
    function eqArr(got, want, type, msg) {
        assert(got instanceof type, msg + ': not a ' + type.name + ' (' + got + ')');
        assert(got.length === want.length && want.every((v, i) => got[i] === v),
               msg + ': got [' + Array.from(got) + '] want [' + want + ']');
    }

    // --- Object identity ---
    const buf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, buf);
    assert(gl.getParameter(gl.ARRAY_BUFFER_BINDING) === buf, 'ARRAY_BUFFER_BINDING is the created buffer');
    const tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 4, 4, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
    assert(gl.getParameter(gl.TEXTURE_BINDING_2D) === tex, 'TEXTURE_BINDING_2D is the created texture');
    const fbo = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, tex, 0);
    assert(gl.getParameter(gl.FRAMEBUFFER_BINDING) === fbo, 'FRAMEBUFFER_BINDING is the created framebuffer');
    assert(gl.getFramebufferAttachmentParameter(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0,
                                                gl.FRAMEBUFFER_ATTACHMENT_OBJECT_NAME) === tex,
           'the attachment object is the created texture');
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    const vao = gl.createVertexArray();
    gl.bindVertexArray(vao);
    assert(gl.getParameter(gl.VERTEX_ARRAY_BINDING) === vao, 'VERTEX_ARRAY_BINDING is the created vertex array');
    gl.bindVertexArray(null);
    const smp = gl.createSampler();
    gl.bindSampler(3, smp);
    assert(gl.getParameter(gl.SAMPLER_BINDING) === null, 'SAMPLER_BINDING of unit 0 is null');
    gl.activeTexture(gl.TEXTURE3);
    assert(gl.getParameter(gl.SAMPLER_BINDING) === smp, 'SAMPLER_BINDING of unit 3 is the created sampler');
    gl.activeTexture(gl.TEXTURE0);
    expectError(gl.NO_ERROR, 'identity');

    // --- getBufferParameter ---
    gl.bufferData(gl.ARRAY_BUFFER, 48, gl.DYNAMIC_COPY);
    assert(gl.getBufferParameter(gl.ARRAY_BUFFER, gl.BUFFER_SIZE) === 48, 'BUFFER_SIZE');
    assert(gl.getBufferParameter(gl.ARRAY_BUFFER, gl.BUFFER_USAGE) === gl.DYNAMIC_COPY, 'BUFFER_USAGE');
    gl.bufferData(gl.ARRAY_BUFFER, 48, 0x88E3);
    expectError(gl.INVALID_ENUM, 'bufferData with an invalid usage');
    assert(gl.getBufferParameter(gl.ARRAY_BUFFER, gl.BUFFER_USAGE) === gl.DYNAMIC_COPY, 'the failed call kept the usage');
    assert(gl.getBufferParameter(gl.COPY_READ_BUFFER, gl.BUFFER_SIZE) === null, 'nothing bound');
    expectError(gl.INVALID_OPERATION, 'getBufferParameter with nothing bound');
    assert(gl.getBufferParameter(gl.ARRAY_BUFFER, gl.BUFFER_MAPPED) === null, 'an unknown pname');
    expectError(gl.INVALID_ENUM, 'getBufferParameter of an unknown pname');

    // --- getVertexAttrib ---
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 3, -1, -1, 3]), gl.STATIC_DRAW);
    gl.vertexAttribPointer(2, 3, gl.SHORT, true, 12, 4);
    gl.enableVertexAttribArray(2);
    gl.vertexAttribDivisor(2, 5);
    gl.vertexAttribIPointer(4, 2, gl.UNSIGNED_BYTE, 0, 8);
    assert(gl.getVertexAttrib(2, gl.VERTEX_ATTRIB_ARRAY_BUFFER_BINDING) === buf, 'BUFFER_BINDING is the buffer');
    assert(gl.getVertexAttrib(2, gl.VERTEX_ATTRIB_ARRAY_ENABLED) === true, 'ENABLED');
    assert(gl.getVertexAttrib(4, gl.VERTEX_ATTRIB_ARRAY_ENABLED) === false, 'ENABLED (not)');
    assert(gl.getVertexAttrib(2, gl.VERTEX_ATTRIB_ARRAY_SIZE) === 3, 'SIZE');
    assert(gl.getVertexAttrib(2, gl.VERTEX_ATTRIB_ARRAY_STRIDE) === 12, 'STRIDE');
    assert(gl.getVertexAttrib(4, gl.VERTEX_ATTRIB_ARRAY_STRIDE) === 0, 'STRIDE as given, not the effective one');
    assert(gl.getVertexAttrib(2, gl.VERTEX_ATTRIB_ARRAY_TYPE) === gl.SHORT, 'TYPE');
    assert(gl.getVertexAttrib(2, gl.VERTEX_ATTRIB_ARRAY_NORMALIZED) === true, 'NORMALIZED');
    assert(gl.getVertexAttrib(2, gl.VERTEX_ATTRIB_ARRAY_INTEGER) === false, 'INTEGER (float pointer)');
    assert(gl.getVertexAttrib(4, gl.VERTEX_ATTRIB_ARRAY_INTEGER) === true, 'INTEGER (IPointer)');
    assert(gl.getVertexAttrib(2, gl.VERTEX_ATTRIB_ARRAY_DIVISOR) === 5, 'DIVISOR');
    assert(gl.getVertexAttribOffset(2, gl.VERTEX_ATTRIB_ARRAY_POINTER) === 4, 'getVertexAttribOffset');
    assert(gl.getVertexAttribOffset(4, gl.VERTEX_ATTRIB_ARRAY_POINTER) === 8, 'getVertexAttribOffset (IPointer)');
    assert(gl.getVertexAttrib(5, gl.VERTEX_ATTRIB_ARRAY_BUFFER_BINDING) === null, 'an attribute with no buffer');
    expectError(gl.NO_ERROR, 'getVertexAttrib');
    eqArr(gl.getVertexAttrib(6, gl.CURRENT_VERTEX_ATTRIB), [0, 0, 0, 1], Float32Array, 'initial generic value');
    gl.vertexAttrib3f(6, 0.5, 2, -3);
    eqArr(gl.getVertexAttrib(6, gl.CURRENT_VERTEX_ATTRIB), [0.5, 2, -3, 1], Float32Array, 'vertexAttrib3f');
    gl.vertexAttribI4i(6, -1, 2, -3, 4);
    eqArr(gl.getVertexAttrib(6, gl.CURRENT_VERTEX_ATTRIB), [-1, 2, -3, 4], Int32Array, 'vertexAttribI4i');
    gl.vertexAttribI4ui(6, 1, 0xFFFFFFFF, 3, 4);
    eqArr(gl.getVertexAttrib(6, gl.CURRENT_VERTEX_ATTRIB), [1, 0xFFFFFFFF, 3, 4], Uint32Array, 'vertexAttribI4ui');
    // Attribute state is the vertex array's; generic values are the context's.
    gl.bindVertexArray(vao);
    assert(gl.getVertexAttrib(2, gl.VERTEX_ATTRIB_ARRAY_ENABLED) === false, 'a fresh vertex array');
    assert(gl.getVertexAttrib(2, gl.VERTEX_ATTRIB_ARRAY_BUFFER_BINDING) === null, 'a fresh vertex array has no buffer');
    eqArr(gl.getVertexAttrib(6, gl.CURRENT_VERTEX_ATTRIB), [1, 0xFFFFFFFF, 3, 4], Uint32Array, 'generic values are context state');
    gl.bindVertexArray(null);
    assert(gl.getVertexAttrib(16, gl.VERTEX_ATTRIB_ARRAY_SIZE) === null, 'index 16');
    expectError(gl.INVALID_VALUE, 'getVertexAttrib of index 16');
    assert(gl.getVertexAttrib(2, gl.BUFFER_SIZE) === null, 'an unknown pname');
    expectError(gl.INVALID_ENUM, 'getVertexAttrib of an unknown pname');
    gl.getParameter(0x1234);
    expectError(gl.INVALID_ENUM, 'getParameter of an unknown pname');
    gl.getVertexAttribOffset(2, gl.VERTEX_ATTRIB_ARRAY_SIZE);
    expectError(gl.INVALID_ENUM, 'getVertexAttribOffset of a pname other than POINTER');

    // --- getUniform ---
    const vs = `#version 300 es
        in vec2 p;
        void main() { gl_Position = vec4(p, 0.0, 1.0); }`;
    const prog = program(vs, `#version 300 es
        precision highp float;
        uniform float f;
        uniform vec3 v3;
        uniform ivec2 i2;
        uniform uvec4 u4;
        uniform bool b;
        uniform bvec2 b2;
        uniform mat2x3 m;
        uniform float arr[3];
        uniform sampler2D tex;
        out vec4 o;
        void main() {
            float s = f + v3.x + float(i2.y) + float(u4.w) + (b ? 1.0 : 0.0) + (b2.y ? 1.0 : 0.0);
            s += m[1][2] + arr[2] + texture(tex, vec2(0.5)).r;
            o = vec4(s);
        }`);
    gl.useProgram(prog);
    const loc = (n) => gl.getUniformLocation(prog, n);
    assert(gl.getUniform(prog, loc('f')) === 0, 'an unset float is 0');
    eqArr(gl.getUniform(prog, loc('v3')), [0, 0, 0], Float32Array, 'an unset vec3');
    gl.uniform1f(loc('f'), 1.5);
    gl.uniform3f(loc('v3'), 1, -2, 0.25);
    gl.uniform2i(loc('i2'), -7, 9);
    gl.uniform4ui(loc('u4'), 1, 2, 3, 0xFFFFFFFE);
    gl.uniform1i(loc('b'), 5);
    gl.uniform2f(loc('b2'), 0, 0.5);
    gl.uniformMatrix2x3fv(loc('m'), false, [1, 2, 3, 4, 5, 6]);
    gl.uniform1fv(loc('arr'), [10, 20, 30]);
    gl.uniform1i(loc('tex'), 7);
    expectError(gl.NO_ERROR, 'setting the uniforms');
    assert(gl.getUniform(prog, loc('f')) === 1.5, 'float');
    eqArr(gl.getUniform(prog, loc('v3')), [1, -2, 0.25], Float32Array, 'vec3');
    eqArr(gl.getUniform(prog, loc('i2')), [-7, 9], Int32Array, 'ivec2');
    eqArr(gl.getUniform(prog, loc('u4')), [1, 2, 3, 0xFFFFFFFE], Uint32Array, 'uvec4');
    assert(gl.getUniform(prog, loc('b')) === true, 'bool');
    const b2 = gl.getUniform(prog, loc('b2'));
    assert(Array.isArray(b2) && b2.length === 2 && b2[0] === false && b2[1] === true, 'bvec2: ' + b2);
    eqArr(gl.getUniform(prog, loc('m')), [1, 2, 3, 4, 5, 6], Float32Array, 'mat2x3, column-major');
    gl.uniformMatrix2x3fv(loc('m'), true, [1, 2, 3, 4, 5, 6]);
    eqArr(gl.getUniform(prog, loc('m')), [1, 3, 5, 2, 4, 6], Float32Array, 'mat2x3 set transposed');
    assert(gl.getUniform(prog, loc('arr[1]')) === 20, 'an array element');
    assert(gl.getUniform(prog, loc('arr')) === 10, 'an array\'s first element');
    assert(gl.getUniform(prog, loc('tex')) === 7, 'a sampler answers its unit');
    // Another program's location.
    const other = program(vs, `#version 300 es
        precision highp float;
        uniform float f;
        out vec4 o;
        void main() { o = vec4(f); }`);
    assert(gl.getUniform(other, loc('f')) === null, 'another program\'s location');
    expectError(gl.INVALID_OPERATION, 'getUniform with another program\'s location');

    // --- validateProgram and sampler-type conflicts ---
    const mixed = program(vs, `#version 300 es
        precision highp float;
        uniform sampler2D a;
        uniform samplerCube c;
        out vec4 o;
        void main() { o = texture(a, vec2(0.5)) + texture(c, vec3(1.0, 0.0, 0.0)); }`);
    assert(gl.getProgramParameter(mixed, gl.VALIDATE_STATUS) === false, 'not validated yet');
    gl.validateProgram(mixed);
    assert(gl.getProgramParameter(mixed, gl.VALIDATE_STATUS) === false,
           'a sampler2D and a samplerCube both on unit 0 do not validate');
    gl.useProgram(mixed);
    gl.bindVertexArray(null);
    gl.disableVertexAttribArray(2);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    gl.enableVertexAttribArray(0);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    expectError(gl.INVALID_OPERATION, 'a draw with two sampler types on one unit');
    gl.uniform1i(gl.getUniformLocation(mixed, 'c'), 1);
    gl.validateProgram(mixed);
    assert(gl.getProgramParameter(mixed, gl.VALIDATE_STATUS) === true, 'validates on separate units');
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    expectError(gl.NO_ERROR, 'the draw on separate units');
    gl.linkProgram(mixed);
    assert(gl.getProgramParameter(mixed, gl.VALIDATE_STATUS) === false, 'relinking resets VALIDATE_STATUS');
    assert(gl.getProgramParameter(mixed, gl.DELETE_STATUS) === false, 'DELETE_STATUS');
    gl.getProgramParameter(mixed, gl.BUFFER_SIZE);
    expectError(gl.INVALID_ENUM, 'getProgramParameter of an unknown pname');
}
