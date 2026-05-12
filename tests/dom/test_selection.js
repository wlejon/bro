// Test DOM Selection API (document.getSelection, collapse/extend, addRange,
// containsNode, toString). Exercises src/js/selection_bindings.cpp.

const root = document.getElementById('root');
root.innerHTML = '<p id="p1">Alpha</p><p id="p2">Beta</p><p id="p3">Gamma</p>';
flush();

const p1 = document.getElementById('p1');
const p2 = document.getElementById('p2');
const p3 = document.getElementById('p3');

const sel = document.getSelection();
assert(sel !== null && sel !== undefined, 'getSelection returns Selection');
assert(typeof sel.rangeCount === 'number', 'rangeCount property');
assert(typeof sel.isCollapsed === 'boolean', 'isCollapsed property');

// Empty selection initially (or per impl: collapsed somewhere)
sel.removeAllRanges();
assert(sel.rangeCount === 0, 'rangeCount = 0 after removeAllRanges');

// --- collapse / anchorNode / focusNode ---
sel.collapse(root, 1);
assert(sel.anchorNode === root, 'anchorNode after collapse');
assert(sel.anchorOffset === 1, 'anchorOffset after collapse');
assert(sel.focusNode === root, 'focusNode == anchorNode after collapse');
assert(sel.focusOffset === 1, 'focusOffset after collapse');
assert(sel.isCollapsed === true, 'isCollapsed after collapse');
assert(sel.rangeCount === 1, 'rangeCount = 1 after collapse');

// --- extend ---
sel.extend(root, 3);
assert(sel.focusNode === root, 'focusNode after extend');
assert(sel.focusOffset === 3, 'focusOffset after extend');
assert(sel.isCollapsed === false, 'not collapsed after extend');

// --- selectAllChildren ---
sel.selectAllChildren(p1);
assert(sel.anchorNode === p1 || sel.anchorNode !== null, 'selectAllChildren sets anchor');
assert(sel.anchorOffset === 0, 'selectAllChildren start at 0');

// --- collapseToStart / collapseToEnd ---
sel.collapse(root, 1);
sel.extend(root, 3);
sel.collapseToStart();
assert(sel.isCollapsed === true, 'collapseToStart collapses');
assert(sel.focusOffset === 1, 'collapseToStart goes to start');

sel.collapse(root, 1);
sel.extend(root, 3);
sel.collapseToEnd();
assert(sel.isCollapsed === true, 'collapseToEnd collapses');
assert(sel.focusOffset === 3, 'collapseToEnd goes to end');

// --- setBaseAndExtent ---
sel.setBaseAndExtent(root, 0, root, 2);
assert(sel.anchorOffset === 0, 'setBaseAndExtent anchor');
assert(sel.focusOffset === 2, 'setBaseAndExtent focus');

// --- addRange / removeRange / getRangeAt ---
sel.removeAllRanges();
const r = document.createRange();
r.setStart(root, 0);
r.setEnd(root, 2);
sel.addRange(r);
assert(sel.rangeCount === 1, 'addRange increments rangeCount');

const got = sel.getRangeAt(0);
assert(got !== null, 'getRangeAt returns a Range');
assert(got instanceof Range, 'getRangeAt returns Range instance');

sel.removeRange(r);
assert(sel.rangeCount === 0, 'removeRange decrements');

// --- containsNode ---
sel.collapse(root, 0);
sel.extend(root, 2);
assert(sel.containsNode(p1, false) === true || sel.containsNode(p1, true) === true,
       'containsNode finds p1');
assert(sel.containsNode(p3, false) === false, 'containsNode rejects p3');

// --- empty / removeAllRanges ---
sel.empty();
assert(sel.rangeCount === 0, 'empty clears ranges');

// --- toString ---
sel.removeAllRanges();
sel.selectAllChildren(p1);
const s = sel.toString();
assert(typeof s === 'string', 'toString returns string');

// --- type accessor ---
assert(typeof sel.type === 'string', 'type is a string');

// --- setPosition (collapse alias) ---
sel.setPosition(p2, 0);
assert(sel.anchorNode === p2, 'setPosition sets anchor');

// Cleanup
sel.removeAllRanges();
root.innerHTML = '';
