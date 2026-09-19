// The Element members the port left as `undefined`, and the two attribute
// families whose reflection rules it flattened.
//
// `innerText`, `outerHTML`, `isConnected` and `getClientRects()` are on every
// element on the web, and a library that feature-detects one of them takes a
// worse path when it is missing. `href`/`download`/`target`/`rel` reflect an
// ATTRIBUTE only on the tags HTML gives them to — everywhere else they are
// ordinary expandos, which is why `div.download = fn` has to keep the
// function rather than stringify it into an attribute. And `<a href>` and
// `<img>` are draggable with no attribute at all.

const root = document.getElementById('root');

root.innerHTML = '<div id="box"><b>bold</b> and <i>italic</i></div>';
flush();
const box = document.getElementById('box');

// --- innerText ------------------------------------------------------------
assert(typeof box.innerText === 'string', 'innerText is a string, not undefined');
assert(box.innerText.indexOf('bold') >= 0, 'innerText includes nested text');
assert(box.innerText.indexOf('italic') >= 0, 'innerText includes all nested text');
assert(box.innerText.indexOf('<b>') < 0, 'innerText is text, not markup');
box.innerText = 'plain';
assert(box.innerText === 'plain', 'innerText writes replace the subtree');
assert(box.children.length === 0, 'and remove the element children');

// --- outerHTML ------------------------------------------------------------
box.innerHTML = '<b>x</b>';
const outer = box.outerHTML;
assert(outer.indexOf('<div') === 0, 'outerHTML starts with the element itself: ' + outer);
assert(outer.indexOf('id="box"') > 0, 'outerHTML carries the attributes');
assert(outer.indexOf('<b>x</b>') > 0, 'outerHTML carries the subtree');
assert(outer.lastIndexOf('</div>') === outer.length - 6, 'and closes the element');

const victim = document.createElement('span');
victim.textContent = 'before';
root.appendChild(victim);
victim.outerHTML = '<p id="after">after</p>';
flush();
assert(document.getElementById('after') !== null,
       'writing outerHTML replaces the element in its parent');

// --- isConnected ----------------------------------------------------------
const made = document.createElement('div');
assert(made.isConnected === false, 'a created element is not connected');
root.appendChild(made);
assert(made.isConnected === true, 'appending connects it');
made.remove();
assert(made.isConnected === false, 'removing disconnects it');

// --- getClientRects / scrollWidth / clientLeft ----------------------------
assert(typeof box.scrollWidth === 'number', 'scrollWidth is a number');
assert(typeof box.clientLeft === 'number', 'clientLeft is a number');
assert(typeof box.clientTop === 'number', 'clientTop is a number');
assert(typeof box.getClientRects === 'function', 'getClientRects is callable');
assert(typeof box.scrollBy === 'function', 'scrollBy is callable');

// --- href/download/target/rel: reflections where they belong --------------
const a = document.createElement('a');
root.appendChild(a);
a.href = 'notes.txt';
assert(a.getAttribute('href') === 'notes.txt', '<a>.href reflects the attribute');
a.download = 'saved.txt';
assert(a.getAttribute('download') === 'saved.txt', '<a>.download reflects the attribute');
a.target = '_blank';
assert(a.getAttribute('target') === '_blank', '<a>.target reflects the attribute');
a.rel = 'noopener';
assert(a.getAttribute('rel') === 'noopener', '<a>.rel reflects the attribute');
a.setAttribute('href', 'other.txt');
assert(a.href === 'other.txt', 'and the read goes back to the attribute');

// On a non-link tag they are plain expandos.
const div = document.createElement('div');
root.appendChild(div);
div.download = 'not-an-attribute';
assert(div.download === 'not-an-attribute', 'a <div> keeps what was assigned to .download');
assert(div.hasAttribute('download') === false,
       'and writes no download attribute');
const fn = function () { return 42; };
div.target = fn;
assert(typeof div.target === 'function', 'a non-string expando keeps its type');
assert(div.target() === 42, 'and stays callable');
div.href = { a: 1 };
assert(typeof div.href === 'object', 'href on a <div> is an expando too');
assert(div.hasAttribute('href') === false, 'and writes no href attribute');

// --- draggable defaults ---------------------------------------------------
const bareA = document.createElement('a');
assert(bareA.draggable === false, 'an <a> with no href is not draggable by default');
bareA.setAttribute('href', 'x');
assert(bareA.draggable === true, 'an <a> WITH href is draggable by default');
bareA.draggable = false;
assert(bareA.draggable === false, 'an explicit draggable=false wins');
assert(bareA.getAttribute('draggable') === 'false', 'and is written as an attribute');

const img = document.createElement('img');
assert(img.draggable === true, 'an <img> is draggable by default');
assert(div.draggable === false, 'a <div> is not');

// --- img.decode() ---------------------------------------------------------
assert(typeof img.decode === 'function', 'img.decode is callable');
let decodeRejected = false;
const p = img.decode();
assert(p && typeof p.then === 'function', 'img.decode returns a promise');
p.then(function () {}, function () { decodeRejected = true; });
flush();
assert(decodeRejected === true,
       'decoding an img with no pixels rejects rather than hanging forever');

root.innerHTML = '';
