// bro.image.gpu.colormap: a 0..1 horizontal ramp through a gray LUT with
// lo 0 / hi 1 reads back as a full 0..255 ramp across a script-sized canvas
// (the canvas's own 64×64 pixels, not a viewport-sized buffer stretched).

const cv = document.createElement('canvas');
cv.width = 64; cv.height = 64;
cv.style.cssText = 'position:absolute;left:0;top:0;width:64px;height:64px';
document.getElementById('root').appendChild(cv);
flush();
if (!bro.kernel.gpu) {
    missingGpuContext('kernel');
} else {
    const lut = new Uint8Array(256 * 4);
    for (let i = 0; i < 256; i++) { lut[i * 4] = lut[i * 4 + 1] = lut[i * 4 + 2] = i; lut[i * 4 + 3] = 255; }
    const fld = new Float32Array(64 * 64);
    for (let i = 0; i < fld.length; i++) fld[i] = (i % 64) / 63;

    bro.image.gpu.colormap(cv, fld, lut, { lo: 0, hi: 1, srcW: 64, srcH: 64 });
    let px = getPixels(0, 0, 64, 64).data;
    const at = (x) => px[(32 * 64 + x) * 4];
    assert(at(0) <= 3, 'left edge is black: ' + at(0));
    assert(at(63) >= 252, 'right edge is white: ' + at(63));
    assert(Math.abs(at(32) - 130) <= 6, 'middle is mid-gray: ' + at(32));
    let mono = true;
    for (let x = 1; x < 64; x++) if (at(x) < at(x - 1)) mono = false;
    assert(mono, 'ramp is monotonic');

    // autoRange finds the same 0..1 span on its own.
    bro.image.gpu.colormap(cv, fld, lut, { autoRange: true, srcW: 64, srcH: 64 });
    px = getPixels(0, 0, 64, 64).data;
    assert(at(0) <= 3 && at(63) >= 252, 'autoRange spans the field: ' + at(0) + '..' + at(63));
}
console.log('PASS');
