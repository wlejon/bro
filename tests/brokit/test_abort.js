// Test AbortController / AbortSignal.

assert(typeof AbortController === 'function', 'AbortController exists');
const ac = new AbortController();
assert(ac.signal, 'signal exists');
assert(ac.signal.aborted === false, 'not aborted initially');

let fired = 0;
ac.signal.addEventListener('abort', () => { fired++; });
ac.abort();
assert(ac.signal.aborted === true, 'aborted after abort()');
assert(fired === 1, 'abort listener fired: ' + fired);

// Second abort should not refire listener (per spec)
ac.abort();
assert(fired === 1, 'abort fires only once: ' + fired);

// reason
const ac2 = new AbortController();
ac2.abort('custom reason');
assert(ac2.signal.aborted === true, 'ac2 aborted');
// reason property may or may not be present
if ('reason' in ac2.signal) {
    assert(ac2.signal.reason === 'custom reason', 'reason set: ' + ac2.signal.reason);
}

// onabort property
const ac3 = new AbortController();
let onabortFired = false;
ac3.signal.onabort = () => { onabortFired = true; };
ac3.abort();
assert(onabortFired, 'onabort property fired');

// AbortSignal.timeout if present
if (typeof AbortSignal !== 'undefined' && typeof AbortSignal.timeout === 'function') {
    const sig = AbortSignal.timeout(50);
    assert(sig.aborted === false, 'timeout signal not aborted yet');
    advanceTime(100);
    flush();
    assert(sig.aborted === true, 'timeout signal aborted');
} else {
    console.warn('AbortSignal.timeout missing');
}
