// `selectionchange` fires at the document when the selection changes —
// from script as well as from the mouse — queued as a task and coalesced, so
// one burst of changes reports once. It never fired at all: the document's
// fireSelectionChange() was an empty stub.

const root = document.getElementById('root');
root.innerHTML = '<p id="a" style="font:20px Arial">alpha beta gamma</p>' +
                 '<p id="b" style="font:20px Arial">delta epsilon</p>';
flush();
const a = document.getElementById('a').firstChild;
const b = document.getElementById('b').firstChild;

let count = 0;
let sawEvent = null;
document.addEventListener('selectionchange', (e) => {
    count++;
    sawEvent = { type: e.type, bubbles: e.bubbles, cancelable: e.cancelable,
                 text: String(getSelection()) };
});

function settle() {
    advanceTime(20);
    flush();
}

const sel = getSelection();

sel.setBaseAndExtent(a, 0, a, 5);
assert(count === 0, 'queued, not dispatched inside the call');
settle();
assert(count === 1, 'setBaseAndExtent fires one selectionchange (got ' + count + ')');
assert(sawEvent && sawEvent.type === 'selectionchange' && !sawEvent.bubbles && !sawEvent.cancelable,
    'a plain non-bubbling, non-cancelable event');
assert(sawEvent.text === 'alpha', 'the listener reads the settled selection (' + sawEvent.text + ')');

count = 0;
sel.collapse(b, 2);
settle();
assert(count === 1, 'collapse fires (got ' + count + ')');

count = 0;
sel.removeAllRanges();
const r = document.createRange();
r.setStart(a, 6);
r.setEnd(a, 10);
sel.addRange(r);
settle();
assert(count === 1, 'removeAllRanges + addRange in one task coalesce to one (got ' + count + ')');

count = 0;
sel.extend(b, 5);
sel.collapseToEnd();
settle();
assert(count === 1, 'extend + collapseToEnd coalesce (got ' + count + ')');

// A DOM change that moves the selection's boundary reports; one elsewhere
// does not.
count = 0;
document.getElementById('a').appendChild(document.createTextNode(' more'));
settle();
assert(count === 0, 'a mutation that leaves the selection alone is silent (got ' + count + ')');
b.deleteData(0, 3);
settle();
assert(count === 1, 'deleting text before the caret moves it and reports (got ' + count + ')');

// The mouse path goes through the same queue.
count = 0;
const ar = document.getElementById('a').getBoundingClientRect();
mouseDown(ar.left + 5, ar.top + ar.height / 2);
mouseUp(ar.left + 5, ar.top + ar.height / 2);
settle();
assert(count === 1, 'a click that moves the caret fires once (got ' + count + ')');

console.log('test_selectionchange: done');
