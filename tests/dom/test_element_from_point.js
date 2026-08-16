// document.elementFromPoint / elementsFromPoint (CSSOM View).
//
// The hit test the engine already runs for a click, reachable from JS. Layout
// code uses it to measure what it cannot ask for directly — CodeMirror probes
// the element at the edge of its scroller to derive the native scrollbar width
// — and a missing elementFromPoint is a TypeError that takes the page with it.

const root = document.getElementById('root');
root.innerHTML =
    '<div id="outer" style="position:absolute;left:40px;top:60px;width:300px;height:200px;background:#345">' +
    '  <div id="inner" style="position:absolute;left:20px;top:20px;width:100px;height:50px;background:#a33"></div>' +
    '</div>';
flush();

const outer = document.getElementById('outer');
const inner = document.getElementById('inner');

assert(typeof document.elementFromPoint === 'function', 'elementFromPoint exists');
assert(typeof document.elementsFromPoint === 'function', 'elementsFromPoint exists');

// --- the deepest element wins ---------------------------------------------
const ir = inner.getBoundingClientRect();
const hitInner = document.elementFromPoint(ir.left + ir.width / 2, ir.top + ir.height / 2);
assert(hitInner === inner,
       'centre of the inner box returns it, got ' +
       (hitInner ? '#' + hitInner.id + '/' + hitInner.tagName : 'null'));

// A point inside outer but outside inner stops at outer.
const or_ = outer.getBoundingClientRect();
const hitOuter = document.elementFromPoint(or_.left + or_.width - 10, or_.top + or_.height - 10);
assert(hitOuter === outer,
       'a point in outer only returns outer, got ' +
       (hitOuter ? '#' + hitOuter.id + '/' + hitOuter.tagName : 'null'));

// --- no layout of its own is required from the caller ----------------------
// An element appended and immediately probed must be found: the query flushes
// layout the same way getBoundingClientRect does.
const fresh = document.createElement('div');
fresh.id = 'fresh';
fresh.style.cssText = 'position:absolute;left:500px;top:60px;width:80px;height:80px;';
root.appendChild(fresh);
const fr = fresh.getBoundingClientRect();
assert(document.elementFromPoint(fr.left + 10, fr.top + 10) === fresh,
       'an element added this turn is already hittable');

// --- outside the viewport is null, per spec --------------------------------
assert(document.elementFromPoint(-5, 20) === null, 'negative x is null');
assert(document.elementFromPoint(20, -5) === null, 'negative y is null');
assert(document.elementFromPoint(window.innerWidth + 50, 20) === null,
       'past the right edge is null');
assert(document.elementFromPoint(20, window.innerHeight + 50) === null,
       'past the bottom edge is null');

// A point over nothing but the page itself still names an element (the
// document element), not null — it is inside the viewport.
const empty = document.elementFromPoint(window.innerWidth - 2, window.innerHeight - 2);
assert(empty !== null, 'an empty spot inside the viewport is not null');

// --- elementsFromPoint returns the chain, topmost first ---------------------
const list = document.elementsFromPoint(ir.left + ir.width / 2, ir.top + ir.height / 2);
assert(Array.isArray(list), 'elementsFromPoint returns an array');
assert(list.length >= 3, 'the chain has depth, got ' + list.length);
assert(list[0] === inner, 'topmost first');
assert(list[1] === outer, 'then its parent');
assert(list[list.length - 1] === document.documentElement,
       'and ends at <html>, got <' + list[list.length - 1].tagName + '>');
assert(document.elementsFromPoint(-5, -5).length === 0,
       'outside the viewport is an empty list');

// --- a detached document has nothing on screen to hit ----------------------
const dp = new DOMParser().parseFromString(
    '<html><body><div id="d" style="width:100px;height:100px"></div></body></html>', 'text/html');
assert(dp.elementFromPoint(10, 10) === null,
       'a DOMParser document is laid out nowhere, so the answer is null');

root.innerHTML = '';
console.log('PASS: elementFromPoint / elementsFromPoint');
