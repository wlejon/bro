// <a download>: saving bytes out of the page. A page builds a Blob, points an
// anchor at an object URL for it and clicks the anchor — the only "save this
// file" path the web has.

const fs = require('fs');

function cleanup(p) {
    try { fs.unlinkSync(p); } catch (e) { /* never existed */ }
}

// ---- element.click() ------------------------------------------------------
const blob = new Blob(['{"hello":"world"}'], { type: 'application/json' });
const url = URL.createObjectURL(blob);

const a = document.createElement('a');
a.href = url;                       // reflected to the attribute...
a.download = 'bro-test-download.json';
assert(a.getAttribute('href') === url, 'href reflects to the attribute');
assert(a.getAttribute('download') === 'bro-test-download.json',
       'download reflects to the attribute');

document.body.appendChild(a);
a.click();

let saved = lastDownload();
assert(saved !== null, 'a download happened');
assert(saved.indexOf('bro-test-download') >= 0, 'named by the download attribute: ' + saved);
const dirOf = p => p.slice(0, Math.max(p.lastIndexOf('/'), p.lastIndexOf(String.fromCharCode(92))));
const downloadsDir = dirOf(saved);
assert(fs.readFileSync(saved, 'utf8') === '{"hello":"world"}', 'the bytes are the blob’s');
cleanup(saved);

// ---- dispatchEvent(new MouseEvent('click')) -------------------------------
// Activation behavior runs for a dispatched event too, which is how a good
// deal of export code triggers its own download.
const b = document.createElement('a');
b.href = URL.createObjectURL(new Blob(['second'], { type: 'text/plain' }));
b.download = 'bro-test-download2.txt';
document.body.appendChild(b);
b.dispatchEvent(new MouseEvent('click'));

const saved2 = lastDownload();
assert(saved2 !== saved, 'the dispatched click downloaded too');
assert(fs.readFileSync(saved2, 'utf8') === 'second', 'second file has its own bytes');
cleanup(saved2);

// ---- preventDefault stops it ----------------------------------------------
const c = document.createElement('a');
c.href = URL.createObjectURL(new Blob(['nope'], { type: 'text/plain' }));
c.download = 'bro-test-download3.txt';
c.addEventListener('click', e => e.preventDefault());
document.body.appendChild(c);
c.click();
assert(lastDownload() === saved2, 'a cancelled click downloads nothing');

// ---- a plain link is not a download ---------------------------------------
const d = document.createElement('a');
d.href = URL.createObjectURL(new Blob(['plain'], { type: 'text/plain' }));
document.body.appendChild(d);
d.click();
assert(lastDownload() === saved2, 'an anchor without [download] downloads nothing');

// ---- the name is sanitized ------------------------------------------------
// The download attribute is page-controlled text: it names a file in the
// downloads folder, never a path.
const e = document.createElement('a');
e.href = URL.createObjectURL(new Blob(['escaped'], { type: 'text/plain' }));
e.download = '../../evil.txt';
document.body.appendChild(e);
e.click();
const saved3 = lastDownload();
assert(saved3 !== saved2, 'the escaping name still downloaded');
assert(dirOf(saved3) === downloadsDir,
       'the file lands in the downloads folder, not up a level: ' + saved3);
cleanup(saved3);
