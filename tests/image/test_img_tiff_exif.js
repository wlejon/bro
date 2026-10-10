// TIFF decodes everywhere an image does (broimage's TIFF decoder): an <img>
// shows one, upright by the Orientation tag in its own IFD0;
// bro.image.decodeOriented / probeDimensions read it. And bro.image.readExif
// reads camera, exposure, date and GPS from the EXIF a JPEG (APP1), a TIFF
// (IFD0 + Exif/GPS sub-IFDs), a WebP (EXIF chunk) and a PNG (eXIf chunk)
// carry (docs/image-api.js).
//
// Every fixture is built here. The TIFF is 40x20 RGB, PackBits-compressed,
// left half red and right half blue, Orientation 6 ("rotate 90 degrees
// clockwise to display"): upright it is 20x40, red on top, blue below.

const fs = require('fs');
const path = require('path');
const os = require('os');

// ── A little-endian TIFF / EXIF writer ───────────────────────────────────────
// Entries: [tag, type, values]; type 2 ASCII (a string), 3 SHORT, 4 LONG,
// 5 RATIONAL ([num, den, num, den, ...]), 1 BYTE / 7 UNDEFINED (bytes).
const out = [];
const u8 = (v) => out.push(v & 255);
const u16 = (v) => { u8(v); u8(v >> 8); };
const u32 = (v) => { u16(v & 0xFFFF); u16(v >>> 16); };
const align = () => { if (out.length & 1) u8(0); };
const SIZE = { 1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 7: 1 };

function valueBytes(type, vals) {
    const save = out.length;
    if (type === 2) { for (const c of vals) u8(c.charCodeAt(0)); u8(0); }
    else if (type === 3) vals.forEach(u16);
    else if (type === 4 || type === 5) vals.forEach(u32);
    else vals.forEach(u8);
    return out.splice(save);
}
function count(type, vals) {
    if (type === 2) return vals.length + 1;
    if (type === 5) return vals.length / 2;
    return vals.length;
}
// Appends the out-of-line values, then the IFD; returns its offset.
function writeIfd(entries) {
    entries.sort((a, b) => a[0] - b[0]);
    const where = entries.map(([, type, vals]) => {
        const bytes = valueBytes(type, vals);
        if (bytes.length <= 4) return -1;
        align();
        const at = out.length;
        out.push(...bytes);
        return at;
    });
    align();
    const ifd = out.length;
    u16(entries.length);
    entries.forEach(([tag, type, vals], i) => {
        u16(tag); u16(type); u32(count(type, vals));
        if (where[i] < 0) {
            const b = valueBytes(type, vals);
            while (b.length < 4) b.push(0);
            out.push(...b);
        } else {
            u32(where[i]);
        }
    });
    u32(0);
    return ifd;
}
function header() { out.length = 0; out.push(0x49, 0x49, 0x2A, 0); u32(0); }
function finish(ifd0) {
    out[4] = ifd0 & 255; out[5] = (ifd0 >> 8) & 255; out[6] = (ifd0 >> 16) & 255; out[7] = ifd0 >>> 24;
    return new Uint8Array(out);
}

const CAMERA = [[0x010F, 2, 'Canon'], [0x0110, 2, 'EOS R5']];
const EXIF = [[0x829A, 5, [1, 250]], [0x829D, 5, [28, 10]], [0x8827, 3, [400]],
              [0x9003, 2, '2024:05:06 14:30:00'], [0x9011, 2, '+02:00'], [0x920A, 5, [50, 1]],
              [0xA405, 3, [75]], [0xA434, 2, 'RF50mm F1.8 STM']];
const GPS = [[1, 2, 'N'], [2, 5, [40, 1, 26, 1, 4614, 100]], [3, 2, 'W'], [4, 5, [79, 1, 58, 1, 5604, 100]],
             [5, 1, [1]], [6, 5, [125, 10]]];

