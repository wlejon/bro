// What a page's write does to each of the host's globals, per the WebIDL of
// the Window's members (src/bronze_host/host_global_kinds.h), as Chromium has
// it: readonly attributes ignore a sloppy write and throw in strict code,
// [Replaceable] ones become the page's own data property, read-write ones
// (name, status, the on* handlers) take the value, and location's write
// navigates (PutForwards=href). The same in a worker, whose self is readonly.

function sloppyWrite(name) {
    // A sloppy function: the write is the global object's [[Set]].
    return new Function('v', name + ' = v; return true;');
}
function strictThrows(name) {
    const f = new Function('v', '"use strict"; ' + name + ' = v;');
    try { f(42); return false; } catch (e) { return e instanceof TypeError; }
}

// ---- readonly attributes ------------------------------------------------------
const readonly = {
    window, document, top, navigator, history, customElements,
    localStorage, sessionStorage, crypto, indexedDB,
};
for (const name of Object.keys(readonly)) {
    const before = readonly[name];
    assert(typeof before === 'object' && before !== null, name + ' is an object');
    sloppyWrite(name)(42);
    assert(globalThis[name] === before, name + ': a sloppy write leaves window.' + name);
    assert(new Function('return ' + name)() === before, name + ': ... and the bare name');
    assert(strictThrows(name), name + ': a strict write is a TypeError');
    globalThis[name] = 43;  // through the property, sloppy
    assert(globalThis[name] === before, name + ': window.' + name + ' = v is ignored');
    const d = Object.getOwnPropertyDescriptor(window, name);
    assert(d && d.writable !== true && !d.set, name + ' has no setter and is not writable');
    assert(name in window, "'" + name + "' in window");
}
assert(window === self && top === window && frames === window && parent === window,
       'a top-level window is its own self, top, frames and parent');
assert(frameElement === null, 'a top-level window has no frameElement');

// ---- [Replaceable] attributes and operations ------------------------------------
// Live values answered by a getter whose setter replaces them.
for (const name of ['innerWidth', 'innerHeight', 'outerWidth', 'outerHeight', 'scrollX', 'scrollY',
                    'pageXOffset', 'pageYOffset', 'screenX', 'screenY', 'screenLeft', 'screenTop',
                    'devicePixelRatio', 'length', 'opener', 'origin', 'event']) {
    const d = Object.getOwnPropertyDescriptor(window, name);
    assert(d && typeof d.get === 'function' && typeof d.set === 'function' && d.configurable,
           name + ' is a configurable accessor with a setter');
}
assert(typeof innerWidth === 'number' && innerWidth > 0, 'innerWidth reads the viewport');
assert(length === 0, 'window.length counts the frames: none');
assert(opener === null, 'opener is null');
assert(typeof origin === 'string' && origin === location.origin, 'origin is the page origin');
assert(event === undefined, 'window.event is undefined outside a listener');
for (const name of ['locationbar', 'menubar', 'personalbar', 'scrollbars', 'statusbar', 'toolbar']) {
    assert(globalThis[name] && globalThis[name].visible === true, name + '.visible');
}
assert(clientInformation === navigator, 'clientInformation is navigator');
assert(typeof external === 'object' && typeof external.AddSearchProvider === 'function', 'external');
assert(closed === false && isSecureContext === true, 'closed / isSecureContext');

// The page's write makes the name its own data property.
window.innerHeight = 7;
let d = Object.getOwnPropertyDescriptor(window, 'innerHeight');
assert(d && d.value === 7 && d.writable && d.enumerable && d.configurable,
       'window.innerHeight = 7 replaces it with a data property');
sloppyWrite('scrollY')('x');
assert(window.scrollY === 'x', 'a bare sloppy write to scrollY replaces it');
assert((function () { 'use strict'; pageYOffset = 5; return window.pageYOffset; })() === 5,
       'a strict write to a [Replaceable] attribute replaces it too');
for (const name of ['screen', 'performance', 'self', 'parent', 'frames', 'alert', 'getComputedStyle']) {
    const before = globalThis[name];
    globalThis[name] = 42;
    assert(globalThis[name] === 42, 'window.' + name + ' = 42 replaces it');
    globalThis[name] = before;
}

// ---- read-write attributes -------------------------------------------------------
sloppyWrite('name')(42);
assert(name === '42' && window.name === '42', 'name keeps the string of what is written');
status = 'busy';
assert(status === 'busy' && window.status === 'busy', 'status keeps what is written');
for (const name of ['onerror', 'onmessage', 'onunhandledrejection', 'onload', 'onclick']) {
    const d2 = Object.getOwnPropertyDescriptor(window, name);
    assert(d2 && d2.get && d2.set, name + ' is an accessor');
    assert(globalThis[name] === null, name + ' starts null');
    const f = () => {};
    globalThis[name] = f;
    assert(globalThis[name] === f, name + ' keeps a function');
    globalThis[name] = 5;
    assert(globalThis[name] === null, name + ' = a non-object is null');
}
let errorSeen = null;
onerror = (msg) => { errorSeen = String(msg); return true; };
assert(window.onerror !== null, 'a bare onerror = f sets the handler');
onerror = null;

// window.event during a listener.
let seen = 'unset';
const btn = document.createElement('button');
document.body.appendChild(btn);
btn.addEventListener('click', (e) => { seen = window.event === e; });
btn.click();
assert(seen === true, 'window.event is the event its listener is running for');
assert(window.event === undefined, 'and undefined again after it');

// ---- location: [PutForwards=href] --------------------------------------------------
const loc = location;
assert(typeof loc.href === 'string', 'location is the Location');
let hashes = 0;
window.addEventListener('hashchange', () => { hashes++; });
location = '#kinds';
assert(location === loc, 'location stays the same object');
assert(location.hash === '#kinds', 'location = "#kinds" navigates to the fragment, hash ' + location.hash);
advanceTime(16);
assert(hashes === 1, 'and fires hashchange');
const ld = Object.getOwnPropertyDescriptor(window, 'location');
assert(ld && ld.get && ld.set, 'location is an accessor with a setter');

// ---- worker: self is readonly -------------------------------------------------------
const src = `
  var r = {};
  var before = self;
  (function () { self = 42; })();
  r.sloppy = self === before;
  try { (function () { 'use strict'; self = 42; })(); r.strict = 'no throw'; }
  catch (e) { r.strict = e instanceof TypeError; }
  r.onmessage = onmessage === null;
  onmessage = function (e) { postMessage(r); };
`;
const w = new Worker(URL.createObjectURL(new Blob([src], { type: 'text/javascript' })));
let got = null;
w.onmessage = (e) => { got = e.data; };
w.postMessage('go');
const deadline = Date.now() + 15000;
while (got === null && Date.now() < deadline) { advanceTime(16); wallSleep(2); }
w.terminate();
assert(got !== null, 'the worker answered through a bare onmessage = f');
assert(got.sloppy === true, 'a sloppy write to a worker\'s self is ignored');
assert(got.strict === true, 'a strict write to a worker\'s self is a TypeError, got ' + got.strict);
assert(got.onmessage === true, 'a worker\'s onmessage starts null');

console.log('test_global_kinds: OK');
