// innerHTML = / textContent = are the DOM's "replace all": ONE childList
// record naming every removed and every added node. `innerHTML = ''` and
// `textContent = ''` used to queue nothing, and `innerHTML = '<b>x</b>'`
// reported the insert without the removal. A removed text node stays a live
// node the record can hand out.

const root = document.getElementById('root');
const box = document.createElement('div');
root.appendChild(box);
box.innerHTML = 'one<i>two</i><!--c-->';
const oldText = box.firstChild;

let recs = [];
const mo = new MutationObserver((r) => { recs.push(...r); });
mo.observe(box, { childList: true, subtree: true });

function drain() {
    recs.push(...mo.takeRecords());
    const out = recs;
    recs = [];
    return out;
}

// innerHTML = '' — one record removing all three children.
box.innerHTML = '';
let r = drain();
assert(r.length === 1, 'innerHTML = "" queues one record, got ' + r.length);
assert(r[0].type === 'childList' && r[0].target === box, 'a childList record on the element');
assert(r[0].removedNodes.length === 3 && r[0].addedNodes.length === 0,
    `removes 3, adds 0; got ${r[0].removedNodes.length} / ${r[0].addedNodes.length}`);
assert(r[0].removedNodes[0] === oldText, 'the removed text node is the one the page held');
assert(oldText.data === 'one', 'and it is still a live node: ' + oldText.data);

// innerHTML on an empty element: added only; nested nodes are not separate records.
box.innerHTML = '<b>x<u>y</u></b>tail';
r = drain();
assert(r.length === 1, 'innerHTML = markup queues one record, got ' + r.length);
assert(r[0].addedNodes.length === 2 && r[0].removedNodes.length === 0,
    `adds 2, removes 0; got ${r[0].addedNodes.length} / ${r[0].removedNodes.length}`);
assert(r[0].addedNodes[0].tagName === 'B', 'the first added node is the <b>');

// Replacing: both lists in one record.
box.innerHTML = '<p>new</p>';
r = drain();
assert(r.length === 1 && r[0].removedNodes.length === 2 && r[0].addedNodes.length === 1,
    'replace: one record, 2 removed and 1 added, got ' +
    r.map((x) => x.removedNodes.length + '/' + x.addedNodes.length).join(','));

// textContent = '' and textContent = 'x'.
box.textContent = '';
r = drain();
assert(r.length === 1 && r[0].removedNodes.length === 1 && r[0].addedNodes.length === 0,
    'textContent = "" removes the child in one record');
box.textContent = 'hello';
r = drain();
assert(r.length === 1 && r[0].addedNodes.length === 1 && r[0].addedNodes[0].nodeType === 3,
    'textContent = text adds one text node');
box.textContent = 'again';
r = drain();
assert(r.length === 1 && r[0].type === 'childList' &&
       r[0].removedNodes.length === 1 && r[0].addedNodes.length === 1,
    'textContent over a text child is a childList replace, got ' +
    r.map((x) => x.type + ' ' + x.removedNodes.length + '/' + x.addedNodes.length).join(','));

// Nothing to replace, nothing to report.
box.textContent = '';
drain();
box.innerHTML = '';
r = drain();
assert(r.length === 0, 'emptying an empty element queues nothing, got ' + r.length);

mo.disconnect();
assert(box.textContent === '', 'final state');
