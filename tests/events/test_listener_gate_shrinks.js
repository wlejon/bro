// removeEventListener has to reach the C++ side, not just the JS array.
//
// dom::Element keeps a per-type gate that js::dispatchDomEvent checks before it
// will look up an element's JS wrapper and walk its listener array. The JS
// removeEventListener binding spliced the array and never told the element, so
// the gate only ever grew: a type listened for once stayed "listening" for the
// life of the element, taking the slow dispatch path forever, and the map was
// only safe to read as an over-approximation. `once` listeners had the same
// hole — dispatch reaps them out of the array itself.
//
// __host.hasJsListener is the only way to see the gate; nothing about dispatch
// behaviour distinguishes an exact gate from a permanently-optimistic one,
// which is exactly why it was able to drift.

const root = document.getElementById('root');
root.innerHTML = '<div id="t" style="position:absolute;left:0;top:0;width:60px;height:40px"></div>';
flush();

const t = document.getElementById('t');
const has = (type) => __host.hasJsListener(t, type);

assert(has('click') === false, 'no gate before any listener is added');

// ── add / remove round-trips to nothing ──────────────────────────────────────
let n = 0;
const h1 = () => { n++; };
t.addEventListener('click', h1);
assert(has('click') === true, 'adding a listener opens the gate');

t.removeEventListener('click', h1);
assert(has('click') === false, 'removing the only listener closes it again');

// ...and dispatch agrees, so the gate is not merely cosmetic.
click(30, 20);
assert(n === 0, 'the removed listener does not run');

// ── the gate counts, it is not a flag ────────────────────────────────────────
let a = 0, b = 0;
const hA = () => { a++; };
const hB = () => { b++; };
t.addEventListener('click', hA);
t.addEventListener('click', hB);
assert(has('click') === true, 'two listeners, gate open');

t.removeEventListener('click', hA);
assert(has('click') === true, 'still open with one listener left');
click(30, 20);
assert(a === 0 && b === 1, 'only the surviving listener ran');

t.removeEventListener('click', hB);
assert(has('click') === false, 'closed once the last one is gone');

// ── a removal that does not match must not close it ──────────────────────────
// Per spec the capture flag is part of a listener's identity, so this
// removeEventListener removes nothing — and must not decrement anything either.
const hC = () => {};
t.addEventListener('mouseover', hC, true);
assert(has('mouseover') === true, 'capture listener opens the gate');
t.removeEventListener('mouseover', hC);            // no capture: no match
assert(has('mouseover') === true,
    'a non-matching removeEventListener leaves the gate open');
t.removeEventListener('mouseover', hC, true);      // matches
assert(has('mouseover') === false, 'the matching removal closes it');

// A removal for a type that was never listened for must be a no-op, not an
// underflow that leaves the gate stuck open.
t.removeEventListener('mouseout', hC);
assert(has('mouseout') === false, 'removing a listener that never existed is inert');

// ── `once` listeners are reaped by dispatch, and count as removals ───────────
let onceRuns = 0;
t.addEventListener('click', () => { onceRuns++; }, { once: true });
assert(has('click') === true, 'once listener opens the gate');
click(30, 20);
assert(onceRuns === 1, 'once listener ran');
assert(has('click') === false, 'and the gate closed when dispatch reaped it');
click(30, 20);
assert(onceRuns === 1, 'once listener does not run a second time');

console.log('PASS');
