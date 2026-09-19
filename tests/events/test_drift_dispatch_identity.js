// dispatchEvent hands the listeners THE OBJECT it was given.
//
// `new CustomEvent('x', {detail: someObject})` then `el.dispatchEvent(ev)` is
// how components talk to their host page, and `detail` is routinely a node, a
// class instance, a callback or a Map. The listener has to receive that value
// — by identity, not by resemblance.
//
// Regression: the port read the descriptor into a struct and rebuilt a fresh
// event object per listener, JSON-stringifying an object `detail` on the way.
// So `e.detail === payload` was false, `e.detail.node` was gone, a function
// detail vanished, and `e === theEventIDispatched` was false — which breaks
// the "stash the event and compare it later" pattern outright.

const root = document.getElementById('root');
root.innerHTML = '<div id="outer"><div id="inner"></div></div>';
flush();

const outer = document.getElementById('outer');
const inner = document.getElementById('inner');

// --- object detail survives, by identity ----------------------------------
const payload = { node: inner, list: [1, 2, 3], fn: function () { return 7; } };
const evt = new CustomEvent('drift-detail', { detail: payload, bubbles: true });

let seen = [];
outer.addEventListener('drift-detail', function (e) { seen.push(e); });
inner.addEventListener('drift-detail', function (e) { seen.push(e); });

const notPrevented = inner.dispatchEvent(evt);
assert(notPrevented === true, 'an uncancelled dispatch returns true');
assert(seen.length === 2, 'both listeners on the path ran: ' + seen.length);

assert(seen[0] === evt, 'the target listener received the very object dispatched');
assert(seen[1] === evt, 'and so did the ancestor listener — one object, not two');
assert(seen[0].detail === payload, 'detail is the same object, not a copy');
assert(seen[0].detail.node === inner, 'a DOM node inside detail survives');
assert(typeof seen[0].detail.fn === 'function', 'a function inside detail survives');
assert(seen[0].detail.fn() === 7, 'and is still callable');
assert(seen[0].detail.list.length === 3, 'an array inside detail survives');
assert(seen[0].type === 'drift-detail', 'the type is untouched');
assert(seen[0].bubbles === true, 'bubbles is untouched');

// --- the per-dispatch facts ARE written onto it ---------------------------
let phases = [];
let targets = [];
seen = [];
const evt2 = new CustomEvent('drift-phase', { detail: payload, bubbles: true });
outer.addEventListener('drift-phase', function (e) {
    phases.push(e.eventPhase);
    targets.push([e.target, e.currentTarget]);
});
inner.addEventListener('drift-phase', function (e) {
    phases.push(e.eventPhase);
    targets.push([e.target, e.currentTarget]);
});
inner.dispatchEvent(evt2);
assert(targets.length === 2, 'both listeners ran again');
assert(targets[0][0] === inner && targets[0][1] === inner,
       'at the target, target and currentTarget are the target');
assert(targets[1][0] === inner && targets[1][1] === outer,
       'while bubbling, currentTarget moves and target does not');
assert(phases[0] === 2 && phases[1] === 3, 'the phases are AT_TARGET then BUBBLING');

// --- preventDefault writes through to the object the caller holds ---------
const evt3 = new CustomEvent('drift-cancel', { detail: 1, cancelable: true });
inner.addEventListener('drift-cancel', function (e) { e.preventDefault(); });
const result = inner.dispatchEvent(evt3);
assert(result === false, 'dispatchEvent returns false when a listener cancelled');
assert(evt3.defaultPrevented === true,
       'and defaultPrevented is visible on the caller\'s own object afterwards');

// --- stopPropagation on the shared object -------------------------------
let reached = 0;
const evt4 = new CustomEvent('drift-stop', { detail: 2, bubbles: true });
inner.addEventListener('drift-stop', function (e) { e.stopPropagation(); });
outer.addEventListener('drift-stop', function () { reached++; });
inner.dispatchEvent(evt4);
assert(reached === 0, 'stopPropagation on the shared object stops the walk');

// --- a string detail still works ------------------------------------------
let strDetail = null;
inner.addEventListener('drift-str', function (e) { strDetail = e.detail; });
inner.dispatchEvent(new CustomEvent('drift-str', { detail: 'plain text' }));
assert(strDetail === 'plain text', 'a string detail is unchanged');

// --- a plain object descriptor (not an Event) still dispatches ------------
// Some app code hands dispatchEvent a bare {type, detail} literal; that path
// must keep working, and the listener sees that same literal.
let literalSeen = null;
inner.addEventListener('drift-literal', function (e) { literalSeen = e; });
const literal = { type: 'drift-literal', detail: { k: 'v' } };
inner.dispatchEvent(literal);
assert(literalSeen === literal, 'a plain descriptor is passed through as the event');
assert(literalSeen.detail.k === 'v', 'and its object detail is intact');

// --- window.dispatchEvent takes the same path -----------------------------
let winSeen = null;
const winEvt = new CustomEvent('drift-window', { detail: payload });
window.addEventListener('drift-window', function (e) { winSeen = e; });
window.dispatchEvent(winEvt);
assert(winSeen === winEvt, 'window.dispatchEvent passes the same object too');
assert(winSeen.detail === payload, 'with its detail intact');

root.innerHTML = '';
