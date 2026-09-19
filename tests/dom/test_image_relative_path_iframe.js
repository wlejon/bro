// A relative <img src> resolves against the DOCUMENT that owns the element,
// not against the app root: an <iframe src="iframe_child"> whose page says
// <img src="pic.png"> means iframe_child/pic.png. The port resolved every
// relative src against the app directory, so a sub-document's own images
// were missing while the host's loaded.

const fs = require('fs');
const path = require('path');

const NAME = 'tmp_subdoc_' + Date.now() + '.png';
const CHILD_DIR = path.join(bro.appDir, 'iframe_child');
const ABS = path.join(CHILD_DIR, NAME);
screenshot(ABS);
assert(fs.existsSync(ABS), 'wrote a PNG into the iframe child directory');

try {
    const frame = document.createElement('iframe');
    frame.setAttribute('src', 'iframe_child');
    frame.style.width = '200px';
    frame.style.height = '120px';
    document.body.appendChild(frame);
    flush();

    const sub = frame.contentDocument;
    assert(sub && typeof sub.createElement === 'function', 'iframe exposes its contentDocument');

    // --- relative to the sub-document: found ---
    const img = sub.createElement('img');
    img.src = NAME;
    sub.body.appendChild(img);
    flush();
    assert(img.complete === true, 'sub-document <img> settled');
    assert(img.naturalWidth > 0 && img.naturalHeight > 0,
           'sub-document <img src="' + NAME + '"> resolved under iframe_child/, got ' +
           img.naturalWidth + 'x' + img.naturalHeight);

    // --- the same bare name in the host document: not there ---
    const hostImg = document.createElement('img');
    hostImg.src = NAME;
    document.body.appendChild(hostImg);
    flush();
    assert(hostImg.naturalWidth === 0,
           'host document <img> with the same name does not find the child file (per-document base path)');

    // --- and the host's own app-relative path keeps working ---
    const hostRel = document.createElement('img');
    hostRel.src = 'iframe_child/' + NAME;
    document.body.appendChild(hostRel);
    flush();
    assert(hostRel.naturalWidth > 0, 'host document resolves iframe_child/<name> against the app dir');

    frame.remove();
    flush();
} finally {
    try { fs.unlinkSync(ABS); } catch (e) {}
}

console.log('image relative path per document OK');
