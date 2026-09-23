// An exception nothing caught is REPORTED to the page, the way a browser
// reports it: window.onerror(message, filename, lineno, colno, error) first,
// then an ErrorEvent at the window's 'error' listeners. onerror returning
// true, or a listener calling preventDefault(), cancels it (the engine log
// line is skipped). Covers the host seams that run page code: timers, rAF,
// event listeners, queueMicrotask.
//
// Unhandled promise rejections are the other report, `unhandledrejection`:
// tests/events/test_unhandled_rejection.js.

const onerrorCalls = [];
const events = [];

window.onerror = function (message, filename, lineno, colno, error) {
    onerrorCalls.push({ message, filename, lineno, colno, error, self: this });
    // Not returning true: the listener below is what cancels.
};
function onErrorEvent(e) {
    events.push(e);
    e.preventDefault();
}
window.addEventListener('error', onErrorEvent);

assert(typeof ErrorEvent === 'function', 'ErrorEvent exists');

// ---- a timer ---------------------------------------------------------------
const timerErr = new Error('boom-timer');
setTimeout(() => { throw timerErr; }, 0);
advanceTime(20);

assert(onerrorCalls.length === 1, 'onerror ran for the timer throw, got ' + onerrorCalls.length);
assert(onerrorCalls[0].message === 'Uncaught Error: boom-timer',
       'onerror message, got ' + JSON.stringify(onerrorCalls[0].message));
assert(onerrorCalls[0].error === timerErr, 'onerror 5th argument is the thrown value');
assert(onerrorCalls[0].filename === '', 'filename is a string');
assert(onerrorCalls[0].lineno === 0 && onerrorCalls[0].colno === 0, 'line/col are numbers');
assert(onerrorCalls[0].self === window, 'onerror is called with the window as this');

assert(events.length === 1, 'error listener ran for the timer throw, got ' + events.length);
const e0 = events[0];
assert(e0 instanceof ErrorEvent, 'listener got an ErrorEvent');
assert(e0 instanceof Event, 'an ErrorEvent is an Event');
assert(e0.type === 'error', 'type is error');
assert(e0.message === 'Uncaught Error: boom-timer', 'ErrorEvent.message');
assert(e0.error === timerErr, 'ErrorEvent.error is the thrown value');
assert(e0.cancelable === true, 'the error event is cancelable');
assert(e0.defaultPrevented === true, 'preventDefault took');

// ---- requestAnimationFrame -------------------------------------------------
requestAnimationFrame(() => { throw new TypeError('boom-raf'); });
advanceTime(50);
assert(events.length === 2, 'error listener ran for the rAF throw, got ' + events.length);
assert(events[1].message === 'Uncaught TypeError: boom-raf',
       'rAF message, got ' + JSON.stringify(events[1].message));
assert(events[1].error instanceof TypeError, 'rAF error is the TypeError');

// ---- an event listener -----------------------------------------------------
const div = document.createElement('div');
document.body.appendChild(div);
let afterThrowRan = false;
div.addEventListener('click', () => { throw new RangeError('boom-listener'); });
div.addEventListener('click', () => { afterThrowRan = true; });
div.dispatchEvent(new MouseEvent('click', { bubbles: true }));
assert(afterThrowRan, 'a throwing listener does not stop the next one');
assert(events.length === 3, 'error listener ran for the listener throw, got ' + events.length);
assert(events[2].message === 'Uncaught RangeError: boom-listener',
       'listener message, got ' + JSON.stringify(events[2].message));

// ---- queueMicrotask --------------------------------------------------------
queueMicrotask(() => { throw 'a string'; });
advanceTime(20);
assert(events.length === 4, 'error listener ran for the microtask throw, got ' + events.length);
assert(events[3].error === 'a string', 'a thrown primitive arrives as .error');
assert(events[3].message === 'Uncaught a string',
       'primitive message, got ' + JSON.stringify(events[3].message));

// ---- a throwing error handler is not re-dispatched --------------------------
function throwingHandler() { throw new Error('handler threw'); }
window.addEventListener('error', throwingHandler);
setTimeout(() => { throw new Error('boom-2'); }, 0);
advanceTime(20);
assert(events.length === 5, 'one dispatch, no recursion, got ' + events.length);
window.removeEventListener('error', throwingHandler);

// ---- onerror returning true cancels; listeners still see it ----------------
window.removeEventListener('error', onErrorEvent);
let sawUncancelled = null;
window.addEventListener('error', (e) => { sawUncancelled = e; });
window.onerror = () => true;
setTimeout(() => { throw new Error('boom-3'); }, 0);
advanceTime(20);
assert(sawUncancelled !== null, 'listener still ran after onerror returned true');
assert(sawUncancelled.defaultPrevented === false, 'the listener did not cancel it');

// ---- removed handlers stop firing ------------------------------------------
window.onerror = null;
const before = onerrorCalls.length;
sawUncancelled = null;
setTimeout(() => { throw new Error('boom-4'); }, 0);
advanceTime(20);
assert(onerrorCalls.length === before, 'a cleared onerror no longer runs');
assert(sawUncancelled !== null, 'the remaining listener still runs');

// ---- the event classes -----------------------------------------------------
const made = new ErrorEvent('error', { message: 'm', filename: 'f.js', lineno: 3, colno: 4,
                                       error: 42 });
assert(made.message === 'm' && made.filename === 'f.js' && made.lineno === 3 &&
       made.colno === 4 && made.error === 42, 'ErrorEvent init dictionary');
const p = Promise.resolve(1);
const pre = new PromiseRejectionEvent('unhandledrejection', { promise: p, reason: 'r' });
assert(pre.promise === p && pre.reason === 'r', 'PromiseRejectionEvent init dictionary');

console.log('window error events: OK');
