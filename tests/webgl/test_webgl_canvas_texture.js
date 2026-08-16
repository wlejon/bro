// Uploading a <canvas> as a WebGL texture must not leak Skia's GL state.
//
// Snapshotting a live 2D canvas runs Ganesh on the shared GL context, which
// leaves the viewport sized to that canvas (plus Skia's FBO/program bound).
// Everything drawn afterwards then lands in a canvas-sized corner — the first
// three.js CanvasTexture used to blank the rest of the frame.

var wc = document.createElement('canvas');
wc.width = 256;
wc.height = 256;
document.body.appendChild(wc);
var gl = wc.getContext('webgl2');
assert(gl !== null, 'webgl2 context created');

// A small 2D canvas to use as the texture source — deliberately a different
// size from the WebGL canvas, so a leaked viewport is unmistakable.
var src = document.createElement('canvas');
src.width = 64;
src.height = 64;
document.body.appendChild(src);
var g = src.getContext('2d');
g.fillStyle = '#00ff00';
g.fillRect(0, 0, 64, 64);
flush();

gl.viewport(0, 0, 256, 256);
var before = gl.getParameter(gl.VIEWPORT);
assert(before[2] === 256 && before[3] === 256,
       'viewport starts at the canvas size, got ' + before[2] + 'x' + before[3]);

// --- texImage2D from a canvas ---------------------------------------------
var tex = gl.createTexture();
gl.bindTexture(gl.TEXTURE_2D, tex);
gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, src);

var after = gl.getParameter(gl.VIEWPORT);
assert(after[0] === before[0] && after[1] === before[1] &&
       after[2] === before[2] && after[3] === before[3],
       'texImage2D(canvas) leaves the viewport alone, got ' +
       after[2] + 'x' + after[3] + ' (was ' + before[2] + 'x' + before[3] + ')');

// --- texStorage2D + texSubImage2D from a canvas (the three.js WebGL2 path) --
var tex2 = gl.createTexture();
gl.bindTexture(gl.TEXTURE_2D, tex2);
gl.texStorage2D(gl.TEXTURE_2D, 7, gl.RGBA8, 64, 64);
gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, gl.RGBA, gl.UNSIGNED_BYTE, src);
gl.generateMipmap(gl.TEXTURE_2D);

var after2 = gl.getParameter(gl.VIEWPORT);
assert(after2[2] === 256 && after2[3] === 256,
       'texSubImage2D(canvas) leaves the viewport alone, got ' +
       after2[2] + 'x' + after2[3]);

// The upload itself must still be correct — read level 0 back through an FBO.
var fb = gl.createFramebuffer();
gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, tex2, 0);
assert(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE,
       'texture is framebuffer-complete');
var texel = new Uint8Array(4);
gl.readPixels(32, 32, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, texel);
assert(texel[0] < 40 && texel[1] > 200 && texel[2] < 40,
       'canvas pixels reached the texture, got ' + Array.from(texel).join(','));
gl.bindFramebuffer(gl.FRAMEBUFFER, null);

// --- and the state is genuinely usable, not merely reported correct --------
// Clear to blue and read a texel far outside a 64x64 corner. A leaked viewport
// does not clip glClear, so prove it with a scissored clear instead, which is
// exactly the kind of draw that would land in the wrong place.
gl.enable(gl.SCISSOR_TEST);
gl.scissor(0, 0, 256, 256);
gl.clearColor(0, 0, 1, 1);
gl.clear(gl.COLOR_BUFFER_BIT);
gl.disable(gl.SCISSOR_TEST);

var far = new Uint8Array(4);
gl.readPixels(200, 200, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, far);
assert(far[2] > 200 && far[0] < 40,
       'the whole drawing buffer is still addressable after the upload, got ' +
       Array.from(far).join(','));

assert(gl.getError() === gl.NO_ERROR, 'no GL errors across the canvas-texture path');

console.log('PASS: canvas-as-texture leaves GL state intact');
