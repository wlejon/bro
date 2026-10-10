// navigator.clipboard.write / read and ClipboardItem: images on the clipboard.
//
// Headless keeps an in-process clipboard (platform::clipboard() — the real
// one is never touched by a test), so every round trip here is exact.

// ------------------------------------------------------------- the surface
assert(typeof ClipboardItem === 'function', 'ClipboardItem exists');
assert(typeof navigator.clipboard.write === 'function', 'navigator.clipboard.write exists');
assert(typeof navigator.clipboard.read === 'function', 'navigator.clipboard.read exists');
assert(ClipboardItem.supports('text/plain'), 'supports text/plain');
assert(ClipboardItem.supports('image/png'), 'supports image/png');
assert(!ClipboardItem.supports('image/bmp'), 'does not support image/bmp');
assert(!ClipboardItem.supports('application/x-made-up'), 'does not support a made-up type');

let threw = null;
try { new ClipboardItem({}); } catch (e) { threw = e; }
assert(threw instanceof TypeError, 'an empty ClipboardItem is a TypeError');

// A 3x2 image with distinct colours and partial alpha, so a channel swap, a
// row flip or a dropped alpha shows.
const W = 3, H = 2;
const pixels = new Uint8Array([
    255, 0, 0, 255,    0, 255, 0, 255,    0, 0, 255, 255,
    10, 20, 30, 128,   200, 100, 50, 64,  255, 255, 255, 0,
]);
const png = bro.image.encodePng(pixels, W, H, 4);
assert(png && png.length > 8, 'encodePng made a PNG');

async function blobBytes(blob) { return new Uint8Array(await blob.arrayBuffer()); }

function samePixels(bytes, what) {
    const img = bro.image.decodeOriented(bytes);
    assert(img.width === W && img.height === H,
           what + ': size ' + img.width + 'x' + img.height);
    for (let i = 0; i < pixels.length; ++i) {
        // A fully transparent pixel's colour is not the image's.
        if (pixels[(i & ~3) + 3] === 0 && (i & 3) !== 3) continue;
        assert(img.pixels[i] === pixels[i],
               what + ': byte ' + i + ' is ' + img.pixels[i] + ', want ' + pixels[i]);
    }
}

async function rejectsWith(promise, name, what) {
    let err = null;
    try { await promise; } catch (e) { err = e; }
    assert(err !== null, what + ' rejects');
    assert(err && err.name === name, what + ' rejects with ' + name + ', got ' + (err && err.name));
}

// ----------------------------------------------------- an image on its own
{
    await navigator.clipboard.writeText('before');
    const item = new ClipboardItem({ 'image/png': new Blob([png], { type: 'image/png' }) });
    assert(item.types.length === 1 && item.types[0] === 'image/png', 'item.types');
    await navigator.clipboard.write([item]);

    const items = await navigator.clipboard.read();
    assert(Array.isArray(items) && items.length === 1, 'read gives one item, got ' + items.length);
    const got = items[0];
    assert(got instanceof ClipboardItem, 'read gives ClipboardItems');
    assert(got.types.join(',') === 'image/png',
           'an image-only write reads back as image/png alone, got ' + got.types.join(','));
    const blob = await got.getType('image/png');
    assert(blob instanceof Blob && blob.type === 'image/png', 'getType gives an image/png Blob');
    samePixels(await blobBytes(blob), 'image-only read');

    await rejectsWith(got.getType('text/plain'), 'NotFoundError', 'getType for an absent type');
    const text = await navigator.clipboard.readText();
    assert(text === '', 'readText after an image-only write is "", got ' + JSON.stringify(text));
}

// ------------------------------------------- text and an image in one item
{
    const item = new ClipboardItem({
        'text/plain': 'hello clipboard',
        'image/png': Promise.resolve(new Blob([png], { type: 'image/png' })),
    });
    // getType on a string value is a Blob of that type.
    const tb = await item.getType('text/plain');
    assert(tb instanceof Blob && tb.type === 'text/plain', 'a string value is a text/plain Blob');
    await navigator.clipboard.write([item]);

    const items = await navigator.clipboard.read();
    assert(items.length === 1, 'one item');
    const types = items[0].types.slice().sort().join(',');
    assert(types === 'image/png,text/plain', 'both representations read back, got ' + types);
    const t = await (await items[0].getType('text/plain')).text();
    assert(t === 'hello clipboard', 'the text representation, got ' + JSON.stringify(t));
    samePixels(await blobBytes(await items[0].getType('image/png')), 'text+image read');
    assert(await navigator.clipboard.readText() === 'hello clipboard',
           'readText sees the text of a text+image item');
}

// ------------------------------------------- writeText replaces the image
{
    await navigator.clipboard.writeText('just text');
    const items = await navigator.clipboard.read();
    assert(items.length === 1 && items[0].types.join(',') === 'text/plain',
           'after writeText only text/plain, got ' + (items[0] && items[0].types.join(',')));
}

