// bro.image.gpu.colormap: a 0..1 horizontal ramp through a gray LUT with
// lo 0 / hi 1 reads back as a full 0..255 ramp across a script-sized canvas.
// The canvas used to get a viewport-sized drawing buffer at getContext, so
// the 64-pixel readback saw only a sliver of the stretched ramp (0..6).

const cv = document.createElement('canvas');
cv.width = 64; cv.height = 64;
const gl = cv.getContext('webgl2');
if (!gl) {
    console.log('no webgl2; skipping');
} else {
    const lut = new Uint8Array(256 * 4);
    for (let i = 0; i < 256; i++) { lut[i * 4] = lut[i * 4 + 1] = lut[i * 4 + 2] = i; lut[i * 4 + 3] = 255; }
    const fld = new Float32Array(64 * 64);
    for (let i = 0; i < fld.length; i++) fld[i] = (i % 64) / 63;

    bro.image.gpu.colormap(cv, fld, lut, { lo: 0, hi: 1, srcW: 64, srcH: 64 });
    const px = new Uint8Array(64 * 64 * 4);
    gl.readPixels(0, 0, 64, 64, gl.RGBA, gl.UNSIGNED_BYTE, px);
    const at = (x) => px[(32 * 64 + x) * 4];
    assert(at(0) <= 3, 'left edge is black: ' + at(0));
    assert(at(63) >= 252, 'right edge is white: ' + at(63));
    assert(Math.abs(at(32) - 130) <= 6, 'middle is mid-gray: ' + at(32));
    let mono = true;
    for (let x = 1; x < 64; x++) if (at(x) < at(x - 1)) mono = false;
    assert(mono, 'ramp is monotonic');

    // autoRange finds the same 0..1 span on its own.
    bro.image.gpu.colormap(cv, fld, lut, { autoRange: true, srcW: 64, srcH: 64 });
    gl.readPixels(0, 0, 64, 64, gl.RGBA, gl.UNSIGNED_BYTE, px);
    assert(at(0) <= 3 && at(63) >= 252, 'autoRange spans the field: ' + at(0) + '..' + at(63));
}
console.log('PASS');
