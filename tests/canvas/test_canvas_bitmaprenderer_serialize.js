// toDataURL / toBlob / texImage2D serialize a canvas's BITMAP, at the
// bitmap's own size — not the width/height attributes.
//
// A bitmaprenderer canvas displays the ImageBitmap it was handed at that
// bitmap's size, and its attributes do not resize it; the serializers read
// the attributes and asked the canvas worker for a snapshot of the wrong
// size, which came back as nothing ("data:," and a null blob). Here a 2x2
// canvas is handed a 3x1 bitmap and every reader must see 3x1.

// Width and height out of a PNG's IHDR (big-endian u32 at bytes 16 and 20).
function pngSize(bytes) {
    const u32 = (o) => ((bytes[o] << 24) | (bytes[o + 1] << 16) | (bytes[o + 2] << 8) | bytes[o + 3]) >>> 0;
    assert(bytes[1] === 0x50 && bytes[2] === 0x4e && bytes[3] === 0x47, 'payload is a PNG');
    return { width: u32(16), height: u32(20) };
}

function dataUrlBytes(url) {
    const b64 = url.slice(url.indexOf(',') + 1);
    const bin = atob(b64);
    const out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
}

const c = document.createElement('canvas');
c.setAttribute('width', '2');
c.setAttribute('height', '2');
document.body.appendChild(c);
flush();
const brc = c.getContext('bitmaprenderer');

const strip = new ImageData(new Uint8ClampedArray([
    255, 0, 0, 255,   0, 255, 0, 255,   0, 0, 255, 255,
]), 3, 1);
brc.transferFromImageBitmap(await createImageBitmap(strip));
flush();

assert(c.width === 2 && c.height === 2, 'the attributes are untouched by the transfer');

// ── toDataURL ────────────────────────────────────────────────────────────
const url = c.toDataURL();
assert(url.indexOf('data:image/png;base64,') === 0, 'toDataURL gives a PNG, got ' + url.slice(0, 30));
const s = pngSize(dataUrlBytes(url));
assert(s.width === 3 && s.height === 1, 'toDataURL encodes the 3x1 bitmap, got ' + s.width + 'x' + s.height);

const jpg = c.toDataURL('image/jpeg');
assert(jpg.indexOf('data:image/jpeg;base64,') === 0, 'JPEG too, got ' + jpg.slice(0, 30));

// The pixels round-trip: decode it back and look.
{
    const img = new Image();
    let loaded = false;
    img.onload = () => { loaded = true; };
    img.src = url;
    for (let i = 0; i < 20 && !loaded; i++) advanceTime(16);
    assert(loaded, 'the data URL decodes');
    assert(img.naturalWidth === 3 && img.naturalHeight === 1, 'decoded size 3x1');
    const r = document.createElement('canvas');
    r.width = 3; r.height = 1;
    const rctx = r.getContext('2d');
    rctx.drawImage(img, 0, 0);
    const d = rctx.getImageData(0, 0, 3, 1).data;
    assert(d[0] === 255 && d[1] === 0 && d[2] === 0, 'pixel 0 is red');
    assert(d[4] === 0 && d[5] === 255 && d[6] === 0, 'pixel 1 is green');
    assert(d[8] === 0 && d[9] === 0 && d[10] === 255, 'pixel 2 is blue');
}

// ── toBlob ───────────────────────────────────────────────────────────────
let blob = 'unset';
c.toBlob((b) => { blob = b; });
for (let i = 0; i < 10 && blob === 'unset'; i++) advanceTime(16);
assert(blob instanceof Blob, 'toBlob delivers a Blob, got ' + blob);
assert(blob.type === 'image/png', 'blob type is image/png, got ' + blob.type);
const bs = pngSize(new Uint8Array(await blob.arrayBuffer()));
assert(bs.width === 3 && bs.height === 1, 'toBlob encodes the 3x1 bitmap, got ' + bs.width + 'x' + bs.height);

// ── createImageBitmap(canvas) already used the bitmap size; still does ───
const ib = await createImageBitmap(c);
assert(ib.width === 3 && ib.height === 1, 'createImageBitmap(canvas) is 3x1, got ' + ib.width + 'x' + ib.height);

// ── a 2D canvas still serializes at its attribute size ───────────────────
const plain = document.createElement('canvas');
plain.width = 5; plain.height = 4;
document.body.appendChild(plain);
const pctx = plain.getContext('2d');
pctx.fillStyle = '#123456';
pctx.fillRect(0, 0, 5, 4);
flush();
const ps = pngSize(dataUrlBytes(plain.toDataURL()));
assert(ps.width === 5 && ps.height === 4, '2D canvas encodes at 5x4, got ' + ps.width + 'x' + ps.height);

console.log('bitmaprenderer serialization OK');
