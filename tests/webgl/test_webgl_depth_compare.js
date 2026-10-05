// Depth-compare sampling: TEXTURE_COMPARE_MODE / TEXTURE_COMPARE_FUNC on a
// depth texture read through sampler2DShadow (each texel's pass/fail by its
// pixel), a sampler object's compare state replacing the texture's, raw depth
// through sampler2D when comparison is off, and the WebGL 2 errors for a
// shadow sampler without comparison and a plain sampler with it.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '4');
canvas.setAttribute('height', '4');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2', { antialias: false });
if (!gl) {
    missingGpuContext('webgl2');
} else {
    function program(fs) {
        const p = gl.createProgram();
        const vs = `#version 300 es
            in vec2 p;
            void main() { gl_Position = vec4(p, 0.0, 1.0); }`;
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
    function row() {
        const b = new Uint8Array(16);
        gl.readPixels(0, 1, 4, 1, gl.RGBA, gl.UNSIGNED_BYTE, b);
        return [b[0], b[4], b[8], b[12]];
    }
    function expectRow(want, msg) {
        const got = row();
        assert(got.every((v, i) => Math.abs(v - want[i]) <= 2), msg + ': got [' + got + '] want [' + want + ']');
    }
    function expectError(want, msg) {
        const got = gl.getError();
        assert(got === want, msg + ': error 0x' + got.toString(16) + ' want 0x' + want.toString(16));
    }

    const vbo = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 3, -1, -1, 3]), gl.STATIC_DRAW);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    gl.enableVertexAttribArray(0);

    // Four texels of depth, one per pixel column.
    const tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.DEPTH_COMPONENT32F, 4, 1, 0, gl.DEPTH_COMPONENT, gl.FLOAT,
                  new Float32Array([0.1, 0.4, 0.6, 0.9]));
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    expectError(gl.NO_ERROR, 'depth texture');

    const shadow = program(`#version 300 es
        precision highp float;
        precision highp sampler2DShadow;
        uniform sampler2DShadow s;
        uniform float ref;
        out vec4 o;
        void main() { o = vec4(texture(s, vec3(gl_FragCoord.x / 4.0, 0.5, ref))); }`);
    const plain = program(`#version 300 es
        precision highp float;
        uniform sampler2D s;
        out vec4 o;
        void main() { o = vec4(texture(s, vec2(gl_FragCoord.x / 4.0, 0.5)).r); }`);

    // Comparison: 1 where `ref FUNC texel` holds.
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_COMPARE_MODE, gl.COMPARE_REF_TO_TEXTURE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_COMPARE_FUNC, gl.LEQUAL);
    assert(gl.getTexParameter(gl.TEXTURE_2D, gl.TEXTURE_COMPARE_MODE) === gl.COMPARE_REF_TO_TEXTURE, 'COMPARE_MODE reads back');
    gl.useProgram(shadow);
    gl.uniform1f(gl.getUniformLocation(shadow, 'ref'), 0.5);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    expectError(gl.NO_ERROR, 'the shadow draw');
    expectRow([0, 0, 255, 255], 'LEQUAL against 0.5');
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_COMPARE_FUNC, gl.GREATER);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    expectRow([255, 255, 0, 0], 'GREATER against 0.5');
    gl.uniform1f(gl.getUniformLocation(shadow, 'ref'), 0.95);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    expectRow([255, 255, 255, 255], 'GREATER against 0.95');
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_COMPARE_FUNC, gl.NEVER);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    expectRow([0, 0, 0, 0], 'NEVER');

    // A sampler object's compare state replaces the texture's.
    const smp = gl.createSampler();
    gl.samplerParameteri(smp, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.samplerParameteri(smp, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.samplerParameteri(smp, gl.TEXTURE_COMPARE_MODE, gl.COMPARE_REF_TO_TEXTURE);
    gl.samplerParameteri(smp, gl.TEXTURE_COMPARE_FUNC, gl.LESS);
    gl.bindSampler(0, smp);
    gl.uniform1f(gl.getUniformLocation(shadow, 'ref'), 0.5);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    expectRow([0, 0, 255, 255], 'the sampler object\'s LESS');
    gl.samplerParameteri(smp, gl.TEXTURE_COMPARE_MODE, gl.NONE);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    expectError(gl.INVALID_OPERATION, 'a shadow sampler reading through a non-comparing sampler object');
    gl.bindSampler(0, null);

    // Comparison off: a shadow sampler may not read it, a plain one reads depth.
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_COMPARE_MODE, gl.NONE);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    expectError(gl.INVALID_OPERATION, 'sampler2DShadow without TEXTURE_COMPARE_MODE');
    gl.useProgram(plain);
    gl.clearColor(0, 0, 0, 0);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    expectError(gl.NO_ERROR, 'sampler2D on uncompared depth');
    expectRow([26, 102, 153, 230], 'raw depth');
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_COMPARE_MODE, gl.COMPARE_REF_TO_TEXTURE);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    expectError(gl.INVALID_OPERATION, 'sampler2D on a comparing depth texture');
}