// IFD0 `image` entries (plus pixel data at `strip`), the camera tags,
// Orientation, and the Exif and GPS sub-IFDs.
function tiffWith(image, strip, orientation) {
    header();
    const entries = image.slice();
    if (strip) {
        entries.push([273, 4, [out.length]], [279, 4, [strip.length]]);
        out.push(...strip);
    }
    entries.push(...CAMERA, [0x0112, 3, [orientation]]);
    entries.push([0x8769, 4, [writeIfd(EXIF.slice())]]);
    entries.push([0x8825, 4, [writeIfd(GPS.slice())]]);
    return finish(writeIfd(entries));
}

// ── The TIFF image ───────────────────────────────────────────────────────────
const SW = 40, SH = 20;
const rgb = new Uint8Array(SW * SH * 3);
for (let y = 0; y < SH; y++) {
    for (let x = 0; x < SW; x++) {
        const i = (y * SW + x) * 3;
        if (x < SW / 2) rgb[i] = 255; else rgb[i + 2] = 255;
    }
}
// PackBits: each half-row of a colour is a repeat of one 3-byte pattern, so
// spell it as literal runs (header n-1, then n bytes) of at most 128 bytes,
// with a repeat run thrown in for the zero bytes of each row's first pixel.
const packed = [];
for (let y = 0; y < SH; y++) {
    const row = rgb.subarray(y * SW * 3, (y + 1) * SW * 3);
    packed.push(0, row[0]);       // literal 1: the first R
    packed.push(0xFF, 0);         // repeat 2: its G and B (zero)
    for (let i = 3; i < row.length; i += 117) {
        const n = Math.min(117, row.length - i);
        packed.push(n - 1, ...row.subarray(i, i + n));
    }
}
const tiff = tiffWith([[256, 4, [SW]], [257, 4, [SH]], [258, 3, [8, 8, 8]], [259, 3, [32773]],
                       [262, 3, [2]], [277, 3, [3]], [278, 4, [SH]]], packed, 6);

const isRed = (p) => p.r > 180 && p.g < 80 && p.b < 80;
const isBlue = (p) => p.b > 180 && p.r < 80 && p.g < 80;
const show = (p) => JSON.stringify(p);

// bro.image: probe, oriented decode.
const probe = bro.image.probeDimensions(tiff);
assert(probe && probe.width === SW && probe.height === SH && probe.channels === 3,
       'probeDimensions reads the TIFF header: ' + JSON.stringify(probe));
assert(bro.image.readExifOrientation(tiff) === 6, 'the TIFF IFD0 carries orientation 6');
const dec = bro.image.decodeOriented(tiff);
assert(dec.width === SH && dec.height === SW, 'decodeOriented turns the TIFF upright: ' + dec.width + 'x' + dec.height);
assert(dec.pixels[0] === 255 && dec.pixels[2] === 0 && dec.pixels[3] === 255, 'top-left is opaque red');
const last = (dec.width * dec.height - 1) * 4;
assert(dec.pixels[last] === 0 && dec.pixels[last + 2] === 255, 'bottom-right is blue');

// <img>: upright box, red above blue; image-orientation:none keeps the stored image.
const dir = path.join(os.tmpdir(), 'bro_img_tiff_' + process.pid + '_' + Date.now());
fs.mkdirSync(dir, { recursive: true });
const file = path.join(dir, 'rot6.tif').replace(/\\/g, '/');
fs.writeFileSync(file, tiff);

document.body.style.margin = '0';
document.body.style.background = 'rgb(0,255,0)';
document.body.innerHTML =
    '<img id="up" src="' + file + '" style="position:absolute;left:0;top:0">' +
    '<img id="raw" src="' + file + '" style="position:absolute;left:100px;top:0;image-orientation:none">';
flush();
const up = document.getElementById('up');
await up.decode();
flush();
assert(up.complete && up.naturalWidth === SH && up.naturalHeight === SW,
       'the TIFF <img> loaded upright: ' + up.naturalWidth + 'x' + up.naturalHeight);
let p = getPixel(10, 10), q = getPixel(10, 30);
assert(isRed(p) && isBlue(q), 'the TIFF <img> paints upright: top ' + show(p) + ', bottom ' + show(q));
const rr = document.getElementById('raw').getBoundingClientRect();
assert(Math.round(rr.width) === SW && Math.round(rr.height) === SH, 'none keeps the stored 40x20 box');
p = getPixel(110, 10); q = getPixel(130, 10);
assert(isRed(p) && isBlue(q), 'none paints the stored pixels: left ' + show(p) + ', right ' + show(q));

