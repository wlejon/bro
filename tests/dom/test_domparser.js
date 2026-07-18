// DOMParser().parseFromString returns a REAL detached bro::dom::Document —
// parsed by the same gumbo path as the app document, exposed through the same
// Document/Element wrapper classes — not the old duck-typed polyfill object.
// Detached means: no layout, no rendering, no engine attachment, and (the old
// polyfill's worst bug) NO leakage into the main document.

const root = document.getElementById('root');

// ---- realness -------------------------------------------------------------
const parser = new DOMParser();
assert(typeof DOMParser === 'function', 'DOMParser is a constructor');
const doc = parser.parseFromString(
    '<html><head><title>Parsed Title</title></head>' +
    '<body><div id="x" class="c">hi</div><p class="c">there</p></body></html>',
    'text/html');

assert(typeof Document === 'function', 'Document global exposed');
assert(doc instanceof Document, 'parsed doc instanceof Document');
assert(document instanceof Document, 'main document instanceof Document too');
assert(doc.nodeType === 9, 'nodeType 9');
assert(doc !== document, 'a NEW document, not the realm document');

// Structure: gumbo normalizes to html/head/body.
assert(doc.documentElement && doc.documentElement.tagName.toUpperCase() === 'HTML',
       'documentElement is <html>');
assert(doc.head && doc.head.tagName.toUpperCase() === 'HEAD', 'head present');
assert(doc.body && doc.body.tagName.toUpperCase() === 'BODY', 'body present');
assert(doc.title === 'Parsed Title', 'title from parsed head, got: ' + doc.title);

// ---- queries --------------------------------------------------------------
const x = doc.querySelector('#x');
assert(x && x.textContent === 'hi', 'querySelector + textContent');
assert(doc.getElementById('x') === x, 'getElementById returns the same wrapper');
assert(doc.querySelectorAll('.c').length === 2, 'querySelectorAll count');
assert(doc.getElementsByTagName('p').length === 1, 'getElementsByTagName');
assert(x.ownerDocument === doc, 'ownerDocument is the parsed doc, not the realm doc');
assert(doc.body.innerHTML.indexOf('<div id="x"') !== -1, 'innerHTML get on parsed body');

// ---- NO leakage into the main document (old polyfill parsed into the host
// ---- doc, so #x was findable globally) ------------------------------------
assert(document.getElementById('x') === null, 'parsed ids do NOT leak into main doc');
assert(document.querySelector('.c') === null, 'parsed classes do NOT leak into main doc');

// ---- mutation of the parsed tree ------------------------------------------
const span = doc.createElement('span');
span.textContent = 'added';
doc.body.appendChild(span);
assert(doc.body.children.length === 3, 'appendChild grew children to 3');
assert(doc.querySelector('span').textContent === 'added', 'created node queryable');
x.innerHTML = '<b>bold</b>';
assert(x.querySelector('b') && x.querySelector('b').textContent === 'bold',
       'innerHTML set re-parses inside the detached doc');
x.setAttribute('data-k', 'v');
assert(x.getAttribute('data-k') === 'v', 'attributes work');
const tn = doc.createTextNode('txt');
span.appendChild(tn);
assert(span.textContent === 'addedtxt', 'createTextNode + append');

// Main document is unaffected by all of the above.
assert(document.querySelector('b') === null, 'mutations stay in the detached doc');

// ---- fragment / non-full-document input -----------------------------------
const frag = parser.parseFromString('<p>lone</p>', 'text/html');
assert(frag.body && frag.body.children.length === 1 &&
       frag.body.firstElementChild.tagName.toUpperCase() === 'P',
       'bare fragment lands in body');

// ---- XML mime types keep best-effort HTML behavior (no regression) --------
const svgDoc = parser.parseFromString('<svg><rect/></svg>', 'image/svg+xml');
assert(svgDoc && svgDoc.nodeType === 9, 'image/svg+xml returns a document');
assert(svgDoc.querySelector('svg') !== null, 'svg element reachable');

// ---- XMLSerializer round-trip still works ---------------------------------
const ser = new XMLSerializer().serializeToString(frag);
assert(typeof ser === 'string' && ser.indexOf('lone') !== -1,
       'XMLSerializer serializes a parsed document, got: ' + ser);

// ---- importNode into the main document (the sanctioned migration path) ----
root.innerHTML = '';
const imported = document.importNode(doc.querySelector('#x'), true);
root.appendChild(imported);
flush();
assert(document.querySelector('#x b').textContent === 'bold',
       'importNode(deep) clones the parsed subtree into the live doc');
root.innerHTML = '';
flush();

// ---- two parses are independent documents ---------------------------------
const d1 = parser.parseFromString('<div id="same">one</div>', 'text/html');
const d2 = parser.parseFromString('<div id="same">two</div>', 'text/html');
assert(d1.getElementById('same').textContent === 'one', 'doc 1 independent');
assert(d2.getElementById('same').textContent === 'two', 'doc 2 independent');

// ---- lifetime: a held child keeps its (dropped) document alive ------------
let keeper = (() => {
    const d = parser.parseFromString('<div id="kept"><i>deep</i></div>', 'text/html');
    return d.getElementById('kept');   // only the child survives this scope
})();
// Force several GC passes (headless runs GC + orphan sweep every ~1s virtual).
for (let i = 0; i < 4; i++) advanceTime(1100);
assert(keeper.textContent === 'deep', 'held child keeps its detached doc alive');
assert(keeper.ownerDocument instanceof Document, 'ownerDocument still resolves');
assert(keeper.ownerDocument.querySelector('i').textContent === 'deep',
       'doc reachable back through the held child');
keeper = null;

// ---- GC: parse in a loop, drop everything — no crash, no leak -------------
// (Debug builds assert on QuickJS leaks at exit; this loop plus the drops
// above is the coverage.)
for (let i = 0; i < 50; i++) {
    const d = parser.parseFromString(
        '<div class="g">' + i + '</div><span>s</span>', 'text/html');
    if (i % 2 === 0) d.querySelector('.g').textContent += '!'; // touch wrappers
    if (i % 10 === 0) advanceTime(1100);
}
for (let i = 0; i < 4; i++) advanceTime(1100);

// Main document still fully functional afterwards.
root.innerHTML = '<div id="alive">ok</div>';
flush();
assert(document.getElementById('alive').textContent === 'ok', 'main doc intact');
root.innerHTML = '';

console.log('test_domparser OK');
