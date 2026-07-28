// replaceChildren() must leave the children it removed reattachable, matching
// removeChild, replaceChild and the WHATWG DOM spec: the old children are
// *removed*, not destroyed.
//
// Regression: replaceChildren called invalidateWrapper on every element child,
// so a reference held across the call became a dead wrapper — and appending it
// back was a silent no-op. No error, no exception, childNodes.length stayed
// zero. Holding a reference across a rebuild is an ordinary thing to do (a
// <video> or a <canvas> is expensive to remake), which is what made this one
// vicious: the failure surfaced nowhere near the call that caused it.

const root = document.getElementById('root');
root.innerHTML = '';

// ── a held child survives being replaced ───────────────────────────────────

const kept = document.createElement('div');
kept.id = 'kept';
kept.className = 'held';
kept.textContent = 'payload';
const inner = document.createElement('span');
inner.textContent = 'nested';
kept.appendChild(inner);

let clicks = 0;
kept.addEventListener('click', function () { clicks++; });

root.appendChild(kept);
flush();
assert(root.children.length === 1, 'kept is in the tree');

const replacement = document.createElement('p');
root.replaceChildren(replacement);
flush();

assert(root.children.length === 1, 'only the replacement is left');
assert(root.firstChild === replacement, 'and it is the one that was passed in');
assert(kept.parentNode === null, 'the old child is detached');

// Detached, but alive: everything it carried is still readable.
assert(kept.id === 'kept', 'id preserved across replaceChildren');
assert(kept.className === 'held', 'class preserved across replaceChildren');
assert(kept.tagName === 'DIV', 'tagName preserved across replaceChildren');
assert(kept.childNodes.length === 2, 'its own children came with it');
assert(kept.textContent === 'payloadnested', 'its text came with it');
assert(kept.children[0] === inner, 'and its element child is the same object');

// The whole point: it goes back in.
root.appendChild(kept);
flush();
assert(kept.parentNode === root, 'the held child reattached');
assert(root.children.length === 2, 'and the tree says so');
assert(root.children[1] === kept, 'in the position it was appended at');

// Listeners registered before the removal still fire after the reattach —
// they live on the wrapper, which is exactly what used to be thrown away.
kept.dispatchEvent(new Event('click', { bubbles: true }));
assert(clicks === 1, 'a listener registered before replaceChildren still fires');

// ── it is findable again once it is back in the document ──────────────────

assert(root.querySelector('#kept') === kept, 'querySelector finds the reattached child');
// getElementById does not follow a detach/reattach round trip: the id is
// unregistered on removal and appendChild does not re-register it. Same
// behaviour as removeChild + appendChild — see test_replace_child_reattach.js.

// ── while detached it must NOT answer getElementById ──────────────────────

root.innerHTML = '';
const tagged = document.createElement('div');
tagged.id = 'tagged';
root.appendChild(tagged);
flush();
assert(document.getElementById('tagged') === tagged, 'tagged is findable while attached');
root.replaceChildren();
flush();
assert(document.getElementById('tagged') === null,
       'a child removed by replaceChildren stops answering getElementById');
assert(tagged.tagName === 'DIV', 'even though the element itself is still alive');

// ── clearing is a structural change ───────────────────────────────────────
// replaceChildren() with no arguments only removes, so nothing in the append
// loop marks the parent dirty. The layout still has to be redone.

root.innerHTML = '';
const tall = document.createElement('div');
tall.style.height = '120px';
root.appendChild(tall);
flush();
const before = root.getBoundingClientRect().height;
assert(before >= 120, 'root grew to hold its child (got ' + before + ')');
root.replaceChildren();
flush();
const after = root.getBoundingClientRect().height;
assert(after < before, 'emptying via replaceChildren re-laid the parent out (got ' + after + ')');

// ── a swap round trip, both ways ──────────────────────────────────────────

root.innerHTML = '';
const a = document.createElement('div');
a.textContent = 'A';
const b = document.createElement('div');
b.textContent = 'B';
root.appendChild(a);
root.replaceChildren(b);
assert(a.parentNode === null && b.parentNode === root, 'a out, b in');
root.replaceChildren(a);
assert(b.parentNode === null && a.parentNode === root, 'b out, a back in');
assert(a.textContent === 'A', 'a survived two round trips intact');
assert(b.textContent === 'B', 'and so did b');

// ── several children at once, all of them held ────────────────────────────

root.innerHTML = '';
const many = [];
for (let i = 0; i < 8; i++) {
    const d = document.createElement('div');
    d.textContent = 'item-' + i;
    root.appendChild(d);
    many.push(d);
}
flush();
root.replaceChildren();
flush();
assert(root.children.length === 0, 'all eight removed');
for (let i = 0; i < many.length; i++) {
    assert(many[i].parentNode === null, 'item-' + i + ' is detached');
    assert(many[i].textContent === 'item-' + i, 'item-' + i + ' kept its text');
}
// Put them back in reverse and check the tree agrees.
for (let i = many.length - 1; i >= 0; i--) root.appendChild(many[i]);
flush();
assert(root.children.length === 8, 'all eight went back in');
assert(root.children[0] === many[7], 'in the order they were appended');
assert(root.children[7] === many[0], 'right down to the last one');

root.innerHTML = '';
console.log('replaceChildren reattach: OK');
