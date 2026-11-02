// getContext's WebGLContextAttributes decide the drawing buffer, and
// getContextAttributes reports what was granted: alpha, depth and stencil
// exactly as asked (stencil defaults to false), no multisampling, always
// premultiplied, powerPreference as asked. Without alpha the drawing buffer
// reads (and composites) opaque whatever is drawn or cleared into it.
// Without preserveDrawingBuffer it reads as cleared once a composite has shown it;
// with it, it keeps what was drawn.

function makeCanvas() {
    const c = document.createElement('canvas');
    c.setAttribute('width', '8');
    c.setAttribute('height', '8');
    document.body.appendChild(c);
    return c;
}
const canvases = [makeCanvas(), makeCanvas(), makeCanvas(), makeCanvas()];
flush();

const gl = canvases[0].getContext('webgl2');
if (!gl) {
    missingGpuContext('webgl2');
} else {
    function pixel(g, x, y) {
        const px = new Uint8Array(4);
        g.readPixels(x, y, 1, 1, g.RGBA, g.UNSIGNED_BYTE, px);
        return Array.from(px);
    }
    function eq(got, want, msg) {
        assert(got.length === want.length && got.every((v, i) => v === want[i]), msg + ': got [' + got + '] want [' + want + ']');
    }

    // --- The defaults ---
    const a = gl.getContextAttributes();
    eq([a.alpha, a.depth, a.stencil, a.antialias, a.premultipliedAlpha, a.preserveDrawingBuffer,
        a.failIfMajorPerformanceCaveat, a.desynchronized, a.xrCompatible],
       [true, true, false, false, true, false, false, false, false], 'default attributes');
    assert(a.powerPreference === 'default', 'default powerPreference: ' + a.powerPreference);
    assert(gl.getParameter(gl.ALPHA_BITS) === 8, 'ALPHA_BITS 8 with alpha');
    assert(gl.getParameter(gl.DEPTH_BITS) >= 16, 'DEPTH_BITS >= 16 with depth');
    assert(gl.getParameter(gl.STENCIL_BITS) === 0, 'STENCIL_BITS 0 without stencil');

    // --- What a context grants of what it was asked ---
    const opaque = canvases[1].getContext('webgl2', {
        alpha: false, depth: false, stencil: true, antialias: true, premultipliedAlpha: false,
        preserveDrawingBuffer: true, powerPreference: 'high-performance',
    });
    const b = opaque.getContextAttributes();
    eq([b.alpha, b.depth, b.stencil, b.antialias, b.premultipliedAlpha, b.preserveDrawingBuffer],
       [false, false, true, false, true, true], 'granted attributes');
    assert(b.powerPreference === 'high-performance', 'powerPreference as asked: ' + b.powerPreference);
    assert(opaque.getParameter(opaque.ALPHA_BITS) === 0, 'ALPHA_BITS 0 without alpha');
    assert(opaque.getParameter(opaque.DEPTH_BITS) === 0, 'DEPTH_BITS 0 without depth');
    assert(opaque.getParameter(opaque.STENCIL_BITS) === 8, 'STENCIL_BITS 8 with stencil');
    assert(canvases[1].getContext('webgl2', { alpha: true }) === opaque, 'a second getContext answers the same context');
    assert(opaque.getContextAttributes().alpha === false, '... with the attributes it was made with');

    // Without alpha: cleared and drawn alpha both read as one.
    opaque.clearColor(1, 0, 0, 0.25);
    opaque.clear(opaque.COLOR_BUFFER_BIT);
    eq(pixel(opaque, 2, 2), [255, 0, 0, 255], 'clear keeps alpha at one');
    opaque.clearBufferfv(opaque.COLOR, 0, [0, 1, 0, 0]);
    eq(pixel(opaque, 2, 2), [0, 255, 0, 255], 'clearBufferfv keeps alpha at one');
    const prog = opaque.createProgram();
    for (const [type, src] of [[opaque.VERTEX_SHADER, `#version 300 es
            void main() { gl_Position = vec4(vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)) * 4.0 - 1.0, 0.0, 1.0); }`],
        [opaque.FRAGMENT_SHADER, `#version 300 es
            precision highp float; out vec4 c; void main() { c = vec4(0.0, 0.0, 1.0, 0.5); }`]]) {
        const s = opaque.createShader(type);
        opaque.shaderSource(s, src);
        opaque.compileShader(s);
        opaque.attachShader(prog, s);
    }
    opaque.linkProgram(prog);
    opaque.useProgram(prog);
    opaque.drawArrays(opaque.TRIANGLES, 0, 3);
    eq(pixel(opaque, 2, 2), [0, 0, 255, 255], 'a draw keeps alpha at one');
    // DST_ALPHA blending sees the one.
    opaque.enable(opaque.BLEND);
    opaque.blendFunc(opaque.ZERO, opaque.DST_ALPHA);
    opaque.drawArrays(opaque.TRIANGLES, 0, 3);
    eq(pixel(opaque, 2, 2), [0, 0, 255, 255], 'DST_ALPHA reads one');
    opaque.disable(opaque.BLEND);
    // The stencil it asked for works; the depth it did not ask for does not test.
    opaque.enable(opaque.DEPTH_TEST);
    opaque.depthFunc(opaque.NEVER);
    opaque.clearColor(0, 0, 0, 0);
    opaque.clear(opaque.COLOR_BUFFER_BIT);
    opaque.drawArrays(opaque.TRIANGLES, 0, 3);
    eq(pixel(opaque, 2, 2), [0, 0, 255, 255], 'no depth buffer: the depth test passes');
    opaque.disable(opaque.DEPTH_TEST);
    opaque.enable(opaque.STENCIL_TEST);
    opaque.clearStencil(1);
    opaque.clear(opaque.COLOR_BUFFER_BIT | opaque.STENCIL_BUFFER_BIT);
    opaque.stencilFunc(opaque.EQUAL, 0, 0xFF);
    opaque.drawArrays(opaque.TRIANGLES, 0, 3);
    eq(pixel(opaque, 2, 2), [0, 0, 0, 255], 'the stencil buffer tests');
    opaque.disable(opaque.STENCIL_TEST);
    assert(opaque.getError() === opaque.NO_ERROR, 'no error on the opaque context');

    // A context without stencil: the stencil test has nothing to test.
    gl.enable(gl.STENCIL_TEST);
    gl.stencilFunc(gl.NEVER, 0, 0xFF);
    gl.clearColor(0, 1, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    eq(pixel(gl, 2, 2), [0, 255, 0, 255], 'a clear without stencil');
    gl.disable(gl.STENCIL_TEST);

    // --- preserveDrawingBuffer ---
    // canvases[0] (gl) does not preserve, canvases[1] (opaque) does; both
    // hold a known color, then a frame shows them.
    opaque.clearColor(1, 1, 0, 1);
    opaque.clear(opaque.COLOR_BUFFER_BIT);
    gl.clearColor(0, 0, 1, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    frameImage();  // a composite shows the canvases
    eq(pixel(gl, 2, 2), [0, 0, 0, 0], 'shown without preserveDrawingBuffer: reads as cleared');
    eq(pixel(opaque, 2, 2), [255, 255, 0, 255], 'shown with preserveDrawingBuffer: kept');
    // A partial clear after showing still sees the rest cleared.
    gl.clearColor(1, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    frameImage();
    gl.enable(gl.SCISSOR_TEST);
    gl.scissor(0, 0, 4, 8);
    gl.clearColor(0, 1, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.disable(gl.SCISSOR_TEST);
    eq(pixel(gl, 1, 1), [0, 255, 0, 255], 'the scissored clear');
    eq(pixel(gl, 6, 1), [0, 0, 0, 0], 'outside it, the cleared drawing buffer');
    // Until it is shown again, what was drawn stays.
    eq(pixel(gl, 1, 1), [0, 255, 0, 255], 'unshown: kept');

    // An opaque context without preserveDrawingBuffer clears to opaque black.
    const opaque2 = canvases[2].getContext('webgl2', { alpha: false });
    opaque2.clearColor(1, 1, 1, 1);
    opaque2.clear(opaque2.COLOR_BUFFER_BIT);
    frameImage();
    eq(pixel(opaque2, 2, 2), [0, 0, 0, 255], 'opaque and shown: opaque black');

    // powerPreference is an enum: anything else is a TypeError.
    let threw = null;
    try { canvases[3].getContext('webgl2', { powerPreference: 'fastest' }); } catch (e) { threw = e; }
    assert(threw instanceof TypeError, 'an unknown powerPreference throws a TypeError: ' + threw);
    const low = canvases[3].getContext('webgl2', { powerPreference: 'low-power' });
    assert(low.getContextAttributes().powerPreference === 'low-power', 'low-power as asked');
    assert(gl.getError() === gl.NO_ERROR, 'no error');
}
