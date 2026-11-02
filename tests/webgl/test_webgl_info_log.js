// A compile or link with nothing to say has an empty info log, as a GL
// driver's: no note about default precisions, whatever precision the
// fragment shader states (or does not), in either shading language. A
// failing compile still says why.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '4');
canvas.setAttribute('height', '4');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2', { antialias: false });
if (!gl) {
    missingGpuContext('webgl2');
} else {
    function shader(type, src) {
        const s = gl.createShader(type);
        gl.shaderSource(s, src);
        gl.compileShader(s);
        return s;
    }
    const vs300 = `#version 300 es
        in vec4 p; uniform highp mat4 m; void main() { gl_Position = m * p; }`;
    const fragments = {
        'ES 3.00, precision statement': `#version 300 es
            precision mediump float; out vec4 c; void main() { c = vec4(1.0); }`,
        'ES 3.00, qualified declarations': `#version 300 es
            precision highp float; uniform mediump vec4 tint; uniform lowp sampler2D map;
            out highp vec4 c; void main() { c = tint * texture(map, vec2(0.5)); }`,
        'ES 3.00, no precision statement': `#version 300 es
            out highp vec4 c; void main() { c = vec4(1.0); }`,
        'ES 1.00, precision statement': `precision mediump float;
            uniform vec4 tint; void main() { gl_FragColor = tint; }`,
        'ES 1.00, qualified declaration': `precision highp float;
            varying mediump vec2 uv; void main() { gl_FragColor = vec4(uv, 0.0, 1.0); }`,
    };
    const vs100 = `attribute vec4 p; varying mediump vec2 uv; void main() { uv = p.xy; gl_Position = p; }`;
    for (const [what, src] of Object.entries(fragments)) {
        const es100 = what.startsWith('ES 1.00');
        const v = shader(gl.VERTEX_SHADER, es100 ? vs100 : vs300);
        const f = shader(gl.FRAGMENT_SHADER, src);
        assert(gl.getShaderParameter(v, gl.COMPILE_STATUS), what + ': the vertex shader compiles');
        assert(gl.getShaderParameter(f, gl.COMPILE_STATUS), what + ': compiles: ' + gl.getShaderInfoLog(f));
        assert(gl.getShaderInfoLog(v) === '', what + ': empty vertex log: ' + JSON.stringify(gl.getShaderInfoLog(v)));
        assert(gl.getShaderInfoLog(f) === '', what + ': empty fragment log: ' + JSON.stringify(gl.getShaderInfoLog(f)));
        const p = gl.createProgram();
        gl.attachShader(p, v);
        gl.attachShader(p, f);
        gl.linkProgram(p);
        assert(gl.getProgramParameter(p, gl.LINK_STATUS), what + ': links: ' + gl.getProgramInfoLog(p));
        assert(gl.getProgramInfoLog(p) === '', what + ': empty link log: ' + JSON.stringify(gl.getProgramInfoLog(p)));
    }

    // A program before any link has an empty log too.
    assert(gl.getProgramInfoLog(gl.createProgram()) === '', 'an unlinked program has an empty log');

    // An error is still reported, with its message.
    const bad = shader(gl.FRAGMENT_SHADER, `#version 300 es
        precision highp float; out vec4 c; void main() { c = undefinedThing; }`);
    assert(!gl.getShaderParameter(bad, gl.COMPILE_STATUS), 'a bad shader fails to compile');
    const log = gl.getShaderInfoLog(bad);
    assert(log.indexOf('undefinedThing') >= 0, 'its log names the problem: ' + JSON.stringify(log));
    assert(log.indexOf('precision') < 0, 'and says nothing about precisions: ' + JSON.stringify(log));
    assert(gl.getError() === gl.NO_ERROR, 'no error');
}
