// Range.deleteContents frees the content it removes, except what a script
// still holds. It used to keep every removed node (and its scratch fragment)
// alive until the document was torn down, so each delete an editing engine
// made — typing over a selection in a contenteditable — leaked what it cut.
// Freeing is only safe if nothing the page can reach is freed: the nodes a
// script holds, and the nodes a MutationObserver record names, survive
// detached and whole.

const root = document.getElementById('root');

// A held element survives, detached, with its content.
root.innerHTML = '<p id="p">aa<b id="b">bold <u>under</u></b>cc<i>ital</i>dd</p>';
flush();
const p = document.getElementById('p');
const b = document.getElementById('b');
let r = document.createRange();
r.setStart(p.firstChild, 1);
r.setEnd(p.lastChild, 1);
r.deleteContents();
assert(p.textContent === 'ad', 'deleted (' + p.textContent + ')');
assert(b.parentNode === null, 'the held <b> is detached');
assert(b.textContent === 'bold under', 'and whole (' + b.textContent + ')');
p.appendChild(b);
flush();
assert(p.textContent === 'adbold under', 'and can go back in (' + p.textContent + ')');
assert(b.getBoundingClientRect().width > 0, 'and lays out again');

// A held text node below an unheld element survives on its own.
root.innerHTML = '<p id="q">xx<span><em>keep</em> drop</span>yy</p>';
flush();
const q = document.getElementById('q');
const keep = q.querySelector('em').firstChild;
r = document.createRange();
r.selectNodeContents(q);
r.deleteContents();
assert(q.childNodes.length === 0, 'emptied');
assert(keep.data === 'keep', 'a held text node keeps its data (' + keep.data + ')');
const again = document.createElement('div');
again.appendChild(keep);
assert(again.textContent === 'keep', 'and is still a usable node');

// Nodes a MutationObserver record names survive until the record is read.
root.innerHTML = '<p id="m">one<b>two</b>three</p>';
flush();
const m = document.getElementById('m');
const obs = new MutationObserver(() => {});
obs.observe(m, { childList: true, subtree: true });
r = document.createRange();
r.selectNodeContents(m);
r.deleteContents();
const recs = obs.takeRecords();
obs.disconnect();
let removedText = '';
for (const rec of recs)
    for (const n of rec.removedNodes) removedText += n.textContent;
assert(removedText.indexOf('two') >= 0, 'a record\'s removed nodes are readable (' + removedText + ')');

// surroundContents moves the content out of its scratch fragment and frees
// the fragment; the moved nodes are intact in their new parent.
root.innerHTML = '<p id="s">ab<b>cd</b>ef</p>';
flush();
const s = document.getElementById('s');
r = document.createRange();
r.setStart(s.firstChild, 1);
r.setEnd(s.lastChild, 1);
const wrapEl = document.createElement('mark');
r.surroundContents(wrapEl);
assert(s.innerHTML === 'a<mark>b<b>cd</b>e</mark>f', 'surrounded (' + s.innerHTML + ')');
flush();
assert(wrapEl.getBoundingClientRect().width > 0, 'the wrapper lays out');

// The engine's editing path: typing over selections in a contenteditable
// deletes through the same code, many times over. The document keeps
// working and the edits land.
root.innerHTML = '<div id="ed" contenteditable="true">start</div>';
flush();
const ed = document.getElementById('ed');
ed.focus();
for (let i = 0; i < 100; ++i) {
    ed.innerHTML = 'ab<b>cd</b><i>ef</i>gh';
    const sel = window.getSelection();
    const rr = document.createRange();
    rr.setStart(ed.firstChild, 1);
    rr.setEnd(ed.lastChild, 1);
    sel.removeAllRanges();
    sel.addRange(rr);
    textInput('Z');
    if (i % 10 === 0) { advanceTime(16); flush(); }
}
flush();
assert(ed.textContent === 'aZh', 'typing over a selection replaces it (' + ed.textContent + ')');

root.innerHTML = '';
