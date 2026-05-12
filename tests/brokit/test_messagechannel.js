// Test MessageChannel.

assert(typeof MessageChannel === 'function', 'MessageChannel exists');

const mc = new MessageChannel();
assert(mc.port1, 'port1 exists');
assert(mc.port2, 'port2 exists');
assert(mc.port1 !== mc.port2, 'ports are different');

assert(typeof mc.port1.postMessage === 'function', 'port1.postMessage');
assert(typeof mc.port2.postMessage === 'function', 'port2.postMessage');

let received1 = null;
let received2 = null;

mc.port2.onmessage = (e) => { received2 = e.data; };
mc.port1.onmessage = (e) => { received1 = e.data; };

// MessagePort needs start() to begin receiving in spec, but onmessage setter
// implicitly starts in many impls.
if (typeof mc.port1.start === 'function') mc.port1.start();
if (typeof mc.port2.start === 'function') mc.port2.start();

mc.port1.postMessage('hello-from-1');
mc.port2.postMessage({ x: 42 });

// Allow event loop to deliver
for (let i = 0; i < 10; i++) { flush(); advanceTime(10); flush(); sleep(5); flush(); }

assert(received2 === 'hello-from-1', 'port2 got from port1: ' + JSON.stringify(received2));
assert(received1 && received1.x === 42, 'port1 got from port2: ' + JSON.stringify(received1));

// close
if (typeof mc.port1.close === 'function') mc.port1.close();