// ------------------------------------------------------- what write refuses
{
    await rejectsWith(navigator.clipboard.write([
        new ClipboardItem({ 'application/x-made-up': 'x' })]), 'NotAllowedError', 'an unsupported type');
    await rejectsWith(navigator.clipboard.write([
        new ClipboardItem({ 'text/plain': 'a' }), new ClipboardItem({ 'text/plain': 'b' })]),
        'NotAllowedError', 'two items');
    await rejectsWith(navigator.clipboard.write([
        new ClipboardItem({ 'image/png': new Blob(['not a png'], { type: 'image/png' }) })]),
        'DataError', 'image/png bytes that are not a PNG');
    assert(await navigator.clipboard.readText() === 'just text',
           'a refused write leaves the clipboard as it was');
}

// ----------------------- an image held as a bitmap reads back as image/png
// What a Windows screenshot is (CF_DIB, image/bmp on the platform side): put
// one on the in-process clipboard through the primitive write() uses, as a
// hand-built 32-bit bottom-up BMP, and read() must answer with a PNG of the
// same pixels.
{
    const rowBytes = W * 4, pixelBytes = rowBytes * H, off = 14 + 40;
    const bmp = new Uint8Array(off + pixelBytes);
    const dv = new DataView(bmp.buffer);
    bmp[0] = 0x42; bmp[1] = 0x4D;
    dv.setUint32(2, bmp.length, true);
    dv.setUint32(10, off, true);
    dv.setUint32(14, 40, true);
    dv.setInt32(18, W, true);
    dv.setInt32(22, H, true);
    dv.setUint16(26, 1, true);
    dv.setUint16(28, 32, true);
    dv.setUint32(30, 0, true);           // BI_RGB
    dv.setUint32(34, pixelBytes, true);
    for (let y = 0; y < H; ++y) {
        for (let x = 0; x < W; ++x) {
            const s = (y * W + x) * 4, d = off + (H - 1 - y) * rowBytes + x * 4;
            bmp[d] = pixels[s + 2]; bmp[d + 1] = pixels[s + 1];
            bmp[d + 2] = pixels[s]; bmp[d + 3] = pixels[s + 3];
        }
    }
    assert(navigator.clipboard.__writeItems(['image/bmp'], [bmp.buffer]), 'a bitmap on the clipboard');
    const items = await navigator.clipboard.read();
    assert(items.length === 1 && items[0].types.join(',') === 'image/png',
           'a bitmap reads as image/png, got ' + (items[0] && items[0].types.join(',')));
    const bytes = await blobBytes(await items[0].getType('image/png'));
    assert(bytes[0] === 0x89 && bytes[1] === 0x50 && bytes[2] === 0x4E && bytes[3] === 0x47,
           'the image/png representation is a PNG');
    samePixels(bytes, 'bitmap converted to PNG');
}

// ------------------------------- Ctrl+V with an image: the paste event's file
// What a paste handler written for Chromium reads: types has "Files", files
// holds an image.png File, items a {kind: 'file'} entry.
{
    await navigator.clipboard.write([new ClipboardItem({
        'text/plain': 'caption',
        'image/png': new Blob([png], { type: 'image/png' }),
    })]);
    let ev = null;
    const onPaste = (e) => { ev = e; };
    document.addEventListener('paste', onPaste);
    keyDown(118 /* v */, 0, 0x0040 /* LCTRL */);
    keyUp(118, 0, 0x0040);
    flush();
    document.removeEventListener('paste', onPaste);
    assert(ev !== null, 'Ctrl+V dispatched paste');
    const dt = ev.clipboardData;
    assert(dt.getData('text/plain') === 'caption', 'paste text, got ' + dt.getData('text/plain'));
    assert(Array.from(dt.types).indexOf('Files') !== -1, 'paste types has Files: ' + Array.from(dt.types));
    assert(dt.files.length === 1, 'one pasted file, got ' + dt.files.length);
    const f = dt.files[0];
    assert(f instanceof File && f.type === 'image/png' && f.name === 'image.png',
           'the pasted file is image.png, got ' + f.name + ' ' + f.type);
    samePixels(await blobBytes(f), 'pasted file');
    const kinds = Array.from(dt.items).map((it) => it.kind + ':' + it.type).join(',');
    assert(kinds === 'string:text/plain,file:image/png', 'paste items, got ' + kinds);
    const viaItem = Array.from(dt.items)[1].getAsFile();
    assert(viaItem && viaItem.size === f.size, 'items[1].getAsFile() is the same file');
}

// --------------------------------------------- an empty clipboard reads []
{
    assert(navigator.clipboard.__writeItems([], []), 'clear');
    const items = await navigator.clipboard.read();
    assert(Array.isArray(items) && items.length === 0, 'an empty clipboard reads as []');
}
