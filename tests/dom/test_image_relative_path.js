// A relative image src must resolve against the app, not against whichever
// realm installed the image bindings last.
//
// install() runs once per realm — the app, then every system panel, then any
// <iframe> sub-document — and the base path used to be a single process-global
// that each install overwrote. System panels load after the app, so an app's own
// `<img src="images/x.png">` resolved to `<panel dir>/images/x.png`: every DOM
// image used as a canvas or WebGL source came back empty, with nothing but a
// load-failure warning naming a path the app never wrote.

const fs = require('fs');
const path = require('path');

// A real PNG inside the app directory, so the src below is genuinely relative.
const NAME = 'tmp_relpath_' + Date.now() + '.png';
const ABS = path.join(bro.appDir, NAME);
screenshot(ABS);
assert(fs.existsSync(ABS), 'wrote a PNG into the app directory');

try {
    // bro.resolvePath is the reference answer for app-relative resolution.
    assert(bro.resolvePath(NAME).indexOf(bro.appDir) === 0,
           'bro.resolvePath puts a relative path under the app dir, got: ' + bro.resolvePath(NAME));

    // --- new Image() (the decode-helper path) ---
    const helper = new Image();
    helper.src = NAME;
    assert(helper.complete === true, 'new Image() settled');
    assert(helper.width > 0 && helper.height > 0,
           'new Image() with a relative src decoded, got ' + helper.width + 'x' + helper.height);

    // --- DOM <img> (the element path) ---
    const img = document.createElement('img');
    img.src = NAME;
    flush();
    assert(img.complete === true, 'DOM <img> settled');
    assert(img.naturalWidth > 0 && img.naturalHeight > 0,
           'DOM <img> with a relative src probed its size, got ' +
           img.naturalWidth + 'x' + img.naturalHeight);

    // --- and the pixels actually reach a canvas ---
    const cv = document.createElement('canvas');
    cv.width = 32;
    cv.height = 32;
    document.body.appendChild(cv);
    const g = cv.getContext('2d');
    g.drawImage(img, 0, 0, 32, 32);
    flush();

    const data = g.getImageData(0, 0, 32, 32).data;
    let opaque = 0;
    for (let i = 0; i < data.length; i += 4) {
        if (data[i + 3] !== 0) opaque++;
    }
    assert(opaque > 0,
           'drawImage(<img> with a relative src) put pixels on the canvas, got ' +
           opaque + ' of 1024 non-transparent');

    // --- and reach a WebGL texture ---
    const wc = document.createElement('canvas');
    wc.width = 32;
    wc.height = 32;
    document.body.appendChild(wc);
    const gl = wc.getContext('webgl2');
    if (gl) {
        const tex = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, tex);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, img);
        const fb = gl.createFramebuffer();
        gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
        gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, tex, 0);
        assert(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE,
               'texImage2D(<img> with a relative src) produced a complete texture ' +
               '(incomplete means the upload never happened)');
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    }

    // A src that really is missing must still fail, rather than silently
    // resolving somewhere else that happens to hold a file.
    const missing = document.createElement('img');
    missing.src = 'definitely_not_here_' + Date.now() + '.png';
    flush();
    assert(missing.naturalWidth === 0, 'a genuinely missing image stays broken');

    console.log('PASS: relative image paths resolve against the app');
} finally {
    try { fs.unlinkSync(ABS); } catch (e) {}
}
