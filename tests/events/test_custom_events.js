// Test CustomEvent, dispatchEvent, Event constructor, event.detail.
// Exercises src/js/element_bindings.cpp dispatchEvent path,
// src/js/event_dispatch.cpp, and the CustomEvent polyfill in
// src/js/js/dom_polyfills.js.

const root = document.getElementById('root');
root.innerHTML = '<div id="parent"><div id="child">x</div></div>';
flush();

const parent = document.getElementById('parent');
const child = document.getElementById('child');

// =========================================================================
// Event constructor
// =========================================================================
const e = new Event('myevent');
assert(e.type === 'myevent', 'Event type');
assert(e.bubbles === false, 'default bubbles = false');
assert(e.cancelable === false, 'default cancelable = false');
assert(e.defaultPrevented === false, 'defaultPrevented = false');

const e2 = new Event('foo', { bubbles: true, cancelable: true });
assert(e2.bubbles === true, 'bubbles option');
assert(e2.cancelable === true, 'cancelable option');

// =========================================================================
// CustomEvent — detail
// =========================================================================
const ce = new CustomEvent('change', { detail: { value: 42, name: 'x' }, bubbles: true });
assert(ce.type === 'change', 'CustomEvent type');
assert(ce.bubbles === true, 'CustomEvent bubbles');
assert(typeof ce.detail === 'object', 'detail object');
assert(ce.detail.value === 42, 'detail.value');
assert(ce.detail.name === 'x', 'detail.name');

// =========================================================================
// dispatchEvent on element — handler receives the event
// =========================================================================
const got = [];
child.addEventListener('myevent', (ev) => got.push({ type: ev.type, t: 'child' }));
parent.addEventListener('myevent', (ev) => got.push({ type: ev.type, t: 'parent', target: ev.target.id }));

const evt = new CustomEvent('myevent', { bubbles: true, detail: { x: 1 } });
const result = child.dispatchEvent(evt);
assert(result === true, 'dispatchEvent returns true when not cancelled');

// Both listeners fire (bubbling)
assert(got.length === 2, 'both listeners fired, got ' + got.length);
assert(got[0].t === 'child', 'child first');
assert(got[1].t === 'parent', 'parent bubbles');
assert(got[1].target === 'child', 'event.target is original');

// =========================================================================
// preventDefault returns false from dispatchEvent
// =========================================================================
const ev2 = new CustomEvent('cancelable_evt', { cancelable: true });
child.addEventListener('cancelable_evt', (e) => e.preventDefault());
const r2 = child.dispatchEvent(ev2);
assert(r2 === false, 'dispatchEvent returns false when defaultPrevented');

// =========================================================================
// stopPropagation
// =========================================================================
const stopLog = [];
child.addEventListener('stoppy', (e) => { stopLog.push('child'); e.stopPropagation(); });
parent.addEventListener('stoppy', () => stopLog.push('parent'));
child.dispatchEvent(new CustomEvent('stoppy', { bubbles: true }));
assert(stopLog.length === 1, 'stopPropagation stopped at child, got ' + stopLog.length);
assert(stopLog[0] === 'child', 'child saw event');

// =========================================================================
// Multiple listeners on same event
// =========================================================================
const multi = [];
const h1 = () => multi.push(1);
const h2 = () => multi.push(2);
const h3 = () => multi.push(3);
child.addEventListener('multi', h1);
child.addEventListener('multi', h2);
child.addEventListener('multi', h3);
child.dispatchEvent(new CustomEvent('multi'));
assert(multi.length === 3, 'all 3 listeners fired');
assert(multi[0] === 1 && multi[1] === 2 && multi[2] === 3, 'in order');

// removeEventListener
child.removeEventListener('multi', h2);
multi.length = 0;
child.dispatchEvent(new CustomEvent('multi'));
assert(multi.length === 2, 'after remove h2, 2 listeners fire');

// =========================================================================
// once option
// =========================================================================
let onceCount = 0;
child.addEventListener('once_evt', () => onceCount++, { once: true });
child.dispatchEvent(new CustomEvent('once_evt'));
child.dispatchEvent(new CustomEvent('once_evt'));
child.dispatchEvent(new CustomEvent('once_evt'));
assert(onceCount === 1, 'once option: 1 fire, got ' + onceCount);

// =========================================================================
// capture phase
// =========================================================================
const capLog = [];
parent.addEventListener('cap', () => capLog.push('parent-bubble'));
parent.addEventListener('cap', () => capLog.push('parent-capture'), true);
child.addEventListener('cap', () => capLog.push('child'));
child.dispatchEvent(new CustomEvent('cap', { bubbles: true }));
// Expected order: parent-capture, child, parent-bubble
assert(capLog.indexOf('parent-capture') === 0, 'capture fires first');
assert(capLog.indexOf('child') > capLog.indexOf('parent-capture'), 'child after capture');
assert(capLog.indexOf('parent-bubble') > capLog.indexOf('child'), 'bubble fires after child');

// =========================================================================
// Cleanup
// =========================================================================
root.innerHTML = '';
