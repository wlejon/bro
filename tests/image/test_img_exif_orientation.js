// image-orientation (docs/file-api.js): `from-image`, the initial value,
// turns an EXIF-rotated photo upright in an <img> (its box and naturalWidth
// too) and in a CSS background; `image-orientation: none` shows the stored
// pixels as they are. createImageBitmap(blob) follows its own
// imageOrientation option the same way.
//
// The fixture is an 80x40 JPEG (left half red, right half blue) carrying EXIF
// orientation 6 — "rotate 90 degrees clockwise to display" — so upright it is
// 40x80 with red on top and blue below.

const fs = require('fs');
const path = require('path');
const os = require('os');

const SW = 80, SH = 40;
const rgb = new Uint8Array(SW * SH * 3);
for (let y = 0; y < SH; y++) {
    for (let x = 0; x < SW; x++) {
        const i = (y * SW + x) * 3;
        if (x < SW / 2) { rgb[i] = 255; } else { rgb[i + 2] = 255; }
    }
}
const plain = bro.image.encodeJpeg(rgb, SW, SH, 3, 95);
assert(plain && plain[0] === 0xFF && plain[1] === 0xD8, 'encoded the JPEG');

// An APP1 Exif segment right after SOI: a little-endian TIFF header and one
// IFD entry, Orientation (0x0112) SHORT = 6.
const exif = [
    0xFF, 0xE1, 0x00, 0x22,                         // APP1, length 34
    0x45, 0x78, 0x69, 0x66, 0x00, 0x00,             // "Exif\0\0"
    0x49, 0x49, 0x2A, 0x00, 0x08, 0x00, 0x00, 0x00, // II*\0, IFD at 8
    0x01, 0x00,                                     // 1 entry
    0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,                         // no next IFD
];
const jpeg = new Uint8Array(plain.length + exif.length);
jpeg.set(plain.subarray(0, 2), 0);
jpeg.set(exif, 2);
jpeg.set(plain.subarray(2), 2 + exif.length);
assert(bro.image.readExifOrientation(jpeg) === 6, 'the fixture carries EXIF orientation 6');

const dir = path.join(os.tmpdir(), 'bro_img_exif_' + process.pid + '_' + Date.now());
fs.mkdirSync(dir, { recursive: true });
const file = path.join(dir, 'rot6.jpg').replace(/\\/g, '/');
fs.writeFileSync(file, jpeg);

document.body.style.margin = '0';
document.body.style.background = 'rgb(0,255,0)';
document.body.innerHTML =
    '<img id="up" src="' + file + '" style="position:absolute;left:0;top:0">' +
    '<img id="raw" src="' + file + '" style="position:absolute;left:100px;top:0;image-orientation:none">' +
    '<div id="bg" style="position:absolute;left:0;top:120px;width:40px;height:80px;' +
    'background:url(' + file + ') 0 0 / 100% 100% no-repeat"></div>' +
    '<div id="bgRaw" style="position:absolute;left:100px;top:120px;width:40px;height:80px;' +
    'image-orientation:none;background:url(' + file + ') 0 0 / 100% 100% no-repeat"></div>';
flush();

const isRed = (p) => p.r > 180 && p.g < 80 && p.b < 80;
const isBlue = (p) => p.b > 180 && p.r < 80 && p.g < 80;
const show = (p) => JSON.stringify(p);

// <img>, from-image: an upright 40x80 box, red above blue.
const up = document.getElementById('up');
const ur = up.getBoundingClientRect();
assert(Math.round(ur.width) === 40 && Math.round(ur.height) === 80,
       'from-image lays the <img> out upright (40x80), got ' + ur.width + 'x' + ur.height);
assert(up.naturalWidth === 40 && up.naturalHeight === 80,
       'naturalWidth/Height are upright, got ' + up.naturalWidth + 'x' + up.naturalHeight);
let p = getPixel(20, 20), q = getPixel(20, 60);
assert(isRed(p) && isBlue(q), 'the <img> paints upright: top ' + show(p) + ', bottom ' + show(q));

// <img>, none: the stored 80x40, red left, blue right.
const rr = document.getElementById('raw').getBoundingClientRect();
assert(Math.round(rr.width) === 80 && Math.round(rr.height) === 40,
       'image-orientation:none keeps the stored 80x40 box, got ' + rr.width + 'x' + rr.height);
p = getPixel(120, 20); q = getPixel(160, 20);
assert(isRed(p) && isBlue(q), 'none paints the stored pixels: left ' + show(p) + ', right ' + show(q));

// Backgrounds stretched over a 40x80 box: upright red-over-blue, or the
// stored image squeezed (red left, blue right).
p = getPixel(20, 140); q = getPixel(20, 180);
assert(isRed(p) && isBlue(q), 'a background is upright: top ' + show(p) + ', bottom ' + show(q));
p = getPixel(110, 160); q = getPixel(130, 160);
assert(isRed(p) && isBlue(q), 'a background with none is stored: left ' + show(p) + ', right ' + show(q));

// createImageBitmap(blob): upright by default, stored with imageOrientation 'none'.
const blob = new Blob([jpeg], { type: 'image/jpeg' });
const b1 = await createImageBitmap(blob);
assert(b1.width === 40 && b1.height === 80, 'createImageBitmap(blob) is upright, got ' + b1.width + 'x' + b1.height);
const b2 = await createImageBitmap(blob, { imageOrientation: 'none' });
assert(b2.width === 80 && b2.height === 40, "imageOrientation:'none' keeps the stored size, got " + b2.width + 'x' + b2.height);

fs.rmSync(dir, { recursive: true, force: true });
console.log('PASS');
