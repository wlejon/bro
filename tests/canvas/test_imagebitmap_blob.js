// createImageBitmap(Blob), and the ImageBitmap interface object.
//
// A Blob is the spec's canonical source and the one every fetch-based image
// path produces — `fetch(url).then(r => r.blob()).then(createImageBitmap)` is
// how three.js's ImageBitmapLoader loads a texture, and therefore how
// GLTFLoader loads every texture in a model. bro rejected it outright
// ("unsupported or malformed source").
//
// `ImageBitmap` itself was not a global either, so `x instanceof ImageBitmap`
// threw or answered false. Code branches on that to tell a decoded bitmap from
// an <img> or a raw pixel object: three's texture serializer takes the
// unrecognised branch and refuses to save the texture, so an imported scene
// came back textureless after a reload.

const fs = require('fs');
const os = require('os');
const path = require('path');

assert(typeof ImageBitmap === 'function', 'ImageBitmap is a global');
let threw = false;
try { new ImageBitmap(); } catch (e) { threw = true; }
assert(threw, 'ImageBitmap is not directly constructible');

const PNG = path.join(os.tmpdir(), 'bro_ib_blob_' + Date.now() + '.png');
screenshot(PNG);
const bytes = fs.readFileSync(PNG);

await (async function () {
    const blob = new Blob([bytes], { type: 'image/png' });
    const bmp = await createImageBitmap(blob);
    assert(bmp instanceof ImageBitmap, 'the result is an ImageBitmap');
    assert(bmp.width > 1 && bmp.height > 1,
           'decoded to real dimensions, got ' + bmp.width + 'x' + bmp.height);

    // A File is a Blob, and it is what a drop hands the page.
    const file = new File([bytes], 'shot.png', { type: 'image/png' });
    const fromFile = await createImageBitmap(file);
    assert(fromFile.width === bmp.width && fromFile.height === bmp.height,
           'a File decodes the same as a Blob');

    // Crop applies to a blob source like any other.
    const cropped = await createImageBitmap(blob, 0, 0, 8, 6);
    assert(cropped.width === 8 && cropped.height === 6,
           'crop rect honoured, got ' + cropped.width + 'x' + cropped.height);

    // The bitmap is drawable — the point of decoding it.
    const canvas = document.createElement('canvas');
    canvas.width = bmp.width;
    canvas.height = bmp.height;
    const ctx = canvas.getContext('2d');
    ctx.drawImage(bmp, 0, 0);
    const url = canvas.toDataURL('image/png');
    assert(url.indexOf('data:image/png;base64,') === 0,
           'the drawn bitmap round-trips through toDataURL');

    // Undecodable bytes reject rather than producing a blank bitmap.
    let rejected = false;
    try {
        await createImageBitmap(new Blob(['not an image at all'],
                                         { type: 'image/png' }));
    } catch (e) { rejected = true; }
    assert(rejected, 'a blob that is not an image rejects');

    fs.unlinkSync(PNG);
    console.log('PASS: createImageBitmap from a Blob');
})();