// ── readExif in every container ──────────────────────────────────────────────
const block = tiffWith([], null, 3);  // a bare EXIF block: no pixels

const jpegPlain = bro.image.encodeJpeg(new Uint8Array(8 * 8 * 3), 8, 8, 3, 90);
const app1 = [0xFF, 0xE1, 0, 0, 0x45, 0x78, 0x69, 0x66, 0, 0, ...block];
app1[2] = (app1.length - 2) >> 8; app1[3] = (app1.length - 2) & 255;
const jpeg = new Uint8Array([...jpegPlain.subarray(0, 2), ...app1, ...jpegPlain.subarray(2)]);

const le32 = (n) => [n & 255, (n >> 8) & 255, (n >> 16) & 255, n >>> 24];
const webpBody = [0x57, 0x45, 0x42, 0x50,
                  0x56, 0x50, 0x38, 0x58, ...le32(10), 0x08, 0, 0, 0, 7, 0, 0, 7, 0, 0,
                  0x45, 0x58, 0x49, 0x46, ...le32(block.length), ...block];
if (block.length & 1) webpBody.push(0);
const webp = new Uint8Array([0x52, 0x49, 0x46, 0x46, ...le32(webpBody.length), ...webpBody]);

const pngPlain = bro.image.encodePng(new Uint8Array(2 * 2 * 4), 2, 2, 4);
let crc = 0xFFFFFFFF;
const chunk = [0x65, 0x58, 0x49, 0x66, ...block];  // "eXIf" + data, what the CRC covers
for (const b of chunk) {
    crc ^= b;
    for (let k = 0; k < 8; k++) crc = (crc >>> 1) ^ (0xEDB88320 & -(crc & 1));
}
crc = (crc ^ 0xFFFFFFFF) >>> 0;
const be32 = (n) => [n >>> 24, (n >> 16) & 255, (n >> 8) & 255, n & 255];
const png = new Uint8Array([...pngPlain.subarray(0, 33), ...be32(block.length), ...chunk, ...be32(crc),
                            ...pngPlain.subarray(33)]);
assert(bro.image.probeDimensions(png).width === 2, 'the PNG with eXIf is still a PNG');

const close = (a, b) => typeof a === 'number' && Math.abs(a - b) < 1e-6;
function checkExif(bytes, what, orientation) {
    const e = bro.image.readExif(bytes);
    assert(e, what + ': readExif found the EXIF');
    const where = what + ' ' + JSON.stringify(e);
    assert(e.make === 'Canon' && e.model === 'EOS R5', where + ': make/model');
    assert(e.lensModel === 'RF50mm F1.8 STM' && !('lensMake' in e), where + ': lens (no LensMake tag)');
    assert(close(e.exposureTime, 0.004) && e.exposureTimeText === '1/250', where + ': exposure');
    assert(close(e.fNumber, 2.8) && e.iso === 400, where + ': f-number / ISO');
    assert(close(e.focalLength, 50) && e.focalLength35mm === 75, where + ': focal length');
    assert(e.dateTaken === '2024-05-06T14:30:00+02:00' && e.offsetTime === '+02:00', where + ': date taken');
    assert(close(e.latitude, 40 + 26 / 60 + 46.14 / 3600), where + ': latitude');
    assert(close(e.longitude, -(79 + 58 / 60 + 56.04 / 3600)), where + ': longitude (west is negative)');
    assert(close(e.altitude, -12.5), where + ': altitude (below sea level is negative)');
    assert(e.orientation === orientation, where + ': orientation');
    assert(bro.image.readExifOrientation(bytes) === orientation, what + ': readExifOrientation agrees');
}
checkExif(jpeg, 'JPEG', 3);
checkExif(tiff, 'TIFF', 6);
checkExif(webp, 'WebP', 3);
checkExif(png, 'PNG', 3);
assert(bro.image.readExif(jpegPlain) === null, 'a JPEG without EXIF reads as null');
assert(bro.image.readExif(file).make === 'Canon', 'readExif takes a path');

fs.rmSync(dir, { recursive: true, force: true });
console.log('PASS');
