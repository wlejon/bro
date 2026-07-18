// Cross-document insertion adopts, per the DOM "pre-insert" algorithm.
//
// appendChild/insertBefore/replaceChild of a node whose ownerDocument differs
// from the target's must run "adopt" first: transfer ownership, retarget
// ownerDocument across the whole subtree. Previously importNode was the only
// path that worked — a node appended straight from a DOMParser document stayed
// owned by that document, so it rendered until the parser document was
// collected and then its holder destroyed a node sitting in the live tree.
// document.adoptNode() was a stub that only detached from the parent.

const root = document.getElementById('root');
const parser = new DOMParser();

// ---------------------------------------------------------------------------
// appendChild adopts a single element
// ---------------------------------------------------------------------------
const src = parser.parseFromString(
    '<html><body><div id="a" class="moved">alpha</div></body></html>', 'text/html');
const a = src.querySelector('#a');
assert(a.ownerDocument === src, 'starts owned by the parsed document');

root.appendChild(a);
flush();

assert(a.ownerDocument === document, 'appendChild retargeted ownerDocument');
assert(a.parentNode === root, 'node is in the live tree');
assert(document.getElementById('a') === a, 'id registered in the live document');
assert(src.getElementById('a') === null, 'id unregistered from the source document');
assert(a.textContent === 'alpha', 'content intact after adoption');

// It renders — the whole point of adopting rather than leaving it foreign.
assert(a.getBoundingClientRect().width > 0, 'adopted element has layout');

// ---------------------------------------------------------------------------
// A whole subtree is adopted, not just the root
// ---------------------------------------------------------------------------
const src2 = parser.parseFromString(
    '<html><body><section id="s"><p id="p1">one</p><p id="p2">two<!--c--></p>' +
    '</section></body></html>', 'text/html');
const s = src2.querySelector('#s');
const p1 = src2.querySelector('#p1');
const p2 = src2.querySelector('#p2');

root.appendChild(s);
flush();

assert(s.ownerDocument === document, 'subtree root adopted');
assert(p1.ownerDocument === document, 'descendant adopted');
assert(p2.ownerDocument === document, 'second descendant adopted');
assert(document.getElementById('p1') === p1, 'descendant id registered in target');
assert(src2.querySelector('#p1') === null, 'descendant gone from source document');
assert(s.getBoundingClientRect().width > 0, 'adopted subtree has layout');
assert(root.contains(p1), 'descendant is reachable from the live root');

// Text nodes inside the adopted subtree stay usable — their wrappers hold
// generation-checked handles naming the OLD document, so adoption has to
// re-point them or a live node would read as freed.
const t1 = p1.childNodes[0];
assert(t1.nodeType === 3, 'grabbed the text node');
assert(t1.data === 'one', 'adopted text node still readable');
t1.data = 'ONE';
flush();
assert(p1.textContent === 'ONE', 'adopted text node still writable');

// ---------------------------------------------------------------------------
// ownerDocument is a Node property, not an Element property. Text and Comment
// wrappers had no getter at all, so `t.ownerDocument` read `undefined` — which
// made adopted text nodes look un-adopted even though ownership had in fact
// moved correctly.
// ---------------------------------------------------------------------------
{
    const srcT = parser.parseFromString(
        '<html><body><div id="tw">text<!--cmt--></div></body></html>', 'text/html');
    const holder = srcT.querySelector('#tw');
    const txt = holder.childNodes[0];
    const cmt = holder.childNodes[1];
    assert(txt.nodeType === 3 && cmt.nodeType === 8, 'grabbed text + comment');
    assert(txt.ownerDocument === srcT, 'text node ownerDocument before adoption');
    assert(cmt.ownerDocument === srcT, 'comment node ownerDocument before adoption');

    root.appendChild(txt);
    flush();
    assert(txt.ownerDocument === document, 'text node ownerDocument after adoption');
    assert(txt.parentNode === root, 'adopted text node is in the live tree');
    assert(txt.data === 'text', 'adopted text node data intact');

    // Same-document nodes report the live document too.
    const localText = document.createTextNode('local');
    assert(localText.ownerDocument === document,
           'createTextNode ownerDocument is the live document');
    const localFrag = document.createDocumentFragment();
    assert(localFrag.ownerDocument === document,
           'createDocumentFragment ownerDocument is the live document');
}

// ---------------------------------------------------------------------------
// insertBefore and replaceChild adopt too
// ---------------------------------------------------------------------------
const src3 = parser.parseFromString(
    '<html><body><b id="ib">before</b><i id="rc">replaced</i></body></html>',
    'text/html');
const ib = src3.querySelector('#ib');
const rc = src3.querySelector('#rc');

root.insertBefore(ib, s);
flush();
assert(ib.ownerDocument === document, 'insertBefore adopted');
assert(root.childNodes[root.childNodes.length - 1] !== ib, 'insertBefore placed it before #s');

root.replaceChild(rc, ib);
flush();
assert(rc.ownerDocument === document, 'replaceChild adopted');
assert(rc.parentNode === root, 'replaceChild put the new node in the tree');
assert(ib.parentNode === null, 'replaced node left the tree');

// ---------------------------------------------------------------------------
// Explicit document.adoptNode()
// ---------------------------------------------------------------------------
const src4 = parser.parseFromString(
    '<html><body><div id="explicit"><span id="inner">x</span></div></body></html>',
    'text/html');
const ex = src4.querySelector('#explicit');
const inner = src4.querySelector('#inner');

const returned = document.adoptNode(ex);
assert(returned === ex, 'adoptNode returns the node');
assert(ex.ownerDocument === document, 'adoptNode retargeted ownerDocument');
assert(inner.ownerDocument === document, 'adoptNode retargeted the subtree');
assert(ex.parentNode === null, 'adoptNode detaches from the old parent');
assert(src4.getElementById('explicit') === null, 'gone from the source document');

// Adopted-but-not-inserted still inserts and renders normally afterwards.
root.appendChild(ex);
flush();
assert(ex.getBoundingClientRect().width > 0, 'adopted node renders once inserted');
assert(document.getElementById('inner') === inner, 'inner id registered after insertion');

// ---------------------------------------------------------------------------
// Adoption survives the source document being dropped. This is the crash the
// old stub set up: the parser document owned nodes living in the live tree.
// ---------------------------------------------------------------------------
let src5 = parser.parseFromString(
    '<html><body><div id="survivor">still here</div></body></html>', 'text/html');
const survivor = src5.querySelector('#survivor');
root.appendChild(survivor);
src5 = null;   // drop the only reference to the parsed document
for (let i = 0; i < 10; i++) flush();

assert(survivor.ownerDocument === document, 'survivor still owned by the live document');
assert(survivor.textContent === 'still here', 'survivor content intact after source died');
assert(survivor.parentNode === root, 'survivor still in the tree');
assert(survivor.getBoundingClientRect().width > 0, 'survivor still lays out');

// ---------------------------------------------------------------------------
// Same-document appendChild is unaffected (adopt is a no-op but still moves).
// ---------------------------------------------------------------------------
const local = document.createElement('div');
local.textContent = 'local';
root.appendChild(local);
flush();
assert(local.ownerDocument === document, 'local node ownerDocument unchanged');
const holderEl = document.createElement('div');
root.appendChild(holderEl);
holderEl.appendChild(local);
flush();
assert(local.parentNode === holderEl, 'same-document move still works');
assert(root.contains(local), 'moved node still under root');

console.log('adoptNode: OK');
