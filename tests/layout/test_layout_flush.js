// Geometry read in the same turn as the change that moves it.
//
// Layout runs on the frame loop, so an app that built an element and then
// measured it got the box from before the element existed — and not honestly
// empty either: getBoundingClientRect() answered 0 while clientWidth answered
// the parent's width, because they fall back differently when there is no box.
// Reading geometry now lays the document out first (CSSOM calls it flushing
// pending layout), so every question below is answered about the DOM as it
// stands at the moment it is asked.

const root = document.getElementById('root');
root.innerHTML = '';

function box(el) { return el.getBoundingClientRect(); }

// ── an element created and measured in one turn ────────────────────────────

const fresh = document.createElement('div');
fresh.style.width = '150px';
fresh.style.height = '40px';
root.appendChild(fresh);

assert(box(fresh).width === 150, 'a fresh element has its width immediately');
assert(box(fresh).height === 40, 'and its height');
assert(fresh.clientWidth === 150, 'clientWidth agrees');
assert(fresh.offsetWidth === 150, 'offsetWidth agrees');
assert(fresh.offsetHeight === 40, 'offsetHeight agrees');

// A frame later, nothing has changed.
flush();
assert(box(fresh).width === 150, 'and the frame loop agrees with the flush');

// ── a style write, measured on the next line ───────────────────────────────

fresh.style.width = '260px';
assert(box(fresh).width === 260, 'a restyled element reports its new width');
// Padding, with bro's border-box default, moves the content in rather than
// the border out — so it is a child that shows the change.
const inner = document.createElement('div');
fresh.appendChild(inner);
const innerLeft = box(inner).x;
fresh.style.padding = '10px';
assert(box(inner).x === innerLeft + 10, 'padding set this turn moves the content');
assert(box(fresh).width === 260, 'and leaves the border box where it was');

// ── content that changes size ──────────────────────────────────────────────

const text = document.createElement('span');
text.textContent = 'short';
root.appendChild(text);
const short = box(text).width;
assert(short > 0, 'a span sizes to its text');
text.textContent = 'considerably longer than it was a moment ago';
assert(box(text).width > short, 'and re-sizes when the text changes');

// ── position follows the element above it ──────────────────────────────────

const first = document.createElement('div');
first.style.height = '30px';
const second = document.createElement('div');
second.style.height = '30px';
root.appendChild(first);
root.appendChild(second);
const secondTop = box(second).y;
first.style.height = '90px';
assert(box(second).y === secondTop + 60, 'a sibling moves when what precedes it grows');

// ── getComputedStyle resolves against the same pending change ──────────────

const styled = document.createElement('div');
styled.style.width = '120px';
root.appendChild(styled);
assert(getComputedStyle(styled).width === '120px',
       'computed style sees a style set this turn');
styled.style.display = 'none';
assert(getComputedStyle(styled).display === 'none', 'and a later change to it');
assert(box(styled).width === 0, 'display:none has no box');

// ── removal is visible immediately too ─────────────────────────────────────

const doomed = document.createElement('div');
doomed.style.height = '50px';
root.appendChild(doomed);
const below = document.createElement('div');
root.appendChild(below);
const belowTop = box(below).y;
root.removeChild(doomed);
assert(box(below).y === belowTop - 50, 'removing an element moves what followed it');

// ── the flush does not swallow the frame ───────────────────────────────────
//
// The measurement lays the document out, and a dirty flag cleared too eagerly
// there would tell the frame loop it had nothing left to do — the change would
// sit in the tree, correctly laid out, and never be drawn or reported. A
// ResizeObserver is how that becomes visible from script: its callbacks are
// delivered by the frame drain, so a drain that was skipped is a callback that
// never arrives.

const watched = document.createElement('div');
watched.style.width = '100px';
watched.style.height = '20px';
root.appendChild(watched);

let observed = 0;
let lastWidth = 0;
const ro = new ResizeObserver((entries) => {
    observed++;
    lastWidth = entries[entries.length - 1].contentRect.width;
});
ro.observe(watched);
flush();
const initial = observed;

// Resize it and measure it in the same turn — the measurement flushes layout.
watched.style.width = '175px';
assert(box(watched).width === 175, 'the read sees the resize');
flush();
assert(observed > initial, 'the frame still ran after a read-flush');
assert(lastWidth === 175, 'and reported the size the read already saw');
ro.disconnect();

// ── measuring in a loop stays correct ──────────────────────────────────────

root.innerHTML = '';
const rows = [];
for (let i = 0; i < 8; i++) {
    const row = document.createElement('div');
    row.style.height = (10 + i) + 'px';
    root.appendChild(row);
    rows.push(row);
}
let sum = 0;
for (const row of rows) sum += box(row).height;
assert(sum === 8 * 10 + 28, 'every row in a read loop reports its own height');

// Alternating write and read — the layout thrash case, which must be correct
// however expensive it is.
let y = box(rows[0]).y;
for (let i = 0; i < 8; i++) {
    rows[i].style.height = '20px';
    assert(box(rows[i]).y === y, 'row ' + i + ' sits where the rows above put it');
    y += 20;
}

console.log('PASS layout flush');
