// unhandledrejection / rejectionhandled at the window (HTML "notify about
// rejected promises"). A promise rejected with no handler is reported at the
// end of the microtask checkpoint as a TASK: a cancelable PromiseRejectionEvent
// at window.onunhandledrejection and then the listeners. A handler attached
// before that task runs means nothing is fired; one attached after it fires
// `rejectionhandled`. Every uncancelled report fails a headless run, so each
// case below cancels what it provokes.
//
// Tasks run at the top of a frame, so advanceTime() is what lets them fire.

const seen = [];
function record(e) {
    seen.push({ type: e.type, promise: e.promise, reason: e.reason, event: e });
}
window.addEventListener('unhandledrejection', (e) => {
    record(e);
    e.preventDefault();
});
window.addEventListener('rejectionhandled', record);

assert(typeof PromiseRejectionEvent === 'function', 'PromiseRejectionEvent exists');

// ---- a plain unhandled rejection --------------------------------------------
const r1 = new Error('r1');
const p1 = Promise.reject(r1);
assert(seen.length === 0, 'nothing fires synchronously');
advanceTime(20);
assert(seen.length === 1, 'one unhandledrejection, got ' + seen.length);
const e1 = seen[0].event;
assert(seen[0].type === 'unhandledrejection', 'type');
assert(e1 instanceof PromiseRejectionEvent, 'a PromiseRejectionEvent');
assert(e1 instanceof Event, 'which is an Event');
assert(seen[0].promise === p1, 'event.promise is the rejected promise');
assert(seen[0].reason === r1, 'event.reason is the rejection reason');
assert(e1.cancelable === true, 'unhandledrejection is cancelable');
assert(e1.defaultPrevented === true, 'preventDefault took');
assert(e1.isTrusted !== true || e1.isTrusted === true, 'isTrusted readable');

// Fired once: later frames say nothing more about the same promise.
advanceTime(20);
assert(seen.length === 1, 'reported once, got ' + seen.length);

// ---- handled later: rejectionhandled ------------------------------------------
p1.catch(() => {});
assert(seen.length === 1, 'rejectionhandled is not synchronous either');
advanceTime(20);
assert(seen.length === 2, 'rejectionhandled fired, got ' + seen.length);
assert(seen[1].type === 'rejectionhandled', 'type rejectionhandled');
assert(seen[1].promise === p1 && seen[1].reason === r1, 'same promise and reason');
assert(seen[1].event.cancelable === false, 'rejectionhandled is not cancelable');
p1.then(null, () => {});
advanceTime(20);
assert(seen.length === 2, 'a second handler does not fire it again');

// ---- handled within the same turn: nothing fires --------------------------------
const p2 = Promise.reject(new Error('r2'));
p2.catch(() => {});
advanceTime(20);
assert(seen.length === 2, 'a promise handled before the checkpoint is never reported');

// ---- handled in a later microtask of the same turn: nothing fires -----------------
const p3 = Promise.reject(new Error('r3'));
queueMicrotask(() => queueMicrotask(() => p3.catch(() => {})));
advanceTime(20);
assert(seen.length === 2,
       'a handler attached before the notify task runs cancels the report, got ' + seen.length);

// ---- handled by a later task: unhandled, then handled -------------------------------
// Rejected in an rAF callback, handled by a 0 ms timer. The checkpoint after
// rAF queues the notify task; the next frame runs its tasks before its timers,
// so the page hears `unhandledrejection` first and `rejectionhandled` after.
const afterRaf = seen.length;
requestAnimationFrame(() => {
    const p = Promise.reject(new Error('raf'));
    setTimeout(() => p.catch(() => {}), 0);
});
advanceTime(20);
advanceTime(20);
advanceTime(20);
assert(seen.length === afterRaf + 2 &&
       seen[afterRaf].type === 'unhandledrejection' &&
       seen[afterRaf + 1].type === 'rejectionhandled',
       'raf rejection: unhandled then handled, got ' +
       seen.slice(afterRaf).map((s) => s.type).join(','));

// ---- window.onunhandledrejection -----------------------------------------------------
const viaAttr = [];
window.onunhandledrejection = function (e) {
    viaAttr.push({ e, self: this });
};
const before = seen.length;
const r4 = 'plain string reason';
const p4 = Promise.reject(r4);
advanceTime(20);
assert(viaAttr.length === 1, 'onunhandledrejection ran, got ' + viaAttr.length);
assert(viaAttr[0].self === window, 'called with the window as this');
assert(viaAttr[0].e.promise === p4 && viaAttr[0].e.reason === r4, 'attribute gets the event');
assert(seen.length === before + 1, 'listeners still run after the attribute');
assert(seen[before].event === viaAttr[0].e, 'the attribute and the listener share one event');

// Returning false from the attribute cancels, like any event-handler attribute.
window.onunhandledrejection = () => false;
let listenerSawPrevented = null;
const probe = (e) => { listenerSawPrevented = e.defaultPrevented; };
window.addEventListener('unhandledrejection', probe);
Promise.reject(new Error('r5'));
advanceTime(20);
assert(listenerSawPrevented === true,
       'onunhandledrejection returning false cancels, listener saw ' + listenerSawPrevented);
window.removeEventListener('unhandledrejection', probe);
window.onunhandledrejection = null;

// ---- async functions and rejections from other seams -----------------------------------
const mark = seen.length;
(async () => { throw new TypeError('async-throw'); })();
setTimeout(() => { Promise.reject(new RangeError('from-timer')); }, 0);
advanceTime(20);
advanceTime(20);
const reasons = seen.slice(mark).filter((s) => s.type === 'unhandledrejection').map((s) => s.reason);
assert(reasons.length === 2, 'both reported, got ' + reasons.length);
assert(reasons[0] instanceof TypeError && reasons[0].message === 'async-throw', 'async throw reported');
assert(reasons[1] instanceof RangeError, 'timer rejection reported');

// ---- order: reported in rejection order -------------------------------------------------
const orderMark = seen.length;
const pa = Promise.reject('a');
const pb = Promise.reject('b');
const pc = Promise.reject('c');
advanceTime(20);
const order = seen.slice(orderMark).map((s) => s.reason).join('');
assert(order === 'abc', 'reported in rejection order, got ' + order);
[pa, pb, pc].forEach((p) => p.catch(() => {}));
advanceTime(20);

console.log('unhandledrejection OK');
