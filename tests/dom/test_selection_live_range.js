// getRangeAt() returns the selection's live range, not a copy: mutating it
// moves the selection, and addRange() of a range makes that range the
// selection's own. surroundContents on the live range leaves the selection on
// the new wrapper, which is what a "bold" toolbar button relies on.

const root = document.getElementById('root');
root.innerHTML = '<p id="p" style="font:20px Arial">alpha beta gamma</p>';
flush();
const p = document.getElementById('p');
const t = p.firstChild;
const sel = getSelection();

sel.setBaseAndExtent(t, 0, t, 5);
const r = sel.getRangeAt(0);
assert(sel.getRangeAt(0) === r, 'getRangeAt returns the same object each time');
r.setEnd(t, 10);
assert(String(sel) === 'alpha beta', 'mutating the live range moves the selection, got ' + JSON.stringify(String(sel)));

const own = document.createRange();
own.setStart(t, 6);
own.setEnd(t, 10);
sel.removeAllRanges();
sel.addRange(own);
assert(sel.getRangeAt(0) === own, 'addRange makes the range the selection\'s own');
assert(r.toString() === 'alpha beta', 'the old live range is detached, not reset');
own.setStart(t, 0);
assert(String(sel) === 'alpha beta', 'the added range stays live');

own.setStart(t, 6);
own.surroundContents(document.createElement('b'));
const b = p.querySelector('b');
assert(b && b.textContent === 'beta', 'surroundContents wrapped the text');
assert(String(sel) === 'beta', 'the selection follows the wrapped range, got ' + JSON.stringify(String(sel)));
assert(sel.anchorNode === p, 'selection now selects the wrapper within its parent');

sel.collapse(t, 0);
assert(own.toString() === 'beta', 'collapse() replaces the range instead of mutating the old one');
