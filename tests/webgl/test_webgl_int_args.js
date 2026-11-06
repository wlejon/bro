// WebIDL integer arguments: a GLuint / GLint / GLenum argument converts by
// ECMAScript ToUint32 / ToInt32 — truncated toward zero and taken modulo
// 2^32, NaN and the infinities 0 — on every path of the binding's converter:
// the ordinary in-range values, negative and fractional ones, values past
// 2^32 and 2^53, and those past 2^63 (bronze_host/host_numeric.h).

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '8');
canvas.setAttribute('height', '8');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2');
if (!gl) {
    missingGpuContext('webgl2');
} else {
    function compile(type, src) {
        const s = gl.createShader(type);
        gl.shaderSource(s, src);
        gl.compileShader(s);
        if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s));
        return s;
    }
    const p = gl.createProgram();
    gl.attachShader(p, compile(gl.VERTEX_SHADER,
        '#version 300 es\nvoid main(){ gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }'));
    gl.attachShader(p, compile(gl.FRAGMENT_SHADER,
        '#version 300 es\nprecision highp float;\nprecision highp int;\n' +
        'uniform uint uU;\nuniform int uI;\nout vec4 o;\n' +
        'void main(){ o = vec4(float(uU) + float(uI)); }'));
    gl.linkProgram(p);
    assert(gl.getProgramParameter(p, gl.LINK_STATUS), 'link: ' + gl.getProgramInfoLog(p));
    gl.useProgram(p);
    const uU = gl.getUniformLocation(p, 'uU');
    const uI = gl.getUniformLocation(p, 'uI');

    const two32 = 4294967296;
    const cases = [
        // [argument, ToUint32, ToInt32]
        [0, 0, 0],
        [7, 7, 7],
        [3.75, 3, 3],
        [-3.75, two32 - 3, -3],
        [-1, 4294967295, -1],
        [2147483648, 2147483648, -2147483648],
        [4294967295, 4294967295, -1],
        [two32 + 5, 5, 5],
        [-(two32 + 5), two32 - 5, -5],
        [2 ** 53 + 2, 2, 2],
        [2 ** 62 + 2 ** 40 + 3 * 2 ** 12, 12288, 12288],
        [2 ** 63, 0, 0],
        [-(2 ** 63), 0, 0],
        [2 ** 64 + 2 ** 33 * 3, 0, 0],
        [2 ** 64 + 7 * 2 ** 12, 28672, 28672],
        [-(2 ** 64 + 7 * 2 ** 12), two32 - 28672, -28672],
        [1e300, 0, 0],
        [NaN, 0, 0],
        [Infinity, 0, 0],
        [-Infinity, 0, 0],
        [-0, 0, 0],
    ];
    for (const [arg, u, i] of cases) {
        gl.uniform1ui(uU, arg);
        gl.uniform1i(uI, arg);
        const gu = gl.getUniform(p, uU), gi = gl.getUniform(p, uI);
        assert(gu === u, 'uniform1ui(' + arg + ') reads ' + gu + ', want ' + u);
        assert(gi === i, 'uniform1i(' + arg + ') reads ' + gi + ', want ' + i);
    }
    assert(gl.getError() === gl.NO_ERROR, 'no error from the conversions');

    // A GLenum past 2^32 wraps onto the enum it is congruent to.
    gl.enable(gl.DEPTH_TEST + two32);
    assert(gl.isEnabled(gl.DEPTH_TEST), 'enable(DEPTH_TEST + 2^32) enables DEPTH_TEST');
    gl.disable(gl.DEPTH_TEST - two32);
    assert(!gl.isEnabled(gl.DEPTH_TEST), 'disable(DEPTH_TEST - 2^32) disables DEPTH_TEST');
    assert(gl.getError() === gl.NO_ERROR, 'no error from the wrapped enums');
}
