// WebGL §4.1: a framebuffer attachment reads as the *default clear values*
// before anything is drawn into it — colour (0,0,0,0), depth 1.0, stencil 0 —
// where plain GL leaves the contents undefined.
//
// Depth is the one that bites. GL hands back a renderbuffer that in practice
// reads as 0.0, so with the default LESS test every fragment drawn before the
// first clear is rejected and the target comes out black, with no GL error
// anywhere to explain it. three.js's PMREMGenerator does exactly that — it
// renders six cube faces with `autoClear = false` — so an uninitialized depth
// buffer turns every environment map black and every material lit by one with
// it. See src/webgl/webgl2_context.cpp initializeRenderbuffer().
//
// Also covers the emulated default framebuffer: WebGL's "null framebuffer" is
// bro's canvas FBO, so the page's `BACK` — the only colour buffer the spec
// lets it name there — has to be translated to that FBO's attachment.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '32');
canvas.setAttribute('height', '32');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2');
if (!gl) {
    console.log('no webgl2; skipping');
} else {

// A shader that paints solid red at z = 0 (NDC), i.e. window depth 0.5.
function compile(type, src) {
    const s = gl.createShader(type);
    gl.shaderSource(s, src); gl.compileShader(s);
    assert(gl.getShaderParameter(s, gl.COMPILE_STATUS),
           'shader compiled: ' + gl.getShaderInfoLog(s));
    return s;
}
const prog = gl.createProgram();
gl.attachShader(prog, compile(gl.VERTEX_SHADER,
    '#version 300 es\nin vec2 aPos;\nvoid main(){ gl_Position = vec4(aPos, 0.0, 1.0); }'));
gl.attachShader(prog, compile(gl.FRAGMENT_SHADER,
    '#version 300 es\nprecision highp float;\nuniform vec4 uColor;\nout vec4 frag;\nvoid main(){ frag = uColor; }'));
gl.linkProgram(prog);
assert(gl.getProgramParameter(prog, gl.LINK_STATUS), 'program linked');
gl.useProgram(prog);
gl.uniform4f(gl.getUniformLocation(prog, 'uColor'), 1, 0, 0, 1);

const quad = gl.createBuffer();
gl.bindBuffer(gl.ARRAY_BUFFER, quad);
gl.bufferData(gl.ARRAY_BUFFER,
              new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
const vao = gl.createVertexArray();
gl.bindVertexArray(vao);
gl.enableVertexAttribArray(0);
gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
const drawQuad = () => gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);

// --- depth renderbuffer starts at 1.0 -------------------------------------
// The whole point: draw WITHOUT clearing first. A buffer left at 0.0 fails
// LESS for every fragment and the read comes back black.
const fbo = gl.createFramebuffer();
gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);

const color = gl.createTexture();
gl.bindTexture(gl.TEXTURE_2D, color);
gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 32, 32, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, color, 0);

const depth = gl.createRenderbuffer();
gl.bindRenderbuffer(gl.RENDERBUFFER, depth);
gl.renderbufferStorage(gl.RENDERBUFFER, gl.DEPTH_COMPONENT24, 32, 32);
gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_ATTACHMENT, gl.RENDERBUFFER, depth);

assert(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE,
       'colour + depth framebuffer is complete');

gl.viewport(0, 0, 32, 32);
gl.enable(gl.DEPTH_TEST);
gl.depthFunc(gl.LESS);
drawQuad();                                 // no clear, deliberately

const px = new Uint8Array(4);
gl.readPixels(16, 16, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
assert(px[0] === 255 && px[1] === 0 && px[2] === 0,
       'a draw into an uncleared buffer passes the depth test — the buffer ' +
       'reads 1.0, not 0.0 (got ' + px[0] + ',' + px[1] + ',' + px[2] + ')');
assert(gl.getError() === gl.NO_ERROR, 'no GL error from the uncleared draw');

// A depth-stencil renderbuffer gets the same treatment.
const ds = gl.createRenderbuffer();
gl.bindRenderbuffer(gl.RENDERBUFFER, ds);
gl.renderbufferStorage(gl.RENDERBUFFER, gl.DEPTH24_STENCIL8, 32, 32);
gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_ATTACHMENT, gl.RENDERBUFFER, null);
gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_STENCIL_ATTACHMENT, gl.RENDERBUFFER, ds);
assert(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE,
       'depth-stencil framebuffer is complete');
gl.clearColor(0, 0, 0, 1);
gl.clear(gl.COLOR_BUFFER_BIT);              // colour only — depth stays as allocated
drawQuad();
gl.readPixels(16, 16, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
assert(px[0] === 255, 'DEPTH24_STENCIL8 is initialized to depth 1.0 as well');

// Allocating a renderbuffer must not disturb the caller's state — the clear
// that initializes it runs against its own scratch framebuffer.
gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
gl.enable(gl.SCISSOR_TEST);
gl.scissor(0, 0, 8, 8);
const spare = gl.createRenderbuffer();
gl.bindRenderbuffer(gl.RENDERBUFFER, spare);
gl.renderbufferStorage(gl.RENDERBUFFER, gl.DEPTH_COMPONENT24, 16, 16);
assert(gl.isEnabled(gl.SCISSOR_TEST) === true,
       'the scissor test survives a renderbuffer allocation');
gl.disable(gl.SCISSOR_TEST);
// Functional check that the caller's binding survived: the FBO is still the
// one being drawn into and read from, so a draw+read here still works.
gl.clearColor(0, 1, 0, 1);
gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
gl.readPixels(16, 16, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
assert(px[1] === 255 && px[0] === 0,
       'the caller keeps its draw AND read framebuffer across the allocation');
assert(gl.getError() === gl.NO_ERROR,
       'and no error is left behind (a stray readBuffer(NONE) would show up here)');

// --- drawBuffers / readBuffer on the emulated default framebuffer ---------
gl.bindFramebuffer(gl.FRAMEBUFFER, null);
while (gl.getError() !== gl.NO_ERROR) { /* drain */ }
gl.drawBuffers([gl.BACK]);
assert(gl.getError() === gl.NO_ERROR,
       'drawBuffers([BACK]) is accepted on the default framebuffer');
gl.drawBuffers([gl.NONE]);
assert(gl.getError() === gl.NO_ERROR, 'drawBuffers([NONE]) is accepted too');
gl.drawBuffers([gl.BACK]);
gl.readBuffer(gl.BACK);
assert(gl.getError() === gl.NO_ERROR,
       'readBuffer(BACK) is accepted on the default framebuffer');

// And it still draws there afterwards, which is what the translation is for.
gl.viewport(0, 0, 32, 32);
gl.clearColor(0, 0, 0, 1);
gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
drawQuad();
gl.readPixels(16, 16, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
assert(px[0] === 255 && px[1] === 0,
       'the default framebuffer still receives draws after drawBuffers([BACK])');

console.log('PASS: WebGL resource init — depth starts at 1.0, BACK maps to the canvas FBO');
}
