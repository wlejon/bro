// The global object has a prototype chain: window -> Window.prototype ->
// EventTarget.prototype -> Object.prototype. Without it String(window) threw
// ("Cannot convert object to primitive value") and Object.prototype methods
// were missing on window.

assert(String(window) === '[object Window]', 'String(window), got ' + String(window));
assert(`${globalThis}` === '[object Window]', 'template string of globalThis');
assert(Object.prototype.toString.call(window) === '[object Window]', 'toStringTag is Window');
assert(typeof Window === 'function', 'Window is a global interface object');
assert(window instanceof Window, 'window instanceof Window');
assert(window instanceof EventTarget, 'window instanceof EventTarget');
assert(Object.getPrototypeOf(window) === Window.prototype, 'the global\'s prototype is Window.prototype');
assert(Object.getPrototypeOf(Window.prototype) === EventTarget.prototype, 'Window.prototype extends EventTarget.prototype');
assert(Window.prototype.constructor === Window, 'Window.prototype.constructor');
assert(window.constructor === Window, 'window.constructor');
assert(window.hasOwnProperty('document'), 'Object.prototype methods reach the global');
assert(!Object.keys(Window.prototype).includes('constructor'), 'constructor is not enumerable');

let threw = false;
try { new Window(); } catch (e) { threw = e instanceof TypeError; }
assert(threw, 'new Window() is an Illegal constructor TypeError');

// The window's own event API still wins over EventTarget.prototype's.
let fired = 0;
const onPing = () => { fired++; };
window.addEventListener('bro-proto-ping', onPing);
window.dispatchEvent(new Event('bro-proto-ping'));
window.removeEventListener('bro-proto-ping', onPing);
window.dispatchEvent(new Event('bro-proto-ping'));
assert(fired === 1, 'window events still dispatch through the window\'s own listeners, got ' + fired);

// Plain globals still resolve and the page can still add its own.
globalThis.__protoProbe = 42;
assert(window.__protoProbe === 42 && Object.getOwnPropertyNames(window).includes('__protoProbe'),
    'page globals are own properties of the global');
delete globalThis.__protoProbe;

console.log('test_window_prototype: OK');
