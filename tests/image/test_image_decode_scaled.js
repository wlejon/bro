// bro.image.decodeScaled / decodeExifThumbnail (docs/image-api.js): a JPEG
// decoded for a box at the IDCT scale that covers it, upright; the camera's
// EXIF thumbnail standing in when the box is that small, its bars cropped.
//
// The fixture is a 96x64 JPEG, left half red and right half blue, carrying
// EXIF orientation 6 (upright it is 64x96, red on top) and an IFD1 thumbnail:
// 40x30, solid green with black bars above and below, as cameras pad 3:2.

const fs = require('fs');
const path = require('path');
const os = require('os');

const W = 96, H = 64;
const rgb = new Uint8Array(W * H * 3);
for (let y = 0; y < H; y++)
    for (let x = 0; x < W; x++) {
        const i = (y * W + x) * 3;
        if (x < W / 2) rgb[i] = 255; else rgb[i + 2] = 255;
    }
const main = bro.image.encodeJpeg(rgb, W, H, 3, 95);

const TW = 40, TH = 30, content = 27, top = 1;
const trgb = new Uint8Array(TW * TH * 3);
for (let y = top; y < top + content; y++)
    for (let x = 0; x < TW; x++) trgb[(y * TW + x) * 3 + 1] = 230;
const thumb = bro.image.encodeJpeg(trgb, TW, TH, 3, 95);

// APP1: "Exif\0\0", a little-endian TIFF: IFD0 (Orientation 6) at 8, IFD1
// (the thumbnail's offset and length) at 26, the thumbnail at 56.
const tiff = [];
const u16 = (v) => tiff.push(v & 255, (v >> 8) & 255);
const u32 = (v) => { u16(v & 0xFFFF); u16(v >>> 16); };
tiff.push(0x49, 0x49, 0x2A, 0x00); u32(8);
u16(1); u16(0x0112); u16(3); u32(1); u16(6); u16(0); u32(26);
u16(2); u16(0x0201); u16(4); u32(1); u32(56); u16(0x0202); u16(4); u32(1); u32(thumb.length); u32(0);
const segLen = 2 + 6 + tiff.length + thumb.length;
const jpeg = new Uint8Array(main.length + 2 + segLen);
let o = 0;
jpeg.set(main.subarray(0, 2), o); o += 2;
jpeg.set([0xFF, 0xE1, segLen >> 8, segLen & 255, 0x45, 0x78, 0x69, 0x66, 0, 0], o); o += 10;
jpeg.set(tiff, o); o += tiff.length;
jpeg.set(thumb, o); o += thumb.length;
jpeg.set(main.subarray(2), o);
assert(bro.image.readExifOrientation(jpeg) === 6, 'the fixture carries orientation 6');

const px = (img, x, y) => {
    const i = (y * img.width + x) * 4;
    return { r: img.pixels[i], g: img.pixels[i + 1], b: img.pixels[i + 2] };
};
const isRed = (p) => p.r > 180 && p.g < 80 && p.b < 80;
const isBlue = (p) => p.b > 180 && p.r < 80 && p.g < 80;
const isGreen = (p) => p.g > 180 && p.r < 80 && p.b < 80;

