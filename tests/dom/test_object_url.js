// URL.createObjectURL — a blob: URL that actually resolves.
//
// brokit minted object URLs into a JS-side Map that nothing read: `<img src>`
// went looking for a file named "blob:brokit/1-…", and fetch() did the same. A
// page could name its own bytes and then find nothing would load them, which is
// the whole point of the API — it is how an imported model's embedded textures,
// a generated image, or a decoded buffer gets handed to a URL consumer.
//
// The bytes now live in a process-global table (src/util/object_url.h) that the
// native consumers read, and the Blob itself stays alive for fetch.

const os = require('os');
const path = require('path');
const fs = require('fs');

// A real PNG without checking a binary into the tree, same trick as
// tests/dom/test_image.js.
const PNG_PATH = path.join(os.tmpdir(), 'bro_object_url_' + Date.now() + '.png');
screenshot(PNG_PATH);
const pngBytes = fs.readFileSync(PNG_PATH);
assert(pngBytes.length > 100, 'the screenshot produced a PNG');

// ------------------------------------------------------------------ the URL
const blob = new Blob([pngBytes], { type: 'image/png' });
const url = URL.createObjectURL(blob);
assert(typeof url === 'string' && url.lastIndexOf('blob:', 0) === 0,
       'createObjectURL returns a blob: URL, got ' + url);
assert(URL.createObjectURL(blob) !== url, 'each call mints a distinct URL');

let threw = false;
try { URL.createObjectURL('not a blob'); } catch (e) { threw = true; }
assert(threw, 'createObjectURL rejects a non-Blob');

// ------------------------------------------------------- <img src="blob:…">
// The path three.js's TextureLoader takes: an ImageLoader sets src on a
// detached <img> and waits for load. A URL that resolves nowhere fires error
// with no size — a texture that silently never appears.
const img = new Image();
let loaded = false, errored = false;
img.onload = function () { loaded = true; };
img.onerror = function () { errored = true; };
img.src = url;

assert(loaded && !errored, 'an <img> loads from an object URL');
assert(img.naturalWidth > 1 && img.naturalHeight > 1,
       'and gets the image\'s real size, got ' +
       img.naturalWidth + 'x' + img.naturalHeight);

// In the document too, so the layout/paint side resolves it as well.
const root = document.getElementById('root');
root.innerHTML = '<img id="shot" src="' + url + '">';
flush();
const shown = document.getElementById('shot');
assert(shown.getBoundingClientRect().width > 1,
       'a laid-out <img> gets a box from an object URL, got ' +
       shown.getBoundingClientRect().width);

// ----------------------------------------------------------- fetch(blob:…)
// The path a glTF's .bin buffer takes.
const text = new Blob(['{"buffers":[1,2,3]}'], { type: 'application/json' });
const textURL = URL.createObjectURL(text);

await (async function () {
    const res = await fetch(textURL);
    assert(res.ok && res.status === 200, 'fetching an object URL succeeds');
    assert(res.headers.get('content-type') === 'application/json',
           'the Blob\'s type becomes the Content-Type, got ' +
           res.headers.get('content-type'));
    assert((await res.text()) === '{"buffers":[1,2,3]}', 'the body is the blob');

    const asJson = await (await fetch(textURL)).json();
    assert(asJson.buffers.length === 3, 'and it parses as JSON');

    const bin = await (await fetch(url)).arrayBuffer();
    assert(bin.byteLength === pngBytes.length,
           'binary comes back byte-for-byte, got ' + bin.byteLength +
           ' of ' + pngBytes.length);

    // ------------------------------------------------------------- revoke
    URL.revokeObjectURL(textURL);
    let rejected = false;
    try { await fetch(textURL); } catch (e) { rejected = true; }
    assert(rejected, 'fetching a revoked object URL fails');

    URL.revokeObjectURL(url);
    const dead = new Image();
    let deadErrored = false;
    dead.onerror = function () { deadErrored = true; };
    dead.src = url;
    assert(deadErrored && dead.naturalWidth === 0,
           'a revoked URL no longer loads as an image');

    fs.unlinkSync(PNG_PATH);
    root.innerHTML = '';
    console.log('PASS: object URLs resolve for <img> and fetch');
})();
