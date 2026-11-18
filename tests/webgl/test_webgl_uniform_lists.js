// The uniform*v / uniformMatrix*fv data argument is a Float32List (Int32List,
// Uint32List): a typed array of the list's own type is read as it is, and any
// other typed array or array is a sequence converted element by element. A
// Float64Array (xeokit's matrices) was read as raw float32 bits: twice the
// values, garbage, and an INVALID_OPERATION for a count over one. And a
// one-element uniform array is found by its bare name.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '4');
canvas.setAttribute('height', '4');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2', { antialias: false });
if (!gl) {
    missingGpuContext('webgl2');
} else {
    const vs = `#version 300 es
    uniform mat4 m; uniform vec3 v3; uniform ivec2 i2; uniform uvec2 u2; out vec4 c;
    void main() {
        vec2 p[3] = vec2[3](vec2(-1, -1), vec2(3, -1), vec2(-1, 3));
        gl_Position = m * vec4(p[gl_VertexID], 0, 1);
        c = vec4(v3.x, float(i2.y) / 255.0, float(u2.y) / 255.0, 1.0);
    }`;
    const fs = `#version 300 es
    precision highp float; in vec4 c; out vec4 o; void main() { o = c; }`;
    const p = gl.createProgram();
    for (const [t, s] of [[gl.VERTEX_SHADER, vs], [gl.FRAGMENT_SHADER, fs]]) {
        const sh = gl.createShader(t);
        gl.shaderSource(sh, s);
        gl.compileShader(sh);
        gl.attachShader(p, sh);
    }
    gl.linkProgram(p);
    gl.useProgram(p);
    const loc = (n) => gl.getUniformLocation(p, n);
    for (const [name, F, I, U] of [['own types', Float32Array, Int32Array, Uint32Array],
                                   ['Float64Array / Int16Array / Uint8Array', Float64Array, Int16Array, Uint8Array],
                                   ['arrays', Array, Array, Array]]) {
        const mk = (A, v) => (A === Array ? v : new A(v));
        // A matrix that halves x, so the triangle still covers pixel (0,0).
        gl.uniformMatrix4fv(loc('m'), false, mk(F, [0.5, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -0.5, 0, 0, 1]));
        gl.uniform3fv(loc('v3'), mk(F, [0.4, 0, 0]));
        gl.uniform2iv(loc('i2'), mk(I, [0, 200]));
        gl.uniform2uiv(loc('u2'), mk(U, [0, 100]));
        assert(gl.getError() === 0, name + ': no GL error');
        gl.clearColor(0, 0, 0, 0);
        gl.clear(gl.COLOR_BUFFER_BIT);
        gl.drawArrays(gl.TRIANGLES, 0, 3);
        const px = new Uint8Array(4);
        gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
        assert(px.join() === '102,200,100,255', name + ': pixel ' + px.join());
        // The top-right corner lies outside the halved triangle only if m arrived.
        gl.readPixels(3, 3, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
        assert(px[3] === 0, name + ': the matrix applied, top-right pixel ' + px.join());
        gl.uniformMatrix4fv(loc('m'), false, new Float32Array(16));
    }

    // An array of one element is found by its bare name as well as by
    // "name[0]" (three.js r49 looks its one light up as "pointLightColor").
    const vs1 = `#version 300 es
    uniform vec3 one[1]; uniform vec2 two[2]; out vec4 c;
    void main() { gl_Position = vec4(0, 0, 0, 1); c = vec4(one[0], two[1].x); }`;
    const p1 = gl.createProgram();
    for (const [t, s] of [[gl.VERTEX_SHADER, vs1], [gl.FRAGMENT_SHADER, fs]]) {
        const sh = gl.createShader(t);
        gl.shaderSource(sh, s);
        gl.compileShader(sh);
        gl.attachShader(p1, sh);
    }
    gl.linkProgram(p1);
    gl.useProgram(p1);
    const bare = gl.getUniformLocation(p1, 'one');
    assert(bare !== null && gl.getUniformLocation(p1, 'one[0]') !== null, 'a one-element array by name and by name[0]');
    gl.uniform3fv(bare, [0.25, 0.5, 0.75]);
    assert(Array.from(gl.getUniform(p1, gl.getUniformLocation(p1, 'one[0]'))).join() === '0.25,0.5,0.75',
           'the bare name is element 0');
    assert(gl.getUniformLocation(p1, 'one[1]') === null, 'past the end of a one-element array is null');
    assert(gl.getUniformLocation(p1, 'two') !== null && gl.getUniformLocation(p1, 'two[1]') !== null, 'a longer array');
    console.log('test_webgl_uniform_lists: OK');
}