function checkScaled(src, label) {
    // The image itself: no box.
    let r = bro.image.decodeScaled(src);
    assert(!r.error && r.width === 64 && r.height === 96, label + ': own size upright, got ' + r.width + 'x' + r.height);
    assert(r.path === 'full' && r.sourceWidth === 64 && r.sourceHeight === 96, label + ': path full, got ' + r.path);

    // 32x48 upright is 48x32 stored: the IDCT at 4/8.
    r = bro.image.decodeScaled(src, { width: 32, height: 48 });
    assert(r.width === 32 && r.height === 48 && r.channels === 4 && r.pixels.length === 32 * 48 * 4,
        label + ': 32x48, got ' + r.width + 'x' + r.height);
    assert(r.path === 'dct' && r.dctScale === 4, label + ': dct 4/8, got ' + r.path + ' ' + r.dctScale);
    assert(r.decodedWidth === 32 && r.decodedHeight === 48, label + ': decoded 32x48 upright');
    assert(isRed(px(r, 16, 8)) && isBlue(px(r, 16, 40)), label + ': upright, red on top: ' +
        JSON.stringify([px(r, 16, 8), px(r, 16, 40)]));

    // Cover: at least 48x48 -> 48x72 (6/8).
    r = bro.image.decodeScaled(src, { width: 48, height: 48, fit: 'cover' });
    assert(r.width === 48 && r.height === 72 && r.dctScale === 6, label + ': cover 48x72 at 6/8, got ' +
        r.width + 'x' + r.height + ' ' + r.dctScale);

    // Small enough for the EXIF thumbnail: green, no bars, upright 27x40 source.
    r = bro.image.decodeScaled(src, { width: 20, height: 20 });
    assert(r.path === 'exif-thumbnail', label + ': exif thumbnail, got ' + r.path);
    assert(r.width === 13 && r.height === 20, label + ': 13x20, got ' + r.width + 'x' + r.height);
    assert(r.decodedWidth === content && r.decodedHeight === TW, label + ': thumbnail cropped to 27x40');
    for (let x = 0; x < r.width; x++)
        assert(isGreen(px(r, x, 10)), label + ': no bar left at column ' + x + ': ' + JSON.stringify(px(r, x, 10)));

    // ... unless declined.
    r = bro.image.decodeScaled(src, { width: 20, height: 20, embedded: false });
    assert(r.path === 'dct' && r.dctScale === 2 && r.width === 13 && r.height === 20,
        label + ': declined thumbnail -> dct 2/8, got ' + r.path + ' ' + r.dctScale);
    assert(isRed(px(r, 6, 3)) && isBlue(px(r, 6, 16)), label + ': still upright');

    const t = bro.image.decodeExifThumbnail(src);
    assert(t && t.width === content && t.height === TW, label + ': decodeExifThumbnail 27x40 upright');
    assert(isGreen(px(t, 13, 0)) && isGreen(px(t, 13, TW - 1)), label + ': thumbnail edges are green');
}

checkScaled(jpeg, 'bytes');
const dir = path.join(os.tmpdir(), 'bro_decode_scaled_' + process.pid + '_' + Date.now());
fs.mkdirSync(dir, { recursive: true });
const file = path.join(dir, 'photo.jpg').replace(/\\/g, '/');
fs.writeFileSync(file, jpeg);
checkScaled(file, 'path');

// A JPEG without an EXIF thumbnail, and a PNG (decoded whole, then resized).
assert(bro.image.decodeExifThumbnail(main) === null, 'no thumbnail -> null');
const png = bro.image.encodePng(new Uint8Array(50 * 40 * 4).fill(200), 50, 40, 4);
let r = bro.image.decodeScaled(png, { width: 25 });
assert(r.width === 25 && r.height === 20 && r.path === 'full' && r.decodedWidth === 50, 'png resized whole');

// Failures.
r = bro.image.decodeScaled(new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]), { width: 10 });
assert(r.width === 0 && r.pixels.length === 0 && typeof r.error === 'string' && r.error.length > 0,
    'junk -> error: ' + r.error);
let threw = false;
try { bro.image.decodeScaled(jpeg, { width: 10, fit: 'stretch' }); } catch (e) { threw = e instanceof TypeError; }
assert(threw, "fit 'stretch' throws a TypeError");

assert(bro.image.canDecode('cr3') === 'probably' && bro.image.canDecode('image/x-canon-cr3') === 'probably',
    'CR3 decodes (its embedded JPEGs)');
fs.rmSync(dir, { recursive: true, force: true });
console.log('test_image_decode_scaled: ok');
