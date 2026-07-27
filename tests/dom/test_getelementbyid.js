// getElementById against a tree that is being rewritten.
//
// The id lookup is a cache of a tree query, and every case here is one where
// the cache and the tree can disagree: two elements holding one id at the same
// time, an element that left the tree, an element that came back, an id handed
// from one element to another. The lookup is only allowed to answer with an
// element that is in the document and still wears the id.

const root = document.getElementById('root');
assert(root !== null, 'root element exists');

function clear() {
    while (root.firstChild) root.removeChild(root.firstChild);
}

// ── the id is registered when it is assigned ───────────────────────────────

const first = document.createElement('div');
first.id = 'target';
root.appendChild(first);
assert(document.getElementById('target') === first, 'appended element is found');

// ── an element outside the tree is not "in the document" ───────────────────

root.removeChild(first);
assert(document.getElementById('target') === null,
       'a detached element is not returned');

root.appendChild(first);
assert(document.getElementById('target') === first,
       're-attaching without touching the id attribute finds it again');

// ── build the replacement first, clear afterwards ──────────────────────────
//
// The pattern that motivated all of this: a redraw evaluates its new children
// (registering their ids) and only then drops the old ones. The removal of the
// old element must not evict the new element's registration — they only share
// a string.

const replacement = document.createElement('div');
replacement.id = 'target';                 // now two elements wear it
replacement.className = 'replacement';
root.appendChild(replacement);
root.removeChild(first);                   // the old one leaves

const found = document.getElementById('target');
assert(found === replacement, 'the surviving element is found, not the removed one');
assert(found.className === 'replacement', 'and it is the live node, not a stale copy');
assert(found.parentNode === root, 'the returned element is in the tree');

// A second round of the same, to catch a cache that goes stale one step later.
const third = document.createElement('div');
third.id = 'target';
third.className = 'third';
root.appendChild(third);
root.removeChild(replacement);
assert(document.getElementById('target') === third, 'still correct after a second redraw');
assert(document.getElementById('target').className === 'third', 'and it is the newest node');

clear();
assert(document.getElementById('target') === null, 'gone once nothing holds the id');

// ── duplicate ids resolve in document order ────────────────────────────────

const dupA = document.createElement('div');
dupA.id = 'dup';
dupA.className = 'a';
const dupB = document.createElement('div');
dupB.id = 'dup';
dupB.className = 'b';
root.appendChild(dupA);
root.appendChild(dupB);
assert(document.getElementById('dup') === dupA, 'first in document order wins');
root.removeChild(dupA);
assert(document.getElementById('dup') === dupB, 'the survivor answers once the first leaves');

clear();

// ── moving an id between elements ──────────────────────────────────────────

const holder = document.createElement('div');
holder.id = 'moving';
root.appendChild(holder);
const taker = document.createElement('div');
root.appendChild(taker);

taker.id = 'moving';                       // both hold it, holder is first
assert(document.getElementById('moving') === holder, 'document order still decides');
holder.id = 'renamed';
assert(document.getElementById('moving') === taker, 'the remaining holder answers');
assert(document.getElementById('renamed') === holder, 'the new id resolves');

taker.removeAttribute('id');
assert(document.getElementById('moving') === null, 'removeAttribute drops the claim');

clear();

// ── ids arriving and leaving through innerHTML ─────────────────────────────

root.innerHTML = '<div id="inner"><span id="deep">x</span></div>';
const inner = document.getElementById('inner');
assert(inner !== null, 'innerHTML registers ids');
assert(document.getElementById('deep') !== null, 'including nested ones');

root.innerHTML = '<div id="inner"><em id="deep">y</em></div>';
const rebuilt = document.getElementById('inner');
assert(rebuilt !== null, 'ids survive innerHTML replacing the same ids');
assert(rebuilt !== inner, 'and resolve to the new element');
assert(rebuilt.parentNode === root, 'which is the one in the tree');
assert(document.getElementById('deep').tagName === 'EM', 'nested ids follow too');

root.innerHTML = '';
assert(document.getElementById('inner') === null, 'and are gone when the markup is');

// ── replaceChild and replaceChildren ───────────────────────────────────────

const old = document.createElement('div');
old.id = 'swap';
root.appendChild(old);
const fresh = document.createElement('div');
fresh.id = 'swap';
fresh.className = 'fresh';
root.replaceChild(fresh, old);
assert(document.getElementById('swap') === fresh, 'replaceChild leaves the new element findable');
assert(document.getElementById('swap').className === 'fresh', 'and it is the new one');

const newer = document.createElement('div');
newer.id = 'swap';
newer.className = 'newer';
root.replaceChildren(newer);
assert(document.getElementById('swap') === newer, 'replaceChildren too');
assert(document.getElementById('swap').className === 'newer', 'and it is the newest');

clear();

// ── a document fragment's contents only count once inserted ────────────────

const frag = document.createDocumentFragment();
const inFrag = document.createElement('div');
inFrag.id = 'fragged';
frag.appendChild(inFrag);
assert(document.getElementById('fragged') === null,
       'an element in a fragment is not in the document');
root.appendChild(frag);
assert(document.getElementById('fragged') === inFrag, 'and is once the fragment is inserted');

clear();

// ── querySelector('#id') agrees with getElementById ────────────────────────

const sel = document.createElement('div');
sel.id = 'byselector';
root.appendChild(sel);
const selReplacement = document.createElement('div');
selReplacement.id = 'byselector';
root.appendChild(selReplacement);
root.removeChild(sel);
assert(document.querySelector('#byselector') === selReplacement,
       'querySelector agrees with getElementById');
assert(document.getElementById('byselector') === selReplacement, 'and vice versa');

clear();

// ── absent ids ─────────────────────────────────────────────────────────────

assert(document.getElementById('never-existed') === null, 'an unknown id is null');
assert(document.getElementById('') === null, 'the empty id is null');

console.log('PASS getElementById');
