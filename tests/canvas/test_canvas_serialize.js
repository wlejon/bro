// canvas.toDataURL() / canvas.toBlob() — HTMLCanvasElement serialization.
// Covers both backings: the 2D surface snapshot and a WebGL canvas's own FBO.

// --- 2D canvas -------------------------------------------------------------
var cv = document.createElement('canvas');
cv.width = 32;
cv.height = 16;
document.body.appendChild(cv);
var g = cv.getContext('2d');
g.fillStyle = '#ff0000';
g.fillRect(0, 0, 32, 16);
flush();

assert(typeof cv.toDataURL === 'function', 'canvas has toDataURL');
assert(typeof cv.toBlob === 'function', 'canvas has toBlob');

var png = cv.toDataURL();
assert(png.indexOf('data:image/png;base64,') === 0,
       'toDataURL() defaults to PNG, got: ' + png.slice(0, 40));
assert(png.length > 100, 'PNG data URL carries a payload (' + png.length + ' chars)');

// Base64 body must be well-formed: length a multiple of 4, alphabet only.
var body = png.slice('data:image/png;base64,'.length);
assert(body.length % 4 === 0, 'base64 body length is a multiple of 4');
assert(/^[A-Za-z0-9+/]+={0,2}$/.test(body), 'base64 body uses the standard alphabet');

// The PNG signature is \x89PNG\r\n\x1a\n — base64 of those 8 bytes starts "iVBORw0KGgo".
assert(body.indexOf('iVBORw0KGgo') === 0, 'decoded payload starts with the PNG signature');

// JPEG
var jpg = cv.toDataURL('image/jpeg');
assert(jpg.indexOf('data:image/jpeg;base64,') === 0,
       'toDataURL("image/jpeg") returns a JPEG, got: ' + jpg.slice(0, 40));
// JPEG SOI marker FF D8 FF -> base64 "/9j/"
assert(jpg.slice('data:image/jpeg;base64,'.length).indexOf('/9j/') === 0,
       'JPEG payload starts with the SOI marker');

var jpgLow = cv.toDataURL('image/jpeg', 0.1);
assert(jpgLow.length < jpg.length, 'lower quality produces a smaller JPEG');

// An unrecognised type falls back to PNG (HTML spec).
var weird = cv.toDataURL('image/tiff');
assert(weird.indexOf('data:image/png;base64,') === 0,
       'unknown MIME type falls back to PNG, got: ' + weird.slice(0, 40));

// Content actually round-trips: a red canvas and a blue one must not encode alike.
var cv2 = document.createElement('canvas');
cv2.width = 32; cv2.height = 16;
document.body.appendChild(cv2);
var g2 = cv2.getContext('2d');
g2.fillStyle = '#0000ff';
g2.fillRect(0, 0, 32, 16);
flush();
assert(cv2.toDataURL() !== png, 'different canvas content encodes differently');

// --- WebGL canvas ----------------------------------------------------------
var wc = document.createElement('canvas');
wc.width = 32;
wc.height = 16;
document.body.appendChild(wc);
var gl = wc.getContext('webgl2');
if (gl) {
    gl.clearColor(0, 1, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    flush();
    var wpng = wc.toDataURL();
    assert(wpng.indexOf('data:image/png;base64,') === 0,
           'WebGL canvas toDataURL() returns a PNG');
    assert(wpng.length > 100, 'WebGL PNG carries a payload');
    assert(wpng !== png, 'WebGL canvas encodes its own content, not the 2D one');

    // Reading the canvas back must not disturb the app's GL state: the very
    // next draw has to still land in the canvas, at full size.
    gl.clearColor(0, 0, 1, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    flush();
    var after = wc.toDataURL();
    assert(after !== wpng, 'GL state survives the readback — the next clear still takes effect');
}

// --- toBlob ----------------------------------------------------------------
var blobResult = 'pending';
cv.toBlob(function (b) { blobResult = b; });
assert(blobResult === 'pending', 'toBlob delivers its callback asynchronously');

// Drain the microtask queue.
Promise.resolve().then(function () {}).then(function () {
    assert(blobResult !== 'pending', 'toBlob callback ran');
    assert(blobResult !== null, 'toBlob produced a Blob');
    assert(blobResult.type === 'image/png', 'Blob carries the PNG type, got: ' + blobResult.type);
    assert(blobResult.size > 100, 'Blob has the encoded bytes (' + blobResult.size + ')');

    cv.toBlob(function (b) {
        assert(b.type === 'image/jpeg', 'toBlob honours the requested type, got: ' + b.type);
        console.log('PASS: canvas serialization');
    }, 'image/jpeg');
});

flush();
