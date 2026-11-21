// A GLSL variable without an initializer is zero in WebGL (ANGLE initialises
// every one), where SPIR-V leaves it undefined: bro gives each Private and
// Function variable a null initializer. PlayCanvas accumulates its lighting
// into an uninitialised global (`vec3 dDiffuseLight; ... dDiffuseLight +=
// ambient`), and every StandardMaterial drew black.

// Within one step a channel: a value halfway between two (0.5 is 127.5)
// rounds either way on a unorm target, by the driver.
function near(got, want) {
    const a = got.split(',').map(Number);
    const b = want.split(',').map(Number);
    return a.length === b.length && a.every((v, i) => Math.abs(v - b[i]) <= 1);
}

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
    struct S { vec2 a; float b; };
    S sv; int iv;
    out vec4 c;
    void main() {
        vec2 p[3] = vec2[3](vec2(-1, -1), vec2(3, -1), vec2(-1, 3));
        gl_Position = vec4(p[gl_VertexID], 0, 1);
        sv.b += 0.5; iv += 1;
        c = vec4(sv.a, sv.b, float(iv));
    }`;
    function draw(fs) {
        const p = gl.createProgram();
        for (const [t, s] of [[gl.VERTEX_SHADER, vs], [gl.FRAGMENT_SHADER, fs]]) {
            const sh = gl.createShader(t);
            gl.shaderSource(sh, s);
            gl.compileShader(sh);
            gl.attachShader(p, sh);
        }
        gl.linkProgram(p);
        assert(gl.getProgramParameter(p, gl.LINK_STATUS), 'links: ' + gl.getProgramInfoLog(p));
        gl.useProgram(p);
        gl.clearColor(1, 1, 1, 1);
        gl.clear(gl.COLOR_BUFFER_BIT);
        gl.drawArrays(gl.TRIANGLES, 0, 3);
        const px = new Uint8Array(4);
        gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
        return Array.from(px).join(',');
    }
    const global = draw(`#version 300 es
    precision highp float; in vec4 c; out vec4 o;
    vec3 acc;
    void add() { acc += vec3(0.25, 0.5, 0.0); }
    void main() { add(); o = vec4(acc, 1.0); }`);
    assert(near(global, '64,128,0,255'), 'an uninitialised global starts at zero, got ' + global);
    const local = draw(`#version 300 es
    precision highp float; in vec4 c; out vec4 o;
    void main() { vec3 acc; float arr[2]; acc += vec3(0.5); arr[1] += 0.25; o = vec4(acc.x, arr[0], arr[1], 1.0); }`);
    assert(near(local, '128,0,64,255'), 'uninitialised locals and arrays start at zero, got ' + local);
    const vert = draw(`#version 300 es
    precision highp float; in vec4 c; out vec4 o; void main() { o = c; }`);
    assert(near(vert, '0,0,128,255'), 'vertex-stage globals of struct and int type start at zero, got ' + vert);
}
