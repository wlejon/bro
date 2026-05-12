// Test DOM Range API (document.createRange, new Range, content manipulation,
// boundary queries, geometry, live-mutation tracking).

const root = document.getElementById('root');

// --- Construction ---
const r1 = document.createRange();
assert(r1 !== null && r1 !== undefined, 'createRange returns a Range');
assert(r1 instanceof Range, 'instance is a Range');
assert(r1.collapsed === true, 'fresh range is collapsed');
assert(r1.startOffset === 0, 'fresh range start offset 0');
assert(r1.endOffset === 0, 'fresh range end offset 0');

const r2 = new Range();
assert(r2 instanceof Range, 'new Range() works');

// Spec constants
assert(Range.START_TO_START === 0, 'START_TO_START');
assert(Range.END_TO_START === 3, 'END_TO_START');

// --- setStart / setEnd over a parent's children ---
root.innerHTML = '<p id="p1">first</p><p id="p2">second</p><p id="p3">third</p>';
flush();
const p1 = document.getElementById('p1');
const p2 = document.getElementById('p2');
const p3 = document.getElementById('p3');

const r = document.createRange();
r.setStart(root, 0);
r.setEnd(root, 3);
assert(r.startContainer === root, 'startContainer is root');
assert(r.endContainer === root, 'endContainer is root');
assert(r.startOffset === 0, 'start offset 0');
assert(r.endOffset === 3, 'end offset 3');
assert(r.collapsed === false, 'not collapsed');
assert(r.commonAncestorContainer === root, 'common ancestor is root');

// --- selectNode / selectNodeContents ---
r.selectNode(p2);
assert(r.startContainer === root, 'selectNode start container = parent');
assert(r.endContainer === root, 'selectNode end container = parent');
assert(r.startOffset === 1, 'selectNode start offset = node index');
assert(r.endOffset === 2, 'selectNode end offset = node index + 1');

r.selectNodeContents(p2);
assert(r.startContainer === p2, 'selectNodeContents container = node');
assert(r.startOffset === 0, 'selectNodeContents starts at 0');

// --- setStartBefore/After, setEndBefore/After ---
r.setStartBefore(p2);
assert(r.startContainer === root && r.startOffset === 1, 'setStartBefore');
r.setStartAfter(p2);
assert(r.startOffset === 2, 'setStartAfter');
r.setEndBefore(p3);
assert(r.endOffset === 2, 'setEndBefore');
r.setEndAfter(p3);
assert(r.endOffset === 3, 'setEndAfter');

// --- collapse ---
r.setStart(root, 0);
r.setEnd(root, 3);
r.collapse(true);
assert(r.collapsed === true, 'collapsed after collapse(true)');
assert(r.startOffset === 0 && r.endOffset === 0, 'collapse to start');

r.setStart(root, 0);
r.setEnd(root, 3);
r.collapse(false);
assert(r.startOffset === 3 && r.endOffset === 3, 'collapse to end');

// --- comparePoint / isPointInRange / intersectsNode ---
r.setStart(root, 1);
r.setEnd(root, 2);
assert(r.comparePoint(root, 0) === -1, 'comparePoint before = -1');
assert(r.comparePoint(root, 1) === 0, 'comparePoint at start = 0');
assert(r.comparePoint(root, 3) === 1, 'comparePoint after = 1');
assert(r.isPointInRange(root, 1) === true, 'isPointInRange inside');
assert(r.isPointInRange(root, 3) === false, 'isPointInRange outside');
assert(r.intersectsNode(p2) === true, 'intersectsNode within');
assert(r.intersectsNode(p1) === false, 'intersectsNode outside (before)');

// --- cloneRange ---
const clone = r.cloneRange();
assert(clone instanceof Range, 'cloneRange returns a Range');
assert(clone.startOffset === r.startOffset, 'clone start offset equal');
assert(clone.endOffset === r.endOffset, 'clone end offset equal');

// --- cloneContents (returns a DocumentFragment-ish node) ---
r.setStart(root, 0);
r.setEnd(root, 2);
const frag = r.cloneContents();
assert(frag !== null, 'cloneContents returns a fragment');
// Original DOM unchanged
assert(root.children.length === 3, 'cloneContents does not mutate source');

// --- extractContents removes from DOM ---
const r3 = document.createRange();
r3.setStart(root, 0);
r3.setEnd(root, 1);
const extracted = r3.extractContents();
assert(extracted !== null, 'extractContents returns fragment');
assert(root.children.length === 2, 'extractContents removed one child');
// p1 is no longer a child of root
let stillThere = false;
for (let i = 0; i < root.children.length; ++i) if (root.children[i].id === 'p1') stillThere = true;
assert(!stillThere, 'p1 no longer in root after extractContents');

// --- deleteContents ---
const r4 = document.createRange();
r4.setStart(root, 0);
r4.setEnd(root, 1);
r4.deleteContents();
assert(root.children.length === 1, 'deleteContents removed one child');
assert(document.getElementById('p2') === null, 'p2 deleted');

// --- insertNode ---
const newDiv = document.createElement('div');
newDiv.id = 'inserted';
newDiv.textContent = 'X';
const r5 = document.createRange();
r5.setStart(root, 0);
r5.collapse(true);
r5.insertNode(newDiv);
assert(document.getElementById('inserted') !== null, 'insertNode added to DOM');
assert(root.firstChild === newDiv || root.children[0] === newDiv,
       'insertNode placed at start');

// --- surroundContents ---
root.innerHTML = '<span id="a">A</span><span id="b">B</span><span id="c">C</span>';
flush();
const wrap = document.createElement('em');
const r6 = document.createRange();
r6.setStart(root, 0);
r6.setEnd(root, 2);
r6.surroundContents(wrap);
assert(wrap.children.length === 2, 'surroundContents wraps 2 nodes');
assert(wrap.parentNode === root, 'wrapper inserted in tree');

// --- toString on text ---
root.innerHTML = '<p>Hello World</p>';
flush();
const p = root.querySelector('p');
const r7 = document.createRange();
r7.selectNodeContents(p);
const str = r7.toString();
assert(typeof str === 'string', 'toString returns string');
assert(str.indexOf('Hello') !== -1, 'toString contains content');

// --- createContextualFragment ---
const r8 = document.createRange();
r8.setStart(root, 0);
const ctxFrag = r8.createContextualFragment('<span>x</span><span>y</span>');
assert(ctxFrag !== null, 'createContextualFragment returns fragment');

// --- Geometry ---
root.innerHTML = '<p id="geo">Hello geometry world</p>';
flush();
const geoP = document.getElementById('geo');
const r9 = document.createRange();
r9.selectNodeContents(geoP);
const rect = r9.getBoundingClientRect();
assert(typeof rect.width === 'number', 'getBoundingClientRect width');
assert(typeof rect.height === 'number', 'getBoundingClientRect height');

const rects = r9.getClientRects();
assert(typeof rects.length === 'number', 'getClientRects returns array-like');

// --- Live-mutation tracking ---
root.innerHTML = '<div id="live"><span>a</span><span>b</span><span>c</span></div>';
flush();
const live = document.getElementById('live');
const r10 = document.createRange();
r10.setStart(live, 1);
r10.setEnd(live, 3);
// Remove the first child: endpoints (offsets 1 and 3) should shift down by 1.
live.removeChild(live.children[0]);
assert(r10.startOffset === 0, 'endpoint shifts after removing earlier sibling, got ' + r10.startOffset);
assert(r10.endOffset === 2, 'endpoint shifts (end), got ' + r10.endOffset);

// detach is a no-op
r10.detach();

// --- Cleanup ---
root.innerHTML = '';
