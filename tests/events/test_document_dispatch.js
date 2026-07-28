// document.dispatchEvent — aiming an event at the document itself.
//
// bro gave Document addEventListener and removeEventListener but no
// dispatchEvent, so there was no way to fire an event *at* the document: test
// code and any script wanting to announce something document-wide had to pick
// some element, dispatch there and rely on bubbling to carry it up. That works
// and is what a real key press does, but it is not the same thing — a
// non-bubbling event could not reach a document listener at all, and nothing
// could say "this happened to the page" rather than "this happened to a div".
//
// Exercises src/js/document_bindings.cpp (js_document_dispatchEvent) against
// src/js/event_dispatch.cpp. Document listeners are stored on documentElement,
// so dispatching is delegated there and the event takes the ordinary
// propagation path: window capture, the document's listeners at target, then
// the bubble back out to window.

const root = document.getElementById('root');
root.innerHTML = '<div id="child">child</div>';
flush();
const child = document.getElementById('child');

// ====== a listener on document receives an event dispatched on document =====

let seen = 0;
let seenType = null;
function onDoc(e) { seen++; seenType = e.type; }
document.addEventListener('doc-ping', onDoc);

const ok = document.dispatchEvent(new Event('doc-ping'));
assert(seen === 1, 'document listener fired once (got ' + seen + ')');
assert(seenType === 'doc-ping', 'and got the right type');
assert(ok === true, 'dispatchEvent returns true when nothing prevented default');

// It does not bubble down: an element listener must not see it.
let childSaw = 0;
child.addEventListener('doc-ping', function () { childSaw++; });
document.dispatchEvent(new Event('doc-ping'));
assert(seen === 2, 'document listener fired again');
assert(childSaw === 0, 'a descendant does not see an event aimed at the document');

document.removeEventListener('doc-ping', onDoc);
document.dispatchEvent(new Event('doc-ping'));
assert(seen === 2, 'removeEventListener still works against dispatchEvent');

// ====== window still sees it, on the engine's existing bubbling contract ====

let winBubble = 0;
let winCapture = 0;
window.addEventListener('doc-bubble', function () { winBubble++; });
window.addEventListener('doc-capture', function () { winCapture++; }, true);

document.dispatchEvent(new Event('doc-bubble', { bubbles: true }));
assert(winBubble === 1, 'a bubbling event dispatched on document reaches window');

// The capture pass runs before the target and is not gated on bubbles — same
// as for an event dispatched on any element.
document.dispatchEvent(new Event('doc-capture'));
assert(winCapture === 1, 'a window capture listener sees it too');

// ====== phase and order ====================================================

const order = [];
window.addEventListener('doc-order', function () { order.push('win-capture'); }, true);
document.addEventListener('doc-order', function () { order.push('document'); });
window.addEventListener('doc-order', function () { order.push('win-bubble'); });

document.dispatchEvent(new Event('doc-order', { bubbles: true }));
assert(order.join(',') === 'win-capture,document,win-bubble',
       'capture, target, bubble in that order (got ' + order.join(',') + ')');

// ====== preventDefault and stopPropagation =================================

document.addEventListener('doc-cancel', function (e) { e.preventDefault(); });
const cancelled = document.dispatchEvent(
    new Event('doc-cancel', { bubbles: true, cancelable: true }));
assert(cancelled === false, 'dispatchEvent returns false when default was prevented');

let afterStop = 0;
window.addEventListener('doc-stop', function () { afterStop++; });
document.addEventListener('doc-stop', function (e) { e.stopPropagation(); });
document.dispatchEvent(new Event('doc-stop', { bubbles: true }));
assert(afterStop === 0, 'stopPropagation on document keeps it from reaching window');

// ====== CustomEvent detail survives the crossing ===========================

let detail = null;
document.addEventListener('doc-detail', function (e) { detail = e.detail; });
document.dispatchEvent(new CustomEvent('doc-detail', { detail: { n: 42 } }));
assert(detail !== null && detail.n === 42, 'CustomEvent detail reached the listener');

// ====== once, and a listener added on documentElement directly =============

let onceCount = 0;
document.addEventListener('doc-once', function () { onceCount++; }, { once: true });
document.dispatchEvent(new Event('doc-once'));
document.dispatchEvent(new Event('doc-once'));
assert(onceCount === 1, 'once: true is honoured (got ' + onceCount + ')');

// document.addEventListener stores on documentElement, so the two are one
// list — a listener registered either way must see a document dispatch.
let rootSaw = 0;
document.documentElement.addEventListener('doc-root', function () { rootSaw++; });
document.dispatchEvent(new Event('doc-root'));
assert(rootSaw === 1, 'a listener on documentElement sees a document dispatch');

// ====== an element dispatch still bubbles to the document ==================
// The route that already worked must be untouched.

let bubbled = 0;
document.addEventListener('from-child', function () { bubbled++; });
child.dispatchEvent(new Event('from-child', { bubbles: true }));
assert(bubbled === 1, 'an event dispatched on an element still bubbles to document');

root.innerHTML = '';
console.log('PASS: document.dispatchEvent');
