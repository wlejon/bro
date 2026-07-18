// Text/comment node wrappers must not outlive the node they wrap.
//
// The wrappers are cached strongly in __bro_node_map, so they are never
// collected — but Document::freeNode + drainPendingFrees destroy the node
// underneath them. fireNodeFreed only invalidated *element* wrappers, so a
// held text wrapper kept a raw pointer into destroyed memory and the next
// `.data` / `.length` / `.parentNode` read it. With enough heap churn to
// reuse the block, that is a hard segfault.
//
// Post-fix the wrapper resolves through a generation-checked handle: once the
// node is gone it goes inert (null data, zero length, null parent) exactly
// like an invalidated element wrapper does.

const root = document.getElementById('root');

// ---------------------------------------------------------------------------
// Text nodes freed while JS holds their wrappers
// ---------------------------------------------------------------------------
const texts = [];
const divs = [];
for (let i = 0; i < 200; i++) {
    const d = document.createElement('div');
    d.textContent = 'payload-payload-payload-payload-' + i;
    root.appendChild(d);
    divs.push(d);
    texts.push(d.childNodes[0]);
}
flush();

assert(texts.length === 200, 'held 200 text wrappers');
assert(texts[0].data === 'payload-payload-payload-payload-0', 'text data readable while alive');
assert(texts[0].parentNode !== null, 'text has a parent while alive');

// replaceChildren() frees the text children outright (doc->freeNode).
for (const d of divs) d.replaceChildren();
flush();
flush();

// Churn the C++ heap hard so the freed TextNode blocks get reused. Pre-fix
// this is where the dangling reads turned into a crash.
for (let round = 0; round < 20; round++) {
    const tmp = [];
    for (let i = 0; i < 200; i++) {
        const d = document.createElement('div');
        d.textContent = 'churn-churn-churn-churn-churn-c' + i;
        root.appendChild(d);
        tmp.push(d);
    }
    flush();
    for (const d of tmp) d.replaceChildren();
    flush();
}
flush();

// Every held wrapper must now be inert — and reading them must not crash.
let inert = 0;
for (const t of texts) {
    if (t.data === null && t.length === 0 && t.parentNode === null) inert++;
    // Mutating a freed node must be a silent no-op, never a write to freed memory.
    t.data = 'ignored';
    t.appendData('ignored');
    t.deleteData(0, 5);
    assert(t.substringData(0, 5) === '', 'substringData on freed node is empty');
    assert(t.nextSibling === null, 'nextSibling of freed node is null');
    assert(t.previousSibling === null, 'previousSibling of freed node is null');
}
assert(inert === texts.length,
       'all ' + texts.length + ' freed text wrappers went inert (got ' + inert + ')');

// nodeType/nodeName are plain data properties captured at wrap time — they stay
// readable, which is fine: they never touch the node.
assert(texts[0].nodeType === 3, 'freed text wrapper still reports nodeType 3');

// ---------------------------------------------------------------------------
// Comment nodes take the same path
// ---------------------------------------------------------------------------
const host = document.createElement('div');
root.appendChild(host);
const comment = document.createComment('a comment with enough bytes to heap-allocate');
host.appendChild(comment);
flush();
assert(comment.data === 'a comment with enough bytes to heap-allocate', 'comment data readable');

host.replaceChildren();
flush();
flush();
assert(comment.data === null, 'freed comment wrapper went inert');
assert(comment.length === 0, 'freed comment wrapper reports zero length');
assert(comment.parentNode === null, 'freed comment wrapper has no parent');

// ---------------------------------------------------------------------------
// A live text node re-wrapped after unrelated frees still works — the fix must
// not make wrapping itself lossy.
// ---------------------------------------------------------------------------
const survivor = document.createElement('div');
survivor.textContent = 'still here';
root.appendChild(survivor);
flush();
const st = survivor.childNodes[0];
assert(st.data === 'still here', 'live text node still readable');
st.data = 'updated';
flush();
assert(st.data === 'updated', 'live text node still writable');
assert(survivor.textContent === 'updated', 'write reached the DOM');
assert(survivor.childNodes[0] === st, 'wrapper identity is still cached');

console.log('node wrapper invalidation: OK');
