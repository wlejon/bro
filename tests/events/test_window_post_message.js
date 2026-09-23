// window.postMessage(data, targetOrigin[, transfer]) delivers a `message`
// MessageEvent to the window itself, as a TASK: after the posting code and
// its microtasks, with the data structured-cloned at the call.

assert(typeof window.postMessage === 'function', 'window.postMessage exists');
assert(postMessage === window.postMessage, 'bare postMessage is the window one');

const got = [];
const onGot = [];
function listener(e) { got.push(e); }
window.addEventListener('message', listener);
window.onmessage = (e) => { onGot.push(e); };

const payload = { a: 1, nested: { list: [1, 2, 3] }, when: new Date(5) };
window.postMessage(payload, '*');
payload.a = 99;               // after the call: must not reach the receiver
let microtaskRan = false;
Promise.resolve().then(() => { microtaskRan = true; });

assert(got.length === 0, 'not delivered synchronously');
advanceTime(20);
assert(microtaskRan, 'the microtask ran');
assert(got.length === 1, 'listener got one message, got ' + got.length);
assert(onGot.length === 1, 'onmessage got one message, got ' + onGot.length);

const e = got[0];
assert(e instanceof MessageEvent, 'a MessageEvent');
assert(e.type === 'message', 'type is message');
assert(e.data !== payload, 'data is a clone, not the posted object');
assert(e.data.a === 1, 'the clone was taken at the call, got ' + e.data.a);
assert(e.data.nested.list.length === 3 && e.data.nested.list[2] === 3, 'nested data survived');
assert(e.data.when instanceof Date && e.data.when.getTime() === 5, 'a Date clones as a Date');
assert(e.origin === location.origin, 'origin is the page origin, got ' + e.origin);
assert(e.source === window, 'source is the window');
assert(Array.isArray(e.ports) && e.ports.length === 0, 'no ports');
assert(onGot[0] === e, 'onmessage and the listener see the same event');

// Ordering: two posts arrive in order.
got.length = 0;
window.postMessage('first', '*');
window.postMessage('second', '/');
advanceTime(20);
assert(got.length === 2 && got[0].data === 'first' && got[1].data === 'second',
       'messages arrive in post order');

// Options-dictionary overload.
got.length = 0;
window.postMessage(7, { targetOrigin: '*' });
advanceTime(20);
assert(got.length === 1 && got[0].data === 7, 'options overload delivers');

// A same-origin URL delivers, a different origin is silently dropped.
got.length = 0;
window.postMessage('same', location.origin + '/some/page');
window.postMessage('other', 'https://example.com');
advanceTime(20);
assert(got.length === 1 && got[0].data === 'same',
       'only the matching origin delivered, got ' + got.map((m) => m.data).join(','));

// A targetOrigin that is not a URL is a SyntaxError.
let threw = null;
try { window.postMessage('x', 'not a url'); } catch (err) { threw = err; }
assert(threw && threw.name === 'SyntaxError', 'bad targetOrigin throws SyntaxError');

// An uncloneable value throws at the call, and nothing is delivered.
got.length = 0;
threw = null;
try { window.postMessage({ f() {} }, '*'); } catch (err) { threw = err; }
assert(threw !== null, 'a function is not cloneable');
advanceTime(20);
assert(got.length === 0, 'nothing delivered after a failed clone');

// removeEventListener stops delivery.
window.removeEventListener('message', listener);
window.onmessage = null;
const onGotBefore = onGot.length;
window.postMessage('late', '*');
advanceTime(20);
assert(got.length === 0, 'a removed listener does not fire');
assert(onGot.length === onGotBefore, 'a cleared onmessage does not fire');

console.log('window.postMessage: OK');
